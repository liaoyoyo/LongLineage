// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/cooccurrence/statistics.hpp"

#include <algorithm>
#include <boost/math/distributions/beta.hpp>
#include <boost/math/distributions/chi_squared.hpp>
#include <boost/math/special_functions/beta.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace longlineage::cooccurrence {
namespace {

[[nodiscard]] double log_choose(std::uint64_t n, std::uint64_t k) {
    if (k > n) {
        return -std::numeric_limits<double>::infinity();
    }
    return std::lgamma(static_cast<double>(n) + 1.0) - std::lgamma(static_cast<double>(k) + 1.0) -
           std::lgamma(static_cast<double>(n - k) + 1.0);
}

[[nodiscard]] double log_add_exp(double lhs, double rhs) noexcept {
    if (std::isinf(lhs) && lhs < 0.0) {
        return rhs;
    }
    if (std::isinf(rhs) && rhs < 0.0) {
        return lhs;
    }
    const double maximum = std::max(lhs, rhs);
    const double minimum = std::min(lhs, rhs);
    return maximum + std::log1p(std::exp(minimum - maximum));
}

[[nodiscard]] std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs, std::string_view field) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw std::overflow_error(std::string(field) + " overflows uint64");
    }
    return lhs + rhs;
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

}  // namespace

std::string_view to_string(ExactStateStatus status) noexcept {
    switch (status) {
        case ExactStateStatus::kExactEnumerated:
            return "EXACT_ENUMERATED";
        case ExactStateStatus::kNotIdentifiableStateSpaceLimit:
            return "NOT_IDENTIFIABLE_STATE_SPACE_LIMIT";
        case ExactStateStatus::kNotIdentifiableDegenerateTable:
            return "NOT_IDENTIFIABLE_DEGENERATE_TABLE";
    }
    return "NOT_IDENTIFIABLE_DEGENERATE_TABLE";
}

ExactKx2Result fisher_freeman_halton_kx2(const std::vector<Kx2Row>& table, std::uint64_t state_space_ceiling) {
    if (state_space_ceiling == 0) {
        throw std::invalid_argument("state_space_ceiling must be positive");
    }
    ExactKx2Result result;
    result.state_space_ceiling = state_space_ceiling;

    std::vector<Kx2Row> observed;
    observed.reserve(table.size());
    std::uint64_t ref_total = 0;
    std::uint64_t alt_total = 0;
    for (const auto& row : table) {
        const std::uint64_t row_total = checked_add(row[0], row[1], "Kx2 row total");
        if (row_total == 0) {
            continue;
        }
        observed.push_back(row);
        ref_total = checked_add(ref_total, row[0], "Kx2 REF total");
        alt_total = checked_add(alt_total, row[1], "Kx2 ALT total");
    }
    if (observed.size() < 2 || ref_total == 0 || alt_total == 0) {
        return result;
    }

    std::vector<std::uint64_t> row_totals;
    std::vector<std::uint64_t> observed_alt;
    std::vector<std::vector<double>> log_choose_by_row;
    row_totals.reserve(observed.size());
    observed_alt.reserve(observed.size());
    log_choose_by_row.reserve(observed.size());
    double observed_log_weight = 0.0;
    double compensation = 0.0;
    for (const auto& row : observed) {
        const std::uint64_t row_total = row[0] + row[1];
        row_totals.push_back(row_total);
        observed_alt.push_back(row[1]);
        if (row_total > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() - 1U)) {
            throw std::length_error("Kx2 row is too large for exact-state storage");
        }
        std::vector<double> values(static_cast<std::size_t>(row_total + 1));
        for (std::uint64_t alt = 0; alt <= row_total; ++alt) {
            values[static_cast<std::size_t>(alt)] = log_choose(row_total, alt);
        }
        const double value = values[static_cast<std::size_t>(row[1])];
        const double adjusted = value - compensation;
        const double next = observed_log_weight + adjusted;
        compensation = (next - observed_log_weight) - adjusted;
        observed_log_weight = next;
        log_choose_by_row.push_back(std::move(values));
    }

    std::vector<std::uint64_t> suffix_capacity(row_totals.size() + 1, 0);
    for (std::size_t index = row_totals.size(); index-- > 0;) {
        suffix_capacity[index] = checked_add(suffix_capacity[index + 1], row_totals[index], "Kx2 suffix capacity");
    }

    std::uint64_t state_count = 0;
    double total_log_weight = -std::numeric_limits<double>::infinity();
    double tail_log_weight = -std::numeric_limits<double>::infinity();
    bool ceiling_exceeded = false;

    const auto visit = [&](double log_weight) {
        ++state_count;
        if (state_count > state_space_ceiling) {
            ceiling_exceeded = true;
            return;
        }
        total_log_weight = log_add_exp(total_log_weight, log_weight);
        if (log_weight <= observed_log_weight + 1e-12) {
            tail_log_weight = log_add_exp(tail_log_weight, log_weight);
        }
    };

    std::function<void(std::size_t, std::uint64_t, double)> enumerate;
    enumerate = [&](std::size_t index, std::uint64_t remaining_alt, double log_weight) {
        if (ceiling_exceeded) {
            return;
        }
        const std::uint64_t row_total = row_totals[index];
        if (index + 1 == row_totals.size()) {
            if (remaining_alt <= row_total) {
                visit(log_weight + log_choose_by_row[index][static_cast<std::size_t>(remaining_alt)]);
            }
            return;
        }
        const std::uint64_t remaining_capacity = suffix_capacity[index + 1];
        const std::uint64_t lower = remaining_alt > remaining_capacity ? remaining_alt - remaining_capacity : 0;
        const std::uint64_t upper = std::min(row_total, remaining_alt);
        for (std::uint64_t alt = lower; alt <= upper; ++alt) {
            enumerate(index + 1, remaining_alt - alt,
                      log_weight + log_choose_by_row[index][static_cast<std::size_t>(alt)]);
            if (ceiling_exceeded || alt == std::numeric_limits<std::uint64_t>::max()) {
                break;
            }
        }
    };
    enumerate(0, alt_total, 0.0);

    if (ceiling_exceeded) {
        result.status = ExactStateStatus::kNotIdentifiableStateSpaceLimit;
        result.state_space_lower_bound = state_space_ceiling + 1;
        return result;
    }
    if (state_count == 0 || !std::isfinite(total_log_weight)) {
        return result;
    }
    result.status = ExactStateStatus::kExactEnumerated;
    result.identifiable = true;
    result.state_space_size = state_count;
    result.p_value = std::min(1.0, std::exp(tail_log_weight - total_log_weight));
    result.observed_table_probability = std::min(1.0, std::exp(observed_log_weight - total_log_weight));
    return result;
}

