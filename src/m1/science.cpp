// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/m1/science.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#include "longlineage/common/digest.hpp"

namespace longlineage::m1 {
namespace {

__extension__ typedef unsigned __int128 Uint128;

constexpr std::uint32_t kSeedInitA = 0x43b0d7e5U;
constexpr std::uint32_t kSeedMultA = 0x931e8875U;
constexpr std::uint32_t kSeedInitB = 0x8b51f9ddU;
constexpr std::uint32_t kSeedMultB = 0x58f38dedU;
constexpr std::uint32_t kSeedMixMultLeft = 0xca01f9ddU;
constexpr std::uint32_t kSeedMixMultRight = 0x4973f715U;
constexpr Uint128 kPcgMultiplier =
    (static_cast<Uint128>(0x2360ed051fc65da4ULL) << 64U) | static_cast<Uint128>(0x4385df649fccf645ULL);

[[nodiscard]] std::uint32_t seed_hashmix(std::uint32_t value, std::uint32_t& hash_constant) noexcept {
    value ^= hash_constant;
    hash_constant *= kSeedMultA;
    value *= hash_constant;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] std::uint32_t seed_mix(std::uint32_t lhs, std::uint32_t rhs) noexcept {
    std::uint32_t result = kSeedMixMultLeft * lhs - kSeedMixMultRight * rhs;
    result ^= result >> 16U;
    return result;
}

[[nodiscard]] std::array<std::uint64_t, 4> seed_sequence_words(std::uint64_t seed) noexcept {
    std::vector<std::uint32_t> entropy{static_cast<std::uint32_t>(seed)};
    if ((seed >> 32U) != 0U) {
        entropy.push_back(static_cast<std::uint32_t>(seed >> 32U));
    }
    entropy.resize(4, 0U);

    std::array<std::uint32_t, 4> pool{};
    std::uint32_t hash_constant = kSeedInitA;
    for (std::size_t index = 0; index < pool.size(); ++index) {
        pool[index] = seed_hashmix(entropy[index], hash_constant);
    }
    for (std::size_t source = 0; source < pool.size(); ++source) {
        for (std::size_t destination = 0; destination < pool.size(); ++destination) {
            if (source == destination) {
                continue;
            }
            pool[destination] = seed_mix(pool[destination], seed_hashmix(pool[source], hash_constant));
        }
    }
    for (std::size_t source = pool.size(); source < entropy.size(); ++source) {
        for (std::size_t destination = 0; destination < pool.size(); ++destination) {
            pool[destination] = seed_mix(pool[destination], seed_hashmix(entropy[source], hash_constant));
        }
    }

    std::array<std::uint32_t, 8> generated{};
    hash_constant = kSeedInitB;
    for (std::size_t index = 0; index < generated.size(); ++index) {
        std::uint32_t value = pool[index % pool.size()] ^ hash_constant;
        hash_constant *= kSeedMultB;
        value *= hash_constant;
        value ^= value >> 16U;
        generated[index] = value;
    }

    std::array<std::uint64_t, 4> words{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        words[index] = static_cast<std::uint64_t>(generated[index * 2]) |
                       (static_cast<std::uint64_t>(generated[index * 2 + 1]) << 32U);
    }
    return words;
}

[[nodiscard]] constexpr std::uint64_t rotate_right(std::uint64_t value, unsigned rotation) noexcept {
    rotation &= 63U;
    if (rotation == 0U) {
        return value;
    }
    return (value >> rotation) | (value << (64U - rotation));
}

void validate_rectangular(const Matrix& matrix, std::string_view name) {
    if (matrix.empty()) {
        return;
    }
    const std::size_t columns = matrix.front().size();
    for (const auto& row : matrix) {
        if (row.size() != columns) {
            throw std::invalid_argument(std::string(name) + " must be rectangular");
        }
    }
}

void validate_square(const Matrix& matrix, std::string_view name) {
    validate_rectangular(matrix, name);
    for (const auto& row : matrix) {
        if (row.size() != matrix.size()) {
            throw std::invalid_argument(std::string(name) + " must be square");
        }
    }
}

[[nodiscard]] Matrix symmetric_linkage_distance(const Matrix& distance) {
    validate_square(distance, "distance");
    Matrix symmetric = distance;
    for (std::size_t row = 0; row < symmetric.size(); ++row) {
        symmetric[row][row] = 0.0;
        for (std::size_t column = row + 1; column < symmetric.size(); ++column) {
            const double value = std::max(symmetric[row][column], symmetric[column][row]);
            if (!std::isfinite(value) || value < 0.0) {
                throw std::invalid_argument("distance matrix is not finite and complete");
            }
            symmetric[row][column] = value;
            symmetric[column][row] = value;
        }
    }
    return symmetric;
}

struct RawLinkageRow {
    std::size_t first_position;
    std::size_t second_position;
    double distance;
};

class LinkageUnionFind {
   public:
    explicit LinkageUnionFind(std::size_t leaves) : parent_(2 * leaves - 1), next_label_(leaves) {
        std::iota(parent_.begin(), parent_.end(), std::size_t{0});
    }

    [[nodiscard]] std::size_t find(std::size_t value) {
        std::size_t root = value;
        while (parent_[root] != root) {
            root = parent_[root];
        }
        while (parent_[value] != value) {
            const std::size_t next = parent_[value];
            parent_[value] = root;
            value = next;
        }
        return root;
    }

    std::size_t merge(std::size_t lhs, std::size_t rhs) {
        parent_[lhs] = next_label_;
        parent_[rhs] = next_label_;
        return next_label_++;
    }

