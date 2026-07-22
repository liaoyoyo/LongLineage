// SPDX-License-Identifier: GPL-3.0-only
#include <jansson.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/m1/science.hpp"
#include "longlineage/m2/eligibility.hpp"

namespace {

using longlineage::m1::FullM1Result;
using longlineage::m1::M1Analysis;
using longlineage::m1::M1Options;
using longlineage::m1::M1ReadsetStatus;
using longlineage::m1::Matrix;
using longlineage::m2::AssociationResult;
using longlineage::m2::EightAxisInput;
using longlineage::m2::EightAxisResult;

struct JsonDecref {
    void operator()(json_t* value) const noexcept { json_decref(value); }
};
using JsonPtr = std::unique_ptr<json_t, JsonDecref>;

struct Counts {
    std::size_t m1_fixtures{0};
    std::size_t m1_trace_splits{0};
    std::size_t m2_associations{0};
    std::size_t m2_axes{0};
    std::size_t invariance{0};
    std::size_t negative{0};
};

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_near(double observed, double expected, double tolerance, const std::string& message) {
    if (!std::isfinite(observed) || !std::isfinite(expected) || std::abs(observed - expected) > tolerance) {
        throw std::runtime_error(message + ": observed=" + std::to_string(observed) +
                                 " expected=" + std::to_string(expected));
    }
}

json_t* field(json_t* object, const char* key) {
    json_t* value = json_object_get(object, key);
    check(value != nullptr, std::string("missing JSON field: ") + key);
    return value;
}

std::string string_value(json_t* value) {
    check(json_is_string(value), "expected JSON string");
    return json_string_value(value);
}

std::size_t size_value(json_t* value) {
    check(json_is_integer(value) && json_integer_value(value) >= 0, "expected non-negative JSON integer");
    return static_cast<std::size_t>(json_integer_value(value));
}

double number_value(json_t* value) {
    check(json_is_number(value), "expected JSON number");
    return json_number_value(value);
}

bool bool_value(json_t* value) {
    check(json_is_boolean(value), "expected JSON boolean");
    return json_is_true(value);
}

std::optional<double> optional_number(json_t* value) {
    if (json_is_null(value)) {
        return std::nullopt;
    }
    return number_value(value);
}

std::vector<std::string> string_vector(json_t* value) {
    check(json_is_array(value), "expected JSON string array");
    std::vector<std::string> result;
    result.reserve(json_array_size(value));
    for (std::size_t index = 0; index < json_array_size(value); ++index) {
        result.push_back(string_value(json_array_get(value, index)));
    }
    return result;
}

std::vector<std::size_t> size_vector(json_t* value) {
    check(json_is_array(value), "expected JSON integer array");
    std::vector<std::size_t> result;
    result.reserve(json_array_size(value));
    for (std::size_t index = 0; index < json_array_size(value); ++index) {
        result.push_back(size_value(json_array_get(value, index)));
    }
    return result;
}

std::vector<int> int_vector(json_t* value) {
    check(json_is_array(value), "expected JSON integer array");
    std::vector<int> result;
    result.reserve(json_array_size(value));
    for (std::size_t index = 0; index < json_array_size(value); ++index) {
        check(json_is_integer(json_array_get(value, index)), "expected JSON integer");
        result.push_back(static_cast<int>(json_integer_value(json_array_get(value, index))));
    }
    return result;
}

JsonPtr load_json(const std::filesystem::path& path) {
    json_error_t error{};
    JsonPtr root(json_load_file(path.c_str(), JSON_REJECT_DUPLICATES, &error));
    if (!root) {
        throw std::runtime_error("cannot load full parity oracle: line=" + std::to_string(error.line) +
                                 " detail=" + error.text);
    }
    check(json_is_object(root.get()), "full parity oracle root must be an object");
    return root;
}

std::vector<std::string> stable_keys(std::size_t count) {
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::ostringstream stream;
        stream << "read-" << std::setw(3) << std::setfill('0') << index;
        keys.push_back(stream.str());
    }
    return keys;
}

