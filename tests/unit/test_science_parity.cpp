// SPDX-License-Identifier: GPL-3.0-only
#include <jansson.h>

#include <array>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/cooccurrence/statistics.hpp"
#include "longlineage/m1/hp_family.hpp"
#include "longlineage/m1/science.hpp"
#include "longlineage/m2/eligibility.hpp"

namespace {

using boost::multiprecision::cpp_dec_float_50;
using boost::multiprecision::cpp_int;
using longlineage::AlleleCall;
using longlineage::cooccurrence::CallabilityInput;
using longlineage::cooccurrence::CallabilityStatus;
using longlineage::cooccurrence::ExactStateStatus;
using longlineage::cooccurrence::JointAlleleCounts;
using longlineage::cooccurrence::Kx2Row;
using longlineage::m1::HpFamily;
using longlineage::m1::M1ReadsetStatus;
using longlineage::m1::Matrix;
using longlineage::m1::NumpyPcg64;
using longlineage::m2::AxisInput;
using longlineage::m2::AxisKind;
using longlineage::m2::AxisStatus;
using longlineage::m2::M2PrecedenceInput;

struct JsonDecref {
    void operator()(json_t* value) const noexcept { json_decref(value); }
};
using JsonPtr = std::unique_ptr<json_t, JsonDecref>;

struct CaseCounts {
    std::size_t pinned{0};
    std::size_t independent{0};
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

std::uint64_t uint64_value(json_t* value) {
    if (json_is_integer(value)) {
        const json_int_t integer = json_integer_value(value);
        check(integer >= 0, "expected non-negative JSON integer");
        return static_cast<std::uint64_t>(integer);
    }
    const std::string text = string_value(value);
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    check(result.ec == std::errc{} && result.ptr == text.data() + text.size(), "invalid uint64 JSON string");
    return parsed;
}

double number_value(json_t* value) {
    check(json_is_number(value), "expected JSON number");
    return json_number_value(value);
}

std::optional<double> optional_number(json_t* value) {
    if (json_is_null(value)) {
        return std::nullopt;
    }
    return number_value(value);
}

Matrix matrix_value(json_t* value) {
    check(json_is_array(value), "expected JSON matrix");
    Matrix matrix;
    const std::size_t rows = json_array_size(value);
    matrix.reserve(rows);
    for (std::size_t row = 0; row < rows; ++row) {
        json_t* source_row = json_array_get(value, row);
        check(json_is_array(source_row), "expected JSON matrix row");
        std::vector<double> values;
        const std::size_t columns = json_array_size(source_row);
        values.reserve(columns);
        for (std::size_t column = 0; column < columns; ++column) {
            values.push_back(number_value(json_array_get(source_row, column)));
        }
        matrix.push_back(std::move(values));
    }
    return matrix;
}

JsonPtr load_vectors(const std::filesystem::path& path) {
    json_error_t error{};
    JsonPtr root(json_load_file(path.c_str(), JSON_REJECT_DUPLICATES, &error));
    if (!root) {
        throw std::runtime_error("cannot load frozen vectors: line=" + std::to_string(error.line) +
                                 " detail=" + error.text);
    }
    check(json_is_object(root.get()), "frozen vector root must be an object");
    return root;
}

void test_m1_vectors(json_t* root, CaseCounts& counts) {
    check(string_value(field(root, "scope")) == "SYNTHETIC_ONLY_NOT_PRODUCTION_AUTHORITY",
          "frozen vector scope must remain synthetic");

    json_t* point_vectors = field(root, "m1_point_estimator");
    for (std::size_t index = 0; index < json_array_size(point_vectors); ++index) {
        json_t* row = json_array_get(point_vectors, index);
        const auto raw = static_cast<std::uint8_t>(uint64_value(field(row, "ml_raw")));
        const double point = number_value(field(row, "point"));
        const double interval_lower = number_value(field(row, "p2_interval_lower"));
        check_near(longlineage::m1::ml_raw_to_frozen_m1_point(raw), point, 1e-15,
                   "ML/255 frozen point estimator drift");
        if (raw == 1 || raw == 254) {
            check(std::abs(point - interval_lower) > 1e-6,
                  "M1 point estimator must not collapse to P2 probability_lower");
        }
        ++counts.pinned;
    }

    json_t* seed_vectors = field(root, "stable_seeds");
    for (std::size_t index = 0; index < json_array_size(seed_vectors); ++index) {
        json_t* row = json_array_get(seed_vectors, index);
        const std::uint32_t observed = longlineage::m1::stable_site_seed(
            string_value(field(row, "sample")), string_value(field(row, "contig")),
            uint64_value(field(row, "position1")), static_cast<std::int64_t>(json_integer_value(field(row, "offset"))));
        check(observed == uint64_value(field(row, "seed")), "BLAKE2b8 site seed drift");
        ++counts.pinned;
    }

    json_t* pcg_vectors = field(root, "pcg64");
    for (std::size_t index = 0; index < json_array_size(pcg_vectors); ++index) {
        json_t* row = json_array_get(pcg_vectors, index);
        const std::uint64_t seed = uint64_value(field(row, "seed"));
        NumpyPcg64 generator(seed);
        json_t* raw_values = field(row, "raw_u64");
        for (std::size_t raw_index = 0; raw_index < json_array_size(raw_values); ++raw_index) {
            check(generator.next_u64() == uint64_value(json_array_get(raw_values, raw_index)),
                  "NumPy PCG64 raw stream drift");
        }
        NumpyPcg64 permutation_generator(seed);
        const auto permutation = permutation_generator.permutation(10);
        json_t* expected_permutation = field(row, "permutation_10");
        check(permutation.size() == json_array_size(expected_permutation), "PCG permutation length drift");
        for (std::size_t permutation_index = 0; permutation_index < permutation.size(); ++permutation_index) {
            check(
                permutation[permutation_index] == uint64_value(json_array_get(expected_permutation, permutation_index)),
                "NumPy Generator.permutation drift");
        }
        ++counts.pinned;
    }

    json_t* bernoulli = field(root, "bernoulli");
    const Matrix methylation = matrix_value(field(bernoulli, "methylation"));
    const Matrix expected_distance = matrix_value(field(bernoulli, "distance"));
    const Matrix observed_distance = longlineage::m1::bernoulli_distance(methylation);
    check(observed_distance.size() == expected_distance.size(), "Bernoulli distance row count drift");
    for (std::size_t row = 0; row < expected_distance.size(); ++row) {
        for (std::size_t column = 0; column < expected_distance[row].size(); ++column) {
            check_near(observed_distance[row][column], expected_distance[row][column], 2e-15,
                       "Bernoulli distance drift");
        }
    }
    const auto preparation = longlineage::m1::prepare_minimal_clustering(methylation);
    check(preparation.status == M1ReadsetStatus::kPrimitiveParityReady,
          "legacy first-stage M1 primitive preparation drift");
    ++counts.pinned;

    json_t* linkage_vectors = field(root, "average_linkage");
    for (std::size_t index = 0; index < json_array_size(linkage_vectors); ++index) {
        json_t* row = json_array_get(linkage_vectors, index);
        const auto observed = longlineage::m1::average_linkage(matrix_value(field(row, "distance")));
        json_t* expected = field(row, "linkage");
        check(observed.size() == json_array_size(expected), "average-linkage row count drift");
        for (std::size_t linkage_index = 0; linkage_index < observed.size(); ++linkage_index) {
            json_t* expected_row = json_array_get(expected, linkage_index);
            check(observed[linkage_index].first == uint64_value(json_array_get(expected_row, 0)) &&
                      observed[linkage_index].second == uint64_value(json_array_get(expected_row, 1)) &&
                      observed[linkage_index].cluster_size == uint64_value(json_array_get(expected_row, 3)),
                  "average-linkage tie/relabel drift");
            check_near(observed[linkage_index].distance, number_value(json_array_get(expected_row, 2)), 1e-15,
                       "average-linkage distance drift");
        }
        ++counts.pinned;
    }

    json_t* peel = field(root, "peel");
    const auto retained = longlineage::m1::peel_complete(matrix_value(field(peel, "distance")));
    json_t* expected_retained = field(peel, "retained");
    check(retained.size() == json_array_size(expected_retained), "complete peel length drift");
    for (std::size_t index = 0; index < retained.size(); ++index) {
        check(retained[index] == uint64_value(json_array_get(expected_retained, index)), "complete peel tie drift");
    }
    ++counts.pinned;

    json_t* percentiles = field(root, "percentile");
    for (std::size_t index = 0; index < json_array_size(percentiles); ++index) {
        json_t* row = json_array_get(percentiles, index);
        std::vector<double> values;
        json_t* source_values = field(row, "values");
        for (std::size_t value_index = 0; value_index < json_array_size(source_values); ++value_index) {
            values.push_back(number_value(json_array_get(source_values, value_index)));
        }
        check_near(longlineage::m1::percentile_linear(values, number_value(field(row, "q"))),
                   number_value(field(row, "expected")), 1e-15, "NumPy linear percentile drift");
        ++counts.pinned;
    }

    std::vector<double> null32;
    for (std::size_t index = 0; index < 32; ++index) {
        null32.push_back(1.1 + static_cast<double>(index) / 100.0);
    }
    check(longlineage::m1::evaluate_split(1.299999999, null32, 95.0).failure ==
              longlineage::m1::SplitFailure::kBelowSeparationMinimumOrUndefined,
          "split separation boundary drift");
    check(longlineage::m1::evaluate_split(2.0, {null32.begin(), null32.end() - 1}, 95.0).failure ==
              longlineage::m1::SplitFailure::kInsufficientValidNull,
          "split 31/32 valid-null boundary drift");
    const double threshold = longlineage::m1::percentile_linear(null32, 95.0);
    const auto equality = longlineage::m1::evaluate_split(threshold, null32, 95.0);
    check(!equality.passed && equality.failure == longlineage::m1::SplitFailure::kNotAboveNullThreshold &&
              equality.exceedance == 2,
          "split strict-threshold equality drift");
    const auto passing = longlineage::m1::evaluate_split(2.0, null32, 95.0);
    check(passing.passed && passing.exceedance == 0 && passing.empirical_p.has_value(), "split pass decision drift");
    check_near(*passing.empirical_p, 1.0 / 33.0, 1e-15, "split empirical p drift");
    counts.pinned += 4;
}

void test_hp_family(const std::filesystem::path& repository, CaseCounts& counts) {
    const std::array<std::pair<std::string_view, HpFamily>, 14> cases = {{
        {"1", HpFamily::kHp1Side},
        {"HP1", HpFamily::kHp1Side},
        {"1-1", HpFamily::kHp1Side},
        {"1-2", HpFamily::kHp1Side},
        {"2", HpFamily::kHp2Side},
        {"HP2", HpFamily::kHp2Side},
        {"2-1", HpFamily::kHp2Side},
        {"2-2", HpFamily::kHp2Side},
        {"3", HpFamily::kHp3Ambiguous},
        {"4", HpFamily::kHp4Both},
        {".", HpFamily::kUntagged},
        {"0", HpFamily::kUntagged},
        {" unknown ", HpFamily::kUntagged},
        {"", HpFamily::kUntagged},
    }};
    for (const auto& [token, expected] : cases) {
        check(longlineage::m1::hp_family(token) == expected, "HP-family mapping drift");
        ++counts.pinned;
    }
    std::ifstream registry(repository / "contracts/v1/hp_family.tsv");
    check(static_cast<bool>(registry), "HP-family registry is missing");
    std::string line;
    std::size_t rows = 0;
    while (std::getline(registry, line)) {
        if (rows == 0) {
            check(line == "schema_name\tschema_version\tprecedence\thp_token\thp_family\tproduction_sidecar_token",
                  "HP-family registry header drift");
        }
        ++rows;
    }
    check(rows == 14, "HP-family registry must have one header plus 13 precedence rows");
    ++counts.pinned;
}

void test_m2_vectors(json_t* root, CaseCounts& counts) {
    json_t* power_rows = field(root, "m2_power");
    for (std::size_t index = 0; index < json_array_size(power_rows); ++index) {
        json_t* row = json_array_get(power_rows, index);
        const std::size_t groups = static_cast<std::size_t>(uint64_value(field(row, "groups")));
        struct PowerCase {
            const char* key;
            AxisKind kind;
            std::size_t levels;
            double threshold;
        };
        const std::array<PowerCase, 4> cases = {{
            {"hp_exact", AxisKind::kCategorical, 7, 0.30},
            {"hp_family", AxisKind::kCategorical, 5, 0.30},
            {"strand", AxisKind::kCategorical, 2, 0.30},
            {"continuous", AxisKind::kContinuous, 0, 0.14},
        }};
        for (const auto& power_case : cases) {
            json_t* expected = field(row, power_case.key);
            const std::size_t minimum_n = static_cast<std::size_t>(uint64_value(json_array_get(expected, 0)));
            check(longlineage::m2::minimum_n_for_target_power(power_case.kind, groups, power_case.levels,
                                                              power_case.threshold) == minimum_n,
                  "M2 minimum-N decision drift");
            const double previous =
                power_case.kind == AxisKind::kCategorical
                    ? longlineage::m2::categorical_power(minimum_n - 1, groups, power_case.levels, power_case.threshold)
                    : longlineage::m2::continuous_power(minimum_n - 1, groups, power_case.threshold);
            const double at_minimum =
                power_case.kind == AxisKind::kCategorical
                    ? longlineage::m2::categorical_power(minimum_n, groups, power_case.levels, power_case.threshold)
                    : longlineage::m2::continuous_power(minimum_n, groups, power_case.threshold);
            check_near(previous, number_value(json_array_get(expected, 1)), 2e-10, "M2 previous-N power drift");
            check_near(at_minimum, number_value(json_array_get(expected, 2)), 2e-10, "M2 minimum-N power drift");
            check(previous < 0.8 && at_minimum >= 0.8, "M2 minimum-N decision boundary drift");
            ++counts.pinned;
        }
    }

    AxisInput aligned{AxisKind::kCategorical, 10, 2, 5, 2, 7, 0.30, 0.30, 0.002, true};
    const auto aligned_result = longlineage::m2::classify_axis(aligned);
    check(aligned_result.status == AxisStatus::kAlignedEffectAndPermutationPPass &&
              aligned_result.positive_alignment_overrides_power,
          "positive M2 alignment must override negative-evaluability power");
    AxisInput indeterminate{AxisKind::kContinuous, 105, 10, 5, 0, 0, 0.14, 0.14, 0.05, false};
    check(
        longlineage::m2::classify_axis(indeterminate).status == AxisStatus::kIndeterminateEffectAboveThresholdWithoutP,
        "M2 p<0.05 exclusivity drift");
    AxisInput low_power{AxisKind::kContinuous, 104, 10, 5, 0, 0, 0.14, 0.13, 0.05, false};
    check(longlineage::m2::classify_axis(low_power).status == AxisStatus::kIndeterminateInsufficientInformation,
          "M2 low-power classification drift");
    AxisInput low_effect{AxisKind::kContinuous, 105, 10, 5, 0, 0, 0.14, 0.13, 0.05, false};
    check(longlineage::m2::classify_axis(low_effect).status == AxisStatus::kNotAlignedEffectBelowThresholdAdequatePower,
          "M2 adequate low-effect classification drift");
    AxisInput constant{AxisKind::kCategorical, 10, 2, 5, 1, 7, 0.30, std::nullopt, std::nullopt, false};
    check(longlineage::m2::classify_axis(constant).status == AxisStatus::kNotAlignedConstantAxis,
          "M2 constant categorical axis drift");
    AxisInput missing{AxisKind::kContinuous, 10, 2, 5, 0, 0, 0.14, std::nullopt, std::nullopt, false};
    check(longlineage::m2::classify_axis(missing).status == AxisStatus::kIndeterminateAxisStatisticMissing,
          "M2 missing continuous axis drift");
    counts.pinned += 6;

    json_t* precedence = field(root, "m2_precedence");
    for (std::size_t index = 0; index < json_array_size(precedence); ++index) {
        json_t* row = json_array_get(precedence, index);
        json_t* input = field(row, "input");
        M2PrecedenceInput parsed{
            json_is_true(json_array_get(input, 0)), static_cast<std::size_t>(uint64_value(json_array_get(input, 1))),
            json_is_true(json_array_get(input, 2)), json_is_true(json_array_get(input, 3)),
            json_is_true(json_array_get(input, 4)), json_is_true(json_array_get(input, 5)),
            json_is_true(json_array_get(input, 6)),
        };
        const auto decision = longlineage::m2::evaluate_precedence(parsed);
        check(longlineage::m2::to_string(decision.reason) == string_value(field(row, "reason")),
              "M2 reason precedence drift");
        ++counts.pinned;
    }
}

cpp_int choose_exact(std::uint64_t n, std::uint64_t k) {
    k = std::min(k, n - k);
    cpp_int result = 1;
    for (std::uint64_t index = 1; index <= k; ++index) {
        result *= n - k + index;
        result /= index;
    }
    return result;
}

struct BruteForceExact {
    std::uint64_t states{0};
    cpp_int total_weight{0};
    cpp_int tail_weight{0};
};

BruteForceExact brute_force_kx2(const std::vector<Kx2Row>& table) {
    std::vector<std::uint64_t> totals;
    std::vector<std::uint64_t> observed_alt;
    std::uint64_t target_alt = 0;
    cpp_int observed_weight = 1;
    for (const auto& row : table) {
        if (row[0] + row[1] == 0) {
            continue;
        }
        totals.push_back(row[0] + row[1]);
        observed_alt.push_back(row[1]);
        target_alt += row[1];
        observed_weight *= choose_exact(row[0] + row[1], row[1]);
    }
    BruteForceExact result;
    std::function<void(std::size_t, std::uint64_t, cpp_int)> enumerate;
    enumerate = [&](std::size_t row, std::uint64_t remaining, cpp_int weight) {
        if (row + 1 == totals.size()) {
            if (remaining <= totals[row]) {
                const cpp_int state_weight = weight * choose_exact(totals[row], remaining);
                ++result.states;
                result.total_weight += state_weight;
                if (state_weight <= observed_weight) {
                    result.tail_weight += state_weight;
                }
            }
            return;
        }
        std::uint64_t remaining_capacity = 0;
        for (std::size_t suffix = row + 1; suffix < totals.size(); ++suffix) {
            remaining_capacity += totals[suffix];
        }
        const std::uint64_t lower = remaining > remaining_capacity ? remaining - remaining_capacity : 0;
        const std::uint64_t upper = std::min(totals[row], remaining);
        for (std::uint64_t alt = lower; alt <= upper; ++alt) {
            enumerate(row + 1, remaining - alt, weight * choose_exact(totals[row], alt));
        }
    };
    enumerate(0, target_alt, cpp_int{1});
    return result;
}

AlleleCall allele_call(char value) {
    switch (value) {
        case 'R':
            return AlleleCall::kReference;
        case 'A':
            return AlleleCall::kAlternate;
        case 'O':
            return AlleleCall::kOther;
        case 'X':
            return AlleleCall::kUnobservable;
        default:
            throw std::runtime_error("invalid synthetic allele call");
    }
}

void test_cooccurrence_vectors(json_t* root, CaseCounts& counts) {
    json_t* endpoint = field(root, "endpoint_a");
    for (std::size_t index = 0; index < json_array_size(endpoint); ++index) {
        json_t* row = json_array_get(endpoint, index);
        json_t* table_json = field(row, "table");
        std::vector<Kx2Row> table;
        for (std::size_t table_index = 0; table_index < json_array_size(table_json); ++table_index) {
            json_t* source = json_array_get(table_json, table_index);
            table.push_back({uint64_value(json_array_get(source, 0)), uint64_value(json_array_get(source, 1))});
        }
        const auto result =
            longlineage::cooccurrence::fisher_freeman_halton_kx2(table, uint64_value(field(row, "ceiling")));
        check(longlineage::cooccurrence::to_string(result.status) == string_value(field(row, "status")),
              "Endpoint-A exact status drift");
        if (result.identifiable) {
            check(result.state_space_size == uint64_value(field(row, "state_space_size")),
                  "Endpoint-A state count drift");
            check_near(*result.p_value, number_value(field(row, "p_value")), 2e-12, "Endpoint-A exact p drift");
            check_near(*result.observed_table_probability, number_value(field(row, "observed_table_probability")),
                       2e-12, "Endpoint-A observed probability drift");
        }
        if (result.status == ExactStateStatus::kNotIdentifiableStateSpaceLimit) {
            check(result.state_space_lower_bound == uint64_value(field(row, "state_space_lower_bound")),
                  "Endpoint-A state ceiling lower-bound drift");
        }
        ++counts.pinned;
    }

    const std::array<std::vector<Kx2Row>, 2> brute_force_tables = {{
        {{{1, 9}}, {{11, 3}}},
        {{{2, 1}}, {{1, 2}}, {{2, 2}}},
    }};
    for (const auto& table : brute_force_tables) {
        const auto production = longlineage::cooccurrence::fisher_freeman_halton_kx2(table);
        const auto oracle = brute_force_kx2(table);
        const cpp_dec_float_50 exact_probability = cpp_dec_float_50(oracle.tail_weight.convert_to<std::string>()) /
                                                   cpp_dec_float_50(oracle.total_weight.convert_to<std::string>());
        check(production.state_space_size == oracle.states, "independent brute-force state count disagrees");
        check_near(*production.p_value, exact_probability.convert_to<double>(), 2e-12,
                   "independent integer-weight exact p disagrees");
        ++counts.independent;
    }

    json_t* fdr = field(root, "fdr");
    std::vector<std::optional<double>> p_values;
    json_t* p_json = field(fdr, "p_values");
    for (std::size_t index = 0; index < json_array_size(p_json); ++index) {
        p_values.push_back(optional_number(json_array_get(p_json, index)));
    }
    const auto bh = longlineage::cooccurrence::benjamini_hochberg(p_values);
    const auto by = longlineage::cooccurrence::benjamini_yekutieli(p_values);
    for (const auto& [observed, key] :
         std::array<std::pair<const std::vector<std::optional<double>>*, const char*>, 2>{{{&bh, "bh"}, {&by, "by"}}}) {
        json_t* expected = field(fdr, key);
        for (std::size_t index = 0; index < observed->size(); ++index) {
            const auto expected_value = optional_number(json_array_get(expected, index));
            check((*observed)[index].has_value() == expected_value.has_value(), "FDR nullability drift");
            if (expected_value.has_value()) {
                check_near(*(*observed)[index], *expected_value, 1e-15, "FDR value drift");
            }
        }
        ++counts.pinned;
    }

    json_t* joint = field(root, "joint_16_cells");
    JointAlleleCounts joint_counts;
    json_t* sequence = field(joint, "focal_partner_sequence");
    for (std::size_t index = 0; index < json_array_size(sequence); ++index) {
        const std::string pair = string_value(json_array_get(sequence, index));
        check(pair.size() == 2, "joint synthetic pair must have two calls");
        joint_counts.add(allele_call(pair[0]), allele_call(pair[1]));
    }
    json_t* expected_cells = field(joint, "expected_row_major");
    for (std::size_t index = 0; index < 16; ++index) {
        check(joint_counts.row_major_cells()[index] == uint64_value(json_array_get(expected_cells, index)),
              "R/A/O/X row-major cell drift");
    }
    check(joint_counts.conservation_holds(uint64_value(field(joint, "expected_total"))),
          "R/A/O/X cell conservation failed");
    ++counts.pinned;

    json_t* callability = field(root, "callability_precedence");
    for (std::size_t index = 0; index < json_array_size(callability); ++index) {
        json_t* row = json_array_get(callability, index);
        CallabilityInput input{
            uint64_value(field(row, "noncallable")),
            json_is_true(field(row, "testable")),
            optional_number(field(row, "q")),
            optional_number(field(row, "v")),
        };
        check(longlineage::cooccurrence::to_string(longlineage::cooccurrence::evaluate_callability(input)) ==
                  string_value(field(row, "status")),
              "Endpoint-B callability precedence drift");
        ++counts.pinned;
    }
}

template <typename Callable>
void expect_throw(Callable&& callable, CaseCounts& counts) {
    bool threw = false;
    try {
        callable();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "negative case did not fail closed");
    ++counts.negative;
}

void test_negative_cases(CaseCounts& counts) {
    expect_throw(
        [] {
            static_cast<void>(longlineage::cooccurrence::fisher_freeman_halton_kx2({{{1, 1}}, {{1, 1}}}, 0));
        },
        counts);
    expect_throw([] { static_cast<void>(longlineage::cooccurrence::benjamini_hochberg({-0.1})); }, counts);
    expect_throw(
        [] {
            AxisInput off_grid{AxisKind::kContinuous, 105, 10, 5, 0, 0, 0.14, 0.13, 0.0501, false};
            static_cast<void>(longlineage::m2::classify_axis(off_grid));
        },
        counts);
    expect_throw(
        [] {
            static_cast<void>(longlineage::m1::bernoulli_distance({{1.1, 0.0, 0.0}, {0.0, 0.0, 0.0}}));
        },
        counts);
    expect_throw(
        [] {
            M2PrecedenceInput malformed{true, 1, false, false, false, false, false};
            static_cast<void>(longlineage::m2::evaluate_precedence(malformed));
        },
        counts);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path repository = argc > 1 ? std::filesystem::path(argv[1]) : ".";
        const auto root = load_vectors(repository / "oracle/frozen_vectors/p3_p4_synthetic_v1.json");
        CaseCounts counts;
        test_m1_vectors(root.get(), counts);
        test_hp_family(repository, counts);
        test_m2_vectors(root.get(), counts);
        test_cooccurrence_vectors(root.get(), counts);
        test_negative_cases(counts);
        std::cout << "science_parity: PASS pinned_cases=" << counts.pinned
                  << " independent_cases=" << counts.independent << " negative_cases=" << counts.negative
                  << " m1_full_clustering=IMPLEMENTED_IN_P3_PARITY"
                  << " production_python_calls=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "science_parity: FAIL detail=" << error.what() << '\n';
        return 1;
    }
}
