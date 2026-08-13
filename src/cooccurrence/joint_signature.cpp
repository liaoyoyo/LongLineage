// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/cooccurrence/joint_signature.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "longlineage/common/digest.hpp"
#include "longlineage/m1/science.hpp"

namespace longlineage::cooccurrence {
namespace {

[[nodiscard]] bool is_snv_base(char base) noexcept { return base == 'A' || base == 'C' || base == 'G' || base == 'T'; }

[[nodiscard]] bool is_lower_hex_sha256(std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] std::uint64_t absolute_distance(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

[[nodiscard]] std::string sha256_or_throw(std::string_view bytes) {
    auto digest = sha256_hex(bytes);
    if (!digest.ok() || !digest.value.has_value()) {
        throw std::runtime_error("SHA-256 failed while serializing joint-signature state: " + digest.detail);
    }
    return *digest.value;
}

void append_sized(std::ostringstream& stream, std::string_view value) { stream << value.size() << ':' << value; }

[[nodiscard]] std::string complete_readset_sha256(const std::vector<std::string>& sorted_read_ids) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "longlineage.topology_joint_complete_readset\t1.0.0\n"
              << "reads=" << sorted_read_ids.size() << '\n';
    for (const auto& read_id : sorted_read_ids) {
        canonical << read_id.size() << ':' << read_id << "|\n";
    }
    return sha256_or_throw(canonical.str());
}

void append_optional_double(std::ostringstream& stream, const std::optional<double>& value) {
    if (value.has_value()) {
        stream << std::hexfloat << *value << std::defaultfloat;
    } else {
        stream << "null";
    }
}

[[nodiscard]] double cramer_v(const std::vector<std::vector<std::uint64_t>>& table) {
    if (table.size() < 2 || table.front().size() < 2) {
        return 0.0;
    }
    const std::size_t columns = table.front().size();
    std::vector<std::uint64_t> row_totals(table.size(), 0);
    std::vector<std::uint64_t> column_totals(columns, 0);
    std::uint64_t total = 0;
    for (std::size_t row = 0; row < table.size(); ++row) {
        if (table[row].size() != columns) {
            throw std::invalid_argument("joint-signature contingency table must be rectangular");
        }
        for (std::size_t column = 0; column < columns; ++column) {
            if (std::numeric_limits<std::uint64_t>::max() - total < table[row][column] ||
                std::numeric_limits<std::uint64_t>::max() - row_totals[row] < table[row][column] ||
                std::numeric_limits<std::uint64_t>::max() - column_totals[column] < table[row][column]) {
                throw std::overflow_error("joint-signature contingency count overflow");
            }
            row_totals[row] += table[row][column];
            column_totals[column] += table[row][column];
            total += table[row][column];
        }
    }
    if (total == 0) {
        return 0.0;
    }
    double chi_square = 0.0;
    for (std::size_t row = 0; row < table.size(); ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            const double expected = static_cast<double>(row_totals[row]) * static_cast<double>(column_totals[column]) /
                                    static_cast<double>(total);
            if (expected > 0.0) {
                const double delta = static_cast<double>(table[row][column]) - expected;
                chi_square += delta * delta / expected;
            }
        }
    }
    const std::size_t scale = std::min(table.size() - 1, columns - 1);
    const double denominator = static_cast<double>(total) * static_cast<double>(scale);
    return denominator > 0.0 ? std::sqrt(chi_square / denominator) : 0.0;
}

[[nodiscard]] JointCategoricalAssociation categorical_association(const std::vector<std::string>& labels,
                                                                  const std::vector<std::string>& categories) {
    if (labels.size() != categories.size()) {
        throw std::invalid_argument("joint-signature labels/categories length mismatch");
    }
    const std::set<std::string> group_set(labels.begin(), labels.end());
    const std::set<std::string> category_set(categories.begin(), categories.end());
    JointCategoricalAssociation result;
    result.groups.assign(group_set.begin(), group_set.end());
    result.categories.assign(category_set.begin(), category_set.end());
    result.n = labels.size();
    result.table.assign(result.groups.size(), std::vector<std::uint64_t>(result.categories.size(), 0));
    std::map<std::string, std::size_t> group_index;
    std::map<std::string, std::size_t> category_index;
    for (std::size_t index = 0; index < result.groups.size(); ++index) {
        group_index.emplace(result.groups[index], index);
    }
    for (std::size_t index = 0; index < result.categories.size(); ++index) {
        category_index.emplace(result.categories[index], index);
    }
    for (std::size_t index = 0; index < labels.size(); ++index) {
        ++result.table[group_index.at(labels[index])][category_index.at(categories[index])];
    }
    if (result.groups.size() < 2 || result.categories.size() < 2 || result.n < kJointSignatureMinimumTotal) {
        result.status = JointAssociationStatus::kInsufficientJointInformation;
        return result;
    }
    const auto minimum_group =
        std::min_element(result.table.begin(), result.table.end(), [](const auto& lhs, const auto& rhs) {
            return std::accumulate(lhs.begin(), lhs.end(), std::uint64_t{0}) <
                   std::accumulate(rhs.begin(), rhs.end(), std::uint64_t{0});
        });
    if (std::accumulate(minimum_group->begin(), minimum_group->end(), std::uint64_t{0}) < kJointSignatureMinimumGroup) {
        result.status = JointAssociationStatus::kGroupBelowMinimum;
        return result;
    }
    for (std::size_t column = 0; column < result.categories.size(); ++column) {
        std::uint64_t count = 0;
        for (const auto& row : result.table) {
            count += row[column];
        }
        if (count < kJointSignatureMinimumCategory) {
            result.status = JointAssociationStatus::kCategoryBelowMinimum;
            return result;
        }
    }
    result.testable = true;
    result.status = JointAssociationStatus::kOk;
    result.cramers_v = cramer_v(result.table);
    return result;
}

[[nodiscard]] JointMarkerSupport marker_support(std::uint64_t position1, const std::vector<std::string>& labels,
                                                const std::vector<JointMarkerCall>& calls) {
    if (labels.size() != calls.size()) {
        throw std::invalid_argument("same-readset marker labels/calls length mismatch");
    }
    const std::set<std::string> group_set(labels.begin(), labels.end());
    JointMarkerSupport result;
    result.position1 = position1;
    result.groups.assign(group_set.begin(), group_set.end());
    result.table.assign(result.groups.size(), {0, 0});
    std::map<std::string, std::size_t> group_index;
    for (std::size_t index = 0; index < result.groups.size(); ++index) {
        group_index.emplace(result.groups[index], index);
    }
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (calls[index] != JointMarkerCall::kReference && calls[index] != JointMarkerCall::kAlternate) {
            throw std::logic_error("complete-read marker support received a non-R/A call");
        }
        ++result.table[group_index.at(labels[index])][calls[index] == JointMarkerCall::kReference ? 0U : 1U];
    }
    result.n_informative = labels.size();
    if (result.groups.size() < 2 || result.n_informative < kJointSignatureMinimumTotal) {
        result.status = JointAssociationStatus::kInsufficientJointInformation;
        return result;
    }
    for (const auto& row : result.table) {
        if (row[0] + row[1] < kJointSignatureMinimumGroup) {
            result.status = JointAssociationStatus::kMethylGroupBelowMinimum;
            return result;
        }
    }
    std::uint64_t reference_count = 0;
    std::uint64_t alternate_count = 0;
    for (const auto& row : result.table) {
        reference_count += row[0];
        alternate_count += row[1];
    }
    if (reference_count < kJointSignatureMinimumCategory || alternate_count < kJointSignatureMinimumCategory) {
        result.status = JointAssociationStatus::kMarkerAlleleBelowMinimum;
        return result;
    }
    std::vector<std::vector<std::uint64_t>> table;
    table.reserve(result.table.size());
    double minimum_alt_fraction = 1.0;
    double maximum_alt_fraction = 0.0;
    for (const auto& row : result.table) {
        table.push_back({row[0], row[1]});
        const double fraction = static_cast<double>(row[1]) / static_cast<double>(row[0] + row[1]);
        minimum_alt_fraction = std::min(minimum_alt_fraction, fraction);
        maximum_alt_fraction = std::max(maximum_alt_fraction, fraction);
    }
    result.testable = true;
    result.status = JointAssociationStatus::kOk;
    result.cramers_v = cramer_v(table);
    result.delta_alt_fraction = maximum_alt_fraction - minimum_alt_fraction;
    result.effect_gate_pass = *result.cramers_v >= kJointSignatureCramersVMinimum &&
                              *result.delta_alt_fraction >= kJointSignatureDeltaAltMinimum;
    return result;
}