Matrix m1_fixture(std::string_view name) {
    const double low = longlineage::m1::ml_raw_to_frozen_m1_point(12);
    const double low_perturbed = longlineage::m1::ml_raw_to_frozen_m1_point(25);
    const double high = longlineage::m1::ml_raw_to_frozen_m1_point(243);
    const double high_perturbed = longlineage::m1::ml_raw_to_frozen_m1_point(230);
    if (name == "two_group_positive") {
        Matrix matrix(12, std::vector<double>(12, 0.0));
        for (std::size_t row = 0; row < matrix.size(); ++row) {
            const bool high_group = row >= 6;
            const std::size_t within_group = row % 6;
            for (std::size_t column = 0; column < matrix[row].size(); ++column) {
                const bool perturbed = (column + within_group) % 5 == 0;
                matrix[row][column] =
                    high_group ? (perturbed ? high_perturbed : high) : (perturbed ? low_perturbed : low);
            }
        }
        return matrix;
    }
    if (name == "three_group_recursive") {
        const std::vector<std::vector<double>> patterns = {
            {low, low, low, low, low, low, low, low, high, high, high, high},
            {low, low, low, low, high, high, high, high, low, low, low, low},
            {high, high, high, high, low, low, low, low, low, low, low, low},
        };
        Matrix matrix;
        matrix.reserve(18);
        for (const auto& pattern : patterns) {
            for (std::size_t row = 0; row < 6; ++row) {
                std::vector<double> values = pattern;
                for (std::size_t column = 0; column < values.size(); ++column) {
                    if ((row + column) % 7 == 0) {
                        values[column] = values[column] == low ? low_perturbed : high_perturbed;
                    }
                }
                matrix.push_back(std::move(values));
            }
        }
        return matrix;
    }
    if (name == "one_group_negative") {
        Matrix matrix(12, std::vector<double>(12, low));
        for (std::size_t row = 0; row < matrix.size(); ++row) {
            for (std::size_t column = 0; column < matrix[row].size(); ++column) {
                if ((row * 3 + column * 5) % 7 == 0) {
                    matrix[row][column] = low_perturbed;
                }
            }
        }
        return matrix;
    }
    throw std::runtime_error("unknown M1 fixture recipe");
}

void compare_trace(const std::vector<longlineage::m1::SplitTrace>& observed, json_t* expected, Counts& counts,
                   std::string_view context) {
    check(json_is_array(expected), "expected M1 trace must be an array");
    check(observed.size() == json_array_size(expected), std::string(context) + " trace length drift");
    for (std::size_t index = 0; index < observed.size(); ++index) {
        json_t* row = json_array_get(expected, index);
        json_t* child_sizes = field(row, "child_sizes");
        check(observed[index].node_size == size_value(field(row, "n_node")) &&
                  observed[index].first_child_size == size_value(json_array_get(child_sizes, 0)) &&
                  observed[index].second_child_size == size_value(json_array_get(child_sizes, 1)),
              std::string(context) + " trace topology drift");
        const auto& decision = observed[index].decision;
        check(decision.observed_between_within.has_value() && decision.null_threshold.has_value() &&
                  decision.empirical_p.has_value(),
              std::string(context) + " trace lost numeric evidence");
        check_near(*decision.observed_between_within, number_value(field(row, "observed_between_within")), 2e-12,
                   std::string(context) + " observed split ratio drift");
        check_near(decision.null_percentile, number_value(field(row, "null_percentile")), 1e-15,
                   std::string(context) + " percentile drift");
        check_near(*decision.null_threshold, number_value(field(row, "null_threshold")), 2e-12,
                   std::string(context) + " null threshold drift");
        check_near(*decision.empirical_p, number_value(field(row, "empirical_p")), 1e-15,
                   std::string(context) + " empirical p drift");
        check(decision.valid_null_replicates == size_value(field(row, "n_valid_null")) &&
                  decision.passed == bool_value(field(row, "passed")),
              std::string(context) + " split decision drift");
        ++counts.m1_trace_splits;
    }
}

