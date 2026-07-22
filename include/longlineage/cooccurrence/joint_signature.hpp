// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/common/parse_result.hpp"

namespace longlineage::cooccurrence {

inline constexpr std::size_t kJointSignatureTopMarkers = 3;
inline constexpr std::size_t kJointSignaturePermutations = 999;
inline constexpr std::size_t kJointSignatureMinimumTotal = 10;
inline constexpr std::size_t kJointSignatureMinimumGroup = 3;
inline constexpr std::size_t kJointSignatureMinimumCategory = 3;
inline constexpr std::uint64_t kJointSignatureMinimumSpacingBp = 20;
inline constexpr double kJointSignatureCramersVMinimum = 0.30;
inline constexpr double kJointSignatureDeltaAltMinimum = 0.50;
inline constexpr double kJointSignatureConditionalPMaximum = 0.05;
inline constexpr double kJointSignatureGlobalQMaximum = 0.05;

enum class JointMarkerCall {
    kReference,
    kAlternate,
    kOther,
    kNoCall,
};

[[nodiscard]] char to_char(JointMarkerCall call) noexcept;

struct JointPartnerCandidate {
    std::uint64_t position1{0};
    char reference{'N'};
    char alternate{'N'};
    std::size_t endpoint_a_n_informative{0};
};

struct JointMarkerObservation {
    std::uint64_t position1{0};
    JointMarkerCall call{JointMarkerCall::kNoCall};
};

struct JointCoreRead {
    std::string stable_key;
    std::string label;
    std::string hp_family;
    std::string phase_set;
    std::string strand;
    std::vector<JointMarkerObservation> marker_calls;
};

enum class JointAssociationStatus {
    kOk,
    kFewerThanTwoTopMarkers,
    kInsufficientJointInformation,
    kGroupBelowMinimum,
    kMethylGroupBelowMinimum,
    kCategoryBelowMinimum,
    kMarkerAlleleBelowMinimum,
};

[[nodiscard]] std::string_view to_string(JointAssociationStatus status) noexcept;

struct JointCategoricalAssociation {
    std::vector<std::string> groups;
    std::vector<std::string> categories;
    std::vector<std::vector<std::uint64_t>> table;
    std::size_t n{0};
    bool testable{false};
    JointAssociationStatus status{JointAssociationStatus::kInsufficientJointInformation};
    std::optional<double> cramers_v;
};

enum class JointConditionalStatus {
    kNotRunJointSignatureNotTestable,
    kNotIdentifiableDegenerateTable,
    kNotIdentifiableNonexchangeable,
    kPermutable,
};

[[nodiscard]] std::string_view to_string(JointConditionalStatus status) noexcept;

struct JointConditionalResult {
    std::optional<double> p_value;
    std::size_t permutations{0};
    std::size_t exceedance{0};
    std::size_t strata{0};
    std::size_t exchangeable_strata{0};
    std::size_t informative_exchangeable_strata{0};
    bool permutable{false};
    JointConditionalStatus status{JointConditionalStatus::kNotRunJointSignatureNotTestable};
};

struct JointMarkerSupport {
    std::uint64_t position1{0};
    std::vector<std::string> groups;
    std::vector<std::array<std::uint64_t, 2>> table;
    std::size_t n_informative{0};
    bool testable{false};
    JointAssociationStatus status{JointAssociationStatus::kInsufficientJointInformation};
    std::optional<double> cramers_v;
    std::optional<double> delta_alt_fraction;
    bool effect_gate_pass{false};
};

enum class JointTopologyGateStatus {
    kNotEvaluatedM2Ineligible,
    kFailJointSignatureNotTestable,
    kFailJointSignatureNotPermutable,
    kFailGlobalBy,
    kFailInsufficientConfirmedPairs,
    kFailInsufficientTopConfirmedPairs,
    kFailInsufficientSameCompleteReadSupport,
    kPass,
};

[[nodiscard]] std::string_view to_string(JointTopologyGateStatus status) noexcept;

struct JointSignatureResult {
    std::vector<JointPartnerCandidate> top_partners;
    std::size_t n_core_reads{0};
    std::size_t n_complete_reads{0};
    // Canonical identity of the unique opaque read IDs that are R/A complete
    // at every selected marker. This deliberately excludes partial R/A/O/X
    // projections and is the P4 core-readset digest consumed by P5.
    std::string complete_readset_sha256;
    std::vector<std::pair<std::string, std::size_t>> signature_counts;
    JointCategoricalAssociation association;
    JointConditionalResult conditional;
    bool conditional_sensitivity_pass{false};
    std::vector<JointMarkerSupport> marker_support;
    std::vector<std::uint64_t> effect_supported_positions;
    bool postselection_fdr_calibrated{false};
    bool topology_gate_evaluated{false};
    JointTopologyGateStatus topology_gate_status{JointTopologyGateStatus::kNotEvaluatedM2Ineligible};
    bool passed{false};
    // Present only when passed. Values preserve P4 coverage-ranking order,
    // while membership in the set is determined by the frozen >=20 bp
    // same-complete-read/formal-pair gate.
    std::vector<std::uint64_t> selected_partner_site_orders;
    std::string topology_gate_sha256;
    std::string semantic_sha256;
};

// Ranking authority: informative complete-core coverage descending, then
// absolute distance from the focal, position, REF and ALT. Positions must be
// unique because the historical read-call payload is position keyed.
[[nodiscard]] ParseResult<std::vector<JointPartnerCandidate>> select_top_joint_partners(
    std::uint64_t focal_position1, const std::vector<JointPartnerCandidate>& candidates,
    std::size_t maximum_markers = kJointSignatureTopMarkers);

// Complete-read authority: a read contributes one joint category only when
// every selected marker is R/A callable. Inputs and returned labels are
// canonicalized by stable_key before the PCG64 permutation stream is consumed.
// stable_key is a lowercase 64-hex opaque read ID. The complete-readset digest
// uses the frozen longlineage.topology_joint_complete_readset 1.0.0 byte
// encoding shared with the P5 topology-membership boundary.
[[nodiscard]] ParseResult<JointSignatureResult> evaluate_joint_signature(
    std::uint64_t focal_position1, const std::vector<JointPartnerCandidate>& candidates,
    const std::vector<JointCoreRead>& core_reads, std::uint64_t seed,
    std::size_t maximum_markers = kJointSignatureTopMarkers, std::size_t permutations = kJointSignaturePermutations);

enum class JointGlobalFdrStatus {
    kIneligibleM2Screen,
    kEligibleJointSignatureNotTestable,
    kEligibleJointSignatureNotPermutable,
    kInvalidPermutationEvidence,
    kEligibleJointSignatureGlobalFdrFamily,
};

[[nodiscard]] std::string_view to_string(JointGlobalFdrStatus status) noexcept;

struct JointGlobalFdrInput {
    std::string stable_site_key;
    bool m2_screen_eligible{false};
    bool joint_signature_testable{false};
    bool joint_signature_permutable{false};
    std::size_t permutations{0};
    std::optional<double> conditional_p;
};

struct JointGlobalFdrRow {
    std::string stable_site_key;
    JointGlobalFdrStatus status{JointGlobalFdrStatus::kIneligibleM2Screen};
    std::optional<double> conditional_p;
    std::optional<double> q_global_bh;
    std::optional<double> q_global_by;
    bool global_bh_discovery{false};
    bool global_by_discovery{false};
};

struct JointGlobalFdrResult {
    std::vector<JointGlobalFdrRow> rows;
    std::size_t family_size{0};
    std::size_t bh_discoveries{0};
    std::size_t by_discoveries{0};
    bool all_permutation_evidence_valid{true};
    std::string semantic_sha256;
};

struct JointPartnerFormalEvidence {
    std::uint64_t position1{0};
    std::uint64_t site_order{0};
    bool formal_pair_by_confirmed{false};
};

struct JointTopologyGateInput {
    std::string stable_site_key;
    bool m2_screen_eligible{false};
    JointGlobalFdrRow global_fdr;
    // Complete focal->partner universe for this site, not only the selected
    // top markers. The all-pair >=20 bp gate is part of the historical formal
    // candidate contract.
    std::vector<JointPartnerFormalEvidence> partner_evidence;
};

// The global family is all M2-eligible, testable, permutable sites carrying an
// exact 999-permutation add-one-grid p-value. Malformed permutation evidence is
// excluded with an explicit fail-closed status and makes the aggregate
// all_permutation_evidence_valid flag false.
[[nodiscard]] ParseResult<JointGlobalFdrResult> apply_joint_signature_global_fdr(
    const std::vector<JointGlobalFdrInput>& inputs);

// Applies the post-global historical molecular-haplotype gate. PASS requires:
// M2 eligibility, valid global-BY discovery, >=2 spaced formal pairs, >=2
// spaced top formal pairs, and >=2 spaced top formal pairs retaining the
// same-complete-read effect gate. Selected partner site orders are emitted
// only on PASS and retain top-partner coverage-ranking order.
[[nodiscard]] ParseResult<bool> finalize_joint_signature_topology_gate(JointSignatureResult& result,
                                                                       const JointTopologyGateInput& input);

}  // namespace longlineage::cooccurrence
