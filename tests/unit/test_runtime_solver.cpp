// SPDX-License-Identifier: GPL-3.0-only

#include <htslib/bgzf.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "longlineage/artifact/bgzf_tsv_writer.hpp"
#include "longlineage/artifact/run_state.hpp"
#include "longlineage/runtime/byte_bounded_queue.hpp"
#include "longlineage/runtime/ordered_thread_pool.hpp"
#include "longlineage/solver/small_q_oracle.hpp"

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
using longlineage::solver::solve_small_q_exact;
using longlineage::solver::TopologyFamilyStatus;
using longlineage::solver::TopologyObjectiveStatus;
using longlineage::solver::TopologyProblem;
using longlineage::solver::TopologyReason;

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

}  // namespace

int main() {
    try {
        test_byte_bounded_queue();
        test_ordered_thread_pool();
        test_bgzf_writer_and_run_state();
        test_small_q_oracle();
        std::cout << "PASS test_runtime_solver: queue,pool,bgzf,state,small-q oracle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL test_runtime_solver: " << error.what() << '\n';
        return 1;
    }
}