std::vector<std::optional<double>> benjamini_hochberg(const std::vector<std::optional<double>>& p_values) {
    std::vector<std::pair<std::size_t, double>> valid;
    valid.reserve(p_values.size());
    for (std::size_t index = 0; index < p_values.size(); ++index) {
        if (!p_values[index].has_value() || !std::isfinite(*p_values[index])) {
            continue;
        }
        if (*p_values[index] < 0.0 || *p_values[index] > 1.0) {
            throw std::invalid_argument("FDR p-values must be in [0,1]");
        }
        valid.emplace_back(index, *p_values[index]);
    }
    std::stable_sort(valid.begin(), valid.end(),
                     [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    std::vector<std::optional<double>> adjusted(p_values.size());
    double running = 1.0;
    for (std::size_t reverse = valid.size(); reverse-- > 0;) {
        const double rank = static_cast<double>(reverse + 1);
        running = std::min(running, valid[reverse].second * static_cast<double>(valid.size()) / rank);
        adjusted[valid[reverse].first] = std::min(1.0, running);
    }
    return adjusted;
}

std::vector<std::optional<double>> benjamini_yekutieli(const std::vector<std::optional<double>>& p_values) {
    auto adjusted = benjamini_hochberg(p_values);
    const std::size_t valid = static_cast<std::size_t>(
        std::count_if(adjusted.begin(), adjusted.end(), [](const auto& value) { return value.has_value(); }));
    if (valid == 0) {
        return adjusted;
    }
    double harmonic = 0.0;
    for (std::size_t rank = 1; rank <= valid; ++rank) {
        harmonic += 1.0 / static_cast<double>(rank);
    }
    for (auto& value : adjusted) {
        if (value.has_value()) {
            value = std::min(1.0, *value * harmonic);
        }
    }
    return adjusted;
}

std::size_t JointAlleleCounts::index(AlleleCall focal, AlleleCall partner) noexcept {
    return allele_index(focal) * 4 + allele_index(partner);
}

void JointAlleleCounts::add(AlleleCall focal, AlleleCall partner) {
    auto& cell = cells_[index(focal, partner)];
    if (cell == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("joint allele cell count overflows uint64");
    }
    ++cell;
}

std::uint64_t JointAlleleCounts::at(AlleleCall focal, AlleleCall partner) const noexcept {
    return cells_[index(focal, partner)];
}

std::uint64_t JointAlleleCounts::total() const noexcept {
    return std::accumulate(cells_.begin(), cells_.end(), std::uint64_t{0});
}

bool JointAlleleCounts::conservation_holds(std::uint64_t expected_pairs) const noexcept {
    return total() == expected_pairs;
}

std::string_view to_string(CallabilityStatus status) noexcept {
    switch (status) {
        case CallabilityStatus::kPassAllCoreReadsCallable:
            return "PASS_ALL_CORE_READS_CALLABLE";
        case CallabilityStatus::kPassNoStrongDifferentialCallability:
            return "PASS_NO_STRONG_DIFFERENTIAL_CALLABILITY_DETECTED";
        case CallabilityStatus::kFailDifferentialCallabilitySignal:
            return "FAIL_DIFFERENTIAL_CALLABILITY_SIGNAL";
        case CallabilityStatus::kNotIdentifiableDifferentialCallability:
            return "NOT_IDENTIFIABLE_DIFFERENTIAL_CALLABILITY";
        case CallabilityStatus::kNotIdentifiableMissingCallabilityStatistic:
            return "NOT_IDENTIFIABLE_MISSING_CALLABILITY_STATISTIC";
    }
    return "NOT_IDENTIFIABLE_DIFFERENTIAL_CALLABILITY";
}

CallabilityStatus evaluate_callability(const CallabilityInput& input) {
    if (input.noncallable_core_reads == 0) {
        return CallabilityStatus::kPassAllCoreReadsCallable;
    }
    if (!input.exact_testable) {
        return CallabilityStatus::kNotIdentifiableDifferentialCallability;
    }
    if (!input.by_q.has_value() || !input.cramers_v.has_value() || !std::isfinite(*input.by_q) ||
        !std::isfinite(*input.cramers_v)) {
        return CallabilityStatus::kNotIdentifiableMissingCallabilityStatistic;
    }
    if (*input.by_q < 0.0 || *input.by_q > 1.0 || *input.cramers_v < 0.0 || *input.cramers_v > 1.0) {
        throw std::invalid_argument("callability q and Cramer's V must be in [0,1]");
    }
    if (*input.by_q <= 0.05 || *input.cramers_v >= 0.30) {
        return CallabilityStatus::kFailDifferentialCallabilitySignal;
    }
    return CallabilityStatus::kPassNoStrongDifferentialCallability;
}

GroupAlleleAssociation summarize_group_allele_association(const std::vector<std::string>& labels,
                                                          const std::vector<AlleleCall>& partner_calls,
                                                          std::uint64_t minimum_total, std::uint64_t minimum_group,
                                                          std::uint64_t minimum_allele) {
    if (labels.size() != partner_calls.size()) {
        throw std::invalid_argument("group labels and partner calls have different cardinality");
    }
    if (minimum_total == 0 || minimum_group == 0 || minimum_allele == 0) {
        throw std::invalid_argument("group-allele testability thresholds must be positive");
    }

    std::map<std::string, Kx2Row> by_group;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (labels[index].empty()) {
            throw std::invalid_argument("group label must be non-empty");
        }
        if (partner_calls[index] != AlleleCall::kReference && partner_calls[index] != AlleleCall::kAlternate) {
            continue;
        }
        Kx2Row& row = by_group[labels[index]];
        std::uint64_t& cell = row[partner_calls[index] == AlleleCall::kReference ? 0U : 1U];
        if (cell == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("group-allele cell overflows uint64");
        }
        ++cell;
    }

    GroupAlleleAssociation output;
    output.groups.reserve(by_group.size());
    output.table.reserve(by_group.size());
    output.minimum_group_n = std::numeric_limits<std::uint64_t>::max();
    std::vector<double> alt_fractions;
    alt_fractions.reserve(by_group.size());
    for (const auto& [group, row] : by_group) {
        const std::uint64_t row_total = checked_add(row[0], row[1], "group-allele row total");
        output.groups.push_back(group);
        output.table.push_back(row);
        output.n_informative = checked_add(output.n_informative, row_total, "group-allele informative total");
        output.ref_n = checked_add(output.ref_n, row[0], "group-allele REF total");
        output.alt_n = checked_add(output.alt_n, row[1], "group-allele ALT total");
        output.minimum_group_n = std::min(output.minimum_group_n, row_total);
        if (row_total > 0) {
            alt_fractions.push_back(static_cast<double>(row[1]) / static_cast<double>(row_total));
        }
    }
    if (output.table.empty()) {
        output.minimum_group_n = 0;
    }
    if (output.groups.size() < 2 || output.n_informative < minimum_total) {
        return output;
    }
    if (output.minimum_group_n < minimum_group) {
        output.reason = "METHYL_GROUP_BELOW_MINIMUM";
        return output;
    }
    if (std::min(output.ref_n, output.alt_n) < minimum_allele) {
        output.reason = "PARTNER_ALLELE_BELOW_MINIMUM";
        return output;
    }

    const double total = static_cast<double>(output.n_informative);
    const std::array<double, 2> column_totals{static_cast<double>(output.ref_n), static_cast<double>(output.alt_n)};
    double chi_square = 0.0;
    for (const Kx2Row& row : output.table) {
        const double row_total = static_cast<double>(row[0] + row[1]);
        for (std::size_t column = 0; column < 2; ++column) {
            const double expected = row_total * column_totals[column] / total;
            if (expected > 0.0) {
                const double delta = static_cast<double>(row[column]) - expected;
                chi_square += delta * delta / expected;
            }
        }
    }
    output.testable = true;
    output.reason = "OK";
    output.cramers_v = std::sqrt(chi_square / total);
    const auto [minimum, maximum] = std::minmax_element(alt_fractions.begin(), alt_fractions.end());
    output.delta_alt_fraction = *maximum - *minimum;
    return output;
}

BinaryCategoryAssociation summarize_binary_category_association(const std::vector<std::string>& labels,
                                                                const std::vector<bool>& second_category,
                                                                std::uint64_t minimum_total,
                                                                std::uint64_t minimum_group,
                                                                std::uint64_t minimum_category) {
    if (labels.size() != second_category.size()) {
        throw std::invalid_argument("binary category labels/categories length mismatch");
    }
    std::vector<AlleleCall> categories;
    categories.reserve(second_category.size());
    for (const bool second : second_category) {
        categories.push_back(second ? AlleleCall::kAlternate : AlleleCall::kReference);
    }

    BinaryCategoryAssociation output;
    output.summary =
        summarize_group_allele_association(labels, categories, minimum_total, minimum_group, minimum_category);
    if (!output.summary.testable) {
        return output;
    }
    if (output.summary.table.size() == 2) {
        const ExactKx2Result exact = fisher_freeman_halton_kx2(output.summary.table);
        if (!exact.identifiable || !exact.p_value.has_value()) {
            throw std::runtime_error("testable 2x2 callability table was not exact-identifiable");
        }
        output.p_analytic = exact.p_value;
        output.analytic_method = "FISHER_EXACT_2X2";
        return output;
    }

    const double total = static_cast<double>(output.summary.n_informative);
    const double reference_total = static_cast<double>(output.summary.ref_n);
    const double alternate_total = static_cast<double>(output.summary.alt_n);
    double statistic = 0.0;
    for (const Kx2Row& row : output.summary.table) {
        const double row_total = static_cast<double>(row[0] + row[1]);
        const double expected_reference = row_total * reference_total / total;
        const double expected_alternate = row_total * alternate_total / total;
        const double reference_delta = static_cast<double>(row[0]) - expected_reference;
        const double alternate_delta = static_cast<double>(row[1]) - expected_alternate;
        statistic += reference_delta * reference_delta / expected_reference;
        statistic += alternate_delta * alternate_delta / expected_alternate;
    }
    const boost::math::chi_squared distribution(static_cast<double>(output.summary.table.size() - 1));
    output.p_analytic = boost::math::cdf(boost::math::complement(distribution, statistic));
    output.analytic_method = "PEARSON_CHI_SQUARE_UNCORRECTED";
    return output;
}

std::string_view to_string(FixedErrorGateStatus status) noexcept {
    switch (status) {
        case FixedErrorGateStatus::kCompatibleWithFixedErrorCeiling:
            return "COMPATIBLE_WITH_FIXED_ERROR_CEILING";
        case FixedErrorGateStatus::kViolatesFixedErrorCeiling:
            return "VIOLATES_FIXED_ERROR_CEILING";
        case FixedErrorGateStatus::kNotIdentifiableLowPrecision:
            return "NOT_IDENTIFIABLE_LOW_PRECISION";
        case FixedErrorGateStatus::kNotIdentifiableNoDenominator:
            return "NOT_IDENTIFIABLE_NO_DENOMINATOR";
    }
    return "NOT_IDENTIFIABLE_NO_DENOMINATOR";
}

std::string_view to_string(RelationModel model) noexcept {
    switch (model) {
        case RelationModel::kFocalAncestor:
            return "FOCAL_ANCESTOR";
        case RelationModel::kPartnerAncestor:
            return "PARTNER_ANCESTOR";
        case RelationModel::kBranching:
            return "BRANCHING";
    }
    return "BRANCHING";
}

std::string_view to_string(RelationCompatibilityStatus status) noexcept {
    switch (status) {
        case RelationCompatibilityStatus::kFocalAncestorCompatible:
            return "FOCAL_ANCESTOR_COMPATIBLE_UNDER_FIXED_ERROR_MODEL";
        case RelationCompatibilityStatus::kPartnerAncestorCompatible:
            return "PARTNER_ANCESTOR_COMPATIBLE_UNDER_FIXED_ERROR_MODEL";
        case RelationCompatibilityStatus::kBranchingCompatible:
            return "BRANCHING_COMPATIBLE_UNDER_FIXED_ERROR_MODEL";
        case RelationCompatibilityStatus::kMultipleModelsCompatible:
            return "MULTIPLE_MUTATION_ORDER_MODELS_COMPATIBLE_UNDER_FIXED_ERROR_MODEL";
        case RelationCompatibilityStatus::kIncompatibleOrComplex:
            return "INCOMPATIBLE_OR_COMPLEX_UNDER_FIXED_ERROR_MODEL";
        case RelationCompatibilityStatus::kNotIdentifiableInsufficientFourStateDepth:
            return "NOT_IDENTIFIABLE_INSUFFICIENT_FOUR_STATE_DEPTH";
        case RelationCompatibilityStatus::kNotIdentifiableFixedErrorCeiling:
            return "NOT_IDENTIFIABLE_FIXED_ERROR_CEILING";
    }
    return "NOT_IDENTIFIABLE_FIXED_ERROR_CEILING";
}

namespace {

[[nodiscard]] FixedErrorGateResult fixed_error_gate(std::uint64_t violations, std::uint64_t denominator,
                                                    double error_ceiling, double confidence) {
    FixedErrorGateResult output;
    output.violations = violations;
    output.denominator = denominator;
    output.threshold = error_ceiling;
    output.confidence = confidence;
    if (violations > denominator) {
        throw std::invalid_argument("fixed-error violations exceed their denominator");
    }
    if (denominator == 0) {
        return output;
    }
    output.rate = static_cast<double>(violations) / static_cast<double>(denominator);
    if (violations == 0) {
        output.p_exact_greater = 1.0;
    } else {
        // P[X >= violations], X ~ Binomial(denominator, error_ceiling).
        output.p_exact_greater = boost::math::ibeta(static_cast<double>(violations),
                                                    static_cast<double>(denominator - violations + 1), error_ceiling);
    }
    if (violations == denominator) {
        output.upper_bound = 1.0;
    } else {
        const boost::math::beta_distribution<double> distribution(static_cast<double>(violations + 1),
                                                                  static_cast<double>(denominator - violations));
        output.upper_bound = boost::math::quantile(distribution, confidence);
    }
    const double alpha = 1.0 - confidence;
    if (*output.upper_bound <= error_ceiling + 1e-15) {
        output.status = FixedErrorGateStatus::kCompatibleWithFixedErrorCeiling;
    } else if (*output.p_exact_greater <= alpha + 1e-15) {
        output.status = FixedErrorGateStatus::kViolatesFixedErrorCeiling;
    } else {
        output.status = FixedErrorGateStatus::kNotIdentifiableLowPrecision;
    }
    return output;
}

}  // namespace

FourStateSummary summarize_four_state_relations(const JointAlleleCounts& counts, double error_ceiling,
                                                double familywise_confidence) {
    if (!(error_ceiling > 0.0 && error_ceiling < 1.0) ||
        !(familywise_confidence > 0.0 && familywise_confidence < 1.0)) {
        throw std::invalid_argument("fixed-error ceiling and confidence must both be in (0,1)");
    }
    const std::uint64_t rr = counts.at(AlleleCall::kReference, AlleleCall::kReference);
    const std::uint64_t ra = counts.at(AlleleCall::kReference, AlleleCall::kAlternate);
    const std::uint64_t ar = counts.at(AlleleCall::kAlternate, AlleleCall::kReference);
    const std::uint64_t aa = counts.at(AlleleCall::kAlternate, AlleleCall::kAlternate);

    FourStateSummary output;
    output.counts = counts;
    output.error_ceiling = error_ceiling;
    output.familywise_confidence = familywise_confidence;
    output.per_relation_confidence = 1.0 - (1.0 - familywise_confidence) / 3.0;
    output.called_ra_depth = checked_add(checked_add(rr, ra, "four-state depth"),
                                         checked_add(ar, aa, "four-state depth"), "four-state depth");
    output.focal_ref = checked_add(rr, ra, "four-state focal REF");
    output.focal_alt = checked_add(ar, aa, "four-state focal ALT");
    output.partner_ref = checked_add(rr, ar, "four-state partner REF");
    output.partner_alt = checked_add(ra, aa, "four-state partner ALT");
    output.focal_ancestor = fixed_error_gate(ra, output.partner_alt, error_ceiling, output.per_relation_confidence);
    output.partner_ancestor = fixed_error_gate(ar, output.focal_alt, error_ceiling, output.per_relation_confidence);
    output.branching =
        fixed_error_gate(aa, checked_add(checked_add(ar, ra, "branching denominator"), aa, "branching denominator"),
                         error_ceiling, output.per_relation_confidence);
    output.minimum_zero_violation_depth = static_cast<std::uint64_t>(
        std::ceil(std::log(1.0 - output.per_relation_confidence) / std::log(1.0 - error_ceiling)));
    output.complete_four_state_testable =
        output.called_ra_depth >= 10 &&
        std::min(std::min(output.focal_ref, output.focal_alt), std::min(output.partner_ref, output.partner_alt)) >= 3;

    const bool focal_split = ar >= 3 && aa >= 3;
    const bool partner_split = ra >= 3 && aa >= 3;
    const bool branching_split = ar >= 3 && ra >= 3;
    if (output.complete_four_state_testable && focal_split &&
        output.focal_ancestor.status == FixedErrorGateStatus::kCompatibleWithFixedErrorCeiling) {
        output.compatible_models.push_back(RelationModel::kFocalAncestor);
    }
    if (output.complete_four_state_testable && partner_split &&
        output.partner_ancestor.status == FixedErrorGateStatus::kCompatibleWithFixedErrorCeiling) {
        output.compatible_models.push_back(RelationModel::kPartnerAncestor);
    }
    if (output.complete_four_state_testable && branching_split &&
        output.branching.status == FixedErrorGateStatus::kCompatibleWithFixedErrorCeiling) {
        output.compatible_models.push_back(RelationModel::kBranching);
    }

    if (!output.complete_four_state_testable) {
        output.compatibility = RelationCompatibilityStatus::kNotIdentifiableInsufficientFourStateDepth;
    } else if (output.compatible_models.size() > 1) {
        output.compatibility = RelationCompatibilityStatus::kMultipleModelsCompatible;
    } else if (output.compatible_models == std::vector<RelationModel>{RelationModel::kFocalAncestor}) {
        output.compatibility = RelationCompatibilityStatus::kFocalAncestorCompatible;
    } else if (output.compatible_models == std::vector<RelationModel>{RelationModel::kPartnerAncestor}) {
        output.compatibility = RelationCompatibilityStatus::kPartnerAncestorCompatible;
    } else if (output.compatible_models == std::vector<RelationModel>{RelationModel::kBranching}) {
        output.compatibility = RelationCompatibilityStatus::kBranchingCompatible;
    } else {
        std::vector<FixedErrorGateStatus> relevant;
        if (focal_split) {
            relevant.push_back(output.focal_ancestor.status);
        }
        if (partner_split) {
            relevant.push_back(output.partner_ancestor.status);
        }
        if (branching_split) {
            relevant.push_back(output.branching.status);
        }
        const bool any_not_identifiable =
            std::any_of(relevant.begin(), relevant.end(), [](FixedErrorGateStatus status) {
                return status == FixedErrorGateStatus::kNotIdentifiableLowPrecision ||
                       status == FixedErrorGateStatus::kNotIdentifiableNoDenominator;
            });
        output.compatibility = relevant.empty() || any_not_identifiable
                                   ? RelationCompatibilityStatus::kNotIdentifiableFixedErrorCeiling
                                   : RelationCompatibilityStatus::kIncompatibleOrComplex;
    }
    return output;
}

}  // namespace longlineage::cooccurrence