void compare_m1(const FullM1Result& result, json_t* expected, const std::vector<std::string>& keys, Counts& counts) {
    check(result.status == M1ReadsetStatus::kFullClusteringReady && result.analysis.has_value(),
          "full M1 did not reach implemented state");
    check(result.retained_indices.size() == keys.size() && result.retained_keys == keys,
          "full M1 complete fixture unexpectedly peeled reads");
    check(result.forced.groups == size_value(field(expected, "forced_k")), "forced maxclust K drift");
    const auto forced_score = optional_number(field(expected, "forced_silhouette"));
    check(result.forced.silhouette.has_value() == forced_score.has_value(), "forced silhouette nullability drift");
    if (forced_score.has_value()) {
        check_near(*result.forced.silhouette, *forced_score, 2e-12, "forced silhouette value drift");
    }
    check(result.forced.labels == int_vector(field(expected, "forced_labels")), "forced maxclust partition drift");
    const M1Analysis& observed = *result.analysis;
    check(observed.coarse_groups == size_value(field(expected, "coarse_ng")) &&
              observed.fine_groups == size_value(field(expected, "fine_ng")) &&
              observed.other_count == size_value(field(expected, "n_other")) &&
              observed.outlier_count == size_value(field(expected, "n_outlier")) &&
              observed.minimum_seed_groups == size_value(field(expected, "ng_min")) &&
              observed.maximum_seed_groups == size_value(field(expected, "ng_max")),
          "M1 group-count summary drift");
    check_near(observed.modal_fraction, number_value(field(expected, "modal_fraction")), 1e-15,
               "M1 modal fraction drift");
    check(observed.unstable == bool_value(field(expected, "unstable")) &&
              observed.hidden_heterogeneity == bool_value(field(expected, "hidden_heterogeneity")) &&
              observed.stable_null_multigroup == bool_value(field(expected, "stable_null_multigroup")),
          "M1 stability flag drift");
    check(observed.seed_group_counts == size_vector(field(expected, "seed_group_counts")),
          "M1 seed group-count sequence drift");
    check(observed.modal_assignment_pair_count == size_value(field(expected, "modal_assignment_pair_count")),
          "M1 modal ARI pair count drift");
    check_near(observed.modal_assignment_ari_median, number_value(field(expected, "modal_assignment_ari_median")),
               2e-15, "M1 median ARI drift");
    check_near(observed.modal_assignment_ari_minimum, number_value(field(expected, "modal_assignment_ari_min")), 2e-15,
               "M1 minimum ARI drift");
    check(observed.all_modal_assignment_pairs_ari_at_least_0_8 ==
              bool_value(field(expected, "modal_assignment_all_pairs_ari_ge_0_8")),
          "M1 ARI stability decision drift");
    check(observed.coarse_labels == string_vector(field(expected, "coarse_labels")) &&
              observed.fine_labels == string_vector(field(expected, "fine_labels")),
          "M1 historical hierarchical labels drift");
    check(observed.partition_sha256 == string_value(field(expected, "partition_sha256")),
          "M1 canonical partition digest drift");
    compare_trace(observed.representative_coarse_trace, field(expected, "coarse_trace"), counts, "coarse");
    compare_trace(observed.fine_trace, field(expected, "fine_trace"), counts, "fine");
}

void test_round4(json_t* root, Counts& counts) {
    json_t* rows = field(root, "round4_points");
    for (std::size_t index = 0; index < json_array_size(rows); ++index) {
        json_t* row = json_array_get(rows, index);
        const auto raw = static_cast<std::uint8_t>(size_value(field(row, "ml_raw")));
        const double raw_point = longlineage::m1::ml_raw_to_frozen_m1_point(raw);
        check_near(raw_point, number_value(field(row, "raw_point")), 1e-15, "raw float32 ML point drift");
        check_near(longlineage::m1::ml_raw_to_historical_matrix_point_round4(raw),
                   number_value(field(row, "historical_matrix_point_round4")), 1e-15,
                   "historical round4 ML point drift");
        if (raw != 0 && raw != 255) {
            check(raw_point != longlineage::m1::ml_raw_to_historical_matrix_point_round4(raw),
                  "raw and historical-round4 representations were silently collapsed");
        }
        ++counts.invariance;
    }
}