   private:
    std::vector<std::size_t> parent_;
    std::size_t next_label_;
};

[[nodiscard]] Matrix subset_square(const Matrix& matrix, const std::vector<std::size_t>& indices) {
    Matrix subset(indices.size(), std::vector<double>(indices.size(), 0.0));
    for (std::size_t row = 0; row < indices.size(); ++row) {
        for (std::size_t column = 0; column < indices.size(); ++column) {
            subset[row][column] = matrix[indices[row]][indices[column]];
        }
    }
    return subset;
}

[[nodiscard]] Matrix subset_rows(const Matrix& matrix, const std::vector<std::size_t>& indices) {
    Matrix subset;
    subset.reserve(indices.size());
    for (const std::size_t index : indices) {
        if (index >= matrix.size()) {
            throw std::invalid_argument("matrix row subset index is outside the matrix");
        }
        subset.push_back(matrix[index]);
    }
    return subset;
}

[[nodiscard]] std::string sha256_or_throw(std::string_view bytes) {
    auto digest = sha256_hex(bytes);
    if (!digest.ok() || !digest.value.has_value()) {
        throw std::runtime_error("SHA-256 failed while serializing M1 scientific state: " + digest.detail);
    }
    return *digest.value;
}

struct TreeMaps {
    Matrix symmetric;
    std::vector<std::vector<std::size_t>> descendants;
    std::vector<std::pair<std::size_t, std::size_t>> children;
    std::size_t root{0};
    std::size_t leaves{0};
};

[[nodiscard]] TreeMaps build_tree_maps(const Matrix& distance) {
    Matrix symmetric = symmetric_linkage_distance(distance);
    const std::size_t leaves = symmetric.size();
    if (leaves < 2) {
        throw std::invalid_argument("tree maps require at least two leaves");
    }
    const auto linkage = average_linkage(symmetric);
    if (linkage.size() != leaves - 1) {
        throw std::runtime_error("average linkage did not produce a complete tree");
    }
    const std::size_t sentinel = std::numeric_limits<std::size_t>::max();
    TreeMaps result;
    result.symmetric = std::move(symmetric);
    result.leaves = leaves;
    result.root = 2 * leaves - 2;
    result.descendants.resize(2 * leaves - 1);
    result.children.assign(2 * leaves - 1, {sentinel, sentinel});
    for (std::size_t leaf = 0; leaf < leaves; ++leaf) {
        result.descendants[leaf] = {leaf};
    }
    for (std::size_t row = 0; row < linkage.size(); ++row) {
        const std::size_t node = leaves + row;
        const auto first = linkage[row].first;
        const auto second = linkage[row].second;
        if (first >= node || second >= node) {
            throw std::runtime_error("linkage child does not precede its parent");
        }
        result.children[node] = {first, second};
        result.descendants[node] = result.descendants[first];
        result.descendants[node].insert(result.descendants[node].end(), result.descendants[second].begin(),
                                        result.descendants[second].end());
    }
    return result;
}

[[nodiscard]] bool is_internal(const TreeMaps& tree, std::size_t node) noexcept {
    return node >= tree.leaves && node < tree.children.size();
}

[[nodiscard]] std::optional<double> between_within_ratio(const Matrix& distance, const std::vector<std::size_t>& first,
                                                         const std::vector<std::size_t>& second) {
    double between_sum = 0.0;
    std::size_t between_count = 0;
    for (const std::size_t lhs : first) {
        for (const std::size_t rhs : second) {
            const double value = distance[lhs][rhs];
            if (value >= 0.0) {
                between_sum += value;
                ++between_count;
            }
        }
    }
    double within_sum = 0.0;
    std::size_t within_count = 0;
    const auto add_within = [&](const std::vector<std::size_t>& group) {
        for (std::size_t lhs = 0; lhs < group.size(); ++lhs) {
            for (std::size_t rhs = lhs + 1; rhs < group.size(); ++rhs) {
                const double value = distance[group[lhs]][group[rhs]];
                if (value >= 0.0) {
                    within_sum += value;
                    ++within_count;
                }
            }
        }
    };
    add_within(first);
    add_within(second);
    if (between_count == 0 || within_count == 0) {
        return std::nullopt;
    }
    const double within_mean = within_sum / static_cast<double>(within_count);
    if (within_mean <= 1e-6) {
        return std::nullopt;
    }
    return (between_sum / static_cast<double>(between_count)) / within_mean;
}

[[nodiscard]] Matrix permute_columns(const Matrix& methylation, NumpyPcg64& generator) {
    Matrix permuted = methylation;
    if (permuted.empty()) {
        return permuted;
    }
    for (std::size_t column = 0; column < permuted.front().size(); ++column) {
        std::vector<std::size_t> valid;
        std::vector<double> source;
        for (std::size_t row = 0; row < permuted.size(); ++row) {
            if (!std::isnan(permuted[row][column])) {
                valid.push_back(row);
                source.push_back(permuted[row][column]);
            }
        }
        if (valid.size() > 1) {
            const auto order = generator.permutation(valid.size());
            for (std::size_t index = 0; index < valid.size(); ++index) {
                permuted[valid[index]][column] = source[order[index]];
            }
        }
    }
    return permuted;
}

[[nodiscard]] std::vector<double> null_distribution(const Matrix& methylation, const std::vector<std::size_t>& leaves,
                                                    NumpyPcg64& generator, std::size_t replicates) {
    const Matrix subset = subset_rows(methylation, leaves);
    std::vector<double> ratios;
    ratios.reserve(replicates);
    for (std::size_t replicate = 0; replicate < replicates; ++replicate) {
        const Matrix permuted = permute_columns(subset, generator);
        const Matrix null_distance = bernoulli_distance(permuted);
        try {
            const TreeMaps tree = build_tree_maps(null_distance);
            if (!is_internal(tree, tree.root)) {
                continue;
            }
            const auto [first, second] = tree.children[tree.root];
            const auto ratio = between_within_ratio(tree.symmetric, tree.descendants[first], tree.descendants[second]);
            if (ratio.has_value()) {
                ratios.push_back(*ratio);
            }
        } catch (const std::invalid_argument&) {
            continue;
        }
    }
    return ratios;
}

[[nodiscard]] SplitDecision evaluate_split_with_options(std::optional<double> observed_between_within,
                                                        const std::vector<double>& valid_null_ratios,
                                                        double null_percentile, std::size_t null_replicates,
                                                        double minimum_valid_null_fraction,
                                                        std::optional<double> empirical_alpha) {
    SplitDecision result;
    result.observed_between_within = observed_between_within;
    result.null_percentile = null_percentile;
    result.null_replicates_requested = null_replicates;
    result.minimum_valid_null = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(static_cast<double>(null_replicates) * minimum_valid_null_fraction)));
    if (!observed_between_within.has_value() || !std::isfinite(*observed_between_within) ||
        *observed_between_within < kMinimumSeparation) {
        result.failure = SplitFailure::kBelowSeparationMinimumOrUndefined;
        return result;
    }
    for (const double ratio : valid_null_ratios) {
        if (!std::isfinite(ratio)) {
            throw std::invalid_argument("valid null ratios must be finite");
        }
    }
    result.valid_null_replicates = valid_null_ratios.size();
    if (valid_null_ratios.size() < result.minimum_valid_null) {
        result.failure = SplitFailure::kInsufficientValidNull;
        return result;
    }
    result.null_threshold = percentile_linear(valid_null_ratios, null_percentile);
    result.exceedance =
        static_cast<std::size_t>(std::count_if(valid_null_ratios.begin(), valid_null_ratios.end(),
                                               [&](double value) { return value >= *observed_between_within; }));
    result.empirical_p =
        (1.0 + static_cast<double>(*result.exceedance)) / (static_cast<double>(valid_null_ratios.size()) + 1.0);
    if (*observed_between_within <= *result.null_threshold) {
        result.failure = SplitFailure::kNotAboveNullThreshold;
        return result;
    }
    if (empirical_alpha.has_value() && *result.empirical_p > *empirical_alpha) {
        result.failure = SplitFailure::kEmpiricalPAboveAlpha;
        return result;
    }
    result.passed = true;
    result.failure = SplitFailure::kNone;
    return result;
}

