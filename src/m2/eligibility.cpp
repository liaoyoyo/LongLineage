// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/m2/eligibility.hpp"

#include <algorithm>
#include <array>
#include <boost/math/distributions/chi_squared.hpp>
#include <boost/math/distributions/fisher_f.hpp>
#include <boost/math/distributions/non_central_chi_squared.hpp>
#include <boost/math/distributions/non_central_f.hpp>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"
#include "longlineage/m1/science.hpp"

namespace longlineage::m2 {
namespace {

void validate_power_inputs(std::size_t n, std::size_t groups, double effect_threshold) {
    if (groups < 2 || groups > kMaximumGroups) {
        throw std::invalid_argument("M2 power requires 2-10 groups");
    }
    if (n == 0 || !std::isfinite(effect_threshold) || effect_threshold <= 0.0 || effect_threshold >= 1.0) {
        throw std::invalid_argument("M2 power input is outside the frozen domain");
    }
}

[[nodiscard]] std::string sha256_or_throw(std::string_view bytes) {
    auto digest = sha256_hex(bytes);
    if (!digest.ok() || !digest.value.has_value()) {
        throw std::runtime_error("SHA-256 failed while serializing M2 scientific state: " + digest.detail);
    }
    return *digest.value;
}

using CountTable = std::vector<std::vector<std::size_t>>;

[[nodiscard]] double cramer_v(const CountTable& table) {
    if (table.size() < 2 || table.front().size() < 2) {
        return 0.0;
    }
    const std::size_t columns = table.front().size();
    std::vector<std::size_t> row_totals(table.size(), 0);
    std::vector<std::size_t> column_totals(columns, 0);
    std::size_t total = 0;
    for (std::size_t row = 0; row < table.size(); ++row) {
        if (table[row].size() != columns) {
            throw std::invalid_argument("Cramer's V table must be rectangular");
        }
        for (std::size_t column = 0; column < columns; ++column) {
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

[[nodiscard]] double eta_squared_impl(const std::vector<double>& values, const std::vector<std::string>& labels) {
    if (values.size() != labels.size()) {
        throw std::invalid_argument("eta-squared values and labels must have equal length");
    }
    std::vector<std::size_t> finite;
    finite.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (std::isfinite(values[index])) {
            finite.push_back(index);
        }
    }
    std::set<std::string> groups;
    for (const std::size_t index : finite) {
        groups.insert(labels[index]);
    }
    if (finite.size() < 2 || groups.size() < 2) {
        return 0.0;
    }
    double sum = 0.0;
    for (const std::size_t index : finite) {
        sum += values[index];
    }
    const double grand_mean = sum / static_cast<double>(finite.size());
    double total = 0.0;
    for (const std::size_t index : finite) {
        const double delta = values[index] - grand_mean;
        total += delta * delta;
    }
    if (total <= 1e-12) {
        return 0.0;
    }
    double between = 0.0;
    for (const auto& group : groups) {
        double group_sum = 0.0;
        std::size_t group_size = 0;
        for (const std::size_t index : finite) {
            if (labels[index] == group) {
                group_sum += values[index];
                ++group_size;
            }
        }
        const double group_mean = group_sum / static_cast<double>(group_size);
        const double delta = group_mean - grand_mean;
        between += static_cast<double>(group_size) * delta * delta;
    }
    return between / total;
}

void append_trace_value(std::ostringstream& stream, double value) {
    stream << std::hexfloat << value << std::defaultfloat;
}

[[nodiscard]] bool is_missing_category(std::string_view value) noexcept {
    return value.empty() || value == "." || value == "NA";
}

}  // namespace

double categorical_power(std::size_t n, std::size_t groups, std::size_t planning_levels, double effect_threshold) {
    validate_power_inputs(n, groups, effect_threshold);
    if (planning_levels < 2) {
        throw std::invalid_argument("categorical power requires at least two planning levels");
    }
    const double degrees = static_cast<double>((groups - 1) * (planning_levels - 1));
    const double scale = static_cast<double>(std::min(groups - 1, planning_levels - 1));
    const boost::math::chi_squared_distribution<double> central(degrees);
    const double critical = boost::math::quantile(boost::math::complement(central, kPowerAlpha));
    const double noncentrality = static_cast<double>(n) * effect_threshold * effect_threshold * scale;
    const boost::math::non_central_chi_squared_distribution<double> alternative(degrees, noncentrality);
    return boost::math::cdf(boost::math::complement(alternative, critical));
}

double continuous_power(std::size_t n, std::size_t groups, double effect_threshold) {
    validate_power_inputs(n, groups, effect_threshold);
    if (n <= groups) {
        return 0.0;
    }
    const double degrees_between = static_cast<double>(groups - 1);
    const double degrees_within = static_cast<double>(n - groups);
    const boost::math::fisher_f_distribution<double> central(degrees_between, degrees_within);
    const double critical = boost::math::quantile(boost::math::complement(central, kPowerAlpha));
    const double noncentrality = static_cast<double>(n) * effect_threshold / (1.0 - effect_threshold);
    const boost::math::non_central_f_distribution<double> alternative(degrees_between, degrees_within, noncentrality);
    return boost::math::cdf(boost::math::complement(alternative, critical));
}

std::size_t minimum_n_for_target_power(AxisKind kind, std::size_t groups, std::size_t planning_levels,
                                       double effect_threshold) {
    if (groups < 2 || groups > kMaximumGroups) {
        throw std::invalid_argument("M2 minimum-N requires 2-10 groups");
    }
    const std::size_t start = std::max(groups + 1, groups * kMinimumGroupN);
    for (std::size_t n = start; n <= 10000; ++n) {
        const double power = kind == AxisKind::kCategorical
                                 ? categorical_power(n, groups, planning_levels, effect_threshold)
                                 : continuous_power(n, groups, effect_threshold);
        if (std::isfinite(power) && power >= kTargetPower) {
            return n;
        }
    }
    throw std::runtime_error("M2 target power is unattainable within the frozen search grid");
}

std::string_view to_string(AxisStatus status) noexcept {
    switch (status) {
        case AxisStatus::kAlignedEffectAndPermutationPPass:
            return "ALIGNED_EFFECT_AND_PERMUTATION_P_PASS";
        case AxisStatus::kIndeterminateEffectAboveThresholdWithoutP:
            return "INDETERMINATE_EFFECT_ABOVE_THRESHOLD_WITHOUT_P_LT_0_05";
        case AxisStatus::kIndeterminateInsufficientInformation:
            return "INDETERMINATE_INSUFFICIENT_INFORMATION_FOR_EFFECT_THRESHOLD";
        case AxisStatus::kIndeterminateAxisStatisticMissing:
            return "INDETERMINATE_AXIS_STATISTIC_MISSING";
        case AxisStatus::kNotAlignedConstantAxis:
            return "NOT_ALIGNED_AXIS_HAS_NO_OBSERVED_VARIATION";
        case AxisStatus::kNotAlignedEffectBelowThresholdAdequatePower:
            return "NOT_ALIGNED_EFFECT_BELOW_THRESHOLD_WITH_ADEQUATE_POWER";
    }
    return "INDETERMINATE_AXIS_STATISTIC_MISSING";
}

AxisDecision classify_axis(const AxisInput& input) {
    if (input.groups < 2 || input.groups > kMaximumGroups || input.n < input.groups ||
        !std::isfinite(input.effect_threshold) || input.effect_threshold <= 0.0 || input.effect_threshold >= 1.0) {
        throw std::invalid_argument("M2 axis input is outside the frozen domain");
    }
    if (input.effect.has_value() != input.permutation_p.has_value()) {
        throw std::invalid_argument("M2 axis has partial effect/p evidence");
    }

    AxisDecision result;
    if (!input.effect.has_value()) {
        if (input.declared_aligned) {
            throw std::invalid_argument("null M2 axis evidence cannot be declared aligned");
        }
        if (input.kind == AxisKind::kCategorical) {
            if (input.observed_category_levels != 1) {
                throw std::invalid_argument("missing categorical statistic is not explained by a constant axis");
            }
            result.status = AxisStatus::kNotAlignedConstantAxis;
            result.adequate_information_for_non_alignment = true;
        } else {
            result.status = AxisStatus::kIndeterminateAxisStatisticMissing;
        }
        return result;
    }

    if (input.kind == AxisKind::kCategorical &&
        (input.observed_category_levels < 2 || input.planning_category_levels < 2)) {
        throw std::invalid_argument("categorical statistic requires observed and planning variation");
    }
    const double effect = *input.effect;
    const double p_value = *input.permutation_p;
    if (!std::isfinite(effect) || effect < 0.0 || effect > 1.0 || !std::isfinite(p_value) || p_value < 0.0 ||
        p_value > 1.0) {
        throw std::invalid_argument("M2 effect or p-value is outside [0,1]");
    }
    if (p_value < kPermutationPFloor - 1e-12) {
        throw std::invalid_argument("M2 permutation p-value is below the add-one floor");
    }
    const double grid_value = p_value * static_cast<double>(kPermutations + 1);
    if (std::abs(grid_value - std::round(grid_value)) > 1e-9) {
        throw std::invalid_argument("M2 permutation p-value is off the frozen add-one grid");
    }

    result.minimum_n =
        minimum_n_for_target_power(input.kind, input.groups, input.planning_category_levels, input.effect_threshold);
    result.power_at_effect_threshold =
        input.kind == AxisKind::kCategorical
            ? categorical_power(input.n, input.groups, input.planning_category_levels, input.effect_threshold)
            : continuous_power(input.n, input.groups, input.effect_threshold);
    result.adequate_information_for_non_alignment = input.minimum_observed_group_n >= kMinimumGroupN &&
                                                    input.n >= *result.minimum_n &&
                                                    *result.power_at_effect_threshold >= kTargetPower;
    result.recomputed_aligned = effect >= input.effect_threshold && p_value < kPMaximumExclusive;
    if (input.declared_aligned != result.recomputed_aligned) {
        throw std::invalid_argument("declared and recomputed M2 alignment disagree");
    }
    result.positive_alignment_overrides_power =
        result.recomputed_aligned && !result.adequate_information_for_non_alignment;

    if (result.recomputed_aligned) {
        result.status = AxisStatus::kAlignedEffectAndPermutationPPass;
    } else if (effect >= input.effect_threshold) {
        result.status = AxisStatus::kIndeterminateEffectAboveThresholdWithoutP;
    } else if (!result.adequate_information_for_non_alignment) {
        result.status = AxisStatus::kIndeterminateInsufficientInformation;
    } else {
        result.status = AxisStatus::kNotAlignedEffectBelowThresholdAdequatePower;
    }
    return result;
}

AssociationResult categorical_permutation_association(const std::vector<std::string>& values,
                                                      const std::vector<std::string>& labels, std::uint64_t seed,
                                                      std::size_t permutations) {
    if (values.size() != labels.size()) {
        throw std::invalid_argument("categorical association values and labels must have equal length");
    }
    if (permutations != kPermutations) {
        throw std::invalid_argument("categorical association requires the frozen 499 permutations");
    }
    std::vector<std::string> categories;
    std::vector<std::string> group_values;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!is_missing_category(values[index])) {
            categories.push_back(values[index]);
            group_values.push_back(labels[index]);
        }
    }
    const std::set<std::string> category_set(categories.begin(), categories.end());
    const std::set<std::string> group_set(group_values.begin(), group_values.end());
    AssociationResult result;
    result.n = categories.size();
    result.observed_levels = category_set.size();
    result.observed_groups = group_set.size();
    std::ostringstream trace;
    trace.imbue(std::locale::classic());
    trace << "longlineage.m2.categorical.v1\nseed=" << seed << ";n=" << result.n << ";levels=" << result.observed_levels
          << ";groups=" << result.observed_groups << '\n';
    if (category_set.size() < 2 || group_set.size() < 2) {
        result.trace_sha256 = sha256_or_throw(trace.str());
        return result;
    }
    const std::vector<std::string> unique_categories(category_set.begin(), category_set.end());
    const std::vector<std::string> unique_groups(group_set.begin(), group_set.end());
    std::map<std::string, std::size_t> category_index;
    std::map<std::string, std::size_t> group_index;
    for (std::size_t index = 0; index < unique_categories.size(); ++index) {
        category_index[unique_categories[index]] = index;
    }
    for (std::size_t index = 0; index < unique_groups.size(); ++index) {
        group_index[unique_groups[index]] = index;
    }
    const auto make_table = [&](const std::vector<std::string>& assigned_groups) {
        CountTable table(unique_categories.size(), std::vector<std::size_t>(unique_groups.size(), 0));
        for (std::size_t index = 0; index < categories.size(); ++index) {
            ++table[category_index.at(categories[index])][group_index.at(assigned_groups[index])];
        }
        return table;
    };
    const double observed = cramer_v(make_table(group_values));
    result.effect = observed;
    trace << "observed=";
    append_trace_value(trace, observed);
    trace << '\n';
    longlineage::m1::NumpyPcg64 generator(seed);
    for (std::size_t permutation = 0; permutation < permutations; ++permutation) {
        const auto order = generator.permutation(group_values.size());
        std::vector<std::string> permuted_groups;
        permuted_groups.reserve(order.size());
        for (const std::size_t index : order) {
            permuted_groups.push_back(group_values[index]);
        }
        const double permuted = cramer_v(make_table(permuted_groups));
        if (permuted >= observed - 1e-12) {
            ++result.exceedance;
        }
        trace << permutation << '=';
        append_trace_value(trace, permuted);
        trace << '\n';
    }
    result.permutation_p = (static_cast<double>(result.exceedance) + 1.0) / (static_cast<double>(permutations) + 1.0);
    result.aligned = observed >= 0.30 && *result.permutation_p < kPMaximumExclusive;
    result.trace_sha256 = sha256_or_throw(trace.str());
    return result;
}

AssociationResult continuous_permutation_association(const std::vector<double>& values,
                                                     const std::vector<std::string>& labels, std::uint64_t seed,
                                                     std::size_t permutations) {
    if (values.size() != labels.size()) {
        throw std::invalid_argument("continuous association values and labels must have equal length");
    }
    if (permutations != kPermutations) {
        throw std::invalid_argument("continuous association requires the frozen 499 permutations");
    }
    AssociationResult result;
    result.n = static_cast<std::size_t>(
        std::count_if(values.begin(), values.end(), [](double value) { return std::isfinite(value); }));
    const std::set<std::string> all_groups(labels.begin(), labels.end());
    result.observed_groups = all_groups.size();
    std::ostringstream trace;
    trace.imbue(std::locale::classic());
    trace << "longlineage.m2.continuous.v1\nseed=" << seed << ";n=" << result.n << ";groups=" << result.observed_groups
          << '\n';
    const double observed = eta_squared_impl(values, labels);
    if (result.n < 2 || all_groups.size() < 2) {
        result.trace_sha256 = sha256_or_throw(trace.str());
        return result;
    }
    result.effect = observed;
    trace << "observed=";
    append_trace_value(trace, observed);
    trace << '\n';
    longlineage::m1::NumpyPcg64 generator(seed);
    for (std::size_t permutation = 0; permutation < permutations; ++permutation) {
        const auto order = generator.permutation(labels.size());
        std::vector<std::string> permuted_labels;
        permuted_labels.reserve(order.size());
        for (const std::size_t index : order) {
            permuted_labels.push_back(labels[index]);
        }
        const double permuted = eta_squared_impl(values, permuted_labels);
        if (permuted >= observed - 1e-12) {
            ++result.exceedance;
        }
        trace << permutation << '=';
        append_trace_value(trace, permuted);
        trace << '\n';
    }
    result.permutation_p = (static_cast<double>(result.exceedance) + 1.0) / (static_cast<double>(permutations) + 1.0);
    result.aligned = observed >= 0.14 && *result.permutation_p < kPMaximumExclusive;
    result.trace_sha256 = sha256_or_throw(trace.str());
    return result;
}

EightAxisResult evaluate_eight_axes(const EightAxisInput& input) {
    const std::size_t count = input.labels.size();
    if (count == 0 || input.stable_keys.size() != count || input.hp_exact.size() != count ||
        input.hp_family.size() != count || input.strand.size() != count || input.start.size() != count ||
        input.end.size() != count || input.length.size() != count || input.mapq.size() != count ||
        input.cpg_called.size() != count) {
        throw std::invalid_argument("M2 eight-axis arrays must be non-empty and aligned");
    }
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t lhs, std::size_t rhs) { return input.stable_keys[lhs] < input.stable_keys[rhs]; });
    for (std::size_t index = 1; index < order.size(); ++index) {
        if (input.stable_keys[order[index - 1]] == input.stable_keys[order[index]]) {
            throw std::invalid_argument("M2 stable keys must be unique");
        }
    }
    const auto order_strings = [&](const std::vector<std::string>& source) {
        std::vector<std::string> result;
        result.reserve(order.size());
        for (const std::size_t index : order) {
            result.push_back(source[index]);
        }
        return result;
    };
    const auto order_doubles = [&](const std::vector<double>& source) {
        std::vector<double> result;
        result.reserve(order.size());
        for (const std::size_t index : order) {
            result.push_back(source[index]);
        }
        return result;
    };
    EightAxisInput ordered;
    ordered.stable_keys = order_strings(input.stable_keys);
    ordered.labels = order_strings(input.labels);
    ordered.hp_exact = order_strings(input.hp_exact);
    ordered.hp_family = order_strings(input.hp_family);
    ordered.strand = order_strings(input.strand);
    ordered.start = order_doubles(input.start);
    ordered.end = order_doubles(input.end);
    ordered.length = order_doubles(input.length);
    ordered.mapq = order_doubles(input.mapq);
    ordered.cpg_called = order_doubles(input.cpg_called);
    ordered.base_seed = input.base_seed;
    std::map<std::string, std::size_t> cluster_sizes;
    for (const auto& label : ordered.labels) {
        ++cluster_sizes[label];
    }
    if (cluster_sizes.size() < 2 || cluster_sizes.size() > kMaximumGroups) {
        throw std::invalid_argument("M2 eight-axis input requires 2-10 methylation groups");
    }
    for (const auto* categorical : {&ordered.hp_exact, &ordered.hp_family, &ordered.strand}) {
        if (std::any_of(categorical->begin(), categorical->end(),
                        [](const std::string& value) { return is_missing_category(value); })) {
            throw std::invalid_argument("integrated M2 categorical axes must be complete");
        }
    }
    for (const auto* continuous : {&ordered.start, &ordered.end, &ordered.length, &ordered.mapq, &ordered.cpg_called}) {
        if (std::any_of(continuous->begin(), continuous->end(), [](double value) { return !std::isfinite(value); })) {
            throw std::invalid_argument("integrated M2 continuous axes must be complete");
        }
    }
    const std::size_t minimum_group =
        std::min_element(cluster_sizes.begin(), cluster_sizes.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second < rhs.second;
        })->second;
    struct AxisSpec {
        const char* name;
        AxisKind kind;
        std::size_t planning_levels;
        double threshold;
        std::size_t seed_offset;
    };
    const std::array<AxisSpec, 8> specs = {{
        {"hp_exact", AxisKind::kCategorical, 7, 0.30, 1},
        {"hp_family", AxisKind::kCategorical, 5, 0.30, 2},
        {"strand", AxisKind::kCategorical, 2, 0.30, 3},
        {"start", AxisKind::kContinuous, 0, 0.14, 4},
        {"end", AxisKind::kContinuous, 0, 0.14, 5},
        {"length", AxisKind::kContinuous, 0, 0.14, 6},
        {"mapq", AxisKind::kContinuous, 0, 0.14, 7},
        {"cpg_called", AxisKind::kContinuous, 0, 0.14, 8},
    }};
    EightAxisResult result;
    result.axes.reserve(specs.size());
    for (const auto& spec : specs) {
        AssociationResult association;
        if (std::string_view(spec.name) == "hp_exact") {
            association = categorical_permutation_association(
                ordered.hp_exact, ordered.labels, static_cast<std::uint64_t>(ordered.base_seed) + spec.seed_offset);
        } else if (std::string_view(spec.name) == "hp_family") {
            association = categorical_permutation_association(
                ordered.hp_family, ordered.labels, static_cast<std::uint64_t>(ordered.base_seed) + spec.seed_offset);
        } else if (std::string_view(spec.name) == "strand") {
            association = categorical_permutation_association(
                ordered.strand, ordered.labels, static_cast<std::uint64_t>(ordered.base_seed) + spec.seed_offset);
        } else {
            const std::vector<double>* values = nullptr;
            if (std::string_view(spec.name) == "start") {
                values = &ordered.start;
            } else if (std::string_view(spec.name) == "end") {
                values = &ordered.end;
            } else if (std::string_view(spec.name) == "length") {
                values = &ordered.length;
            } else if (std::string_view(spec.name) == "mapq") {
                values = &ordered.mapq;
            } else {
                values = &ordered.cpg_called;
            }
            association = continuous_permutation_association(
                *values, ordered.labels, static_cast<std::uint64_t>(ordered.base_seed) + spec.seed_offset);
        }
        if (association.n != count) {
            throw std::invalid_argument("integrated M2 association count drifted from core clusters");
        }
        AxisInput axis_input{spec.kind,
                             association.n,
                             cluster_sizes.size(),
                             minimum_group,
                             association.observed_levels,
                             spec.planning_levels,
                             spec.threshold,
                             association.effect,
                             association.permutation_p,
                             association.aligned};
        result.axes.push_back({spec.name, spec.kind, association, classify_axis(axis_input)});
    }
    const auto is_aligned = [&](std::size_t index) {
        return result.axes[index].decision.status == AxisStatus::kAlignedEffectAndPermutationPPass;
    };
    result.hp_axis_confound = is_aligned(0) || is_aligned(1);
    result.technical_axis_confound = false;
    for (std::size_t index = 2; index < result.axes.size(); ++index) {
        result.technical_axis_confound = result.technical_axis_confound || is_aligned(index);
    }
    const auto is_indeterminate = [](AxisStatus status) {
        return status == AxisStatus::kIndeterminateEffectAboveThresholdWithoutP ||
               status == AxisStatus::kIndeterminateInsufficientInformation ||
               status == AxisStatus::kIndeterminateAxisStatisticMissing;
    };
    result.axis_indeterminate =
        std::any_of(result.axes.begin(), result.axes.end(),
                    [&](const NamedAxisEvidence& evidence) { return is_indeterminate(evidence.decision.status); });
    result.residual_unexplained = !result.hp_axis_confound && !result.technical_axis_confound;
    std::ostringstream trace;
    trace.imbue(std::locale::classic());
    trace << "longlineage.m2.eight_axis.v1\n";
    for (const auto& axis : result.axes) {
        trace << axis.name.size() << ':' << axis.name << ';' << axis.association.trace_sha256 << ';'
              << to_string(axis.decision.status) << '\n';
    }
    trace << "hp=" << result.hp_axis_confound << ";technical=" << result.technical_axis_confound
          << ";indeterminate=" << result.axis_indeterminate << ";residual=" << result.residual_unexplained << '\n';
    result.trace_sha256 = sha256_or_throw(trace.str());
    return result;
}