void test_m1(json_t* root, Counts& counts) {
    json_t* fixtures = field(root, "m1_fixtures");
    for (std::size_t index = 0; index < json_array_size(fixtures); ++index) {
        json_t* fixture = json_array_get(fixtures, index);
        const std::string name = string_value(field(fixture, "name"));
        Matrix matrix = m1_fixture(name);
        std::vector<std::string> keys = stable_keys(matrix.size());
        M1Options serial;
        serial.workers = 1;
        const FullM1Result serial_result = longlineage::m1::run_historical_m1(matrix, keys, serial);
        compare_m1(serial_result, field(fixture, "expected"), keys, counts);

        M1Options parallel = serial;
        parallel.workers = 4;
        const FullM1Result parallel_result = longlineage::m1::run_historical_m1(matrix, keys, parallel);
        check(parallel_result.analysis.has_value() &&
                  parallel_result.analysis->partition_sha256 == serial_result.analysis->partition_sha256 &&
                  parallel_result.analysis->trace_sha256 == serial_result.analysis->trace_sha256 &&
                  parallel_result.analysis->coarse_labels == serial_result.analysis->coarse_labels,
              "M1 worker-count determinism failed");
        ++counts.invariance;

        std::reverse(matrix.begin(), matrix.end());
        std::reverse(keys.begin(), keys.end());
        const FullM1Result reversed = longlineage::m1::run_historical_m1(matrix, keys, parallel);
        check(reversed.analysis.has_value() &&
                  reversed.analysis->partition_sha256 == serial_result.analysis->partition_sha256 &&
                  reversed.analysis->trace_sha256 == serial_result.analysis->trace_sha256 &&
                  reversed.analysis->coarse_labels == serial_result.analysis->coarse_labels,
              "M1 stable-key input-order determinism failed");
        ++counts.invariance;

        std::map<std::string, std::string> renamed;
        std::vector<std::string> renamed_labels;
        for (const auto& label : serial_result.analysis->coarse_labels) {
            const auto [position, inserted] = renamed.emplace(label, "renamed-" + std::to_string(renamed.size()));
            static_cast<void>(inserted);
            renamed_labels.push_back(position->second);
        }
        const auto ordered_keys = stable_keys(renamed_labels.size());
        check(longlineage::m1::partition_digest(ordered_keys, renamed_labels) ==
                      serial_result.analysis->partition_sha256 &&
                  longlineage::m1::adjusted_rand_index(serial_result.analysis->coarse_labels, renamed_labels) == 1.0,
              "M1 label-renaming invariance failed");
        ++counts.invariance;
        ++counts.m1_fixtures;
    }
}

struct AssociationFixture {
    std::vector<std::string> labels;
    std::vector<std::string> categories;
    std::vector<double> continuous;
};