struct LabelRun {
    std::vector<std::string> labels;
    std::vector<SplitTrace> trace;
};

[[nodiscard]] LabelRun phylo_label(const Matrix& distance, const Matrix& methylation, std::uint64_t seed,
                                   double null_percentile, const M1Options& options) {
    const TreeMaps tree = build_tree_maps(distance);
    if (methylation.size() != tree.leaves) {
        throw std::invalid_argument("distance and methylation row counts disagree");
    }
    std::vector<std::string> labels(tree.leaves);
    NumpyPcg64 generator(seed);

    const auto descend = [&](std::size_t node) {
        std::size_t current = node;
        std::vector<std::size_t> quarantined;
        while (is_internal(tree, current)) {
            const auto [first, second] = tree.children[current];
            const std::size_t first_size = tree.descendants[first].size();
            const std::size_t second_size = tree.descendants[second].size();
            if (std::min(first_size, second_size) >= kMinimumGroupSize) {
                return std::make_pair(std::optional<std::size_t>{current}, quarantined);
            }
            const std::size_t small = first_size < second_size ? first : second;
            const std::size_t large = first_size < second_size ? second : first;
            quarantined.insert(quarantined.end(), tree.descendants[small].begin(), tree.descendants[small].end());
            current = large;
        }
        return std::make_pair(std::optional<std::size_t>{}, quarantined);
    };

    LabelRun output;
    std::function<void(std::size_t, const std::string&)> recurse;
    recurse = [&](std::size_t node, const std::string& label) {
        const auto& leaves = tree.descendants[node];
        if (leaves.size() < kMinimumReads) {
            for (const std::size_t leaf : leaves) {
                labels[leaf] = label;
            }
            return;
        }
        auto [balanced, quarantined] = descend(node);
        bool passed = false;
        if (balanced.has_value()) {
            const auto [first, second] = tree.children[*balanced];
            const auto observed =
                between_within_ratio(tree.symmetric, tree.descendants[first], tree.descendants[second]);
            std::vector<double> null_ratios;
            if (observed.has_value() && *observed >= kMinimumSeparation) {
                null_ratios =
                    null_distribution(methylation, tree.descendants[*balanced], generator, options.null_replicates);
            }
            SplitDecision decision =
                evaluate_split_with_options(observed, null_ratios, null_percentile, options.null_replicates,
                                            options.minimum_valid_null_fraction, options.empirical_alpha);
            output.trace.push_back({tree.descendants[*balanced].size(), tree.descendants[first].size(),
                                    tree.descendants[second].size(), decision});
            passed = decision.passed;
        }
        if (!balanced.has_value() || !passed) {
            for (const std::size_t leaf : leaves) {
                labels[leaf] = label;
            }
            return;
        }
        for (const std::size_t leaf : quarantined) {
            labels[leaf] = "outlier";
        }
        const auto [first, second] = tree.children[*balanced];
        const std::size_t large = tree.descendants[first].size() >= tree.descendants[second].size() ? first : second;
        const std::size_t small = large == first ? second : first;
        recurse(large, label + "-1");
        recurse(small, label + "-2");
    };
    recurse(tree.root, "1");

    std::map<std::string, std::size_t> sizes;
    for (const auto& label : labels) {
        if (label != "outlier") {
            ++sizes[label];
        }
    }
    std::set<std::string> too_small;
    for (const auto& [label, size] : sizes) {
        if (size < kMinimumGroupSize) {
            too_small.insert(label);
        }
    }
    for (auto& label : labels) {
        if (too_small.count(label) != 0) {
            label = "outlier";
        }
    }
    const auto outliers = static_cast<std::size_t>(std::count(labels.begin(), labels.end(), "outlier"));
    if (outliers >= kMinimumGroupSize) {
        for (auto& label : labels) {
            if (label == "outlier") {
                label = "other";
            }
        }
    }
    output.labels = std::move(labels);
    return output;
}

[[nodiscard]] std::size_t number_of_groups(const std::vector<std::string>& labels) {
    std::set<std::string> groups;
    for (const auto& label : labels) {
        if (label != "outlier" && label != "other") {
            groups.insert(label);
        }
    }
    return groups.size();
}

[[nodiscard]] std::string trace_digest(const std::vector<LabelRun>& runs, const std::vector<std::size_t>& group_counts,
                                       std::size_t representative_index) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "longlineage.m1.trace.v1\nrepresentative=" << representative_index << '\n';
    for (std::size_t run_index = 0; run_index < runs.size(); ++run_index) {
        stream << "run=" << run_index << ";groups=";
        if (run_index < group_counts.size()) {
            stream << group_counts[run_index];
        } else {
            stream << "fine";
        }
        stream << ";labels=";
        for (const auto& label : runs[run_index].labels) {
            stream << label.size() << ':' << label << ',';
        }
        stream << '\n';
        for (const auto& trace : runs[run_index].trace) {
            const auto& decision = trace.decision;
            stream << "split=" << trace.node_size << ',' << trace.first_child_size << ',' << trace.second_child_size
                   << ';';
            const auto write_optional = [&](const std::optional<double>& value) {
                if (value.has_value()) {
                    stream << std::hexfloat << *value << std::defaultfloat;
                } else {
                    stream << "null";
                }
            };
            write_optional(decision.observed_between_within);
            stream << ';' << std::hexfloat << decision.null_percentile << std::defaultfloat << ';'
                   << decision.null_replicates_requested << ';' << decision.minimum_valid_null << ';'
                   << decision.valid_null_replicates << ';';
            write_optional(decision.null_threshold);
            stream << ';';
            write_optional(decision.empirical_p);
            stream << ';' << static_cast<int>(decision.failure) << ';' << decision.passed << '\n';
        }
    }
    return sha256_or_throw(stream.str());
}