std::string_view to_string(M2Status status) noexcept {
    switch (status) {
        case M2Status::kNotRun:
            return "NOT_RUN";
        case M2Status::kNotEvaluable:
            return "NOT_EVALUABLE";
        case M2Status::kFail:
            return "FAIL";
        case M2Status::kPass:
            return "PASS";
    }
    return "NOT_EVALUABLE";
}

std::string_view to_string(M2Reason reason) noexcept {
    switch (reason) {
        case M2Reason::kM1NotFlagged:
            return "M1_NOT_FLAGGED";
        case M2Reason::kGroupCountExceedsPlanningMaximum:
            return "GROUP_COUNT_EXCEEDS_PLANNING_MODEL_MAXIMUM";
        case M2Reason::kAxisIndeterminate:
            return "AXIS_INDETERMINATE";
        case M2Reason::kHpAxisConfound:
            return "HP_AXIS_CONFOUND";
        case M2Reason::kTechnicalAxisConfound:
            return "TECHNICAL_AXIS_CONFOUND";
        case M2Reason::kNotPhaseAnchoredRobust:
            return "NOT_PHASE_ANCHORED_ROBUST";
        case M2Reason::kAllMeasuredAxesDeterminateNoAlignedConfound:
            return "ALL_MEASURED_AXES_DETERMINATE_NO_ALIGNED_CONFOUND";
    }
    return "AXIS_INDETERMINATE";
}

M2Decision evaluate_precedence(const M2PrecedenceInput& input) {
    if (!input.m1_flagged) {
        return {M2Status::kNotRun, M2Reason::kM1NotFlagged, false, false};
    }
    if (input.group_count > kMaximumGroups) {
        return {M2Status::kNotEvaluable, M2Reason::kGroupCountExceedsPlanningMaximum, false, false};
    }
    if (input.group_count < 2) {
        throw std::invalid_argument("flagged M1 input must contain at least two groups");
    }
    if (input.axis_indeterminate) {
        return {M2Status::kNotEvaluable, M2Reason::kAxisIndeterminate, false, false};
    }
    if (input.hp_axis_confound) {
        return {M2Status::kFail, M2Reason::kHpAxisConfound, true, false};
    }
    if (input.technical_axis_confound) {
        return {M2Status::kFail, M2Reason::kTechnicalAxisConfound, true, false};
    }
    if (!input.residual_unexplained || !input.phase_anchored_robust) {
        return {M2Status::kFail, M2Reason::kNotPhaseAnchoredRobust, true, false};
    }
    return {M2Status::kPass, M2Reason::kAllMeasuredAxesDeterminateNoAlignedConfound, true, true};
}

}  // namespace longlineage::m2