struct ConditionalWork {
    JointConditionalResult result;
    std::vector<double> permutation_statistics;
};

[[nodiscard]] ConditionalWork conditional_permutation(const std::vector<std::string>& labels,
                                                      const std::vector<std::string>& categories,
                                                      const std::vector<std::string>& strata, std::uint64_t seed,
                                                      std::size_t permutations) {
    if (labels.size() != categories.size() || labels.size() != strata.size()) {
        throw std::invalid_argument("joint-signature conditional arrays must have equal length");
    }
    ConditionalWork output;
    output.result.strata = std::set<std::string>(strata.begin(), strata.end()).size();
    const std::set<std::string> groups(labels.begin(), labels.end());
    const std::set<std::string> states(categories.begin(), categories.end());
    if (groups.size() < 2 || states.size() < 2) {
        output.result.status = JointConditionalStatus::kNotIdentifiableDegenerateTable;
        return output;
    }

    std::map<std::string, std::vector<std::size_t>> indices_by_stratum;
    for (std::size_t index = 0; index < strata.size(); ++index) {
        indices_by_stratum[strata[index]].push_back(index);
    }
    std::vector<std::vector<std::size_t>> exchangeable;
    for (const auto& [stratum, indices] : indices_by_stratum) {
        static_cast<void>(stratum);
        std::set<std::string> local_labels;
        std::set<std::string> local_categories;
        for (const std::size_t index : indices) {
            local_labels.insert(labels[index]);
            local_categories.insert(categories[index]);
        }
        if (indices.size() >= 2 && local_labels.size() >= 2) {
            exchangeable.push_back(indices);
            ++output.result.exchangeable_strata;
            if (local_categories.size() >= 2) {
                ++output.result.informative_exchangeable_strata;
            }
        }
    }
    if (output.result.informative_exchangeable_strata == 0) {
        output.result.status = JointConditionalStatus::kNotIdentifiableNonexchangeable;
        return output;
    }

    const double observed = *categorical_association(labels, categories).cramers_v;
    m1::NumpyPcg64 generator(seed);
    output.permutation_statistics.reserve(permutations);
    for (std::size_t permutation = 0; permutation < permutations; ++permutation) {
        std::vector<std::string> candidate_labels = labels;
        for (const auto& indices : exchangeable) {
            const auto order = generator.permutation(indices.size());
            std::vector<std::string> original;
            original.reserve(indices.size());
            for (const std::size_t index : indices) {
                original.push_back(candidate_labels[index]);
            }
            for (std::size_t offset = 0; offset < indices.size(); ++offset) {
                candidate_labels[indices[offset]] = original[order[offset]];
            }
        }
        const double statistic = *categorical_association(candidate_labels, categories).cramers_v;
        output.permutation_statistics.push_back(statistic);
        if (statistic >= observed - 1e-12) {
            ++output.result.exceedance;
        }
    }
    output.result.permutations = permutations;
    output.result.p_value =
        (static_cast<double>(output.result.exceedance) + 1.0) / (static_cast<double>(permutations) + 1.0);
    output.result.permutable = true;
    output.result.status = JointConditionalStatus::kPermutable;
    return output;
}