[[nodiscard]] std::vector<std::size_t> stable_key_order(const std::vector<std::string>& stable_keys) {
    std::vector<std::size_t> order(stable_keys.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t lhs, std::size_t rhs) { return stable_keys[lhs] < stable_keys[rhs]; });
    for (std::size_t index = 1; index < order.size(); ++index) {
        if (stable_keys[order[index - 1]] == stable_keys[order[index]]) {
            throw std::invalid_argument("stable M1 keys must be unique");
        }
    }
    return order;
}

[[nodiscard]] double fixed_decimal_round(double value, int precision) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    return std::stod(stream.str());
}

}  // namespace

double ml_raw_to_frozen_m1_point(std::uint8_t ml_raw) noexcept {
    const float historical_point = static_cast<float>(ml_raw) / 255.0F;
    return static_cast<double>(historical_point);
}

double historical_matrix_point_round4(double raw_point) {
    if (!std::isfinite(raw_point) || raw_point < 0.0 || raw_point > 1.0) {
        throw std::invalid_argument("raw M1 point must be finite in [0,1]");
    }
    return fixed_decimal_round(raw_point, 4);
}

double ml_raw_to_historical_matrix_point_round4(std::uint8_t ml_raw) {
    return historical_matrix_point_round4(ml_raw_to_frozen_m1_point(ml_raw));
}

Matrix historical_methylation_matrix_round4(const Matrix& raw_point_methylation) {
    validate_rectangular(raw_point_methylation, "raw point methylation");
    Matrix result = raw_point_methylation;
    for (auto& row : result) {
        for (double& value : row) {
            if (!std::isnan(value)) {
                value = historical_matrix_point_round4(value);
            }
        }
    }
    return result;
}

Matrix historical_distance_matrix_round6(const Matrix& raw_distance) {
    validate_square(raw_distance, "raw distance");
    Matrix result = raw_distance;
    for (auto& row : result) {
        for (double& value : row) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("historical distance serialization requires finite values");
            }
            value = fixed_decimal_round(value, 6);
        }
    }
    return result;
}

std::uint32_t stable_site_seed(std::string_view sample, std::string_view contig, std::uint64_t position1,
                               std::int64_t offset) {
    const std::string payload = std::string(sample) + "|" + std::string(contig) + "|" + std::to_string(position1) +
                                "|" + std::to_string(offset);
    const std::string digest = blake2b_64_hex(payload);
    std::uint32_t seed = 0;
    const char* begin = digest.data() + (digest.size() - 8);
    const char* end = digest.data() + digest.size();
    const auto parsed = std::from_chars(begin, end, seed, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error("internal BLAKE2b8 seed conversion failed");
    }
    return seed;
}

NumpyPcg64::NumpyPcg64(std::uint64_t seed) {
    const auto words = seed_sequence_words(seed);
    const Uint128 initial_state = (static_cast<Uint128>(words[0]) << 64U) | static_cast<Uint128>(words[1]);
    const Uint128 initial_sequence = (static_cast<Uint128>(words[2]) << 64U) | static_cast<Uint128>(words[3]);
    increment_ = (initial_sequence << 1U) | 1U;
    state_ = increment_;
    state_ += initial_state;
    state_ = state_ * kPcgMultiplier + increment_;
}

std::uint64_t NumpyPcg64::next_u64() noexcept {
    state_ = state_ * kPcgMultiplier + increment_;
    const std::uint64_t high = static_cast<std::uint64_t>(state_ >> 64U);
    const std::uint64_t low = static_cast<std::uint64_t>(state_);
    const std::uint64_t xorshifted = high ^ low;
    const unsigned rotation = static_cast<unsigned>(state_ >> 122U);
    return rotate_right(xorshifted, rotation);
}

std::uint32_t NumpyPcg64::next_u32() noexcept {
    if (has_cached_u32_) {
        has_cached_u32_ = false;
        return cached_u32_;
    }
    const std::uint64_t raw = next_u64();
    cached_u32_ = static_cast<std::uint32_t>(raw >> 32U);
    has_cached_u32_ = true;
    return static_cast<std::uint32_t>(raw);
}

std::uint32_t NumpyPcg64::bounded_u32(std::uint32_t maximum_inclusive) noexcept {
    std::uint32_t mask = maximum_inclusive;
    mask |= mask >> 1U;
    mask |= mask >> 2U;
    mask |= mask >> 4U;
    mask |= mask >> 8U;
    mask |= mask >> 16U;
    while (true) {
        const std::uint32_t value = next_u32() & mask;
        if (value <= maximum_inclusive) {
            return value;
        }
    }
}

std::vector<std::size_t> NumpyPcg64::permutation(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U) {
        throw std::invalid_argument("NumPy-compatible permutation size exceeds uint32 interval");
    }
    std::vector<std::size_t> values(size);
    std::iota(values.begin(), values.end(), std::size_t{0});
    for (std::size_t remaining = size; remaining > 1; --remaining) {
        const std::uint32_t selected = bounded_u32(static_cast<std::uint32_t>(remaining - 1));
        std::swap(values[remaining - 1], values[selected]);
    }
    return values;
}

