// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/pipeline/topology_membership.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "longlineage/common/digest.hpp"

namespace longlineage::pipeline {
namespace {

constexpr std::uint64_t kMaximumPartnerDistanceBp = 5000;
constexpr std::uint64_t kMinimumPartnerSpacingBp = 20;
constexpr std::uint8_t kMinimumBaseQuality = 20;

struct ValidatedCandidate {
    const TopologySiteCandidate* candidate = nullptr;
    std::size_t contig_rank = 0;
    std::vector<TopologyLocusIdentity> selection_order_partners;
    std::vector<TopologyLocusIdentity> sorted_partners;
    std::vector<TopologyLocusIdentity> sorted_loci;
};

struct CanonicalReadPattern {
    std::string read_id;
    solver::ReadPatternObservation observation;
};

template <typename Integer>
void append_integer(std::string& output, Integer value) {
    char buffer[32]{};
    const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (encoded.ec != std::errc{}) {
        throw std::runtime_error("cannot canonicalize topology integer");
    }
    output.append(buffer, encoded.ptr);
}

void append_field(std::string& output, std::string_view value) {
    append_integer(output, value.size());
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

[[nodiscard]] bool checked_add(std::uint64_t& destination, std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - destination) {
        return false;
    }
    destination += value;
    return true;
}

[[nodiscard]] bool is_lower_hex_sha256(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool is_safe_identity_string(std::string_view value) noexcept {
    return !value.empty() && value.find_first_of(std::string_view("\t\r\n\0", 4)) == std::string_view::npos;
}

[[nodiscard]] bool is_snv_base(char value) noexcept {
    return value == 'A' || value == 'C' || value == 'G' || value == 'T';
}

[[nodiscard]] std::optional<std::size_t> canonical_contig_rank(std::string_view contig) {
    for (std::size_t index = 1; index <= 22; ++index) {
        if (contig == "chr" + std::to_string(index)) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::uint64_t absolute_distance(Position1 lhs, Position1 rhs) noexcept {
    return lhs.value() >= rhs.value() ? lhs.value() - rhs.value() : rhs.value() - lhs.value();
}

void append_locus(std::string& output, const TopologyLocusIdentity& locus) {
    append_integer(output, locus.dataset_order);
    output.push_back('|');
    append_field(output, locus.dataset_id);
    append_field(output, locus.contig.value());
    append_integer(output, locus.site_order);
    output.push_back('|');
    append_integer(output, locus.position.value());
    output.push_back('|');
    output.push_back(locus.reference);
    output.push_back('|');
    output.push_back(locus.alternate);
    output.push_back('\n');
}

[[nodiscard]] bool locus_less(const TopologyLocusIdentity& lhs, const TopologyLocusIdentity& rhs) {
    return std::tie(lhs.site_order, lhs.position, lhs.reference, lhs.alternate) <
           std::tie(rhs.site_order, rhs.position, rhs.reference, rhs.alternate);
}

[[nodiscard]] bool joint_state_valid(JointSignatureState state) noexcept {
    return state == JointSignatureState::kPass || state == JointSignatureState::kFail ||
           state == JointSignatureState::kNotEvaluated;
}

[[nodiscard]] ParseResult<ValidatedCandidate> validate_candidate_local(const TopologySiteCandidate& candidate) {
    if (candidate.schema_name != kTopologySiteCandidateSchema ||
        candidate.schema_version != kTopologyMembershipSchemaVersion) {
        return ParseResult<ValidatedCandidate>::failure(ParseReason::kUnsupportedValue,
                                                        "topology site candidate schema name/version is unsupported");
    }
    if (!joint_state_valid(candidate.joint_signature_state)) {
        return ParseResult<ValidatedCandidate>::failure(ParseReason::kUnsupportedValue,
                                                        "topology site candidate has an unknown joint-signature state");
    }
    const auto focal_rank = canonical_contig_rank(candidate.focal.contig.value());
    if (!focal_rank.has_value()) {
        return ParseResult<ValidatedCandidate>::failure(ParseReason::kUnsupportedValue,
                                                        "topology membership accepts only canonical chr1..chr22");
    }
    if (!is_safe_identity_string(candidate.focal.dataset_id) || !is_snv_base(candidate.focal.reference) ||
        !is_snv_base(candidate.focal.alternate) || candidate.focal.reference == candidate.focal.alternate) {
        return ParseResult<ValidatedCandidate>::failure(ParseReason::kMalformedValue,
                                                        "focal locus identity is malformed");
    }
    const bool joint_pass = candidate.joint_signature_state == JointSignatureState::kPass;
    if (joint_pass && (candidate.selected_partners.empty() || candidate.selected_partners.size() > 3)) {
        return ParseResult<ValidatedCandidate>::failure(ParseReason::kMalformedValue,
                                                        "joint-signature PASS must select one to three partners");
    }
    if (joint_pass && !is_lower_hex_sha256(candidate.joint_complete_readset_sha256)) {
        return ParseResult<ValidatedCandidate>::failure(
            ParseReason::kMalformedValue, "joint-signature PASS complete-readset digest is not lowercase SHA-256");
    }
    if (!joint_pass && (!candidate.selected_partners.empty() || !candidate.joint_complete_readset_sha256.empty())) {
        return ParseResult<ValidatedCandidate>::failure(
            ParseReason::kMalformedValue,
            "joint-signature FAIL/NOT_EVALUATED must not carry partners or a readset digest");
    }

    ValidatedCandidate validated;
    validated.candidate = &candidate;
    validated.contig_rank = *focal_rank;
    validated.selection_order_partners = candidate.selected_partners;
    validated.sorted_partners = validated.selection_order_partners;
    for (const TopologyLocusIdentity& partner : validated.sorted_partners) {
        if (partner.dataset_order != candidate.focal.dataset_order ||
            partner.dataset_id != candidate.focal.dataset_id || partner.contig != candidate.focal.contig) {
            return ParseResult<ValidatedCandidate>::failure(ParseReason::kMalformedValue,
                                                            "partner and focal must have the same dataset and contig");
        }
        if (!is_safe_identity_string(partner.dataset_id) || !is_snv_base(partner.reference) ||
            !is_snv_base(partner.alternate) || partner.reference == partner.alternate ||
            !canonical_contig_rank(partner.contig.value()).has_value()) {
            return ParseResult<ValidatedCandidate>::failure(ParseReason::kMalformedValue,
                                                            "partner locus identity is malformed");
        }
        if (partner.site_order == candidate.focal.site_order || partner.position == candidate.focal.position) {
            return ParseResult<ValidatedCandidate>::failure(ParseReason::kMalformedValue,
                                                            "a selected partner duplicates the focal locus");
        }
        if (absolute_distance(partner.position, candidate.focal.position) > kMaximumPartnerDistanceBp) {
            return ParseResult<ValidatedCandidate>::failure(
                ParseReason::kMalformedValue, "a selected partner lies outside the inclusive focal +/-5000 bp window");
        }
    }
    std::sort(validated.sorted_partners.begin(), validated.sorted_partners.end(), locus_less);
    for (std::size_t index = 1; index < validated.sorted_partners.size(); ++index) {
        const TopologyLocusIdentity& previous = validated.sorted_partners[index - 1];
        const TopologyLocusIdentity& current = validated.sorted_partners[index];
        if (previous.site_order == current.site_order || previous.position == current.position) {
            return ParseResult<ValidatedCandidate>::failure(ParseReason::kMalformedValue,
                                                            "selected partner identities contain a duplicate locus");
        }
    }
    for (std::size_t lhs = 0; lhs < validated.sorted_partners.size(); ++lhs) {
        for (std::size_t rhs = lhs + 1; rhs < validated.sorted_partners.size(); ++rhs) {
            if (absolute_distance(validated.sorted_partners[lhs].position, validated.sorted_partners[rhs].position) <
                kMinimumPartnerSpacingBp) {
                return ParseResult<ValidatedCandidate>::failure(
                    ParseReason::kMalformedValue, "selected partners violate the >=20 bp pairwise spacing contract");
            }
        }
    }
    validated.sorted_loci = validated.sorted_partners;
    validated.sorted_loci.push_back(candidate.focal);
    std::sort(validated.sorted_loci.begin(), validated.sorted_loci.end(), locus_less);
    if (joint_pass && (validated.sorted_loci.size() < 2 || validated.sorted_loci.size() > 4)) {
        return ParseResult<ValidatedCandidate>::failure(ParseReason::kMalformedValue,
                                                        "topology unit must contain two to four loci");
    }
    for (std::size_t index = 1; index < validated.sorted_loci.size(); ++index) {
        if (validated.sorted_loci[index - 1].site_order == validated.sorted_loci[index].site_order ||
            validated.sorted_loci[index - 1].position == validated.sorted_loci[index].position) {
            return ParseResult<ValidatedCandidate>::failure(ParseReason::kMalformedValue,
                                                            "topology loci are not unique");
        }
    }
    return ParseResult<ValidatedCandidate>::success(std::move(validated));
}

[[nodiscard]] ParseResult<std::string> membership_digest(const TopologyUnitMembership& membership) {
    try {
        std::string canonical;
        canonical.append(kTopologyUnitMembershipSchema);
        canonical.push_back('\t');
        canonical.append(kTopologyMembershipSchemaVersion);
        canonical.push_back('\n');
        append_integer(canonical, membership.unit_order);
        canonical.push_back('\n');
        append_locus(canonical, membership.focal);
        canonical.append("m2_eligible=1\njoint_signature=PASS\n");
        canonical.append("partners=");
        append_integer(canonical, membership.selected_partners.size());
        canonical.push_back('\n');
        for (const TopologyLocusIdentity& partner : membership.selected_partners) {
            append_locus(canonical, partner);
        }
        canonical.append("loci=");
        append_integer(canonical, membership.loci.size());
        canonical.push_back('\n');
        for (const TopologyLocusIdentity& locus : membership.loci) {
            append_locus(canonical, locus);
        }
        canonical.append("joint_complete_readset_sha256=");
        canonical.append(membership.joint_complete_readset_sha256);
        canonical.push_back('\n');
        return sha256_hex(canonical);
    } catch (const std::exception& error) {
        return ParseResult<std::string>::failure(
            ParseReason::kIoError, std::string("cannot canonicalize topology membership: ") + error.what());
    }
}

[[nodiscard]] bool marker_matches_locus(const VariantSite& marker, const TopologyLocusIdentity& locus) noexcept {
    return marker.dataset_order == locus.dataset_order && marker.dataset_id == locus.dataset_id &&
           marker.contig == locus.contig && marker.site_order == locus.site_order &&
           marker.position == locus.position && marker.reference == locus.reference &&
           marker.alternate == locus.alternate;
}

[[nodiscard]] std::size_t allele_index(AlleleCall call) noexcept {
    switch (call) {
        case AlleleCall::kReference:
            return 0;
        case AlleleCall::kAlternate:
            return 1;
        case AlleleCall::kOther:
            return 2;
        case AlleleCall::kUnobservable:
            return 3;
    }
    return 3;
}

[[nodiscard]] ParseResult<std::string> read_pattern_input_digest(const TopologyUnitMembership& membership,
                                                                 const std::vector<CanonicalReadPattern>& rows) {
    try {
        std::string canonical("longlineage.topology_read_pattern_input\t1.0.0\n");
        canonical.append("membership=");
        canonical.append(membership.membership_sha256);
        canonical.push_back('\n');
        canonical.append("rows=");
        append_integer(canonical, rows.size());
        canonical.push_back('\n');
        for (const CanonicalReadPattern& row : rows) {
            append_field(canonical, row.read_id);
            append_field(canonical, row.observation.pattern_raox);
            for (const auto quality : row.observation.base_qualities) {
                if (quality.has_value()) {
                    append_integer(canonical, *quality);
                } else {
                    canonical.push_back('.');
                }
                canonical.push_back(',');
            }
            canonical.push_back('|');
            append_integer(canonical, row.observation.multiplicity);
            canonical.push_back('\n');
        }
        return sha256_hex(canonical);
    } catch (const std::exception& error) {
        return ParseResult<std::string>::failure(
            ParseReason::kIoError, std::string("cannot canonicalize topology read patterns: ") + error.what());
    }
}

}  // namespace

std::string_view to_string(JointSignatureState state) noexcept {
    switch (state) {
        case JointSignatureState::kPass:
            return "PASS";
        case JointSignatureState::kFail:
            return "FAIL";
        case JointSignatureState::kNotEvaluated:
            return "NOT_EVALUATED";
    }
    return "UNKNOWN";
}

ParseResult<TopologyMembershipPlan> build_topology_membership_plan(
    const std::vector<TopologySiteCandidate>& candidates) {
    try {
        TopologyMembershipPlan output;
        output.funnel.candidates_total = candidates.size();
        std::vector<ValidatedCandidate> validated;
        validated.reserve(candidates.size());

        std::map<std::uint32_t, std::string> dataset_id_by_order;
        std::map<std::string, std::uint32_t> dataset_order_by_id;
        std::map<std::pair<std::uint32_t, std::uint64_t>, TopologyLocusIdentity> locus_by_site;
        std::map<std::tuple<std::uint32_t, std::string, std::uint64_t>, TopologyLocusIdentity> locus_by_position;
        std::set<std::pair<std::uint32_t, std::uint64_t>> focal_keys;

        for (const TopologySiteCandidate& candidate : candidates) {
            auto one = validate_candidate_local(candidate);
            if (!one.ok()) {
                return ParseResult<TopologyMembershipPlan>::failure(one.reason, std::move(one.detail));
            }
            const auto by_order =
                dataset_id_by_order.emplace(candidate.focal.dataset_order, candidate.focal.dataset_id);
            if (!by_order.second && by_order.first->second != candidate.focal.dataset_id) {
                return ParseResult<TopologyMembershipPlan>::failure(
                    ParseReason::kMalformedValue, "one dataset_order is bound to conflicting dataset IDs");
            }
            const auto by_id = dataset_order_by_id.emplace(candidate.focal.dataset_id, candidate.focal.dataset_order);
            if (!by_id.second && by_id.first->second != candidate.focal.dataset_order) {
                return ParseResult<TopologyMembershipPlan>::failure(
                    ParseReason::kMalformedValue, "one dataset ID is bound to conflicting dataset_order values");
            }
            const auto focal_key = std::make_pair(candidate.focal.dataset_order, candidate.focal.site_order);
            if (!focal_keys.insert(focal_key).second) {
                return ParseResult<TopologyMembershipPlan>::failure(
                    ParseReason::kMalformedValue, "duplicate or conflicting topology focal candidate");
            }
            for (const TopologyLocusIdentity& locus : one.value->sorted_loci) {
                const auto site_key = std::make_pair(locus.dataset_order, locus.site_order);
                const auto site_binding = locus_by_site.emplace(site_key, locus);
                if (!site_binding.second && site_binding.first->second != locus) {
                    return ParseResult<TopologyMembershipPlan>::failure(
                        ParseReason::kMalformedValue,
                        "one dataset/site_order is bound to conflicting locus identities");
                }
                const auto position_key =
                    std::make_tuple(locus.dataset_order, locus.contig.value(), locus.position.value());
                const auto position_binding = locus_by_position.emplace(position_key, locus);
                if (!position_binding.second && position_binding.first->second != locus) {
                    return ParseResult<TopologyMembershipPlan>::failure(
                        ParseReason::kMalformedValue,
                        "one dataset genomic position is bound to conflicting locus identities");
                }
            }
            validated.push_back(std::move(*one.value));
        }

        std::sort(validated.begin(), validated.end(), [](const ValidatedCandidate& lhs, const ValidatedCandidate& rhs) {
            const auto locus_orders = [](const std::vector<TopologyLocusIdentity>& loci) {
                std::vector<std::uint64_t> orders;
                orders.reserve(loci.size());
                for (const TopologyLocusIdentity& locus : loci) {
                    orders.push_back(locus.site_order);
                }
                return orders;
            };
            return std::make_tuple(lhs.candidate->focal.dataset_order, lhs.contig_rank, lhs.candidate->focal.site_order,
                                   locus_orders(lhs.sorted_loci)) <
                   std::make_tuple(rhs.candidate->focal.dataset_order, rhs.contig_rank, rhs.candidate->focal.site_order,
                                   locus_orders(rhs.sorted_loci));
        });

        for (const ValidatedCandidate& entry : validated) {
            const TopologySiteCandidate& candidate = *entry.candidate;
            if (!candidate.m2_eligible) {
                ++output.funnel.not_instantiated_m2_ineligible;
                continue;
            }
            if (candidate.joint_signature_state == JointSignatureState::kFail) {
                ++output.funnel.not_instantiated_joint_signature_fail;
                continue;
            }
            if (candidate.joint_signature_state == JointSignatureState::kNotEvaluated) {
                ++output.funnel.not_instantiated_joint_signature_not_evaluated;
                continue;
            }

            const std::uint64_t unit_order = output.units.size();
            TopologyUnitMembership membership(unit_order, candidate.focal);
            membership.selected_partners = entry.selection_order_partners;
            membership.loci = entry.sorted_loci;
            membership.joint_complete_readset_sha256 = candidate.joint_complete_readset_sha256;
            auto digest = membership_digest(membership);
            if (!digest.ok()) {
                return ParseResult<TopologyMembershipPlan>::failure(digest.reason, std::move(digest.detail));
            }
            membership.membership_sha256 = std::move(*digest.value);
            membership.unit_id = "topology-unit:" + membership.membership_sha256;
            output.units.push_back(std::move(membership));
        }
        output.funnel.instantiated_units = output.units.size();
        std::uint64_t funnel_sum = output.funnel.instantiated_units;
        if (!checked_add(funnel_sum, output.funnel.not_instantiated_m2_ineligible) ||
            !checked_add(funnel_sum, output.funnel.not_instantiated_joint_signature_fail) ||
            !checked_add(funnel_sum, output.funnel.not_instantiated_joint_signature_not_evaluated) ||
            funnel_sum != output.funnel.candidates_total) {
            return ParseResult<TopologyMembershipPlan>::failure(
                ParseReason::kMalformedValue, "topology membership funnel does not conserve candidates");
        }

        std::string canonical;
        canonical.append(kTopologyMembershipPlanSchema);
        canonical.push_back('\t');
        canonical.append(kTopologyMembershipSchemaVersion);
        canonical.push_back('\n');
        canonical.append("candidates_total=");
        append_integer(canonical, output.funnel.candidates_total);
        canonical.append("\ninstantiated_units=");
        append_integer(canonical, output.funnel.instantiated_units);
        canonical.append("\nnot_instantiated_m2_ineligible=");
        append_integer(canonical, output.funnel.not_instantiated_m2_ineligible);
        canonical.append("\nnot_instantiated_joint_signature_fail=");
        append_integer(canonical, output.funnel.not_instantiated_joint_signature_fail);
        canonical.append("\nnot_instantiated_joint_signature_not_evaluated=");
        append_integer(canonical, output.funnel.not_instantiated_joint_signature_not_evaluated);
        canonical.push_back('\n');
        canonical.append("canonical_candidates=");
        append_integer(canonical, validated.size());
        canonical.push_back('\n');
        for (const ValidatedCandidate& entry : validated) {
            const TopologySiteCandidate& candidate = *entry.candidate;
            append_locus(canonical, candidate.focal);
            canonical.append("m2_eligible=");
            canonical.push_back(candidate.m2_eligible ? '1' : '0');
            canonical.append("\njoint_signature=");
            canonical.append(to_string(candidate.joint_signature_state));
            canonical.append("\npartners=");
            append_integer(canonical, entry.selection_order_partners.size());
            canonical.push_back('\n');
            for (const TopologyLocusIdentity& partner : entry.selection_order_partners) {
                append_locus(canonical, partner);
            }
            canonical.append("joint_complete_readset_sha256=");
            canonical.append(candidate.joint_complete_readset_sha256);
            canonical.push_back('\n');
        }
        canonical.append("instantiated_memberships=");
        append_integer(canonical, output.units.size());
        canonical.push_back('\n');
        for (const TopologyUnitMembership& membership : output.units) {
            append_integer(canonical, membership.unit_order);
            canonical.push_back('\t');
            canonical.append(membership.membership_sha256);
            canonical.push_back('\n');
        }
        auto plan_digest = sha256_hex(canonical);
        if (!plan_digest.ok()) {
            return ParseResult<TopologyMembershipPlan>::failure(plan_digest.reason, std::move(plan_digest.detail));
        }
        output.plan_sha256 = std::move(*plan_digest.value);
        return output.units.empty() ? ParseResult<TopologyMembershipPlan>::success_empty(std::move(output))
                                    : ParseResult<TopologyMembershipPlan>::success(std::move(output));
    } catch (const std::exception& error) {
        return ParseResult<TopologyMembershipPlan>::failure(
            ParseReason::kMalformedValue, std::string("topology membership planning failed: ") + error.what());
    }
}

ParseResult<std::string> topology_joint_complete_readset_sha256(const std::vector<std::string>& read_ids) {
    try {
        std::vector<std::string> canonical_ids = read_ids;
        for (const std::string& read_id : canonical_ids) {
            if (!is_lower_hex_sha256(read_id)) {
                return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                         "joint complete-readset contains a malformed opaque read ID");
            }
        }
        std::sort(canonical_ids.begin(), canonical_ids.end());
        if (std::adjacent_find(canonical_ids.begin(), canonical_ids.end()) != canonical_ids.end()) {
            return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                     "joint complete-readset contains a duplicate opaque read ID");
        }
        std::string canonical("longlineage.topology_joint_complete_readset\t1.0.0\n");
        canonical.append("reads=");
        append_integer(canonical, canonical_ids.size());
        canonical.push_back('\n');
        for (const std::string& read_id : canonical_ids) {
            append_field(canonical, read_id);
            canonical.push_back('\n');
        }
        return sha256_hex(canonical);
    } catch (const std::exception& error) {
        return ParseResult<std::string>::failure(
            ParseReason::kIoError, std::string("cannot canonicalize topology complete readset: ") + error.what());
    }
}

ParseResult<TopologyReadPatternAdapterResult> build_topology_read_patterns(const BlockScienceEvidence& block,
                                                                           const TopologyUnitMembership& membership) {
    try {
        if (membership.schema_name != kTopologyUnitMembershipSchema ||
            membership.schema_version != kTopologyMembershipSchemaVersion || !membership.m2_eligible ||
            membership.joint_signature_state != JointSignatureState::kPass ||
            !is_lower_hex_sha256(membership.joint_complete_readset_sha256)) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(
                ParseReason::kMalformedValue, "topology membership is not an instantiated schema-v1 unit");
        }
        TopologySiteCandidate candidate(membership.focal);
        candidate.m2_eligible = membership.m2_eligible;
        candidate.joint_signature_state = membership.joint_signature_state;
        candidate.selected_partners = membership.selected_partners;
        candidate.joint_complete_readset_sha256 = membership.joint_complete_readset_sha256;
        auto validated = validate_candidate_local(candidate);
        if (!validated.ok()) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(validated.reason,
                                                                          std::move(validated.detail));
        }
        if (membership.selected_partners != validated.value->selection_order_partners ||
            membership.loci != validated.value->sorted_loci) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(
                ParseReason::kMalformedValue, "topology membership partners/loci are not canonical");
        }
        auto recomputed_membership = membership_digest(membership);
        if (!recomputed_membership.ok() || membership.membership_sha256 != recomputed_membership.value.value_or("") ||
            membership.unit_id != "topology-unit:" + membership.membership_sha256) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(
                ParseReason::kMalformedValue, "topology membership digest or unit ID does not replay");
        }

        std::map<std::uint64_t, const VariantSite*> markers_by_site;
        std::set<std::uint64_t> marker_positions;
        for (const VariantSite& marker : block.markers) {
            if (marker.dataset_order != membership.focal.dataset_order ||
                marker.dataset_id != membership.focal.dataset_id || marker.contig != membership.focal.contig ||
                !markers_by_site.emplace(marker.site_order, &marker).second ||
                !marker_positions.insert(marker.position.value()).second) {
                return ParseResult<TopologyReadPatternAdapterResult>::failure(
                    ParseReason::kMalformedValue, "block marker universe is not unique within the unit dataset/contig");
            }
        }
        for (const TopologyLocusIdentity& locus : membership.loci) {
            const auto found = markers_by_site.find(locus.site_order);
            if (found == markers_by_site.end() || !marker_matches_locus(*found->second, locus)) {
                return ParseResult<TopologyReadPatternAdapterResult>::failure(
                    ParseReason::kMalformedValue,
                    "a topology unit locus is absent or conflicting in the block marker universe");
            }
        }

        TopologyReadPatternAdapterResult output;
        output.counters.unique_read_projections_seen = block.reads.size();
        std::vector<CanonicalReadPattern> rows;
        rows.reserve(block.reads.size());
        std::set<std::string> observed_read_ids;
        std::set<ReadProjectionIdentity> observed_projections;

        for (const JoinedReadEvidence& read : block.reads) {
            if (!is_lower_hex_sha256(read.read_id) || !observed_read_ids.insert(read.read_id).second ||
                !observed_projections.insert(read.projection).second ||
                read.projection.contig != membership.focal.contig || read.raw_alignment_occurrences == 0) {
                return ParseResult<TopologyReadPatternAdapterResult>::failure(
                    ParseReason::kMalformedValue, "block read identity/projection/occurrence contract is malformed");
            }
            if (!checked_add(output.counters.rg_only_duplicate_occurrences_ignored,
                             static_cast<std::uint64_t>(read.raw_alignment_occurrences - 1))) {
                return ParseResult<TopologyReadPatternAdapterResult>::failure(
                    ParseReason::kMalformedValue, "RG-only duplicate occurrence counter overflow");
            }

            std::map<std::uint64_t, const ProjectedAlleleCall*> calls;
            for (const ProjectedAlleleCall& call : read.allele_calls) {
                if (!calls.emplace(call.site_order, &call).second) {
                    return ParseResult<TopologyReadPatternAdapterResult>::failure(
                        ParseReason::kMalformedValue, "one read contains duplicate projected calls for a site_order");
                }
            }

            bool touches = false;
            CanonicalReadPattern row;
            row.read_id = read.read_id;
            row.observation.pattern_raox.reserve(membership.loci.size());
            row.observation.base_qualities.reserve(membership.loci.size());
            row.observation.multiplicity = 1;
            for (const TopologyLocusIdentity& locus : membership.loci) {
                touches = touches || read.projection.reference_interval.contains(locus.position);
                const auto found = calls.find(locus.site_order);
                if (found == calls.end() || found->second->position != locus.position) {
                    return ParseResult<TopologyReadPatternAdapterResult>::failure(
                        ParseReason::kMalformedValue, "a block read lacks an exact projected call for a unit locus");
                }
                const ProjectedAlleleCall& call = *found->second;
                row.observation.pattern_raox.push_back(to_char(call.call));
                if (call.call == AlleleCall::kReference || call.call == AlleleCall::kAlternate) {
                    if (!call.base_quality.has_value() || *call.base_quality < kMinimumBaseQuality ||
                        *call.base_quality == std::numeric_limits<std::uint8_t>::max()) {
                        return ParseResult<TopologyReadPatternAdapterResult>::failure(
                            ParseReason::kMalformedValue, "fixed R/A topology call lacks BQ>=20 or has BQ=255");
                    }
                    row.observation.base_qualities.push_back(*call.base_quality);
                } else {
                    if (call.call == AlleleCall::kUnobservable && call.base_quality.has_value()) {
                        return ParseResult<TopologyReadPatternAdapterResult>::failure(
                            ParseReason::kMalformedValue,
                            "unobservable X topology call unexpectedly carries a base quality");
                    }
                    // O is a measured non-REF/non-ALT call. Its raw BQ may
                    // exist, but the frozen topology likelihood marginalizes
                    // O exactly like X and therefore must receive null BQ.
                    row.observation.base_qualities.push_back(std::nullopt);
                }
            }
            if (!touches) {
                if (std::any_of(row.observation.pattern_raox.begin(), row.observation.pattern_raox.end(),
                                [](char code) { return code != 'X'; })) {
                    return ParseResult<TopologyReadPatternAdapterResult>::failure(
                        ParseReason::kMalformedValue, "a read outside every unit locus has a non-X projected call");
                }
                ++output.counters.reads_not_touching_any_locus;
                continue;
            }
            ++output.counters.reads_touching_at_least_one_locus;
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const CanonicalReadPattern& lhs, const CanonicalReadPattern& rhs) {
            return lhs.read_id < rhs.read_id;
        });

        std::vector<std::string> complete_read_ids;
        complete_read_ids.reserve(rows.size());
        for (const CanonicalReadPattern& row : rows) {
            const bool complete_ra =
                std::all_of(row.observation.pattern_raox.begin(), row.observation.pattern_raox.end(),
                            [](char code) { return code == 'R' || code == 'A'; });
            if (complete_ra) {
                complete_read_ids.push_back(row.read_id);
                ++output.counters.complete_ra_reads;
            } else {
                ++output.counters.partial_raox_reads;
            }
            for (const char code : row.observation.pattern_raox) {
                auto parsed = parse_allele_call(code);
                if (!parsed.ok() || !checked_add(output.counters.raw_code_counts[allele_index(*parsed.value)], 1)) {
                    return ParseResult<TopologyReadPatternAdapterResult>::failure(
                        ParseReason::kMalformedValue, "topology read-pattern code counter failed");
                }
            }
        }
        std::uint64_t read_conservation = output.counters.reads_touching_at_least_one_locus;
        std::uint64_t pattern_conservation = output.counters.complete_ra_reads;
        std::uint64_t code_conservation = 0;
        if (!checked_add(read_conservation, output.counters.reads_not_touching_any_locus) ||
            read_conservation != output.counters.unique_read_projections_seen ||
            !checked_add(pattern_conservation, output.counters.partial_raox_reads) ||
            pattern_conservation != output.counters.reads_touching_at_least_one_locus) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(
                ParseReason::kMalformedValue, "topology read-pattern row conservation failed");
        }
        for (const std::uint64_t count : output.counters.raw_code_counts) {
            if (!checked_add(code_conservation, count)) {
                return ParseResult<TopologyReadPatternAdapterResult>::failure(
                    ParseReason::kMalformedValue, "topology read-pattern code conservation overflow");
            }
        }
        if (membership.loci.size() != 0 && output.counters.reads_touching_at_least_one_locus >
                                               std::numeric_limits<std::uint64_t>::max() / membership.loci.size()) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(
                ParseReason::kMalformedValue, "topology read-pattern expected code count overflows");
        }
        const std::uint64_t expected_codes = output.counters.reads_touching_at_least_one_locus * membership.loci.size();
        if (code_conservation != expected_codes) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(ParseReason::kMalformedValue,
                                                                          "topology R/A/O/X code conservation failed");
        }

        auto complete_digest = topology_joint_complete_readset_sha256(complete_read_ids);
        if (!complete_digest.ok()) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(complete_digest.reason,
                                                                          std::move(complete_digest.detail));
        }
        output.complete_ra_projection_readset_sha256 = std::move(*complete_digest.value);
        auto input_digest = read_pattern_input_digest(membership, rows);
        if (!input_digest.ok()) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(input_digest.reason,
                                                                          std::move(input_digest.detail));
        }
        output.input_sha256 = std::move(*input_digest.value);
        output.canonical_read_ids.reserve(rows.size());
        output.observations.reserve(rows.size());
        for (CanonicalReadPattern& row : rows) {
            output.canonical_read_ids.push_back(std::move(row.read_id));
            output.observations.push_back(std::move(row.observation));
        }

        std::string canonical("longlineage.topology_read_pattern_adapter\t1.0.0\n");
        canonical.append("block_sequence=");
        append_integer(canonical, block.block_sequence);
        canonical.append("\nmembership=");
        canonical.append(membership.membership_sha256);
        canonical.append("\ninput=");
        canonical.append(output.input_sha256);
        canonical.append("\np4_joint_core_complete_readset=");
        canonical.append(membership.joint_complete_readset_sha256);
        canonical.append("\ncomplete_ra_projection_readset=");
        canonical.append(output.complete_ra_projection_readset_sha256);
        canonical.append("\nunique_read_projections_seen=");
        append_integer(canonical, output.counters.unique_read_projections_seen);
        canonical.append("\nreads_touching_at_least_one_locus=");
        append_integer(canonical, output.counters.reads_touching_at_least_one_locus);
        canonical.append("\nreads_not_touching_any_locus=");
        append_integer(canonical, output.counters.reads_not_touching_any_locus);
        canonical.append("\ncomplete_ra_reads=");
        append_integer(canonical, output.counters.complete_ra_reads);
        canonical.append("\npartial_raox_reads=");
        append_integer(canonical, output.counters.partial_raox_reads);
        canonical.append("\nrg_only_duplicate_occurrences_ignored=");
        append_integer(canonical, output.counters.rg_only_duplicate_occurrences_ignored);
        canonical.append("\nraw_code_counts=");
        for (const std::uint64_t count : output.counters.raw_code_counts) {
            append_integer(canonical, count);
            canonical.push_back(',');
        }
        canonical.push_back('\n');
        auto evidence_digest = sha256_hex(canonical);
        if (!evidence_digest.ok()) {
            return ParseResult<TopologyReadPatternAdapterResult>::failure(evidence_digest.reason,
                                                                          std::move(evidence_digest.detail));
        }
        output.evidence_sha256 = std::move(*evidence_digest.value);
        return output.observations.empty()
                   ? ParseResult<TopologyReadPatternAdapterResult>::success_empty(std::move(output))
                   : ParseResult<TopologyReadPatternAdapterResult>::success(std::move(output));
    } catch (const std::exception& error) {
        return ParseResult<TopologyReadPatternAdapterResult>::failure(
            ParseReason::kMalformedValue, std::string("topology read-pattern adaptation failed: ") + error.what());
    }
}

}  // namespace longlineage::pipeline
