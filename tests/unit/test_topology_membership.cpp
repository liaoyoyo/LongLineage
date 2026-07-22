// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <iostream>
#include <locale>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "longlineage/pipeline/topology_membership.hpp"

namespace {

using longlineage::AlleleCall;
using longlineage::ContigId;
using longlineage::Interval0;
using longlineage::LatestTags;
using longlineage::Position1;
using longlineage::ProjectedAlleleCall;
using longlineage::ReadProjectionIdentity;
using longlineage::Strand;
using longlineage::VariantSite;
using longlineage::pipeline::BlockScienceEvidence;
using longlineage::pipeline::JoinedReadEvidence;
using longlineage::pipeline::JointSignatureState;
using longlineage::pipeline::TopologyLocusIdentity;
using longlineage::pipeline::TopologyMembershipPlan;
using longlineage::pipeline::TopologySiteCandidate;
using longlineage::pipeline::TopologyUnitMembership;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ContigId contig(std::string value) {
    auto parsed = ContigId::from_string(std::move(value));
    check(parsed.ok(), parsed.detail);
    return std::move(*parsed.value);
}

Position1 position(std::uint64_t value) {
    auto parsed = Position1::from_value(value);
    check(parsed.ok(), parsed.detail);
    return *parsed.value;
}

Interval0 interval(std::uint64_t begin, std::uint64_t end) {
    auto parsed = Interval0::from_bounds(begin, end);
    check(parsed.ok(), parsed.detail);
    return *parsed.value;
}

TopologyLocusIdentity locus(std::uint32_t dataset_order, std::string dataset_id, std::string chromosome,
                            std::uint64_t site_order, std::uint64_t position1, char reference = 'A',
                            char alternate = 'C') {
    return TopologyLocusIdentity{dataset_order, std::move(dataset_id), contig(std::move(chromosome)),
                                 site_order,    position(position1),   reference,
                                 alternate};
}

VariantSite marker(const TopologyLocusIdentity& identity) {
    return VariantSite{identity.dataset_order, identity.dataset_id, identity.site_order,
                       identity.site_order,    identity.contig,     1000000,
                       identity.position,      identity.reference,  identity.alternate};
}

ProjectedAlleleCall call(const TopologyLocusIdentity& identity, AlleleCall allele,
                         std::optional<std::uint8_t> quality) {
    const bool observed = allele != AlleleCall::kUnobservable;
    return ProjectedAlleleCall{identity.site_order, identity.position, allele,
                               observed ? std::optional<std::uint64_t>{0} : std::nullopt, quality};
}

JoinedReadEvidence read(std::string read_id, const ContigId& chromosome, Interval0 span,
                        std::vector<ProjectedAlleleCall> calls, std::uint32_t raw_occurrences = 1) {
    const std::string qname = "synthetic-" + read_id.substr(0, 8);
    return JoinedReadEvidence{
        std::move(read_id),
        ReadProjectionIdentity{qname, chromosome, span, 60, Strand::kForward},
        0,
        std::string(16, 'b'),
        std::string(64, 'c'),
        span.size(),
        std::nullopt,
        LatestTags{"1-1", 100},
        raw_occurrences,
        raw_occurrences,
        std::move(calls),
        {},
    };
}

std::string complete_readset_digest(std::vector<std::string> ids = {}) {
    auto digest = longlineage::pipeline::topology_joint_complete_readset_sha256(ids);
    check(digest.ok(), digest.detail);
    return std::move(*digest.value);
}

TopologySiteCandidate candidate(TopologyLocusIdentity focal, std::vector<TopologyLocusIdentity> partners,
                                bool m2_eligible = true, JointSignatureState signature = JointSignatureState::kPass,
                                std::string complete_digest = {}) {
    TopologySiteCandidate output(std::move(focal));
    output.m2_eligible = m2_eligible;
    output.joint_signature_state = signature;
    output.selected_partners = std::move(partners);
    output.joint_complete_readset_sha256 =
        complete_digest.empty() ? complete_readset_digest() : std::move(complete_digest);
    return output;
}

struct PositiveFixture {
    TopologyLocusIdentity partner_left;
    TopologyLocusIdentity focal;
    TopologyLocusIdentity partner_right;
    TopologySiteCandidate candidate;
    BlockScienceEvidence block;
};

PositiveFixture positive_fixture() {
    TopologyLocusIdentity left = locus(0, "HCC1395", "chr1", 90, 900, 'C', 'T');
    TopologyLocusIdentity focal = locus(0, "HCC1395", "chr1", 100, 1000, 'A', 'G');
    TopologyLocusIdentity right = locus(0, "HCC1395", "chr1", 110, 1100, 'G', 'A');
    const std::string a(64, 'a');
    const std::string b(64, 'b');
    const std::string c(64, 'c');
    const std::string d(64, 'd');

    TopologySiteCandidate site =
        candidate(focal, {right, left}, true, JointSignatureState::kPass, complete_readset_digest({a}));

    BlockScienceEvidence block;
    block.block_sequence = 17;
    block.markers = {marker(left), marker(focal), marker(right)};
    block.reads = {
        read(c, focal.contig, interval(850, 1200),
             {call(left, AlleleCall::kOther, 40), call(focal, AlleleCall::kReference, 20),
              call(right, AlleleCall::kAlternate, 21)},
             2),
        read(a, focal.contig, interval(850, 1200),
             {call(left, AlleleCall::kReference, 30), call(focal, AlleleCall::kAlternate, 31),
              call(right, AlleleCall::kReference, 32)},
             3),
        read(d, focal.contig, interval(1500, 1600),
             {call(left, AlleleCall::kUnobservable, std::nullopt), call(focal, AlleleCall::kUnobservable, std::nullopt),
              call(right, AlleleCall::kUnobservable, std::nullopt)}),
        read(b, focal.contig, interval(950, 1050),
             {call(left, AlleleCall::kUnobservable, std::nullopt), call(focal, AlleleCall::kAlternate, 25),
              call(right, AlleleCall::kUnobservable, std::nullopt)}),
    };
    return PositiveFixture{std::move(left), std::move(focal), std::move(right), std::move(site), std::move(block)};
}

TopologyUnitMembership positive_membership(const TopologySiteCandidate& site) {
    auto plan = longlineage::pipeline::build_topology_membership_plan({site});
    check(plan.ok(), plan.detail);
    check(plan.value->units.size() == 1, "positive candidate did not instantiate one unit");
    return plan.value->units.front();
}

bool observations_equal(const longlineage::pipeline::TopologyReadPatternAdapterResult& lhs,
                        const longlineage::pipeline::TopologyReadPatternAdapterResult& rhs) {
    if (lhs.canonical_read_ids != rhs.canonical_read_ids || lhs.observations.size() != rhs.observations.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.observations.size(); ++index) {
        if (lhs.observations[index].pattern_raox != rhs.observations[index].pattern_raox ||
            lhs.observations[index].base_qualities != rhs.observations[index].base_qualities ||
            lhs.observations[index].multiplicity != rhs.observations[index].multiplicity) {
            return false;
        }
    }
    return true;
}

void test_positive_membership_and_adapter() {
    PositiveFixture input = positive_fixture();
    auto plan = longlineage::pipeline::build_topology_membership_plan({input.candidate});
    check(plan.ok(), plan.detail);
    check(plan.value->schema_version == "1.0.0" && plan.value->plan_sha256.size() == 64,
          "versioned plan identity is absent");
    check(plan.value->funnel.candidates_total == 1 && plan.value->funnel.instantiated_units == 1 &&
              plan.value->units.front().unit_order == 0,
          "positive membership funnel or dense unit_order drift");
    const TopologyUnitMembership& membership = plan.value->units.front();
    check(membership.loci == std::vector<TopologyLocusIdentity>{input.partner_left, input.focal, input.partner_right},
          "unit loci are not canonical by site_order");
    check(membership.selected_partners == std::vector<TopologyLocusIdentity>{input.partner_right, input.partner_left},
          "P4 selected-partner ranking order was not preserved");
    check(membership.unit_id == "topology-unit:" + membership.membership_sha256,
          "unit ID is not bound to the membership digest");

    auto adapted = longlineage::pipeline::build_topology_read_patterns(input.block, membership);
    check(adapted.ok(), adapted.detail);
    const auto& value = *adapted.value;
    check(value.canonical_read_ids ==
              std::vector<std::string>{std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c')},
          "touching reads are not in canonical opaque-read-ID order");
    check(value.observations.size() == 3 && value.observations[0].pattern_raox == "RAR" &&
              value.observations[1].pattern_raox == "XAX" && value.observations[2].pattern_raox == "ORA",
          "R/A/O/X patterns or partial subcubes drifted");
    check(!value.observations[1].base_qualities[0].has_value() &&
              !value.observations[1].base_qualities[2].has_value() &&
              !value.observations[2].base_qualities[0].has_value(),
          "O/X topology calls did not emit null base qualities");
    check(std::all_of(value.observations.begin(), value.observations.end(),
                      [](const auto& observation) { return observation.multiplicity == 1; }),
          "RG occurrences weighted topology observation multiplicity");
    check(value.counters.unique_read_projections_seen == 4 && value.counters.reads_touching_at_least_one_locus == 3 &&
              value.counters.reads_not_touching_any_locus == 1 && value.counters.complete_ra_reads == 1 &&
              value.counters.partial_raox_reads == 2 && value.counters.rg_only_duplicate_occurrences_ignored == 3,
          "read-pattern adapter census drift");
    check(value.counters.raw_code_counts == std::array<std::uint64_t, 4>{3, 3, 1, 2},
          "R/A/O/X conservation counters drift");
    const auto solver_evidence = longlineage::solver::build_topology_evidence(value.observations);
    check(solver_evidence.state == longlineage::solver::EvidenceAdapterState::kReady &&
              solver_evidence.input_observations == 3 &&
              std::any_of(solver_evidence.structural_patterns.begin(), solver_evidence.structural_patterns.end(),
                          [](const auto& pattern) { return pattern.partial; }),
          "adapter output did not enter the P5 solver with a retained partial subcube");
    check(value.complete_ra_projection_readset_sha256 == input.candidate.joint_complete_readset_sha256 &&
              value.input_sha256.size() == 64 && value.evidence_sha256.size() == 64,
          "adapter input/evidence/projection-readset digest is absent");
    check(plan.value->plan_sha256 == "c50acf9d57888de3dce03c5c25b9cdd3d7263dddc775b99095f5aab3702a2a5f" &&
              membership.membership_sha256 == "a9d11aa0de706e362dcbc7ec2a91ed19c547f74548c80505eaaa7d582f8192f3" &&
              value.complete_ra_projection_readset_sha256 ==
                  "a91c56c2732669a505ba2d0c700f092fed1da27c8bd8a25717fd63cc10c9ce02" &&
              value.input_sha256 == "6263e0f0a5b910be06a2d9fa76b49eea304703407c69394d47ebe3c22c956ae7" &&
              value.evidence_sha256 == "ee619a7baba2a71cf6d9d4b819aacbcab13f710d90ae90cca78bc37b6c2e5a54",
          "frozen topology membership/read-pattern vector drift");
}

TopologySiteCandidate ordered_candidate(std::uint32_t dataset_order, const std::string& dataset_id,
                                        const std::string& chromosome, std::uint64_t focal_order,
                                        std::uint64_t focal_position) {
    auto focal = locus(dataset_order, dataset_id, chromosome, focal_order, focal_position, 'A', 'C');
    auto partner = locus(dataset_order, dataset_id, chromosome, focal_order + 1, focal_position + 100, 'G', 'T');
    return candidate(std::move(focal), {std::move(partner)});
}

void test_canonical_sort_locale_and_input_invariance() {
    std::vector<TopologySiteCandidate> candidates{
        ordered_candidate(1, "dataset-b", "chr1", 300, 3000),
        ordered_candidate(0, "dataset-a", "chr10", 200, 2000),
        ordered_candidate(0, "dataset-a", "chr2", 100, 1000),
    };
    auto original = longlineage::pipeline::build_topology_membership_plan(candidates);
    check(original.ok(), original.detail);
    check(original.value->units.size() == 3 && original.value->units[0].focal.contig.value() == "chr2" &&
              original.value->units[1].focal.contig.value() == "chr10" &&
              original.value->units[2].focal.dataset_order == 1,
          "dataset/canonical-autosome ordering drift");
    for (std::size_t index = 0; index < original.value->units.size(); ++index) {
        check(original.value->units[index].unit_order == index, "unit_order is not dense after canonical sort");
    }

    std::reverse(candidates.begin(), candidates.end());
    const std::locale prior = std::locale();
    std::locale::global(std::locale::classic());
    auto reversed = longlineage::pipeline::build_topology_membership_plan(candidates);
    check(reversed.ok(), reversed.detail);
    check(reversed.value->plan_sha256 == original.value->plan_sha256, "candidate input order changed the plan digest");
    try {
        std::locale::global(std::locale("C.UTF-8"));
    } catch (const std::runtime_error&) {
        std::locale::global(std::locale::classic());
    }
    auto locale_replay = longlineage::pipeline::build_topology_membership_plan(candidates);
    std::locale::global(prior);
    check(locale_replay.ok() && locale_replay.value->plan_sha256 == original.value->plan_sha256,
          "process locale changed canonical topology ordering/digest");
}

void test_not_instantiated_funnel_is_not_solver_abstain() {
    std::vector<TopologySiteCandidate> candidates{
        ordered_candidate(0, "dataset-a", "chr1", 10, 1000),
        ordered_candidate(0, "dataset-a", "chr1", 20, 2000),
        ordered_candidate(0, "dataset-a", "chr1", 30, 3000),
        ordered_candidate(0, "dataset-a", "chr1", 40, 4000),
    };
    candidates[1].m2_eligible = false;
    candidates[2].joint_signature_state = JointSignatureState::kFail;
    candidates[2].selected_partners.clear();
    candidates[2].joint_complete_readset_sha256.clear();
    candidates[3].joint_signature_state = JointSignatureState::kNotEvaluated;
    candidates[3].selected_partners.clear();
    candidates[3].joint_complete_readset_sha256.clear();
    auto plan = longlineage::pipeline::build_topology_membership_plan(candidates);
    check(plan.ok(), plan.detail);
    check(plan.value->units.size() == 1 && plan.value->funnel.candidates_total == 4 &&
              plan.value->funnel.instantiated_units == 1 && plan.value->funnel.not_instantiated_m2_ineligible == 1 &&
              plan.value->funnel.not_instantiated_joint_signature_fail == 1 &&
              plan.value->funnel.not_instantiated_joint_signature_not_evaluated == 1,
          "not-instantiated candidate funnel failed conservation");

    std::vector<TopologySiteCandidate> all_rejected{
        ordered_candidate(0, "dataset-a", "chr2", 50, 5000),
        ordered_candidate(0, "dataset-a", "chr2", 60, 6000),
    };
    all_rejected[0].m2_eligible = false;
    all_rejected[0].joint_signature_state = JointSignatureState::kNotEvaluated;
    all_rejected[0].selected_partners.clear();
    all_rejected[0].joint_complete_readset_sha256.clear();
    all_rejected[1].joint_signature_state = JointSignatureState::kFail;
    all_rejected[1].selected_partners.clear();
    all_rejected[1].joint_complete_readset_sha256.clear();
    auto empty = longlineage::pipeline::build_topology_membership_plan(all_rejected);
    check(empty.ok() && empty.empty() && empty.value->units.empty() && empty.value->funnel.candidates_total == 2 &&
              empty.value->funnel.instantiated_units == 0 && empty.value->funnel.not_instantiated_m2_ineligible == 1 &&
              empty.value->funnel.not_instantiated_joint_signature_fail == 1 && empty.value->plan_sha256.size() == 64,
          "all-rejected candidates did not produce a valid empty plan/funnel");
}

void test_candidate_contract_negatives() {
    const auto focal = locus(0, "dataset-a", "chr1", 10, 1000, 'A', 'C');

    auto inclusive_boundaries = candidate(
        locus(0, "dataset-a", "chr1", 20, 10000, 'A', 'C'),
        {locus(0, "dataset-a", "chr1", 18, 5000, 'G', 'T'), locus(0, "dataset-a", "chr1", 19, 5020, 'C', 'A')});
    check(longlineage::pipeline::build_topology_membership_plan({inclusive_boundaries}).ok(),
          "inclusive 5000 bp window or exact 20 bp spacing was rejected");

    auto outside = candidate(focal, {locus(0, "dataset-a", "chr1", 11, 6001, 'G', 'T')});
    check(!longlineage::pipeline::build_topology_membership_plan({outside}).ok(),
          "partner at focal distance 5001 did not fail closed");

    auto spacing = candidate(
        focal, {locus(0, "dataset-a", "chr1", 11, 1100, 'G', 'T'), locus(0, "dataset-a", "chr1", 12, 1119, 'C', 'A')});
    check(!longlineage::pipeline::build_topology_membership_plan({spacing}).ok(),
          "partner pairwise spacing 19 did not fail closed");

    auto cross_contig = candidate(focal, {locus(0, "dataset-a", "chr2", 11, 1100, 'G', 'T')});
    check(!longlineage::pipeline::build_topology_membership_plan({cross_contig}).ok(),
          "cross-contig partner did not fail closed");

    auto cross_dataset = candidate(focal, {locus(1, "dataset-b", "chr1", 11, 1100, 'G', 'T')});
    check(!longlineage::pipeline::build_topology_membership_plan({cross_dataset}).ok(),
          "cross-dataset partner did not fail closed");

    const auto repeated = locus(0, "dataset-a", "chr1", 11, 1100, 'G', 'T');
    auto duplicate_partner = candidate(focal, {repeated, repeated});
    check(!longlineage::pipeline::build_topology_membership_plan({duplicate_partner}).ok(),
          "duplicate selected partner did not fail closed");

    auto duplicate_focal_a = candidate(focal, {repeated});
    auto duplicate_focal_b = duplicate_focal_a;
    check(!longlineage::pipeline::build_topology_membership_plan({duplicate_focal_a, duplicate_focal_b}).ok(),
          "duplicate focal candidate did not fail closed");

    auto conflict_a = ordered_candidate(0, "dataset-a", "chr1", 100, 10000);
    auto conflict_b = ordered_candidate(0, "dataset-a", "chr1", 200, 12000);
    conflict_b.selected_partners.front() =
        locus(0, "dataset-a", "chr1", conflict_a.selected_partners.front().site_order,
              conflict_a.selected_partners.front().position.value(), 'A', 'G');
    check(!longlineage::pipeline::build_topology_membership_plan({conflict_a, conflict_b}).ok(),
          "conflicting identity for a shared site_order/position did not fail closed");

    auto noncanonical = candidate(locus(0, "dataset-a", "1", 10, 1000), {locus(0, "dataset-a", "1", 11, 1100)});
    check(!longlineage::pipeline::build_topology_membership_plan({noncanonical}).ok(),
          "noncanonical contig did not fail closed");

    TopologySiteCandidate no_partner(focal);
    no_partner.m2_eligible = true;
    no_partner.joint_signature_state = JointSignatureState::kPass;
    no_partner.joint_complete_readset_sha256 = complete_readset_digest();
    check(!longlineage::pipeline::build_topology_membership_plan({no_partner}).ok(),
          "zero selected partners did not fail closed");

    auto too_many = candidate(locus(0, "dataset-a", "chr1", 50, 5000),
                              {locus(0, "dataset-a", "chr1", 51, 5100), locus(0, "dataset-a", "chr1", 52, 5200),
                               locus(0, "dataset-a", "chr1", 53, 5300), locus(0, "dataset-a", "chr1", 54, 5400)});
    check(!longlineage::pipeline::build_topology_membership_plan({too_many}).ok(),
          "four selected partners did not fail closed");

    auto wrong_schema = ordered_candidate(0, "dataset-a", "chr1", 60, 6000);
    wrong_schema.schema_version = "2.0.0";
    check(!longlineage::pipeline::build_topology_membership_plan({wrong_schema}).ok(),
          "unsupported candidate schema version did not fail closed");

    auto pass_missing_sha = ordered_candidate(0, "dataset-a", "chr1", 70, 7000);
    pass_missing_sha.joint_complete_readset_sha256.clear();
    check(!longlineage::pipeline::build_topology_membership_plan({pass_missing_sha}).ok(),
          "joint PASS without complete-readset SHA did not fail closed");

    auto fail_with_selection = ordered_candidate(0, "dataset-a", "chr1", 80, 8000);
    fail_with_selection.joint_signature_state = JointSignatureState::kFail;
    check(!longlineage::pipeline::build_topology_membership_plan({fail_with_selection}).ok(),
          "joint FAIL carrying partner/readset evidence did not fail closed");

    auto not_evaluated_with_sha = ordered_candidate(0, "dataset-a", "chr1", 90, 9000);
    not_evaluated_with_sha.joint_signature_state = JointSignatureState::kNotEvaluated;
    not_evaluated_with_sha.selected_partners.clear();
    check(!longlineage::pipeline::build_topology_membership_plan({not_evaluated_with_sha}).ok(),
          "joint NOT_EVALUATED carrying a readset SHA did not fail closed");
}

void test_adapter_fail_closed_boundaries() {
    PositiveFixture input = positive_fixture();
    TopologyUnitMembership membership = positive_membership(input.candidate);

    BlockScienceEvidence missing_bq = input.block;
    missing_bq.reads[1].allele_calls[0].base_quality = std::nullopt;
    check(!longlineage::pipeline::build_topology_read_patterns(missing_bq, membership).ok(),
          "fixed R/A call without BQ did not fail closed");

    BlockScienceEvidence low_bq = input.block;
    low_bq.reads[1].allele_calls[0].base_quality = 19;
    check(!longlineage::pipeline::build_topology_read_patterns(low_bq, membership).ok(),
          "fixed R/A call below BQ20 did not fail closed");

    BlockScienceEvidence missing_quality_sentinel = input.block;
    missing_quality_sentinel.reads[1].allele_calls[0].base_quality = 255;
    check(!longlineage::pipeline::build_topology_read_patterns(missing_quality_sentinel, membership).ok(),
          "fixed R/A call with BQ255 did not fail closed");

    BlockScienceEvidence missing_call = input.block;
    missing_call.reads[1].allele_calls.pop_back();
    check(!longlineage::pipeline::build_topology_read_patterns(missing_call, membership).ok(),
          "missing projected unit-locus call did not fail closed");

    BlockScienceEvidence missing_marker = input.block;
    missing_marker.markers.pop_back();
    check(!longlineage::pipeline::build_topology_read_patterns(missing_marker, membership).ok(),
          "unit locus absent from block marker universe did not fail closed");

    BlockScienceEvidence x_with_bq = input.block;
    x_with_bq.reads[3].allele_calls[0].base_quality = 30;
    check(!longlineage::pipeline::build_topology_read_patterns(x_with_bq, membership).ok(),
          "X call carrying BQ did not fail closed");

    BlockScienceEvidence duplicate_projection = input.block;
    JoinedReadEvidence duplicate = duplicate_projection.reads.front();
    duplicate.read_id = std::string(64, 'e');
    duplicate_projection.reads.push_back(std::move(duplicate));
    check(!longlineage::pipeline::build_topology_read_patterns(duplicate_projection, membership).ok(),
          "duplicate unique-read projection did not fail closed");

    TopologySiteCandidate distinct_p4_core_candidate = input.candidate;
    distinct_p4_core_candidate.joint_complete_readset_sha256 = complete_readset_digest({std::string(64, 'e')});
    TopologyUnitMembership distinct_p4_core_membership = positive_membership(distinct_p4_core_candidate);
    auto domain_separated =
        longlineage::pipeline::build_topology_read_patterns(input.block, distinct_p4_core_membership);
    check(domain_separated.ok() && domain_separated.value->complete_ra_projection_readset_sha256 !=
                                       distinct_p4_core_membership.joint_complete_readset_sha256,
          "P5 all-projection diagnostic was falsely equated to the P4 M1-core readset");
}

void test_read_and_worker_completion_order_invariance() {
    PositiveFixture input = positive_fixture();
    auto plan = longlineage::pipeline::build_topology_membership_plan({input.candidate});
    check(plan.ok(), plan.detail);
    auto first = longlineage::pipeline::build_topology_read_patterns(input.block, plan.value->units.front());
    check(first.ok(), first.detail);

    std::reverse(input.block.reads.begin(), input.block.reads.end());
    std::reverse(input.block.markers.begin(), input.block.markers.end());
    for (std::size_t index = 0; index < input.block.reads.size(); ++index) {
        input.block.reads[index].latest_tags.hp = "ignored-hp-" + std::to_string(index);
        input.block.reads[index].latest_tags.ps = static_cast<std::uint64_t>(900 + index);
    }
    auto replay = longlineage::pipeline::build_topology_read_patterns(input.block, plan.value->units.front());
    check(replay.ok(), replay.detail);
    check(first.value->input_sha256 == replay.value->input_sha256 &&
              first.value->evidence_sha256 == replay.value->evidence_sha256 &&
              observations_equal(*first.value, *replay.value),
          "worker/input completion order changed topology evidence");

    auto readset_a = complete_readset_digest({std::string(64, 'a'), std::string(64, 'b')});
    auto readset_b = complete_readset_digest({std::string(64, 'b'), std::string(64, 'a')});
    check(readset_a == readset_b, "complete-readset digest depends on input order");
    auto duplicate =
        longlineage::pipeline::topology_joint_complete_readset_sha256({std::string(64, 'a'), std::string(64, 'a')});
    check(!duplicate.ok(), "duplicate complete-readset identity did not fail closed");
}

void test_p4_p5_complete_readset_canonicalization_agreement() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::vector<std::string> ids;
    for (std::size_t index = 0; index < 24; ++index) {
        std::string read_id(64, '0');
        read_id[62] = kHex[index / 16];
        read_id[63] = kHex[index % 16];
        ids.push_back(std::move(read_id));
    }
    const std::string digest = complete_readset_digest(ids);
    check(digest == "f9a34ec03d93a5c1dcce24160fe5eb9d121090f43e286505f8ad72af170c2672",
          "P4/P5 complete-readset canonical byte contract diverged");
}

}  // namespace