Matrix bernoulli_distance(const Matrix& methylation) {
    validate_rectangular(methylation, "methylation");
    const std::size_t reads = methylation.size();
    const std::size_t columns = reads == 0 ? 0 : methylation.front().size();
    Matrix distance(reads, std::vector<double>(reads, -1.0));
    for (std::size_t lhs = 0; lhs < reads; ++lhs) {
        distance[lhs][lhs] = 0.0;
        for (std::size_t rhs = lhs + 1; rhs < reads; ++rhs) {
            std::size_t common = 0;
            double sum_weights = 0.0;
            double weighted_lhs_sum = 0.0;
            double weighted_rhs_sum = 0.0;
            double weighted_product_sum = 0.0;
            for (std::size_t column = 0; column < columns; ++column) {
                const double lhs_probability = methylation[lhs][column];
                const double rhs_probability = methylation[rhs][column];
                if (std::isnan(lhs_probability) || std::isnan(rhs_probability)) {
                    continue;
                }
                if (!std::isfinite(lhs_probability) || !std::isfinite(rhs_probability) || lhs_probability < 0.0 ||
                    lhs_probability > 1.0 || rhs_probability < 0.0 || rhs_probability > 1.0) {
                    throw std::invalid_argument("methylation probability must be NaN or finite in [0,1]");
                }
                ++common;
                const double lhs_weight = 2.0 * std::abs(lhs_probability - 0.5);
                const double rhs_weight = 2.0 * std::abs(rhs_probability - 0.5);
                const double lhs_weighted_probability = lhs_weight * lhs_probability;
                const double rhs_weighted_probability = rhs_weight * rhs_probability;
                sum_weights += lhs_weight * rhs_weight;
                weighted_lhs_sum += lhs_weighted_probability * rhs_weight;
                weighted_rhs_sum += lhs_weight * rhs_weighted_probability;
                weighted_product_sum += lhs_weighted_probability * rhs_weighted_probability;
            }
            const double numerator = weighted_lhs_sum + weighted_rhs_sum - 2.0 * weighted_product_sum;
            const double value =
                common < kMinimumGroupSize || sum_weights < kMinimumWeightSum ? -1.0 : numerator / sum_weights;
            distance[lhs][rhs] = value;
            distance[rhs][lhs] = value;
        }
    }
    return distance;
}

std::vector<std::size_t> peel_complete(const Matrix& distance) {
    validate_square(distance, "distance");
    std::vector<std::size_t> indices(distance.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    while (!indices.empty()) {
        std::size_t first_maximum = 0;
        std::size_t maximum_bad = 0;
        bool any_bad = false;
        for (std::size_t row = 0; row < indices.size(); ++row) {
            std::size_t bad = 0;
            for (std::size_t column = 0; column < indices.size(); ++column) {
                if (row == column) {
                    continue;
                }
                const double value = distance[indices[row]][indices[column]];
                if (value < 0.0 || std::isnan(value)) {
                    ++bad;
                }
            }
            if (bad > maximum_bad) {
                maximum_bad = bad;
                first_maximum = row;
            }
            any_bad = any_bad || bad != 0;
        }
        if (!any_bad) {
            return indices;
        }
        indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(first_maximum));
        if (indices.size() < kMinimumReads) {
            return indices;
        }
    }
    return indices;
}

std::vector<LinkageRow> average_linkage(const Matrix& distance) {
    Matrix working = symmetric_linkage_distance(distance);
    const std::size_t leaves = working.size();
    if (leaves < 2) {
        return {};
    }

    std::vector<std::size_t> sizes(leaves, 1);
    std::vector<std::size_t> chain;
    chain.reserve(leaves);
    std::vector<RawLinkageRow> raw;
    raw.reserve(leaves - 1);

    for (std::size_t merge_index = 0; merge_index + 1 < leaves; ++merge_index) {
        if (chain.empty()) {
            const auto first_active =
                std::find_if(sizes.begin(), sizes.end(), [](std::size_t size) { return size != 0; });
            chain.push_back(static_cast<std::size_t>(std::distance(sizes.begin(), first_active)));
        }

        std::size_t x = 0;
        std::size_t y = 0;
        double current_minimum = 0.0;
        while (true) {
            x = chain.back();
            if (chain.size() > 1) {
                y = chain[chain.size() - 2];
                current_minimum = working[x][y];
            } else {
                current_minimum = std::numeric_limits<double>::infinity();
            }
            for (std::size_t candidate = 0; candidate < leaves; ++candidate) {
                if (sizes[candidate] == 0 || candidate == x) {
                    continue;
                }
                const double candidate_distance = working[x][candidate];
                if (candidate_distance < current_minimum) {
                    current_minimum = candidate_distance;
                    y = candidate;
                }
            }
            if (chain.size() > 1 && y == chain[chain.size() - 2]) {
                break;
            }
            chain.push_back(y);
        }

        chain.resize(chain.size() - 2);
        if (x > y) {
            std::swap(x, y);
        }
        const std::size_t size_x = sizes[x];
        const std::size_t size_y = sizes[y];
        raw.push_back({x, y, current_minimum});
        sizes[x] = 0;
        sizes[y] = size_x + size_y;
        for (std::size_t other = 0; other < leaves; ++other) {
            if (sizes[other] == 0 || other == y) {
                continue;
            }
            working[other][y] = working[y][other] =
                (static_cast<double>(size_x) * working[other][x] + static_cast<double>(size_y) * working[other][y]) /
                static_cast<double>(size_x + size_y);
        }
    }

    std::stable_sort(raw.begin(), raw.end(),
                     [](const RawLinkageRow& lhs, const RawLinkageRow& rhs) { return lhs.distance < rhs.distance; });
    LinkageUnionFind union_find(leaves);
    std::vector<std::size_t> cluster_sizes(2 * leaves - 1, 1);
    std::vector<LinkageRow> result;
    result.reserve(raw.size());
    for (const auto& row : raw) {
        std::size_t first = union_find.find(row.first_position);
        std::size_t second = union_find.find(row.second_position);
        if (first > second) {
            std::swap(first, second);
        }
        const std::size_t merged_size = cluster_sizes[first] + cluster_sizes[second];
        const std::size_t label = union_find.merge(first, second);
        cluster_sizes[label] = merged_size;
        result.push_back({first, second, row.distance, merged_size});
    }
    return result;
}