AssociationFixture association_fixture(std::string_view name) {
    AssociationFixture fixture;
    if (name == "categorical_three_group" || name == "continuous_three_group") {
        fixture.labels.insert(fixture.labels.end(), 6, "G1");
        fixture.labels.insert(fixture.labels.end(), 6, "G2");
        fixture.labels.insert(fixture.labels.end(), 6, "G3");
    } else {
        fixture.labels.insert(fixture.labels.end(), 10, "G1");
        fixture.labels.insert(fixture.labels.end(), 10, "G2");
    }
    if (name == "categorical_positive") {
        fixture.categories.insert(fixture.categories.end(), 10, "A");
        fixture.categories.insert(fixture.categories.end(), 10, "B");
    } else if (name == "categorical_three_group") {
        fixture.categories.insert(fixture.categories.end(), 6, "A");
        fixture.categories.insert(fixture.categories.end(), 6, "B");
        fixture.categories.insert(fixture.categories.end(), 6, "C");
    } else if (name == "categorical_null") {
        for (std::size_t index = 0; index < 20; ++index) {
            fixture.categories.push_back(index % 2 == 0 ? "A" : "B");
        }
    } else if (name == "categorical_constant") {
        fixture.categories.assign(20, "A");
    } else if (name == "continuous_positive") {
        for (std::size_t index = 0; index < 10; ++index) {
            fixture.continuous.push_back(static_cast<double>(index));
        }
        for (std::size_t index = 0; index < 10; ++index) {
            fixture.continuous.push_back(100.0 + static_cast<double>(index));
        }
    } else if (name == "continuous_three_group") {
        for (std::size_t group = 0; group < 3; ++group) {
            for (std::size_t index = 0; index < 6; ++index) {
                fixture.continuous.push_back(static_cast<double>(group * 50 + index));
            }
        }
    } else if (name == "continuous_null" || name == "continuous_missing") {
        for (std::size_t group = 0; group < 2; ++group) {
            static_cast<void>(group);
            for (std::size_t index = 0; index < 10; ++index) {
                fixture.continuous.push_back(name == "continuous_missing" && index == 9
                                                 ? std::numeric_limits<double>::quiet_NaN()
                                                 : static_cast<double>(index));
            }
        }
    } else {
        throw std::runtime_error("unknown M2 association recipe");
    }
    return fixture;
}

void compare_association(const AssociationResult& observed, json_t* expected, std::string_view context) {
    const auto expected_effect = optional_number(field(expected, "effect"));
    const auto expected_p = optional_number(field(expected, "p_perm"));
    check(observed.effect.has_value() == expected_effect.has_value() &&
              observed.permutation_p.has_value() == expected_p.has_value(),
          std::string(context) + " association nullability drift");
    if (expected_effect.has_value()) {
        check_near(*observed.effect, *expected_effect, 2e-12, std::string(context) + " effect drift");
        check_near(*observed.permutation_p, *expected_p, 1e-15, std::string(context) + " p drift");
    }
    check(observed.n == size_value(field(expected, "n")) &&
              observed.exceedance == size_value(field(expected, "exceedance")) &&
              observed.aligned == bool_value(field(expected, "aligned")),
          std::string(context) + " association decision drift");
}

void test_m2_associations(json_t* root, Counts& counts) {
    json_t* fixtures = field(root, "m2_associations");
    for (std::size_t index = 0; index < json_array_size(fixtures); ++index) {
        json_t* fixture_json = json_array_get(fixtures, index);
        const std::string name = string_value(field(fixture_json, "name"));
        const std::string kind = string_value(field(fixture_json, "kind"));
        const std::uint64_t seed = static_cast<std::uint64_t>(size_value(field(fixture_json, "seed")));
        const AssociationFixture fixture = association_fixture(name);
        AssociationResult observed;
        AssociationResult repeated;
        if (kind == "categorical") {
            observed = longlineage::m2::categorical_permutation_association(fixture.categories, fixture.labels, seed);
            repeated = longlineage::m2::categorical_permutation_association(fixture.categories, fixture.labels, seed);
        } else {
            observed = longlineage::m2::continuous_permutation_association(fixture.continuous, fixture.labels, seed);
            repeated = longlineage::m2::continuous_permutation_association(fixture.continuous, fixture.labels, seed);
        }
        compare_association(observed, field(fixture_json, "expected"), name);
        check(observed.trace_sha256 == repeated.trace_sha256, "M2 association trace digest is not deterministic");
        ++counts.invariance;
        ++counts.m2_associations;
    }
}