[[nodiscard]] bool valid_grid_p(double value) {
    if (!std::isfinite(value) || value < 1.0 / static_cast<double>(kJointSignaturePermutations + 1) || value > 1.0) {
        return false;
    }
    const double scaled = value * static_cast<double>(kJointSignaturePermutations + 1);
    return std::abs(scaled - std::round(scaled)) <= 1e-9;
}

[[nodiscard]] bool valid_probability(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] std::vector<std::uint64_t> spatially_separated_positions(std::vector<std::uint64_t> positions) {
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    std::vector<std::uint64_t> selected;
    selected.reserve(positions.size());
    for (const std::uint64_t position : positions) {
        if (selected.empty() || position - selected.back() >= kJointSignatureMinimumSpacingBp) {
            selected.push_back(position);
        }
    }
    return selected;
}

}  // namespace

char to_char(JointMarkerCall call) noexcept {
    switch (call) {
        case JointMarkerCall::kReference:
            return 'R';
        case JointMarkerCall::kAlternate:
            return 'A';
        case JointMarkerCall::kOther:
            return 'O';
        case JointMarkerCall::kNoCall:
            return 'X';
    }
    return 'X';
}

std::string_view to_string(JointAssociationStatus status) noexcept {
    switch (status) {
        case JointAssociationStatus::kOk:
            return "OK";
        case JointAssociationStatus::kFewerThanTwoTopMarkers:
            return "FEWER_THAN_TWO_TOP_MARKERS";
        case JointAssociationStatus::kInsufficientJointInformation:
            return "INSUFFICIENT_JOINT_INFORMATION";
        case JointAssociationStatus::kGroupBelowMinimum:
            return "GROUP_BELOW_MINIMUM";
        case JointAssociationStatus::kMethylGroupBelowMinimum:
            return "METHYL_GROUP_BELOW_MINIMUM";
        case JointAssociationStatus::kCategoryBelowMinimum:
            return "CATEGORY_BELOW_MINIMUM";
        case JointAssociationStatus::kMarkerAlleleBelowMinimum:
            return "MARKER_ALLELE_BELOW_MINIMUM";
    }
    return "INSUFFICIENT_JOINT_INFORMATION";
}

std::string_view to_string(JointConditionalStatus status) noexcept {
    switch (status) {
        case JointConditionalStatus::kNotRunJointSignatureNotTestable:
            return "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE";
        case JointConditionalStatus::kNotIdentifiableDegenerateTable:
            return "NOT_IDENTIFIABLE_DEGENERATE_TABLE";
        case JointConditionalStatus::kNotIdentifiableNonexchangeable:
            return "NOT_IDENTIFIABLE_NONEXCHANGEABLE";
        case JointConditionalStatus::kPermutable:
            return "PERMUTABLE";
    }
    return "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE";
}

std::string_view to_string(JointTopologyGateStatus status) noexcept {
    switch (status) {
        case JointTopologyGateStatus::kNotEvaluatedM2Ineligible:
            return "NOT_EVALUATED_M2_INELIGIBLE";
        case JointTopologyGateStatus::kFailJointSignatureNotTestable:
            return "FAIL_JOINT_SIGNATURE_NOT_TESTABLE";
        case JointTopologyGateStatus::kFailJointSignatureNotPermutable:
            return "FAIL_JOINT_SIGNATURE_NOT_PERMUTABLE";
        case JointTopologyGateStatus::kFailGlobalBy:
            return "FAIL_GLOBAL_BY";
        case JointTopologyGateStatus::kFailInsufficientConfirmedPairs:
            return "FAIL_INSUFFICIENT_CONFIRMED_PAIRS";
        case JointTopologyGateStatus::kFailInsufficientTopConfirmedPairs:
            return "FAIL_INSUFFICIENT_TOP_CONFIRMED_PAIRS";
        case JointTopologyGateStatus::kFailInsufficientSameCompleteReadSupport:
            return "FAIL_INSUFFICIENT_SAME_COMPLETE_READ_SUPPORT";
        case JointTopologyGateStatus::kPass:
            return "PASS";
    }
    return "FAIL_JOINT_SIGNATURE_NOT_TESTABLE";
}