std::vector<int> flat_clusters_maxclust(const std::vector<LinkageRow>& linkage, std::size_t leaves,
                                        std::size_t maximum_clusters) {
    if (leaves == 0 || maximum_clusters == 0 || linkage.size() + 1 != leaves) {
        throw std::invalid_argument("maxclust requires a complete non-empty linkage and positive cluster limit");
    }
    if (leaves == 1) {
        return {1};
    }
    const std::size_t nodes = 2 * leaves - 1;
    const std::size_t sentinel = std::numeric_limits<std::size_t>::max();
    std::vector<std::pair<std::size_t, std::size_t>> children(nodes, {sentinel, sentinel});
    std::vector<double> heights(nodes, 0.0);
    for (std::size_t row = 0; row < linkage.size(); ++row) {
        const std::size_t node = leaves + row;
        if (linkage[row].first >= node || linkage[row].second >= node || !std::isfinite(linkage[row].distance) ||
            linkage[row].distance < 0.0) {
            throw std::invalid_argument("invalid linkage row for maxclust");
        }
        children[node] = {linkage[row].first, linkage[row].second};
        heights[node] = linkage[row].distance;
        if (row > 0 && linkage[row].distance < linkage[row - 1].distance) {
            throw std::invalid_argument("maxclust requires monotonic linkage heights");
        }
    }
    if (maximum_clusters >= leaves) {
        std::vector<int> singleton_labels(leaves, 0);
        for (std::size_t leaf = 0; leaf < leaves; ++leaf) {
            singleton_labels[leaf] = static_cast<int>(leaf + 1);
        }
        return singleton_labels;
    }
    const double threshold = linkage[leaves - maximum_clusters - 1].distance;
    std::vector<int> labels(leaves, 0);
    int next_label = 1;
    const auto assign_descendants = [&](std::size_t start, int label) {
        std::vector<std::size_t> stack{start};
        while (!stack.empty()) {
            const std::size_t node = stack.back();
            stack.pop_back();
            if (node < leaves) {
                labels[node] = label;
                continue;
            }
            const auto [first, second] = children[node];
            stack.push_back(second);
            stack.push_back(first);
        }
    };
    std::function<void(std::size_t)> visit;
    visit = [&](std::size_t node) {
        if (node < leaves || heights[node] <= threshold) {
            assign_descendants(node, next_label);
            ++next_label;
            return;
        }
        const auto [first, second] = children[node];
        visit(first);
        visit(second);
    };
    visit(nodes - 1);
    return labels;
}

double percentile_linear(std::vector<double> values, double percentile) {
    if (values.empty()) {
        throw std::invalid_argument("percentile requires at least one value");
    }
    if (!std::isfinite(percentile) || percentile < 0.0 || percentile > 100.0) {
        throw std::invalid_argument("percentile must be finite in [0,100]");
    }
    for (const double value : values) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("percentile values must be finite");
        }
    }
    std::sort(values.begin(), values.end());
    const double rank = (static_cast<double>(values.size()) - 1.0) * percentile / 100.0;
    const auto lower = static_cast<std::size_t>(std::floor(rank));
    const auto upper = static_cast<std::size_t>(std::ceil(rank));
    const double fraction = rank - static_cast<double>(lower);
    return values[lower] + fraction * (values[upper] - values[lower]);
}

double adjusted_rand_index(const std::vector<std::string>& lhs, const std::vector<std::string>& rhs) {
    if (lhs.size() != rhs.size()) {
        throw std::invalid_argument("ARI label vectors must have equal length");
    }
    if (lhs.size() < 2) {
        return 1.0;
    }
    std::map<std::string, std::size_t> lhs_counts;
    std::map<std::string, std::size_t> rhs_counts;
    std::map<std::pair<std::string, std::string>, std::size_t> contingency;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        ++lhs_counts[lhs[index]];
        ++rhs_counts[rhs[index]];
        ++contingency[{lhs[index], rhs[index]}];
    }
    const auto choose_two = [](std::size_t value) -> long double {
        return value < 2 ? 0.0L : static_cast<long double>(value) * static_cast<long double>(value - 1) / 2.0L;
    };
    long double cell_pairs = 0.0L;
    long double lhs_pairs = 0.0L;
    long double rhs_pairs = 0.0L;
    for (const auto& [unused, count] : contingency) {
        static_cast<void>(unused);
        cell_pairs += choose_two(count);
    }
    for (const auto& [unused, count] : lhs_counts) {
        static_cast<void>(unused);
        lhs_pairs += choose_two(count);
    }
    for (const auto& [unused, count] : rhs_counts) {
        static_cast<void>(unused);
        rhs_pairs += choose_two(count);
    }
    const long double total_pairs = choose_two(lhs.size());
    const long double expected = lhs_pairs * rhs_pairs / total_pairs;
    const long double maximum = (lhs_pairs + rhs_pairs) / 2.0L;
    const long double denominator = maximum - expected;
    if (std::abs(denominator) <= std::numeric_limits<long double>::epsilon()) {
        return 1.0;
    }
    return static_cast<double>((cell_pairs - expected) / denominator);
}

std::string partition_digest(const std::vector<std::string>& stable_keys, const std::vector<std::string>& labels) {
    if (stable_keys.size() != labels.size()) {
        throw std::invalid_argument("partition keys and labels must have equal length");
    }
    std::set<std::string> unique_keys(stable_keys.begin(), stable_keys.end());
    if (unique_keys.size() != stable_keys.size()) {
        throw std::invalid_argument("partition keys must be unique");
    }
    std::map<std::string, std::vector<std::string>> by_label;
    for (std::size_t index = 0; index < stable_keys.size(); ++index) {
        by_label[labels[index]].push_back(stable_keys[index]);
    }
    std::vector<std::vector<std::string>> groups;
    groups.reserve(by_label.size());
    for (auto& [unused, members] : by_label) {
        static_cast<void>(unused);
        std::sort(members.begin(), members.end());
        groups.push_back(std::move(members));
    }
    std::sort(groups.begin(), groups.end());
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "longlineage.partition.v1\n";
    for (const auto& group : groups) {
        stream << "group=" << group.size() << ';';
        for (const auto& member : group) {
            stream << member.size() << ':' << member << ',';
        }
        stream << '\n';
    }
    return sha256_or_throw(stream.str());
}