int main() {
    try {
        test_positive_membership_and_adapter();
        test_canonical_sort_locale_and_input_invariance();
        test_not_instantiated_funnel_is_not_solver_abstain();
        test_candidate_contract_negatives();
        test_adapter_fail_closed_boundaries();
        test_read_and_worker_completion_order_invariance();
        test_p4_p5_complete_readset_canonicalization_agreement();

        PositiveFixture input = positive_fixture();
        auto plan = longlineage::pipeline::build_topology_membership_plan({input.candidate});
        check(plan.ok(), plan.detail);
        auto adapted = longlineage::pipeline::build_topology_read_patterns(input.block, plan.value->units.front());
        check(adapted.ok(), adapted.detail);
        std::cout << "PASS test_topology_membership"
                  << " plan_sha256=" << plan.value->plan_sha256
                  << " membership_sha256=" << plan.value->units.front().membership_sha256
                  << " projection_readset_sha256=" << adapted.value->complete_ra_projection_readset_sha256
                  << " input_sha256=" << adapted.value->input_sha256
                  << " evidence_sha256=" << adapted.value->evidence_sha256
                  << " candidates=" << plan.value->funnel.candidates_total
                  << " units=" << plan.value->funnel.instantiated_units
                  << " touching_reads=" << adapted.value->counters.reads_touching_at_least_one_locus
                  << " multiplicity_sum=" << adapted.value->observations.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL test_topology_membership: " << error.what() << '\n';
        return 1;
    }
}