ParseResult<std::vector<JointPartnerCandidate>> select_top_joint_partners(
    std::uint64_t focal_position1, const std::vector<JointPartnerCandidate>& candidates, std::size_t maximum_markers) {
    if (focal_position1 == 0) {
        return ParseResult<std::vector<JointPartnerCandidate>>::failure(ParseReason::kMalformedValue,
                                                                        "focal position must be one-based");
    }
    if (maximum_markers == 0 || maximum_markers > kJointSignatureTopMarkers) {
        return ParseResult<std::vector<JointPartnerCandidate>>::failure(
            ParseReason::kUnsupportedValue, "joint-signature top-marker limit must be within 1-3");
    }
    std::set<std::uint64_t> positions;
    std::vector<JointPartnerCandidate> selected = candidates;
    for (const auto& candidate : selected) {
        if (candidate.position1 == 0 || candidate.position1 == focal_position1 || !is_snv_base(candidate.reference) ||
            !is_snv_base(candidate.alternate) || candidate.reference == candidate.alternate) {
            return ParseResult<std::vector<JointPartnerCandidate>>::failure(
                ParseReason::kMalformedValue, "joint-signature candidate is not a distinct one-based biallelic sSNV");
        }
        if (!positions.insert(candidate.position1).second) {
            return ParseResult<std::vector<JointPartnerCandidate>>::failure(
                ParseReason::kIndexError, "joint-signature partner positions must be unique");
        }
    }
    std::sort(selected.begin(), selected.end(), [&](const auto& lhs, const auto& rhs) {
        if (lhs.endpoint_a_n_informative != rhs.endpoint_a_n_informative) {
            return lhs.endpoint_a_n_informative > rhs.endpoint_a_n_informative;
        }
        return std::tuple{absolute_distance(lhs.position1, focal_position1), lhs.position1, lhs.reference,
                          lhs.alternate} < std::tuple{absolute_distance(rhs.position1, focal_position1), rhs.position1,
                                                      rhs.reference, rhs.alternate};
    });
    if (selected.size() > maximum_markers) {
        selected.resize(maximum_markers);
    }
    return ParseResult<std::vector<JointPartnerCandidate>>::success(std::move(selected));
}

