// SPDX-License-Identifier: GPL-3.0-only

#include <htslib/bgzf.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "longlineage/artifact/bgzf_tsv_writer.hpp"
#include "longlineage/artifact/run_state.hpp"
#include "longlineage/runtime/byte_bounded_queue.hpp"
#include "longlineage/runtime/ordered_thread_pool.hpp"
#include "longlineage/solver/evidence_builder.hpp"
#include "longlineage/solver/obligation_bnb.hpp"
#include "longlineage/solver/parent_mapping.hpp"
#include "longlineage/solver/small_q_oracle.hpp"
#include "longlineage/solver/terminal_subset_dp.hpp"
#include "longlineage/solver/topology_record.hpp"
#include "longlineage/solver/topology_router.hpp"
#include "longlineage/solver/vertex_set_ranker.hpp"

namespace {

using longlineage::artifact::BgzfTsvWriter;
using longlineage::artifact::FreezeEvidence;
using longlineage::artifact::RunState;
using longlineage::artifact::RunStateGuard;
using longlineage::artifact::sha256_hex;
using longlineage::artifact::ValidationEvidence;
using longlineage::runtime::ByteBoundedQueue;
using longlineage::runtime::OrderedThreadPool;
using longlineage::runtime::PoolStatus;
using longlineage::runtime::QueueOperationStatus;
using longlineage::runtime::QueueState;
using longlineage::solver::build_topology_evidence;
using longlineage::solver::CandidateVertexSetScore;
using longlineage::solver::evaluate_ranked_parent_edges;
using longlineage::solver::EvidenceAdapterState;
using longlineage::solver::EvidenceBuilderOptions;
using longlineage::solver::exact_vertex_set_sha256;
using longlineage::solver::ExactFamilyState;
using longlineage::solver::ExactKernelReason;
using longlineage::solver::ExactObjectiveState;
using longlineage::solver::ExactSolverRoute;
using longlineage::solver::ExactTopologyProblem;
using longlineage::solver::FamilyRankingGate;
using longlineage::solver::HypercubeVertex;
using longlineage::solver::make_unresolved_multi_mutation_projection;
using longlineage::solver::ObligationBnbOptions;
using longlineage::solver::ParentEdgeEndpointRequest;
using longlineage::solver::ParentEdgeEndpointState;
using longlineage::solver::ParentEdgeLocalScore;
using longlineage::solver::QualityPatternEvidence;
using longlineage::solver::rank_complete_vertex_sets;
using longlineage::solver::ReadPatternObservation;
using longlineage::solver::serialize_topology_unit_v2;
using longlineage::solver::solve_obligation_bnb;
using longlineage::solver::solve_small_q_exact;
using longlineage::solver::solve_terminal_subset_objective;
using longlineage::solver::solve_topology_exact;
using longlineage::solver::summarize_exact_parent_mappings;
using longlineage::solver::TopologyEvidenceBundle;
using longlineage::solver::TopologyFamilyStatus;
using longlineage::solver::TopologyObjectiveStatus;
using longlineage::solver::TopologyProblem;
using longlineage::solver::TopologyReason;
using longlineage::solver::TopologyRecordIdentity;
using longlineage::solver::VertexSetRankerOptions;
using longlineage::solver::VertexSetRankingResult;
using longlineage::solver::VertexSetRankingState;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_byte_bounded_queue() {
    ByteBoundedQueue<int> queue(4);
    check(queue.push(7, 3) == QueueOperationStatus::kSuccess, "queue must accept an in-bound item");
    check(queue.snapshot().queued_bytes == 3, "queue must charge declared bytes");
    check(queue.push(8, 5) == QueueOperationStatus::kItemTooLarge, "queue must reject an over-capacity item");
    check(queue.push(8, 0) == QueueOperationStatus::kInvalidByteSize, "queue must reject zero-byte admission");

    auto first = queue.pop();
    check(first.status == QueueOperationStatus::kSuccess && first.value == 7 && first.charged_bytes == 3,
          "queue pop must retain the value and byte charge");
    check(queue.close(), "first queue close must transition state");
    check(queue.pop().status == QueueOperationStatus::kClosed, "closed empty queue must report CLOSED");
    check(queue.snapshot().peak_queued_bytes <= queue.snapshot().capacity_bytes,
          "queue peak bytes must not exceed capacity");

    ByteBoundedQueue<int> cancelled(2);
    check(cancelled.push(1, 2) == QueueOperationStatus::kSuccess, "cancel test must fill queue");
    std::promise<void> entered;
    std::future<QueueOperationStatus> blocked = std::async(std::launch::async, [&] {
        entered.set_value();
        return cancelled.push(2, 1);
    });
    entered.get_future().wait();
    check(cancelled.cancel("synthetic worker failure"), "cancel must transition queue state");
    check(blocked.get() == QueueOperationStatus::kCancelled, "cancel must wake a blocked producer");
    check(cancelled.pop().status == QueueOperationStatus::kCancelled, "cancelled queue must not drain stale work");
    const auto snapshot = cancelled.snapshot();
    check(snapshot.state == QueueState::kCancelled && snapshot.queued_bytes == 0 && snapshot.queued_items == 0,
          "cancel must discard queued work");
}

std::vector<int> run_ordered_pool(std::size_t workers) {
    OrderedThreadPool<int> pool(workers, 64);
    for (int index = 0; index < 8; ++index) {
        const auto submitted = pool.submit(8, [index] {
            std::this_thread::sleep_for(std::chrono::milliseconds((7 - index) % 3));
            return index * index;
        });
        check(submitted.status == PoolStatus::kSuccess && submitted.sequence == static_cast<std::uint64_t>(index),
              "pool submissions must receive frozen monotonic sequence numbers");
    }
    auto batch = pool.finish();
    check(batch.status == PoolStatus::kSuccess, "successful pool must return SUCCESS");
    check(batch.submitted == 8 && batch.completed == 8, "successful pool must account for every task");
    check(batch.peak_queued_bytes <= 64, "thread-pool queue must remain byte bounded");
    return batch.ordered_results;
}

void test_ordered_thread_pool() {
    const std::vector<int> one_worker = run_ordered_pool(1);
    const std::vector<int> four_workers = run_ordered_pool(4);
    check(one_worker == four_workers, "logical result order must be invariant to worker count");
    check(one_worker == std::vector<int>({0, 1, 4, 9, 16, 25, 36, 49}), "ordered pool must restore submission order");

    OrderedThreadPool<int> failing_pool(2, 32);
    std::promise<void> release_failure;
    std::shared_future<void> failure_gate = release_failure.get_future().share();
    check(failing_pool.submit(8, [] { return 1; }).status == PoolStatus::kSuccess, "failure test task 0 must submit");
    check(failing_pool
                  .submit(8,
                          [failure_gate]() -> int {
                              failure_gate.wait();
                              throw std::runtime_error("synthetic worker error");
                          })
                  .status == PoolStatus::kSuccess,
          "failure test task 1 must submit");
    check(failing_pool.submit(8, [] { return 3; }).status == PoolStatus::kSuccess, "failure test task 2 must submit");
    release_failure.set_value();
    auto failed = failing_pool.finish();
    check(failed.status == PoolStatus::kWorkerError, "worker exception must fail the batch");
    check(failed.failed_sequence.has_value(), "worker failure must retain a failing sequence");
    check(failed.ordered_results.empty(), "failed batch must never publish partial ordered results");
}

std::string read_bgzf(const std::string& path) {
    BGZF* input = bgzf_open(path.c_str(), "r");
    check(input != nullptr, "test must reopen written BGZF");
    std::string content;
    std::vector<char> buffer(128);
    while (true) {
        const ssize_t count = bgzf_read(input, buffer.data(), buffer.size());
        check(count >= 0, "BGZF test read must not fail");
        if (count == 0) {
            break;
        }
        content.append(buffer.data(), static_cast<std::size_t>(count));
    }
    check(bgzf_close(input) == 0, "BGZF test reader must close cleanly");
    return content;
}

bool has_canonical_bgzf_eof(const std::string& path) {
    static constexpr std::array<unsigned char, 28> kCanonicalEof = {
        0x1f, 0x8b, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x06, 0x00, 0x42, 0x43,
        0x02, 0x00, 0x1b, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    if (input.tellg() < static_cast<std::streamoff>(kCanonicalEof.size())) {
        return false;
    }
    input.seekg(-static_cast<std::streamoff>(kCanonicalEof.size()), std::ios::end);
    std::array<unsigned char, kCanonicalEof.size()> observed{};
    input.read(reinterpret_cast<char*>(observed.data()), static_cast<std::streamsize>(observed.size()));
    return input.gcount() == static_cast<std::streamsize>(observed.size()) && observed == kCanonicalEof;
}

void test_bgzf_writer_and_run_state() {
    const std::string test_prefix =
        "/tmp/longlineage_test_runtime_solver_" + std::to_string(static_cast<long long>(::getpid()));
    const std::string path = test_prefix + ".tsv.bgz";
    BgzfTsvWriter writer(path, "longlineage.synthetic", "1.0.0", "synthetic-run-a", {"key", "value"}, 2);
    writer.write_row({"1", "alpha"});
    writer.write_row({"2", "beta"});
    const auto receipt = writer.close();
    const std::string expected_physical =
        "##longlineage_schema=longlineage.synthetic\n"
        "##schema_version=1.0.0\n"
        "##run_id=synthetic-run-a\n"
        "#key\tvalue\n"
        "1\talpha\n"
        "2\tbeta\n";
    const std::string expected_semantic = "longlineage.synthetic\t1.0.0\nkey\tvalue\n1\talpha\n2\tbeta\n";
    check(receipt.row_count == 2, "TSV receipt must exclude the header from row count");
    check(receipt.logical_bytes == expected_semantic.size(), "TSV receipt must report exact semantic bytes");
    check(receipt.semantic_sha256 == sha256_hex(expected_semantic),
          "semantic SHA must bind schema/version, exact header, and rows");
    check(read_bgzf(path) == expected_physical, "BGZF decompression must reproduce preamble, header, and rows");
    check(receipt.physical_sha256.size() == 64 && receipt.physical_bytes > 0,
          "TSV receipt must include physical artifact evidence");
    check(receipt.run_id == "synthetic-run-a", "TSV receipt must retain the physical run_id binding");
    check(has_canonical_bgzf_eof(path), "BGZF writer must append the canonical 28-byte EOF marker");
    check(writer.close().semantic_sha256 == receipt.semantic_sha256, "writer close must be idempotent");

    const std::string single_thread_path = test_prefix + "_single_thread.tsv.bgz";
    BgzfTsvWriter single_thread_writer(single_thread_path, "longlineage.synthetic", "1.0.0", "synthetic-run-b",
                                       {"key", "value"}, 1);
    single_thread_writer.write_row({"1", "alpha"});
    single_thread_writer.write_row({"2", "beta"});
    const auto single_thread_receipt = single_thread_writer.close();
    check(single_thread_receipt.semantic_sha256 == receipt.semantic_sha256,
          "logical semantic SHA must be invariant to BGZF compression thread count");

    bool invalid_utf8_rejected = false;
    try {
        BgzfTsvWriter invalid_writer(test_prefix + "_invalid.tsv.bgz", "longlineage.synthetic", "1.0.0",
                                     "synthetic-run-invalid", {"key"}, 1);
        invalid_writer.write_row({std::string("\xc0\xaf", 2)});
    } catch (const std::invalid_argument&) {
        invalid_utf8_rejected = true;
    }
    check(invalid_utf8_rejected, "TSV writer must reject malformed UTF-8");

    RunStateGuard guard;
    check(!guard.mark_frozen({true, "/tmp/final"}).accepted, "RUNNING -> FROZEN must be rejected");
    check(!guard.mark_validated({false, std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c')}).accepted,
          "validator all_pass=false must not create VALIDATED");
    check(!guard.mark_validated({true, std::string(64, 'a'), "not-a-sha", std::string(64, 'c')}).accepted,
          "malformed validator SHA must fail closed");
    check(guard
              .mark_validated(ValidationEvidence{
                  true,
                  std::string(64, 'a'),
                  std::string(64, 'b'),
                  std::string(64, 'c'),
              })
              .accepted,
          "valid independent-validator evidence must create VALIDATED");
    check(guard.state() == RunState::kValidated, "run state must be VALIDATED");
    check(guard.producer_receipt_sha256() == std::string(64, 'a') &&
              guard.validator_receipt_sha256() == std::string(64, 'b') &&
              guard.validator_executable_sha256() == std::string(64, 'c'),
          "VALIDATED state must retain every receipt/executable provenance digest");
    check(guard.mark_frozen(FreezeEvidence{true, "/tmp/validated-frozen"}).accepted,
          "validated atomic rename must create VALIDATED_FROZEN");
    check(guard.state() == RunState::kValidatedFrozen, "run state must be VALIDATED_FROZEN");

    RunStateGuard failed_guard;
    check(failed_guard.mark_failed("synthetic worker error").accepted, "RUNNING -> FAILED must be accepted");
    check(
        !failed_guard.mark_validated({true, std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c')}).accepted,
        "FAILED run must never transition to VALIDATED");
}

void test_small_q_oracle() {
    const auto unique = solve_small_q_exact(TopologyProblem{1, {"A"}, {}});
    check(unique.objective_status == TopologyObjectiveStatus::kObjectiveCertified &&
              unique.family_status == TopologyFamilyStatus::kFamilyComplete,
          "q=1 objective and family must be certified complete");
    check(unique.minimum_hidden_vertices == 0 && unique.minimum_family.size() == 1,
          "root+A must need no hidden state and have one family");
    check(unique.minimum_family[0].vertices == std::vector<std::uint64_t>({0, 1}),
          "q=1 family must contain root and observed ALT");
    check(unique.minimum_family[0].tree_count == 1 && unique.winner_index == 0,
          "unique complete structural family may expose winner 0");

    const auto tied = solve_small_q_exact(TopologyProblem{2, {"AR"}, {"XA"}});
    check(tied.minimum_hidden_vertices == 1 && tied.minimum_family.size() == 2,
          "partial group must yield both exact minimum completions");
    check(tied.minimum_family[0].vertices == std::vector<std::uint64_t>({0, 1, 2}),
          "first minimum family must be canonical");
    check(tied.minimum_family[1].vertices == std::vector<std::uint64_t>({0, 1, 3}),
          "second minimum family must be canonical");
    check(!tied.winner_index.has_value(), "multiple complete structural families must not invent a winner");

    const auto parent_count = solve_small_q_exact(TopologyProblem{2, {"AR", "RA", "AA"}, {}});
    check(parent_count.minimum_family.size() == 1 && parent_count.minimum_family[0].legal_parent_count == 4 &&
              parent_count.minimum_family[0].tree_count == 2,
          "AA with both singletons must have two legal parent mappings");
    check(!parent_count.winner_index.has_value(),
          "unique vertex family with multiple parent mappings must not invent a tree winner");
    const auto& legal = parent_count.minimum_family[0].legal_parents;
    check(legal.back().vertex == 3 && legal.back().parents == std::vector<std::uint64_t>({1, 2}),
          "legal parent enumeration must retain both Hamming-1 predecessors");

    const auto joint_groups = solve_small_q_exact(TopologyProblem{2, {}, {"AX", "XA"}});
    check(joint_groups.minimum_hidden_vertices == 2 && joint_groups.minimum_family.size() == 3,
          "AX+XA authority golden must jointly yield h*=2 and all three vertex sets");
    check(!joint_groups.winner_index.has_value(), "AX+XA complete tie must retain no winner");

    const auto four_bit_chain = solve_small_q_exact(TopologyProblem{4, {"AAAA"}, {}});
    check(four_bit_chain.objective_status == TopologyObjectiveStatus::kObjectiveCertified &&
              four_bit_chain.family_status == TopologyFamilyStatus::kFamilyComplete &&
              four_bit_chain.minimum_hidden_vertices == 3 && four_bit_chain.minimum_family.size() == 24,
          "q=4 root-to-AAAA chain must enumerate all 4! minimum vertex sets");
    check(!four_bit_chain.winner_index.has_value(), "q=4 24-family tie must not invent a winner");

    const auto too_large = solve_small_q_exact(TopologyProblem{5, {"AAAAA"}, {}});
    check(too_large.objective_status == TopologyObjectiveStatus::kAbstainKernelNotVerified &&
              too_large.family_status == TopologyFamilyStatus::kAbstainKernelNotVerified &&
              too_large.reason == TopologyReason::kSolverRouteNotVerified,
          "q>4 P5 scaffold must explicitly abstain because the production kernel is unverified");
    check(!too_large.minimum_hidden_vertices.has_value() && too_large.minimum_family.empty() &&
              !too_large.winner_index.has_value(),
          "abstaining q>4 result must have no objective family or winner");

    const auto malformed = solve_small_q_exact(TopologyProblem{2, {"AZ"}, {}});
    check(malformed.family_status == TopologyFamilyStatus::kAbstainNotIdentifiable &&
              malformed.minimum_family.empty() && !malformed.winner_index.has_value(),
          "invalid input must fail closed without a family or winner");
}

void test_exact_parent_mapping() {
    const auto diamond = summarize_exact_parent_mappings(2, {0, 1, 2, 3});
    check(diamond.valid && diamond.legal_parent_count == "4" && diamond.tree_count == "2",
          "fixed diamond vertex set must factorize to four legal edges and two parent mappings");
    check(diamond.legal_parents.back().vertex == 3 &&
              diamond.legal_parents.back().parents == std::vector<HypercubeVertex>({1, 2}),
          "exact parent factorization must retain both parents of the diamond sink");

    std::vector<HypercubeVertex> six_bit_cube(64);
    std::iota(six_bit_cube.begin(), six_bit_cube.end(), HypercubeVertex{0});
    const auto large = summarize_exact_parent_mappings(6, six_bit_cube);
    check(large.valid && large.legal_parent_count == "192",
          "full six-bit cube must have m*2^(m-1) legal directed edges");
    check(large.tree_count == "11501279977342425366528000000",
          "multiprecision parent mapping count must remain exact beyond uint64");

    const auto disconnected = summarize_exact_parent_mappings(2, {0, 3});
    check(!disconnected.valid && disconnected.tree_count == "0",
          "fixed vertex set with no legal predecessor must fail closed");
}

void test_exact_obligation_bnb_and_dp() {
    const ExactTopologyProblem partial_tie{2, {1}, {{2, 3}}};
    const auto tied = solve_obligation_bnb(partial_tie);
    check(tied.objective_state == ExactObjectiveState::kObjectiveCertified &&
              tied.family_state == ExactFamilyState::kFamilyComplete && tied.objective_h == 1 &&
              tied.minimum_family.size() == 2,
          "dynamic-obligation B&B must certify h*=1 and both tied minimum families");
    check(tied.minimum_family[0].vertices == std::vector<HypercubeVertex>({0, 1, 2}) &&
              tied.minimum_family[1].vertices == std::vector<HypercubeVertex>({0, 1, 3}),
          "B&B minimum family must be canonical and complete");
    check(tied.objective_search_exhausted && tied.family_enumeration_exhausted,
          "complete B&B status must be backed by exhausted objective and family searches");

    const auto dp = solve_terminal_subset_objective(partial_tie);
    check(dp.objective_state == ExactObjectiveState::kObjectiveCertified && dp.objective_h == tied.objective_h,
          "group-terminal subset DP must independently certify only the same objective");

    const ExactTopologyProblem four_bit_chain{4, {15}, {}};
    const auto chain = solve_obligation_bnb(four_bit_chain);
    check(chain.objective_h == 3 && chain.minimum_family.size() == 24,
          "four-bit all-ALT terminal must have h*=3 and all 4! rooted chains");

    ObligationBnbOptions family_cap;
    family_cap.maximum_complete_family_size = 2;
    const auto capped = solve_obligation_bnb(four_bit_chain, family_cap);
    check(capped.objective_state == ExactObjectiveState::kObjectiveCertified && capped.objective_h == 3 &&
              capped.family_state == ExactFamilyState::kFamilyIncompleteCap && capped.minimum_family.empty() &&
              capped.objective_search_exhausted && !capped.family_enumeration_exhausted,
          "family cap may retain exhaustive h* proof but must withhold every partial family candidate");

    ObligationBnbOptions node_cap;
    node_cap.maximum_search_nodes = 1;
    const auto interrupted = solve_obligation_bnb(partial_tie, node_cap);
    check(interrupted.objective_state == ExactObjectiveState::kAbstainResourceLimit &&
              interrupted.family_state == ExactFamilyState::kAbstainResourceLimit &&
              interrupted.reason == ExactKernelReason::kSearchNodeLimitReached &&
              !interrupted.objective_h.has_value() && interrupted.minimum_family.empty(),
          "node-limited B&B must withhold both objective and family");

    const auto unsupported = solve_obligation_bnb(ExactTopologyProblem{13, {}, {}});
    check(unsupported.objective_state == ExactObjectiveState::kAbstainKernelNotVerified &&
              unsupported.family_state == ExactFamilyState::kAbstainKernelNotVerified &&
              unsupported.reason == ExactKernelReason::kUnsupportedBitCount && !unsupported.objective_h.has_value(),
          "B&B must fail closed outside the declared 12-bit boundary");
}

void test_exact_topology_router_differentials() {
    const auto tied = solve_topology_exact(TopologyProblem{2, {"AR"}, {"XA"}});
    check(tied.route == ExactSolverRoute::kSmallQOracleDifferentialBnb && tied.small_q_oracle_checked &&
              tied.small_q_oracle_match && tied.objective_dp_checked && tied.objective_dp_match &&
              tied.structural.objective_h == 1 && tied.structural.minimum_family.size() == 2,
          "q<=4 router must require matching oracle, B&B and objective-DP evidence");

    const auto joint = solve_topology_exact(TopologyProblem{2, {}, {"AX", "XA"}});
    check(joint.small_q_oracle_match && joint.objective_dp_match && joint.structural.objective_h == 2 &&
              joint.structural.minimum_family.size() == 3,
          "AX+XA authority golden must match across all three exact routes");

    const auto noncontiguous = solve_topology_exact(TopologyProblem{5, {"ARRRA"}, {"XARRX"}});
    check(noncontiguous.active_loci == std::vector<std::size_t>({0, 1, 4}) && noncontiguous.small_q_oracle_match &&
              noncontiguous.objective_dp_match,
          "router differential must preserve non-contiguous original-locus compression");

    const auto five_bit = solve_topology_exact(TopologyProblem{5, {"AAAAA"}, {}});
    check(five_bit.route == ExactSolverRoute::kBitsetObligationBnb && !five_bit.small_q_oracle_checked &&
              five_bit.objective_dp_match && five_bit.structural.objective_h == 4 &&
              five_bit.structural.minimum_family.size() == 120,
          "m=5 must route to exact B&B, cross-check h* by DP and enumerate all 5! chains");

    longlineage::solver::TopologyRouterOptions capped_options;
    capped_options.bnb.maximum_complete_family_size = 2;
    const auto capped = solve_topology_exact(TopologyProblem{4, {"AAAA"}, {}}, capped_options);
    check(capped.small_q_oracle_match &&
              capped.structural.objective_state == ExactObjectiveState::kObjectiveCertified &&
              capped.structural.family_state == ExactFamilyState::kFamilyIncompleteCap &&
              capped.structural.minimum_family.empty(),
          "router must preserve objective-certified family-cap status without publishing a partial family");

    longlineage::solver::TopologyRouterOptions interrupted_options;
    interrupted_options.bnb.maximum_search_nodes = 1;
    const auto interrupted = solve_topology_exact(TopologyProblem{2, {"AR"}, {"XA"}}, interrupted_options);
    check(interrupted.structural.objective_state == ExactObjectiveState::kAbstainResourceLimit &&
              interrupted.structural.reason == ExactKernelReason::kSearchNodeLimitReached &&
              !interrupted.small_q_oracle_checked && interrupted.structural.minimum_family.empty(),
          "resource-limited router must retain resource abstention rather than relabel it as a differential mismatch");

    const auto too_large = solve_topology_exact(TopologyProblem{13, {"AAAAAAAAAAAAA"}, {}});
    check(too_large.route == ExactSolverRoute::kAbstain &&
              too_large.structural.objective_state == ExactObjectiveState::kAbstainKernelNotVerified &&
              too_large.structural.minimum_family.empty(),
          "router must abstain outside m<=12 without falling back to an unverified kernel");

    const std::vector<std::string> three_bit_full = {"RRR", "ARR", "RAR", "RRA", "AAR", "ARA", "RAA", "AAA"};
    const std::vector<std::string> three_bit_partial = {"AXX", "XAX", "XXA", "AAX", "AXA", "XAA", "RAX", "XAR"};
    std::size_t differential_cases = 0;
    for (const std::string& full : three_bit_full) {
        for (const std::string& partial : three_bit_partial) {
            const auto result = solve_topology_exact(TopologyProblem{3, {full}, {partial}});
            check(result.small_q_oracle_checked && result.small_q_oracle_match && result.objective_dp_checked &&
                      result.objective_dp_match && result.structural.family_state == ExactFamilyState::kFamilyComplete,
                  "three-bit systematic oracle/B&B/DP differential case must match");
            ++differential_cases;
        }
    }
    for (std::size_t index = 0; index < three_bit_partial.size(); ++index) {
        const auto result = solve_topology_exact(
            TopologyProblem{3,
                            {three_bit_full[index]},
                            {three_bit_partial[index], three_bit_partial[(index + 1) % three_bit_partial.size()]}});
        check(result.small_q_oracle_match && result.objective_dp_match,
              "paired group-terminal systematic differential case must match");
        ++differential_cases;
    }
    check(differential_cases == 72, "systematic q<=3 differential matrix must execute all 72 frozen cases");
}

ReadPatternObservation make_read_pattern(std::string pattern, const std::vector<int>& qualities,
                                         std::uint64_t multiplicity) {
    ReadPatternObservation observation;
    observation.pattern_raox = std::move(pattern);
    observation.multiplicity = multiplicity;
    for (int quality : qualities) {
        if (quality < 0) {
            observation.base_qualities.push_back(std::nullopt);
        } else {
            observation.base_qualities.push_back(static_cast<std::uint8_t>(quality));
        }
    }
    return observation;
}

std::vector<ReadPatternObservation> frozen_bq_patterns() {
    return {
        make_read_pattern("AROX", {30, 30, -1, -1}, 3),
        make_read_pattern("RXAO", {30, -1, 30, -1}, 3),
        make_read_pattern("AOAX", {30, -1, 30, -1}, 1),
        make_read_pattern("XXXX", {-1, -1, -1, -1}, 5),
    };
}

bool quality_groups_equal(const TopologyEvidenceBundle& left, const TopologyEvidenceBundle& right) {
    if (left.scoring_groups.size() != right.scoring_groups.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.scoring_groups.size(); ++index) {
        const QualityPatternEvidence& a = left.scoring_groups[index];
        const QualityPatternEvidence& b = right.scoring_groups[index];
        if (a.pattern_rax != b.pattern_rax || a.base_qualities != b.base_qualities ||
            a.multiplicity != b.multiplicity ||
            std::abs(a.inactive_log_probability - b.inactive_log_probability) > 1e-14) {
            return false;
        }
    }
    return true;
}

double independent_active_emission(char observed, std::int16_t quality, bool state_is_alt) {
    if (observed == 'X') {
        return 1.0;
    }
    check((observed == 'R' || observed == 'A') && quality >= 0, "brute-force oracle received malformed BQ evidence");
    const double error = std::clamp(std::pow(10.0, -static_cast<double>(quality) / 10.0), 1e-6, 0.25);
    const double denominator = 1.0 - 2.0 * error / 3.0;
    const double match = (1.0 - error) / denominator;
    const double flip = (error / 3.0) / denominator;
    return ((observed == 'A') == state_is_alt) ? match : flip;
}

double independent_mixture_log_likelihood(const TopologyEvidenceBundle& evidence,
                                          const std::vector<HypercubeVertex>& vertices,
                                          const std::array<double, 3>& weights) {
    check(vertices.size() == weights.size(), "brute-force simplex dimensions must match");
    double log_likelihood = 0.0;
    for (const QualityPatternEvidence& group : evidence.scoring_groups) {
        double mixture_probability = 0.0;
        for (std::size_t component = 0; component < vertices.size(); ++component) {
            double probability = 1.0;
            for (std::size_t bit = 0; bit < group.pattern_rax.size(); ++bit) {
                const bool state_is_alt =
                    (vertices[component] & static_cast<HypercubeVertex>(HypercubeVertex{1} << bit)) != 0;
                probability *=
                    independent_active_emission(group.pattern_rax[bit], group.base_qualities[bit], state_is_alt);
            }
            mixture_probability += weights[component] * probability;
        }
        check(mixture_probability > 0.0, "brute-force mixture probability must be positive");
        log_likelihood +=
            static_cast<double>(group.multiplicity) * (group.inactive_log_probability + std::log(mixture_probability));
    }
    return log_likelihood;
}

double brute_force_three_component_simplex(const TopologyEvidenceBundle& evidence,
                                           const std::vector<HypercubeVertex>& vertices) {
    check(vertices.size() == 3, "frozen small-q likelihood oracle requires three components");
    constexpr std::size_t kGrid = 2000;
    double best = -std::numeric_limits<double>::infinity();
    for (std::size_t first = 0; first <= kGrid; ++first) {
        for (std::size_t second = 0; second + first <= kGrid; ++second) {
            const std::size_t third = kGrid - first - second;
            const std::array<double, 3> weights = {
                static_cast<double>(first) / static_cast<double>(kGrid),
                static_cast<double>(second) / static_cast<double>(kGrid),
                static_cast<double>(third) / static_cast<double>(kGrid),
            };
            best = std::max(best, independent_mixture_log_likelihood(evidence, vertices, weights));
        }
    }
    return best;
}

void write_text_file(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "topology adapter test output must open");
    output << content;
    check(static_cast<bool>(output), "topology adapter test output must be complete");
}

void test_topology_evidence_ranking_and_record_adapter() {
    EvidenceBuilderOptions evidence_options;
    evidence_options.structural_minimum_multiplicity = 2;
    const std::vector<ReadPatternObservation> observations = frozen_bq_patterns();
    const TopologyEvidenceBundle evidence = build_topology_evidence(observations, evidence_options);
    check(evidence.state == EvidenceAdapterState::kReady && evidence.active_loci == std::vector<std::size_t>({0, 2}),
          "evidence adapter must compress structural ALT support onto original loci 0 and 2");
    check(evidence.structural_patterns.size() == 2 && evidence.scoring_groups.size() == 3,
          "structural minread must not discard a low-count informative group from BQ scoring");
    check(evidence.structural_patterns[0].pattern_rax == "AX" && evidence.structural_patterns[1].pattern_rax == "RA" &&
              std::all_of(evidence.structural_patterns.begin(), evidence.structural_patterns.end(),
                          [](const auto& pattern) { return pattern.pattern_rax.find('O') == std::string::npos; }),
          "measured O calls must be marginalized to X before structural subcube construction");
    check(evidence.input_observations == 12 && evidence.informative_observations == 7 &&
              evidence.all_missing_observations == 5,
          "evidence adapter must account for every input molecule including all-X observations");
    check(evidence.raw_code_counts == std::array<std::uint64_t, 4>({6, 8, 7, 27}),
          "R/A/O/X counters must retain raw evidence classes before O/X marginalization");

    std::vector<ReadPatternObservation> permuted = observations;
    std::reverse(permuted.begin(), permuted.end());
    const TopologyEvidenceBundle reordered = build_topology_evidence(permuted, evidence_options);
    check(reordered.state == EvidenceAdapterState::kReady &&
              reordered.input_evidence_sha256 == evidence.input_evidence_sha256 &&
              reordered.active_loci == evidence.active_loci && quality_groups_equal(evidence, reordered),
          "evidence canonicalization and BQ groups must be invariant to input order");
    std::vector<ReadPatternObservation> regrouped = observations;
    regrouped.front().multiplicity = 1;
    ReadPatternObservation split_group = observations.front();
    split_group.multiplicity = 2;
    regrouped.push_back(split_group);
    const TopologyEvidenceBundle regrouped_evidence = build_topology_evidence(regrouped, evidence_options);
    check(regrouped_evidence.input_evidence_sha256 == evidence.input_evidence_sha256 &&
              quality_groups_equal(evidence, regrouped_evidence),
          "splitting one identical read group must not change canonical evidence or likelihood");

    std::vector<ReadPatternObservation> malformed = observations;
    malformed.front().base_qualities.front().reset();
    check(build_topology_evidence(malformed, evidence_options).state == EvidenceAdapterState::kMalformedEvidence,
          "a fixed R/A call without BQ must fail closed");
    malformed = observations;
    malformed.front().base_qualities[2] = std::uint8_t{30};
    check(build_topology_evidence(malformed, evidence_options).state == EvidenceAdapterState::kMalformedEvidence,
          "a marginalized O/X call carrying BQ must fail closed instead of being reinterpreted");
    check(build_topology_evidence({make_read_pattern("OX", {-1, -1}, 4)}, evidence_options).state ==
              EvidenceAdapterState::kNoInformativeRead,
          "an all-O/X component must be classified as no informative read evidence");
    check(build_topology_evidence({make_read_pattern("RR", {30, 30}, 4)}, evidence_options).state ==
              EvidenceAdapterState::kNoStructuralAlternate,
          "a REF-only component is informative but cannot create a structural ALT universe");
    check(build_topology_evidence({make_read_pattern(std::string(13, 'A'), std::vector<int>(13, 30), 2)},
                                  evidence_options)
                  .state == EvidenceAdapterState::kActiveDimensionUnsupported,
          "thirteen active ALT loci must fail closed outside the exact 12-bit boundary");

    const auto topology = solve_topology_exact(evidence.structural_problem);
    check(topology.structural.family_state == ExactFamilyState::kFamilyComplete &&
              topology.structural.minimum_family.size() == 2,
          "frozen AX plus RA structural evidence must produce the exact two-member vertex-set family");

    const VertexSetRankingResult ranking = rank_complete_vertex_sets(evidence, topology.structural);
    check(ranking.state == VertexSetRankingState::kRankingComplete && ranking.candidate_scores.size() == 2 &&
              ranking.evaluated_vertex_set_count == 2 && ranking.best_vertex_set_tie_class.size() == 1,
          "BQ likelihood must score each complete-family vertex set once and select the supported set");
    std::set<std::string> evaluated;
    for (const CandidateVertexSetScore& score : ranking.candidate_scores) {
        check(score.converged && evaluated.insert(score.vertex_set_sha256).second,
              "every candidate likelihood must have one distinct global-gap certificate");
        const double grid_best = brute_force_three_component_simplex(evidence, score.vertices);
        check(score.log_likelihood + 1e-10 >= grid_best && score.log_likelihood - grid_best < 5e-5,
              "BQ ranker optimum must agree with an independent dense small-q simplex oracle");
    }

    auto reversed_structural = topology.structural;
    std::reverse(reversed_structural.minimum_family.begin(), reversed_structural.minimum_family.end());
    const VertexSetRankingResult reordered_ranking = rank_complete_vertex_sets(reordered, reversed_structural);
    check(reordered_ranking.state == VertexSetRankingState::kRankingComplete &&
              reordered_ranking.ranking_evidence_sha256 == ranking.ranking_evidence_sha256 &&
              reordered_ranking.certificate_sha256 == ranking.certificate_sha256 &&
              reordered_ranking.best_vertex_set_tie_class == ranking.best_vertex_set_tie_class,
          "ranking result and certificate must be invariant to observation and family enumeration order");

    const std::vector<ReadPatternObservation> symmetric_observations = {
        make_read_pattern("AX", {30, -1}, 3),
        make_read_pattern("XA", {-1, 30}, 3),
    };
    const TopologyEvidenceBundle symmetric_evidence = build_topology_evidence(symmetric_observations, evidence_options);
    const auto symmetric_topology = solve_topology_exact(symmetric_evidence.structural_problem);
    const VertexSetRankingResult symmetric_ranking =
        rank_complete_vertex_sets(symmetric_evidence, symmetric_topology.structural);
    check(symmetric_ranking.state == VertexSetRankingState::kRankingComplete &&
              symmetric_ranking.candidate_scores.size() == 3 && symmetric_ranking.best_vertex_set_tie_class.size() == 2,
          "bit-symmetric BQ evidence must retain both indistinguishable optimal vertex sets");
    std::set<std::string> symmetric_score_digests;
    for (const CandidateVertexSetScore& score : symmetric_ranking.candidate_scores) {
        symmetric_score_digests.insert(score.vertex_set_sha256);
    }
    const std::set<std::string> symmetric_tie_digests(symmetric_ranking.best_vertex_set_tie_class.begin(),
                                                      symmetric_ranking.best_vertex_set_tie_class.end());
    check(symmetric_tie_digests.size() == 2 && std::all_of(symmetric_tie_digests.begin(), symmetric_tie_digests.end(),
                                                           [&symmetric_score_digests](const std::string& digest) {
                                                               return symmetric_score_digests.count(digest) == 1;
                                                           }),
          "complete tie class must be duplicate-free and candidate-bound");

    VertexSetRankerOptions numerical_options;
    numerical_options.maximum_iterations = 1;
    numerical_options.global_gap_tolerance = 1e-16;
    const VertexSetRankingResult numerical_abstain =
        rank_complete_vertex_sets(evidence, topology.structural, numerical_options);
    check(numerical_abstain.state == VertexSetRankingState::kAbstainNumericalCertificate &&
              !numerical_abstain.best_log_likelihood.has_value() &&
              numerical_abstain.best_vertex_set_tie_class.empty() && numerical_abstain.certificate_sha256.empty(),
          "an insufficient numerical budget must retain diagnostics but publish no rank or tie class");

    longlineage::solver::TopologyRouterOptions cap_options;
    cap_options.bnb.maximum_complete_family_size = 1;
    const auto capped_topology = solve_topology_exact(evidence.structural_problem, cap_options);
    const VertexSetRankingResult capped_ranking = rank_complete_vertex_sets(evidence, capped_topology.structural);
    check(capped_topology.structural.family_state == ExactFamilyState::kFamilyIncompleteCap &&
              capped_ranking.state == VertexSetRankingState::kNotRunFamilyIncompleteCap &&
              capped_ranking.candidate_scores.empty() && !capped_ranking.best_log_likelihood.has_value(),
          "family cap must suppress every candidate score and published rank");

    VertexSetRankerOptions deadline_options;
    deadline_options.family_gate = FamilyRankingGate::kForceIncompleteDeadline;
    const VertexSetRankingResult deadline_ranking =
        rank_complete_vertex_sets(evidence, topology.structural, deadline_options);
    check(deadline_ranking.state == VertexSetRankingState::kNotRunFamilyIncompleteDeadline &&
              deadline_ranking.candidate_scores.empty() && deadline_ranking.best_vertex_set_tie_class.empty(),
          "family deadline must fail closed without a partial abundance rank");

    std::map<std::pair<HypercubeVertex, HypercubeVertex>, double> unique_edge_scores;
    for (const auto& candidate : topology.structural.minimum_family) {
        for (const auto& choices : candidate.parent_mapping.legal_parents) {
            for (HypercubeVertex parent : choices.parents) {
                unique_edge_scores.emplace(std::make_pair(parent, choices.vertex),
                                           1.0 + static_cast<double>(parent) / 10.0);
            }
        }
    }
    ParentEdgeEndpointRequest edge_request;
    edge_request.requested = true;
    edge_request.evidence_sha256 = std::string(64, 'a');
    for (const auto& score : unique_edge_scores) {
        edge_request.local_scores.push_back(ParentEdgeLocalScore{score.first.first, score.first.second, score.second});
    }
    const auto edge_endpoint =
        evaluate_ranked_parent_edges(evidence.active_loci.size(), topology.structural, ranking, edge_request);
    check(edge_endpoint.state == ParentEdgeEndpointState::kComplete &&
              edge_endpoint.candidate_results.size() == ranking.best_vertex_set_tie_class.size(),
          "parent edge endpoint must run only after the best node-set tie class is fixed");
    ParentEdgeEndpointRequest missing_edge_request = edge_request;
    missing_edge_request.local_scores.erase(
        std::remove_if(missing_edge_request.local_scores.begin(), missing_edge_request.local_scores.end(),
                       [](const ParentEdgeLocalScore& score) { return score.child == HypercubeVertex{3}; }),
        missing_edge_request.local_scores.end());
    const auto missing_edge_endpoint =
        evaluate_ranked_parent_edges(evidence.active_loci.size(), topology.structural, ranking, missing_edge_request);
    check(missing_edge_endpoint.state == ParentEdgeEndpointState::kAbstainEvidenceMissing &&
              missing_edge_endpoint.evidence_sha256.empty() && missing_edge_endpoint.candidate_results.empty(),
          "missing one legal top-candidate edge score must abstain without partial edge results");

    longlineage::solver::ExactStructuralResult diamond_structural;
    diamond_structural.objective_state = ExactObjectiveState::kObjectiveCertified;
    diamond_structural.family_state = ExactFamilyState::kFamilyComplete;
    diamond_structural.objective_h = 0;
    diamond_structural.objective_search_exhausted = true;
    diamond_structural.family_enumeration_exhausted = true;
    longlineage::solver::ExactStructuralCandidate diamond_candidate;
    diamond_candidate.vertices = {0, 1, 2, 3};
    diamond_candidate.parent_mapping = summarize_exact_parent_mappings(2, diamond_candidate.vertices);
    diamond_structural.minimum_family.push_back(diamond_candidate);
    VertexSetRankingResult diamond_ranking;
    diamond_ranking.state = VertexSetRankingState::kRankingComplete;
    diamond_ranking.best_vertex_set_tie_class = {exact_vertex_set_sha256(2, diamond_candidate.vertices)};
    ParentEdgeEndpointRequest diamond_request;
    diamond_request.requested = true;
    diamond_request.evidence_sha256 = std::string(64, 'b');
    diamond_request.local_scores = {{0, 1, 1.0}, {0, 2, 1.0}, {1, 3, 2.0}, {2, 3, 2.0}};
    const auto diamond_edges = evaluate_ranked_parent_edges(2, diamond_structural, diamond_ranking, diamond_request);
    check(diamond_edges.state == ParentEdgeEndpointState::kComplete &&
              diamond_edges.candidate_results.front().best_parent_tie_count == "2" &&
              diamond_edges.candidate_results.front().published_parent_mapping.empty(),
          "an equal fixed-node parent edge tie must retain exact tie count and withhold a mapping");

    const TopologyEvidenceBundle unary_evidence =
        build_topology_evidence({make_read_pattern("AA", {30, 30}, 3)}, evidence_options);
    const auto unary_topology = solve_topology_exact(unary_evidence.structural_problem);
    const VertexSetRankingResult unary_ranking = rank_complete_vertex_sets(unary_evidence, unary_topology.structural);
    check(unary_topology.structural.minimum_family.size() == 2 &&
              unary_ranking.state == VertexSetRankingState::kRankingComplete &&
              unary_ranking.best_vertex_set_tie_class.size() == 2,
          "an observed 11 endpoint with no intermediate read must retain both unary hidden-chain orders");
    std::vector<longlineage::solver::MultiMutationEdgeProjection> unary_projections;
    for (const auto& candidate : unary_topology.structural.minimum_family) {
        unary_projections.push_back(make_unresolved_multi_mutation_projection(
            exact_vertex_set_sha256(unary_evidence.active_loci.size(), candidate.vertices), 0, 3,
            unary_evidence.active_loci, std::string(64, 'c')));
    }
    check(std::all_of(unary_projections.begin(), unary_projections.end(),
                      [](const auto& projection) {
                          return projection.mutation_active_bits == std::vector<std::size_t>({0, 1}) &&
                                 projection.order_count == "2";
                      }),
          "no-read unary hidden chains must collapse to a two-mutation edge with unresolved 2! order");
    bool one_mutation_rejected = false;
    try {
        static_cast<void>(
            make_unresolved_multi_mutation_projection(unary_projections.front().candidate_vertex_set_sha256, 0, 1,
                                                      unary_evidence.active_loci, std::string(64, 'c')));
    } catch (const std::invalid_argument&) {
        one_mutation_rejected = true;
    }
    check(one_mutation_rejected,
          "a single mutation is an ordinary edge and must not be mislabeled as unary-chain equivalence");

    const TopologyRecordIdentity identity{0, "synthetic-bq", 0, "adapter-complete"};
    const auto record = serialize_topology_unit_v2(identity, evidence, topology, ranking, edge_endpoint);
    check(record.valid && record.record_sha256.size() == 64 &&
              record.canonical_json.find("\"ranking_state\":\"RANKING_COMPLETE\"") != std::string::npos &&
              record.canonical_json.find("\"ranking_endpoint_id\":\"BQ_AWARE_READ_PATTERN_MIXTURE_V1\"") !=
                  std::string::npos &&
              record.canonical_json.find("\"child_state\":\"01\"") != std::string::npos,
          "complete topology_unit v2 must preserve active-bit order and separate BQ rank from parent edges");
    const auto record_again =
        serialize_topology_unit_v2(identity, reordered, topology, reordered_ranking, edge_endpoint);
    check(record_again.valid && record_again.canonical_json == record.canonical_json &&
              record_again.record_sha256 == record.record_sha256,
          "topology_unit v2 canonical JSON must be deterministic across input order");
    write_text_file("/tmp/longlineage_topology_adapter_test.json", record.canonical_json);

    const auto unary_record = serialize_topology_unit_v2(
        TopologyRecordIdentity{0, "synthetic-unary", 1, "adapter-unary-projection"}, unary_evidence, unary_topology,
        unary_ranking, longlineage::solver::ParentEdgeEndpointResult{}, unary_projections);
    check(
        unary_record.valid &&
            unary_record.canonical_json.find("\"order_state\":\"UNRESOLVED_NO_READ_EVIDENCE\"") != std::string::npos &&
            unary_record.canonical_json.find("\"projection_only\":true") != std::string::npos,
        "unary topology record must expose only candidate-bound multi-mutation edge equivalences");
    std::reverse(unary_projections.begin(), unary_projections.end());
    const auto unary_record_reordered = serialize_topology_unit_v2(
        TopologyRecordIdentity{0, "synthetic-unary", 1, "adapter-unary-projection"}, unary_evidence, unary_topology,
        unary_ranking, longlineage::solver::ParentEdgeEndpointResult{}, unary_projections);
    check(unary_record_reordered.valid && unary_record_reordered.canonical_json == unary_record.canonical_json,
          "unary projection serialization must be invariant to caller order");
    write_text_file("/tmp/longlineage_topology_adapter_unary_test.json", unary_record.canonical_json);

    const auto numerical_record =
        serialize_topology_unit_v2(TopologyRecordIdentity{0, "synthetic-bq", 2, "adapter-numerical-abstain"}, evidence,
                                   topology, numerical_abstain, longlineage::solver::ParentEdgeEndpointResult{});
    check(numerical_record.valid &&
              numerical_record.canonical_json.find("\"ranking_state\":\"RANKING_ABSTAIN_NUMERICAL_CERTIFICATE\"") !=
                  std::string::npos &&
              numerical_record.canonical_json.find("\"published_rank\":null") != std::string::npos,
          "numerical certificate failure must preserve the complete family but publish no abundance rank");
    write_text_file("/tmp/longlineage_topology_adapter_numerical_test.json", numerical_record.canonical_json);

    const auto capped_record =
        serialize_topology_unit_v2(TopologyRecordIdentity{0, "synthetic-bq", 3, "adapter-family-cap"}, evidence,
                                   capped_topology, capped_ranking, longlineage::solver::ParentEdgeEndpointResult{});
    check(capped_record.valid &&
              capped_record.canonical_json.find("\"family_state\":\"FAMILY_INCOMPLETE_CAP\"") != std::string::npos &&
              capped_record.canonical_json.find("\"published_rank\":null") != std::string::npos &&
              capped_record.canonical_json.find("\"candidate_count\":0") != std::string::npos,
          "family-cap topology record must publish no candidates or rank");
    write_text_file("/tmp/longlineage_topology_adapter_incomplete_test.json", capped_record.canonical_json);

    const auto deadline_record =
        serialize_topology_unit_v2(TopologyRecordIdentity{0, "synthetic-bq", 4, "adapter-family-deadline"}, evidence,
                                   topology, deadline_ranking, longlineage::solver::ParentEdgeEndpointResult{});
    check(deadline_record.valid &&
              deadline_record.canonical_json.find("\"family_state\":\"FAMILY_INCOMPLETE_DEADLINE\"") !=
                  std::string::npos &&
              deadline_record.canonical_json.find("\"published_rank\":null") != std::string::npos,
          "family-deadline topology record must suppress the otherwise available structural family and rank");
    write_text_file("/tmp/longlineage_topology_adapter_deadline_test.json", deadline_record.canonical_json);
}

}  // namespace

int main() {
    try {
        test_byte_bounded_queue();
        test_ordered_thread_pool();
        test_bgzf_writer_and_run_state();
        test_small_q_oracle();
        test_exact_parent_mapping();
        test_exact_obligation_bnb_and_dp();
        test_exact_topology_router_differentials();
        test_topology_evidence_ranking_and_record_adapter();
        std::cout << "PASS test_runtime_solver: queue,pool,bgzf,state,small-q oracle,"
                     "exact B&B,subset DP,parent mapping,router differential,"
                     "BQ ranking,topology v2 adapter\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL test_runtime_solver: " << error.what() << '\n';
        return 1;
    }
}