EightAxisInput eight_axis_fixture(json_t* recipe) {
    const std::size_t per_group = size_value(field(recipe, "n_per_group"));
    const std::string kind = string_value(field(recipe, "kind"));
    EightAxisInput input;
    const std::size_t count = per_group * 2;
    input.stable_keys = stable_keys(count);
    input.labels.insert(input.labels.end(), per_group, "G1");
    input.labels.insert(input.labels.end(), per_group, "G2");
    input.hp_exact.assign(count, "1-1");
    input.hp_family.assign(count, "HP1-side");
    if (kind == "hp") {
        std::fill(input.hp_exact.begin() + static_cast<std::ptrdiff_t>(per_group), input.hp_exact.end(), "2-2");
        std::fill(input.hp_family.begin() + static_cast<std::ptrdiff_t>(per_group), input.hp_family.end(), "HP2-side");
    }
    for (std::size_t group = 0; group < 2; ++group) {
        for (std::size_t index = 0; index < per_group; ++index) {
            input.strand.push_back(kind == "tech" ? (group == 0 ? "+" : "-") : (index % 2 == 0 ? "+" : "-"));
            input.start.push_back(static_cast<double>(index));
            input.end.push_back(static_cast<double>(index) + 100.0);
            input.length.push_back(100.0);
            input.mapq.push_back(50.0 + static_cast<double>(index % 5));
            input.cpg_called.push_back(10.0 + static_cast<double>(index % 7));
        }
    }
    input.base_seed = static_cast<std::uint32_t>(size_value(field(recipe, "base_seed")));
    return input;
}

template <typename T>
void reverse_vector(std::vector<T>& values) {
    std::reverse(values.begin(), values.end());
}

void reverse_eight_axis(EightAxisInput& input) {
    reverse_vector(input.stable_keys);
    reverse_vector(input.labels);
    reverse_vector(input.hp_exact);
    reverse_vector(input.hp_family);
    reverse_vector(input.strand);
    reverse_vector(input.start);
    reverse_vector(input.end);
    reverse_vector(input.length);
    reverse_vector(input.mapq);
    reverse_vector(input.cpg_called);
}

void compare_eight_axis(const EightAxisResult& observed, json_t* expected, Counts& counts) {
    check(observed.hp_axis_confound == bool_value(field(expected, "hp_axis_confound")) &&
              observed.technical_axis_confound == bool_value(field(expected, "technical_axis_confound")) &&
              observed.axis_indeterminate == bool_value(field(expected, "axis_indeterminate")) &&
              observed.residual_unexplained == bool_value(field(expected, "residual_unexplained")),
          "M2 eight-axis aggregate decision drift");
    json_t* axes = field(expected, "axes");
    check(observed.axes.size() == json_array_size(axes), "M2 eight-axis cardinality drift");
    for (std::size_t index = 0; index < observed.axes.size(); ++index) {
        json_t* axis = json_array_get(axes, index);
        check(observed.axes[index].name == string_value(field(axis, "name")), "M2 axis order/name drift");
        const auto effect = optional_number(field(axis, "effect"));
        const auto p = optional_number(field(axis, "p_perm"));
        check(observed.axes[index].association.effect.has_value() == effect.has_value() &&
                  observed.axes[index].association.permutation_p.has_value() == p.has_value(),
              "M2 integrated association nullability drift");
        if (effect.has_value()) {
            check_near(*observed.axes[index].association.effect, *effect, 2e-12,
                       "M2 integrated association effect drift");
            check_near(*observed.axes[index].association.permutation_p, *p, 1e-15, "M2 integrated association p drift");
        }
        check(
            longlineage::m2::to_string(observed.axes[index].decision.status) == string_value(field(axis, "status")) &&
                observed.axes[index].decision.adequate_information_for_non_alignment ==
                    bool_value(field(axis, "adequate")) &&
                observed.axes[index].decision.positive_alignment_overrides_power == bool_value(field(axis, "override")),
            "M2 integrated axis status/power precedence drift");
        ++counts.m2_axes;
    }
}