ParseResult<JointSignatureResult> evaluate_joint_signature(std::uint64_t focal_position1,
                                                           const std::vector<JointPartnerCandidate>& candidates,
                                                           const std::vector<JointCoreRead>& core_reads,
                                                           std::uint64_t seed, std::size_t maximum_markers,
                                                           std::size_t permutations) {
    if (permutations != kJointSignaturePermutations) {
        return ParseResult<JointSignatureResult>::failure(
            ParseReason::kUnsupportedValue, "joint-signature conditional inference requires exactly 999 permutations");
    }
    auto ranked = select_top_joint_partners(focal_position1, candidates, maximum_markers);
    if (!ranked.ok() || !ranked.value.has_value()) {
        return ParseResult<JointSignatureResult>::failure(ranked.reason, ranked.detail);
    }
    std::vector<JointCoreRead> reads = core_reads;
    std::sort(reads.begin(), reads.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.stable_key < rhs.stable_key; });
    for (std::size_t index = 0; index < reads.size(); ++index) {
        const auto& read = reads[index];
        if (!is_lower_hex_sha256(read.stable_key) || read.label.empty() || read.hp_family.empty() ||
            read.phase_set.empty() || read.strand.empty()) {
            return ParseResult<JointSignatureResult>::failure(
                ParseReason::kMalformedValue,
                "joint-signature core read requires a lowercase 64-hex opaque read ID and non-empty label/stratum");
        }
        if (index > 0 && reads[index - 1].stable_key == read.stable_key) {
            return ParseResult<JointSignatureResult>::failure(ParseReason::kIndexError,
                                                              "joint-signature stable read keys must be unique");
        }
        std::set<std::uint64_t> observed;
        for (const auto& call : read.marker_calls) {
            if (call.position1 == 0 || !observed.insert(call.position1).second) {
                return ParseResult<JointSignatureResult>::failure(
                    ParseReason::kIndexError, "joint-signature read has a zero or duplicate marker position");
            }
        }
    }

    JointSignatureResult result;
    result.top_partners = *ranked.value;
    result.n_core_reads = reads.size();
    std::vector<std::string> labels;
    std::vector<std::string> signatures;
    std::vector<std::string> strata;
    std::vector<std::vector<JointMarkerCall>> complete_calls;
    std::vector<std::string> complete_keys;
    for (const auto& read : reads) {
        std::map<std::uint64_t, JointMarkerCall> calls_by_position;
        for (const auto& call : read.marker_calls) {
            calls_by_position.emplace(call.position1, call.call);
        }
        bool complete = true;
        std::string signature;
        std::vector<JointMarkerCall> calls;
        calls.reserve(result.top_partners.size());
        for (const auto& partner : result.top_partners) {
            const auto found = calls_by_position.find(partner.position1);
            if (found == calls_by_position.end()) {
                return ParseResult<JointSignatureResult>::failure(
                    ParseReason::kMissingRequiredField, "joint-signature core read lacks a selected partner call");
            }
            calls.push_back(found->second);
            complete = complete &&
                       (found->second == JointMarkerCall::kReference || found->second == JointMarkerCall::kAlternate);
            signature.push_back(to_char(found->second));
        }
        if (complete) {
            complete_keys.push_back(read.stable_key);
            labels.push_back(read.label);
            signatures.push_back(signature);
            strata.push_back(read.hp_family + "|PS=" + read.phase_set + "|strand=" + read.strand);
            complete_calls.push_back(std::move(calls));
        }
    }
    result.n_complete_reads = labels.size();
    result.complete_readset_sha256 = complete_readset_sha256(complete_keys);
    std::map<std::string, std::size_t> signature_counts;
    for (const auto& signature : signatures) {
        ++signature_counts[signature];
    }
    result.signature_counts.assign(signature_counts.begin(), signature_counts.end());
    if (result.top_partners.size() < 2) {
        result.association = categorical_association(labels, signatures);
        result.association.testable = false;
        result.association.status = JointAssociationStatus::kFewerThanTwoTopMarkers;
        result.association.cramers_v.reset();
        result.association.table.clear();
    } else {
        result.association = categorical_association(labels, signatures);
    }

    ConditionalWork conditional;
    if (result.association.testable) {
        conditional = conditional_permutation(labels, signatures, strata, seed, permutations);
        result.conditional = conditional.result;
    }
    result.conditional_sensitivity_pass = result.conditional.permutable && result.conditional.p_value.has_value() &&
                                          *result.conditional.p_value <= kJointSignatureConditionalPMaximum;

    for (std::size_t marker = 0; marker < result.top_partners.size(); ++marker) {
        std::vector<JointMarkerCall> calls;
        calls.reserve(complete_calls.size());
        for (const auto& read_calls : complete_calls) {
            calls.push_back(read_calls[marker]);
        }
        auto support = marker_support(result.top_partners[marker].position1, labels, calls);
        if (support.effect_gate_pass) {
            result.effect_supported_positions.push_back(support.position1);
        }
        result.marker_support.push_back(std::move(support));
    }

    std::ostringstream trace;
    trace.imbue(std::locale::classic());
    trace << "longlineage.joint_signature.v1\nseed=" << seed << ";permutations=" << permutations
          << ";core=" << result.n_core_reads << ";complete=" << result.n_complete_reads
          << ";complete_readset_sha256=" << result.complete_readset_sha256 << '\n';
    for (const auto& partner : result.top_partners) {
        trace << "partner=" << partner.position1 << ',' << partner.reference << ',' << partner.alternate << ','
              << partner.endpoint_a_n_informative << '\n';
    }
    for (std::size_t index = 0; index < complete_keys.size(); ++index) {
        trace << "read=";
        append_sized(trace, complete_keys[index]);
        trace << ';';
        append_sized(trace, labels[index]);
        trace << ';';
        append_sized(trace, signatures[index]);
        trace << ';';
        append_sized(trace, strata[index]);
        trace << '\n';
    }
    trace << "association=" << to_string(result.association.status) << ';';
    append_optional_double(trace, result.association.cramers_v);
    trace << '\n';
    for (const auto& row : result.association.table) {
        trace << "table=";
        for (const auto count : row) {
            trace << count << ',';
        }
        trace << '\n';
    }
    trace << "conditional=" << to_string(result.conditional.status) << ';' << result.conditional.strata << ';'
          << result.conditional.exchangeable_strata << ';' << result.conditional.informative_exchangeable_strata << ';'
          << result.conditional.exceedance << ';';
    append_optional_double(trace, result.conditional.p_value);
    trace << '\n';
    for (std::size_t index = 0; index < conditional.permutation_statistics.size(); ++index) {
        trace << "perm=" << index << ';' << std::hexfloat << conditional.permutation_statistics[index]
              << std::defaultfloat << '\n';
    }
    for (const auto& support : result.marker_support) {
        trace << "marker=" << support.position1 << ';' << to_string(support.status) << ';';
        append_optional_double(trace, support.cramers_v);
        trace << ';';
        append_optional_double(trace, support.delta_alt_fraction);
        trace << ';' << support.effect_gate_pass << '\n';
    }
    result.semantic_sha256 = sha256_or_throw(trace.str());
    return ParseResult<JointSignatureResult>::success(std::move(result));
}