ForcedSilhouetteSplit forced_silhouette_split(const Matrix& distance) {
    ForcedSilhouetteSplit result;
    const std::size_t reads = distance.size();
    if (reads < kMinimumReads) {
        return result;
    }
    Matrix symmetric;
    std::vector<LinkageRow> linkage;
    try {
        symmetric = symmetric_linkage_distance(distance);
        linkage = average_linkage(symmetric);
    } catch (const std::invalid_argument&) {
        return result;
    }
    const std::size_t maximum_groups = std::min<std::size_t>(6, reads / kMinimumGroupSize);
    for (std::size_t groups = 2; groups <= maximum_groups; ++groups) {
        const auto labels = flat_clusters_maxclust(linkage, reads, groups);
        std::map<int, std::vector<std::size_t>> members;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            members[labels[index]].push_back(index);
        }
        if (members.size() < groups) {
            continue;
        }
        const auto minimum = std::min_element(members.begin(), members.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second.size() < rhs.second.size();
        });
        if (minimum == members.end() || minimum->second.size() < kMinimumGroupSize) {
            continue;
        }
        double silhouette_sum = 0.0;
        for (std::size_t sample = 0; sample < reads; ++sample) {
            const auto& own = members.at(labels[sample]);
            double own_sum = 0.0;
            for (const std::size_t other : own) {
                if (other != sample) {
                    own_sum += symmetric[sample][other];
                }
            }
            const double within = own_sum / static_cast<double>(own.size() - 1);
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto& [label, group] : members) {
                if (label == labels[sample]) {
                    continue;
                }
                double sum = 0.0;
                for (const std::size_t other : group) {
                    sum += symmetric[sample][other];
                }
                nearest = std::min(nearest, sum / static_cast<double>(group.size()));
            }
            const double denominator = std::max(within, nearest);
            silhouette_sum += denominator == 0.0 ? 0.0 : (nearest - within) / denominator;
        }
        const double score = silhouette_sum / static_cast<double>(reads);
        if (!result.silhouette.has_value() || score > *result.silhouette) {
            result.groups = groups;
            result.silhouette = score;
            result.labels = labels;
        }
    }
    return result;
}

M1Preparation prepare_minimal_clustering(const Matrix& methylation) {
    M1Preparation result;
    if (methylation.size() < kMinimumReads) {
        result.status = M1ReadsetStatus::kInsufficientAltReads;
        return result;
    }
    result.distance = bernoulli_distance(methylation);
    result.retained_indices = peel_complete(result.distance);
    if (result.retained_indices.size() < kMinimumReads) {
        result.status = M1ReadsetStatus::kIncompleteDistanceBelowMinimum;
        return result;
    }
    result.linkage = average_linkage(subset_square(result.distance, result.retained_indices));
    result.status = M1ReadsetStatus::kPrimitiveParityReady;
    result.full_clustering_abstained = true;
    return result;
}

SplitDecision evaluate_split(std::optional<double> observed_between_within,
                             const std::vector<double>& valid_null_ratios, double null_percentile,
                             std::optional<double> empirical_alpha) {
    return evaluate_split_with_options(observed_between_within, valid_null_ratios, null_percentile, kNullReplicates,
                                       0.8, empirical_alpha);
}