void test_m2_eight_axis(json_t* root, Counts& counts) {
    json_t* fixtures = field(root, "m2_eight_axis");
    for (std::size_t index = 0; index < json_array_size(fixtures); ++index) {
        json_t* fixture = json_array_get(fixtures, index);
        EightAxisInput input = eight_axis_fixture(field(fixture, "recipe"));
        const EightAxisResult observed = longlineage::m2::evaluate_eight_axes(input);
        compare_eight_axis(observed, field(fixture, "expected"), counts);
        reverse_eight_axis(input);
        const EightAxisResult reversed = longlineage::m2::evaluate_eight_axes(input);
        check(reversed.trace_sha256 == observed.trace_sha256, "M2 stable-key input-order determinism failed");
        ++counts.invariance;

        for (std::string& label : input.labels) {
            if (label == "G1") {
                label = "renamed-zeta";
            } else if (label == "G2") {
                label = "renamed-alpha";
            }
        }
        const EightAxisResult renamed = longlineage::m2::evaluate_eight_axes(input);
        check(renamed.trace_sha256 == observed.trace_sha256 && renamed.hp_axis_confound == observed.hp_axis_confound &&
                  renamed.technical_axis_confound == observed.technical_axis_confound &&
                  renamed.axis_indeterminate == observed.axis_indeterminate &&
                  renamed.residual_unexplained == observed.residual_unexplained,
              "M2 label-renaming invariance failed");
        ++counts.invariance;
    }
}

template <typename Callable>
void expect_throw(Callable&& callable, Counts& counts) {
    bool threw = false;
    try {
        callable();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "negative parity case did not fail closed");
    ++counts.negative;
}

void test_negative(Counts& counts) {
    expect_throw(
        [] {
            Matrix matrix = m1_fixture("two_group_positive");
            auto keys = stable_keys(matrix.size());
            keys.back() = keys.front();
            static_cast<void>(longlineage::m1::run_full_m1(matrix, keys));
        },
        counts);
    expect_throw(
        [] {
            EightAxisInput input;
            input.stable_keys = {"a", "b"};
            input.labels = {"G1", "G2"};
            input.hp_exact = {".", "2"};
            input.hp_family = {"untagged", "HP2-side"};
            input.strand = {"+", "-"};
            input.start = {1, 2};
            input.end = {2, 3};
            input.length = {1, 1};
            input.mapq = {60, 60};
            input.cpg_called = {10, 10};
            static_cast<void>(longlineage::m2::evaluate_eight_axes(input));
        },
        counts);
    expect_throw(
        [] {
            static_cast<void>(longlineage::m2::categorical_permutation_association({"A", "B"}, {"G1", "G2"}, 1, 498));
        },
        counts);
    expect_throw([] { static_cast<void>(longlineage::m1::historical_matrix_point_round4(1.1)); }, counts);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path repository = argc > 1 ? std::filesystem::path(argv[1]) : ".";
        const JsonPtr root = load_json(repository / "oracle/frozen_vectors/p3_full_m1_m2_v1.json");
        check(string_value(field(root.get(), "scope")) == "SYNTHETIC_ONLY_NOT_PRODUCTION_AUTHORITY",
              "full parity oracle scope drifted toward production authority");
        Counts counts;
        test_round4(root.get(), counts);
        test_m1(root.get(), counts);
        test_m2_associations(root.get(), counts);
        test_m2_eight_axis(root.get(), counts);
        test_negative(counts);
        std::cout << "p3_full_parity: PASS m1_fixtures=" << counts.m1_fixtures
                  << " m1_trace_splits=" << counts.m1_trace_splits << " m2_associations=" << counts.m2_associations
                  << " m2_axes=" << counts.m2_axes << " invariance_cases=" << counts.invariance
                  << " negative_cases=" << counts.negative
                  << " m1_full_clustering=IMPLEMENTED production_python_calls=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p3_full_parity: FAIL detail=" << error.what() << '\n';
        return 1;
    }
}
