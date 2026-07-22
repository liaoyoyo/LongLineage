// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/common/types.hpp"
#include "longlineage/pipeline/block_science.hpp"
#include "longlineage/solver/evidence_builder.hpp"

namespace longlineage::pipeline {

inline constexpr std::string_view kTopologySiteCandidateSchema = "longlineage.topology_site_candidate";
inline constexpr std::string_view kTopologyUnitMembershipSchema = "longlineage.topology_unit_membership";
inline constexpr std::string_view kTopologyMembershipPlanSchema = "longlineage.topology_membership_plan";
inline constexpr std::string_view kTopologyMembershipSchemaVersion = "1.0.0";

enum class JointSignatureState {
    kPass,
    kFail,
    kNotEvaluated,
};

[[nodiscard]] std::string_view to_string(JointSignatureState state) noexcept;

struct TopologyLocusIdentity {
    TopologyLocusIdentity(std::uint32_t dataset_order_value, std::string dataset_id_value, ContigId contig_value,
                          std::uint64_t site_order_value, Position1 position_value, char reference_value,
                          char alternate_value)
        : dataset_order(dataset_order_value),
          dataset_id(std::move(dataset_id_value)),
          contig(std::move(contig_value)),
          site_order(site_order_value),
          position(position_value),
          reference(reference_value),
          alternate(alternate_value) {}

    std::uint32_t dataset_order;
    std::string dataset_id;
    ContigId contig;
    std::uint64_t site_order;
    Position1 position;
    char reference;
    char alternate;

    friend bool operator==(const TopologyLocusIdentity& lhs, const TopologyLocusIdentity& rhs) noexcept {
        return lhs.dataset_order == rhs.dataset_order && lhs.dataset_id == rhs.dataset_id && lhs.contig == rhs.contig &&
               lhs.site_order == rhs.site_order && lhs.position == rhs.position && lhs.reference == rhs.reference &&
               lhs.alternate == rhs.alternate;
    }
    friend bool operator!=(const TopologyLocusIdentity& lhs, const TopologyLocusIdentity& rhs) noexcept {
        return !(lhs == rhs);
    }
};

struct TopologySiteCandidate {
    explicit TopologySiteCandidate(TopologyLocusIdentity focal_value) : focal(std::move(focal_value)) {}

    std::string schema_name{std::string(kTopologySiteCandidateSchema)};
    std::string schema_version{std::string(kTopologyMembershipSchemaVersion)};
    TopologyLocusIdentity focal;
    bool m2_eligible = false;
    JointSignatureState joint_signature_state{JointSignatureState::kNotEvaluated};
    // Present as one to three identities only for PASS, preserving the
    // deterministic P4 selection/ranking order.
    std::vector<TopologyLocusIdentity> selected_partners;
    // Lowercase SHA-256 only for PASS; empty means absent otherwise.
    std::string joint_complete_readset_sha256;
};

struct TopologyUnitMembership {
    TopologyUnitMembership(std::uint64_t unit_order_value, TopologyLocusIdentity focal_value)
        : unit_order(unit_order_value), focal(std::move(focal_value)) {}

    std::string schema_name{std::string(kTopologyUnitMembershipSchema)};
    std::string schema_version{std::string(kTopologyMembershipSchemaVersion)};
    std::uint64_t unit_order;
    std::string unit_id;
    TopologyLocusIdentity focal;
    bool m2_eligible = true;
    JointSignatureState joint_signature_state{JointSignatureState::kPass};
    // P4 selection order; distinct from the site_order-canonical loci below.
    std::vector<TopologyLocusIdentity> selected_partners;
    // Canonical site_order, with the focal included exactly once.
    std::vector<TopologyLocusIdentity> loci;
    std::string joint_complete_readset_sha256;
    std::string membership_sha256;
};

struct TopologyMembershipFunnel {
    std::uint64_t candidates_total = 0;
    std::uint64_t instantiated_units = 0;
    std::uint64_t not_instantiated_m2_ineligible = 0;
    std::uint64_t not_instantiated_joint_signature_fail = 0;
    std::uint64_t not_instantiated_joint_signature_not_evaluated = 0;
};

struct TopologyMembershipPlan {
    std::string schema_name{std::string(kTopologyMembershipPlanSchema)};
    std::string schema_version{std::string(kTopologyMembershipSchemaVersion)};
    TopologyMembershipFunnel funnel;
    std::vector<TopologyUnitMembership> units;
    std::string plan_sha256;
};

struct TopologyReadPatternCounters {
    std::uint64_t unique_read_projections_seen = 0;
    std::uint64_t reads_touching_at_least_one_locus = 0;
    std::uint64_t reads_not_touching_any_locus = 0;
    std::uint64_t complete_ra_reads = 0;
    std::uint64_t partial_raox_reads = 0;
    std::uint64_t rg_only_duplicate_occurrences_ignored = 0;
    std::array<std::uint64_t, 4> raw_code_counts{};
};

struct TopologyReadPatternAdapterResult {
    std::string schema_name{"longlineage.topology_read_pattern_adapter"};
    std::string schema_version{std::string(kTopologyMembershipSchemaVersion)};
    std::vector<std::string> canonical_read_ids;
    std::vector<solver::ReadPatternObservation> observations;
    TopologyReadPatternCounters counters;
    // Complete R/A among every touching projection. This diagnostic is not
    // the upstream joint-signature core-read set and must not replace it.
    std::string complete_ra_projection_readset_sha256;
    std::string input_sha256;
    std::string evidence_sha256;
};

// Validates every candidate before applying the eligibility funnel. Only
// M2-eligible + joint-signature PASS candidates instantiate a unit. Rejected
// candidates remain census outcomes and never become solver abstentions.
// PASS requires 1-3 partners and a complete-readset SHA. FAIL/NOT_EVALUATED
// require both fields absent, so a missing selection is never fabricated.
//
// Units are sorted by:
//   dataset_order, canonical chr1..chr22, focal_site_order, sorted loci
// and then assigned dense zero-based unit_order values.
[[nodiscard]] ParseResult<TopologyMembershipPlan> build_topology_membership_plan(
    const std::vector<TopologySiteCandidate>& candidates);

// Frozen readset identity: unique opaque read IDs, sorted by UTF-8 byte order
// and bound by SHA-256. P4 may use this for its complete core-read set; P5 also
// reports a separately named all-projection complete-R/A diagnostic.
[[nodiscard]] ParseResult<std::string> topology_joint_complete_readset_sha256(const std::vector<std::string>& read_ids);

// Builds one observation per unique read projection that touches at least one
// unit locus. It never partitions by HP/PS and never weights RG occurrences.
// Every unit locus must exactly match the block marker universe. R/A retain a
// usable BQ >=20; O/X are emitted with null BQ and retain their distinct code.
// CN/LOH authority is intentionally outside this membership contract; absent
// authority remains NOT_EVALUATED/UNRESOLVED in the downstream P5 serializer.
[[nodiscard]] ParseResult<TopologyReadPatternAdapterResult> build_topology_read_patterns(
    const BlockScienceEvidence& block, const TopologyUnitMembership& membership);

}  // namespace longlineage::pipeline