std::string_view to_string(JointGlobalFdrStatus status) noexcept {
    switch (status) {
        case JointGlobalFdrStatus::kIneligibleM2Screen:
            return "INELIGIBLE_M2_SCREEN";
        case JointGlobalFdrStatus::kEligibleJointSignatureNotTestable:
            return "ELIGIBLE_M2_JOINT_SIGNATURE_NOT_TESTABLE";
        case JointGlobalFdrStatus::kEligibleJointSignatureNotPermutable:
            return "ELIGIBLE_M2_JOINT_SIGNATURE_NOT_PERMUTABLE";
        case JointGlobalFdrStatus::kInvalidPermutationEvidence:
            return "INVALID_JOINT_SIGNATURE_PERMUTATION_EVIDENCE";
        case JointGlobalFdrStatus::kEligibleJointSignatureGlobalFdrFamily:
            return "ELIGIBLE_M2_JOINT_SIGNATURE_GLOBAL_FDR_FAMILY";
    }
    return "INVALID_JOINT_SIGNATURE_PERMUTATION_EVIDENCE";
}

ParseResult<JointGlobalFdrResult> apply_joint_signature_global_fdr(const std::vector<JointGlobalFdrInput>& inputs) {
    std::vector<JointGlobalFdrInput> ordered = inputs;
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.stable_site_key < rhs.stable_site_key; });
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        if (ordered[index].stable_site_key.empty()) {
            return ParseResult<JointGlobalFdrResult>::failure(ParseReason::kMissingRequiredField,
                                                              "global joint-signature row lacks stable_site_key");
        }
        if (index > 0 && ordered[index - 1].stable_site_key == ordered[index].stable_site_key) {
            return ParseResult<JointGlobalFdrResult>::failure(
                ParseReason::kIndexError, "global joint-signature stable_site_key values must be unique");
        }
    }

    JointGlobalFdrResult result;
    result.rows.reserve(ordered.size());
    std::vector<std::size_t> family_indices;
    std::vector<double> family_p;
    for (const auto& input : ordered) {
        JointGlobalFdrRow row;
        row.stable_site_key = input.stable_site_key;
        if (!input.m2_screen_eligible) {
            row.status = JointGlobalFdrStatus::kIneligibleM2Screen;
        } else if (!input.joint_signature_testable) {
            row.status = JointGlobalFdrStatus::kEligibleJointSignatureNotTestable;
        } else if (!input.joint_signature_permutable || input.permutations != kJointSignaturePermutations ||
                   !input.conditional_p.has_value()) {
            row.status = JointGlobalFdrStatus::kEligibleJointSignatureNotPermutable;
        } else if (!valid_grid_p(*input.conditional_p)) {
            row.status = JointGlobalFdrStatus::kInvalidPermutationEvidence;
            result.all_permutation_evidence_valid = false;
        } else {
            row.status = JointGlobalFdrStatus::kEligibleJointSignatureGlobalFdrFamily;
            row.conditional_p = input.conditional_p;
            family_indices.push_back(result.rows.size());
            family_p.push_back(*input.conditional_p);
        }
        result.rows.push_back(std::move(row));
    }
    result.family_size = family_p.size();

    std::vector<std::size_t> order(family_p.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return std::tuple{family_p[lhs], result.rows[family_indices[lhs]].stable_site_key} <
               std::tuple{family_p[rhs], result.rows[family_indices[rhs]].stable_site_key};
    });
    std::vector<double> bh(family_p.size(), 1.0);
    double running = 1.0;
    for (std::size_t reverse = order.size(); reverse-- > 0;) {
        const std::size_t family_index = order[reverse];
        const double rank = static_cast<double>(reverse + 1);
        running = std::min(running, family_p[family_index] * static_cast<double>(family_p.size()) / rank);
        bh[family_index] = std::min(1.0, running);
    }
    double harmonic = 0.0;
    for (std::size_t rank = 1; rank <= family_p.size(); ++rank) {
        harmonic += 1.0 / static_cast<double>(rank);
    }
    for (std::size_t index = 0; index < family_p.size(); ++index) {
        auto& row = result.rows[family_indices[index]];
        row.q_global_bh = bh[index];
        row.q_global_by = std::min(1.0, bh[index] * harmonic);
        row.global_bh_discovery = *row.q_global_bh <= kJointSignatureGlobalQMaximum;
        row.global_by_discovery = *row.q_global_by <= kJointSignatureGlobalQMaximum;
        result.bh_discoveries += static_cast<std::size_t>(row.global_bh_discovery);
        result.by_discoveries += static_cast<std::size_t>(row.global_by_discovery);
    }

    std::ostringstream trace;
    trace.imbue(std::locale::classic());
    trace << "longlineage.joint_signature_global_fdr.v1\nfamily=" << result.family_size
          << ";valid=" << result.all_permutation_evidence_valid << '\n';
    for (const auto& row : result.rows) {
        trace << "row=";
        append_sized(trace, row.stable_site_key);
        trace << ';' << to_string(row.status) << ';';
        append_optional_double(trace, row.conditional_p);
        trace << ';';
        append_optional_double(trace, row.q_global_bh);
        trace << ';';
        append_optional_double(trace, row.q_global_by);
        trace << ';' << row.global_bh_discovery << ';' << row.global_by_discovery << '\n';
    }
    result.semantic_sha256 = sha256_or_throw(trace.str());
    return ParseResult<JointGlobalFdrResult>::success(std::move(result));
}