M1Analysis analyze_phylo(const Matrix& distance, const Matrix& methylation, const M1Options& options) {
    validate_square(distance, "distance");
    validate_rectangular(methylation, "methylation");
    if (distance.size() != methylation.size() || distance.size() < kMinimumReads) {
        throw std::invalid_argument(
            "full M1 analysis requires matching distance/methylation rows and at least six reads");
    }
    if (options.coarse_seeds == 0 || options.null_replicates == 0 || options.workers == 0 ||
        !std::isfinite(options.minimum_valid_null_fraction) || options.minimum_valid_null_fraction <= 0.0 ||
        options.minimum_valid_null_fraction > 1.0) {
        throw std::invalid_argument("M1 options are outside the frozen domain");
    }

    const std::size_t fine_index = options.coarse_seeds;
    std::vector<LabelRun> runs(options.coarse_seeds + 1);
    const auto execute = [&](std::size_t run_index) {
        if (run_index == fine_index) {
            runs[run_index] = phylo_label(distance, methylation, options.base_seed, 90.0, options);
        } else {
            runs[run_index] = phylo_label(distance, methylation, options.base_seed + run_index * 101U, 95.0, options);
        }
    };
    const std::size_t workers = std::min(options.workers, runs.size());
    if (workers == 1) {
        for (std::size_t run_index = 0; run_index < runs.size(); ++run_index) {
            execute(run_index);
        }
    } else {
        std::atomic<std::size_t> next{0};
        std::mutex exception_mutex;
        std::exception_ptr failure;
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (std::size_t worker = 0; worker < workers; ++worker) {
            static_cast<void>(worker);
            threads.emplace_back([&] {
                while (true) {
                    const std::size_t run_index = next.fetch_add(1);
                    if (run_index >= runs.size()) {
                        return;
                    }
                    try {
                        execute(run_index);
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(exception_mutex);
                        if (!failure) {
                            failure = std::current_exception();
                        }
                        return;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    M1Analysis result;
    result.seed_group_counts.reserve(options.coarse_seeds);
    std::map<std::size_t, std::size_t> frequencies;
    for (std::size_t index = 0; index < options.coarse_seeds; ++index) {
        const std::size_t groups = number_of_groups(runs[index].labels);
        result.seed_group_counts.push_back(groups);
        ++frequencies[groups];
    }
    std::size_t modal_groups = result.seed_group_counts.front();
    std::size_t modal_count = 0;
    for (const std::size_t groups : result.seed_group_counts) {
        if (frequencies[groups] > modal_count) {
            modal_groups = groups;
            modal_count = frequencies[groups];
        }
    }
    const auto representative =
        std::find(result.seed_group_counts.begin(), result.seed_group_counts.end(), modal_groups);
    const std::size_t representative_index =
        static_cast<std::size_t>(std::distance(result.seed_group_counts.begin(), representative));
    result.representative_seed_order = representative_index;
    result.coarse_groups = modal_groups;
    result.modal_fraction = static_cast<double>(modal_count) / static_cast<double>(options.coarse_seeds);
    result.fine_groups = number_of_groups(runs[fine_index].labels);
    result.coarse_labels = runs[representative_index].labels;
    result.fine_labels = runs[fine_index].labels;
    result.other_count =
        static_cast<std::size_t>(std::count(result.coarse_labels.begin(), result.coarse_labels.end(), "other"));
    result.outlier_count =
        static_cast<std::size_t>(std::count(result.coarse_labels.begin(), result.coarse_labels.end(), "outlier"));
    result.unstable = result.modal_fraction < kModalConfidence;
    const auto [minimum_groups, maximum_groups] =
        std::minmax_element(result.seed_group_counts.begin(), result.seed_group_counts.end());
    result.minimum_seed_groups = *minimum_groups;
    result.maximum_seed_groups = *maximum_groups;

    std::vector<std::size_t> modal_indices;
    for (std::size_t index = 0; index < result.seed_group_counts.size(); ++index) {
        if (result.seed_group_counts[index] == modal_groups) {
            modal_indices.push_back(index);
        }
    }
    std::vector<double> aris;
    for (std::size_t first = 0; first < modal_indices.size(); ++first) {
        for (std::size_t second = first + 1; second < modal_indices.size(); ++second) {
            aris.push_back(adjusted_rand_index(runs[modal_indices[first]].labels, runs[modal_indices[second]].labels));
        }
    }
    result.modal_assignment_pair_count = aris.size();
    if (!aris.empty()) {
        result.modal_assignment_ari_median = percentile_linear(aris, 50.0);
        result.modal_assignment_ari_minimum = *std::min_element(aris.begin(), aris.end());
    }
    result.all_modal_assignment_pairs_ari_at_least_0_8 =
        result.modal_assignment_ari_minimum >= kMinimumModalAssignmentAri;
    result.hidden_heterogeneity =
        static_cast<double>(result.other_count) / static_cast<double>(result.coarse_labels.size()) >
        kHiddenHeterogeneityFraction;
    result.stable_null_multigroup = result.coarse_groups >= 2 && !result.unstable &&
                                    result.modal_assignment_ari_minimum >= kMinimumModalAssignmentAri;
    result.coarse_runs.reserve(options.coarse_seeds);
    for (std::size_t index = 0; index < options.coarse_seeds; ++index) {
        result.coarse_runs.push_back(M1RunEvidence{
            index,
            options.base_seed + index * 101U,
            result.seed_group_counts[index],
            runs[index].labels,
            runs[index].trace,
        });
    }
    result.fine_run = M1RunEvidence{
        0, options.base_seed, result.fine_groups, runs[fine_index].labels, runs[fine_index].trace,
    };
    result.representative_coarse_trace = runs[representative_index].trace;
    result.fine_trace = runs[fine_index].trace;
    std::vector<std::string> ordinal_keys(result.coarse_labels.size());
    for (std::size_t index = 0; index < ordinal_keys.size(); ++index) {
        ordinal_keys[index] = "row:" + std::to_string(index);
    }
    result.partition_sha256 = partition_digest(ordinal_keys, result.coarse_labels);
    result.trace_sha256 = trace_digest(runs, result.seed_group_counts, representative_index);
    return result;
}

M1Analysis analyze_phylo_canonical(const Matrix& distance, const Matrix& methylation,
                                   const std::vector<std::string>& stable_keys, const M1Options& options) {
    if (distance.size() != stable_keys.size() || methylation.size() != stable_keys.size()) {
        throw std::invalid_argument("canonical M1 keys must match distance and methylation rows");
    }
    const auto order = stable_key_order(stable_keys);
    const Matrix ordered_distance = subset_square(distance, order);
    const Matrix ordered_methylation = subset_rows(methylation, order);
    std::vector<std::string> ordered_keys;
    ordered_keys.reserve(order.size());
    for (const std::size_t index : order) {
        ordered_keys.push_back(stable_keys[index]);
    }
    M1Analysis result = analyze_phylo(ordered_distance, ordered_methylation, options);
    result.partition_sha256 = partition_digest(ordered_keys, result.coarse_labels);
    return result;
}

FullM1Result run_full_m1(const Matrix& methylation, const std::vector<std::string>& stable_keys,
                         const M1Options& options) {
    return run_full_m1_with_observed_distance(bernoulli_distance(methylation), methylation, stable_keys, options);
}

FullM1Result run_historical_m1(const Matrix& raw_point_methylation, const std::vector<std::string>& stable_keys,
                               const M1Options& options) {
    const Matrix observed = historical_distance_matrix_round6(bernoulli_distance(raw_point_methylation));
    const Matrix null_methylation = historical_methylation_matrix_round4(raw_point_methylation);
    return run_full_m1_with_observed_distance(observed, null_methylation, stable_keys, options);
}

FullM1Result run_full_m1_with_observed_distance(const Matrix& observed_distance, const Matrix& null_methylation,
                                                const std::vector<std::string>& stable_keys, const M1Options& options) {
    validate_square(observed_distance, "observed distance");
    validate_rectangular(null_methylation, "null methylation");
    if (observed_distance.size() != stable_keys.size() || null_methylation.size() != stable_keys.size()) {
        throw std::invalid_argument("full M1 keys must match observed distance and null methylation rows");
    }
    const auto order = stable_key_order(stable_keys);
    const Matrix ordered_distance = subset_square(observed_distance, order);
    const Matrix ordered_methylation = subset_rows(null_methylation, order);
    std::vector<std::string> ordered_keys;
    ordered_keys.reserve(order.size());
    for (const std::size_t index : order) {
        ordered_keys.push_back(stable_keys[index]);
    }

    FullM1Result result;
    if (ordered_methylation.size() < kMinimumReads) {
        result.status = M1ReadsetStatus::kInsufficientAltReads;
        return result;
    }
    result.distance = ordered_distance;
    result.retained_indices = peel_complete(result.distance);
    if (result.retained_indices.size() < kMinimumReads) {
        result.status = M1ReadsetStatus::kIncompleteDistanceBelowMinimum;
        return result;
    }
    const Matrix retained_distance = subset_square(result.distance, result.retained_indices);
    const Matrix retained_methylation = subset_rows(ordered_methylation, result.retained_indices);
    result.retained_keys.reserve(result.retained_indices.size());
    for (const std::size_t index : result.retained_indices) {
        result.retained_keys.push_back(ordered_keys[index]);
    }
    result.forced = forced_silhouette_split(retained_distance);
    result.analysis = analyze_phylo(retained_distance, retained_methylation, options);
    result.analysis->partition_sha256 = partition_digest(result.retained_keys, result.analysis->coarse_labels);
    result.status = M1ReadsetStatus::kFullClusteringReady;
    return result;
}

}  // namespace longlineage::m1