ParseResult<bool> finalize_joint_signature_topology_gate(JointSignatureResult& result,
                                                         const JointTopologyGateInput& input) {
    try {
        if (result.topology_gate_evaluated) {
            return ParseResult<bool>::failure(ParseReason::kUnsupportedValue,
                                              "joint-signature topology gate cannot be finalized twice");
        }
        if (input.stable_site_key.empty() || input.global_fdr.stable_site_key != input.stable_site_key) {
            return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                              "joint-signature topology gate stable-site identity mismatch");
        }
        if (result.passed || !result.selected_partner_site_orders.empty() || !result.topology_gate_sha256.empty()) {
            return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                              "joint-signature topology gate received pre-populated final fields");
        }
        if (!input.m2_screen_eligible) {
            if (input.global_fdr.status != JointGlobalFdrStatus::kIneligibleM2Screen ||
                input.global_fdr.conditional_p.has_value() || input.global_fdr.q_global_bh.has_value() ||
                input.global_fdr.q_global_by.has_value() || input.global_fdr.global_bh_discovery ||
                input.global_fdr.global_by_discovery) {
                return ParseResult<bool>::failure(
                    ParseReason::kMalformedValue,
                    "M2-ineligible joint-signature site carries incompatible global-FDR evidence");
            }
            result.topology_gate_evaluated = true;
            result.topology_gate_status = JointTopologyGateStatus::kNotEvaluatedM2Ineligible;
            std::ostringstream trace;
            trace.imbue(std::locale::classic());
            trace << "longlineage.joint_signature_topology_gate.v1\nsite=";
            append_sized(trace, input.stable_site_key);
            trace << "\npre_global=" << result.semantic_sha256
                  << "\ncomplete_readset=" << result.complete_readset_sha256
                  << "\nm2=0\nglobal_status=" << to_string(input.global_fdr.status)
                  << "\nstatus=" << to_string(result.topology_gate_status) << ";passed=0\nselected_orders=\n";
            result.topology_gate_sha256 = sha256_or_throw(trace.str());
            return ParseResult<bool>::success(true);
        }
        if (!is_lower_hex_sha256(result.semantic_sha256) || !is_lower_hex_sha256(result.complete_readset_sha256)) {
            return ParseResult<bool>::failure(
                ParseReason::kMalformedValue,
                "joint-signature topology gate requires frozen pre-global and complete-readset digests");
        }

        std::map<std::uint64_t, const JointPartnerFormalEvidence*> evidence_by_position;
        std::set<std::uint64_t> site_orders;
        for (const auto& evidence : input.partner_evidence) {
            if (evidence.position1 == 0 || !evidence_by_position.emplace(evidence.position1, &evidence).second ||
                !site_orders.insert(evidence.site_order).second) {
                return ParseResult<bool>::failure(
                    ParseReason::kIndexError,
                    "joint-signature topology pair evidence has a zero/duplicate position or duplicate site order");
            }
        }
        for (const auto& partner : result.top_partners) {
            if (evidence_by_position.find(partner.position1) == evidence_by_position.end()) {
                return ParseResult<bool>::failure(
                    ParseReason::kMissingRequiredField,
                    "joint-signature selected partner is absent from formal pair evidence");
            }
        }

        JointGlobalFdrStatus expected_global_status;
        if (!input.m2_screen_eligible) {
            expected_global_status = JointGlobalFdrStatus::kIneligibleM2Screen;
        } else if (!result.association.testable) {
            expected_global_status = JointGlobalFdrStatus::kEligibleJointSignatureNotTestable;
        } else if (!result.conditional.permutable || result.conditional.permutations != kJointSignaturePermutations ||
                   !result.conditional.p_value.has_value()) {
            expected_global_status = JointGlobalFdrStatus::kEligibleJointSignatureNotPermutable;
        } else {
            expected_global_status = JointGlobalFdrStatus::kEligibleJointSignatureGlobalFdrFamily;
        }
        if (input.global_fdr.status != expected_global_status) {
            return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                              "joint-signature topology gate/global-FDR status mismatch");
        }
        const bool in_global_family =
            expected_global_status == JointGlobalFdrStatus::kEligibleJointSignatureGlobalFdrFamily;
        if (in_global_family) {
            if (!input.global_fdr.conditional_p.has_value() || !result.conditional.p_value.has_value() ||
                *input.global_fdr.conditional_p != *result.conditional.p_value ||
                !input.global_fdr.q_global_bh.has_value() || !input.global_fdr.q_global_by.has_value() ||
                !valid_probability(*input.global_fdr.q_global_bh) ||
                !valid_probability(*input.global_fdr.q_global_by) ||
                input.global_fdr.global_bh_discovery !=
                    (*input.global_fdr.q_global_bh <= kJointSignatureGlobalQMaximum) ||
                input.global_fdr.global_by_discovery !=
                    (*input.global_fdr.q_global_by <= kJointSignatureGlobalQMaximum)) {
                return ParseResult<bool>::failure(
                    ParseReason::kMalformedValue,
                    "joint-signature topology gate received inconsistent global-FDR evidence");
            }
        } else if (input.global_fdr.conditional_p.has_value() || input.global_fdr.q_global_bh.has_value() ||
                   input.global_fdr.q_global_by.has_value() || input.global_fdr.global_bh_discovery ||
                   input.global_fdr.global_by_discovery) {
            return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                              "joint-signature non-family row carries global-FDR evidence");
        }

        std::vector<std::uint64_t> confirmed_positions;
        for (const auto& evidence : input.partner_evidence) {
            if (evidence.formal_pair_by_confirmed) {
                confirmed_positions.push_back(evidence.position1);
            }
        }
        const std::set<std::uint64_t> confirmed_set(confirmed_positions.begin(), confirmed_positions.end());
        const std::set<std::uint64_t> effect_supported_set(result.effect_supported_positions.begin(),
                                                           result.effect_supported_positions.end());
        std::vector<std::uint64_t> top_confirmed_positions;
        std::vector<std::uint64_t> same_complete_supported_positions;
        for (const auto& partner : result.top_partners) {
            if (confirmed_set.count(partner.position1) == 0) {
                continue;
            }
            top_confirmed_positions.push_back(partner.position1);
            if (effect_supported_set.count(partner.position1) != 0) {
                same_complete_supported_positions.push_back(partner.position1);
            }
        }
        const auto spaced_confirmed = spatially_separated_positions(confirmed_positions);
        const auto spaced_top_confirmed = spatially_separated_positions(top_confirmed_positions);
        const auto spaced_same_complete = spatially_separated_positions(same_complete_supported_positions);
        const std::set<std::uint64_t> selected_positions(spaced_same_complete.begin(), spaced_same_complete.end());

        result.postselection_fdr_calibrated = in_global_family;
        result.topology_gate_evaluated = true;
        if (!input.m2_screen_eligible) {
            result.topology_gate_status = JointTopologyGateStatus::kNotEvaluatedM2Ineligible;
        } else if (!result.association.testable) {
            result.topology_gate_status = JointTopologyGateStatus::kFailJointSignatureNotTestable;
        } else if (!result.conditional.permutable) {
            result.topology_gate_status = JointTopologyGateStatus::kFailJointSignatureNotPermutable;
        } else if (!input.global_fdr.global_by_discovery || !result.conditional_sensitivity_pass) {
            result.topology_gate_status = JointTopologyGateStatus::kFailGlobalBy;
        } else if (spaced_confirmed.size() < 2) {
            result.topology_gate_status = JointTopologyGateStatus::kFailInsufficientConfirmedPairs;
        } else if (spaced_top_confirmed.size() < 2) {
            result.topology_gate_status = JointTopologyGateStatus::kFailInsufficientTopConfirmedPairs;
        } else if (spaced_same_complete.size() < 2) {
            result.topology_gate_status = JointTopologyGateStatus::kFailInsufficientSameCompleteReadSupport;
        } else {
            result.topology_gate_status = JointTopologyGateStatus::kPass;
            result.passed = true;
            for (const auto& partner : result.top_partners) {
                if (selected_positions.count(partner.position1) != 0) {
                    result.selected_partner_site_orders.push_back(
                        evidence_by_position.at(partner.position1)->site_order);
                }
            }
        }

        std::ostringstream trace;
        trace.imbue(std::locale::classic());
        trace << "longlineage.joint_signature_topology_gate.v1\nsite=";
        append_sized(trace, input.stable_site_key);
        trace << "\npre_global=" << result.semantic_sha256 << "\ncomplete_readset=" << result.complete_readset_sha256
              << "\nm2=" << input.m2_screen_eligible << "\nglobal_status=" << to_string(input.global_fdr.status) << ';';
        append_optional_double(trace, input.global_fdr.conditional_p);
        trace << ';';
        append_optional_double(trace, input.global_fdr.q_global_bh);
        trace << ';';
        append_optional_double(trace, input.global_fdr.q_global_by);
        trace << ';' << input.global_fdr.global_bh_discovery << ';' << input.global_fdr.global_by_discovery << '\n';
        for (const auto& [position, evidence] : evidence_by_position) {
            trace << "pair=" << position << ';' << evidence->site_order << ';' << evidence->formal_pair_by_confirmed
                  << '\n';
        }
        trace << "spaced_confirmed=";
        for (const auto position : spaced_confirmed) {
            trace << position << ',';
        }
        trace << "\nspaced_top_confirmed=";
        for (const auto position : spaced_top_confirmed) {
            trace << position << ',';
        }
        trace << "\nspaced_same_complete=";
        for (const auto position : spaced_same_complete) {
            trace << position << ',';
        }
        trace << "\nstatus=" << to_string(result.topology_gate_status) << ";passed=" << result.passed
              << "\nselected_orders=";
        for (const auto order : result.selected_partner_site_orders) {
            trace << order << ',';
        }
        trace << '\n';
        result.topology_gate_sha256 = sha256_or_throw(trace.str());
        return ParseResult<bool>::success(true);
    } catch (const std::exception& error) {
        return ParseResult<bool>::failure(
            ParseReason::kMalformedValue,
            std::string("joint-signature topology gate finalization failed: ") + error.what());
    }
}

}  // namespace longlineage::cooccurrence
