// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace longlineage::m1 {

using Matrix = std::vector<std::vector<double>>;

inline constexpr std::size_t kMinimumGroupSize = 3;
inline constexpr std::size_t kMinimumReads = 2 * kMinimumGroupSize;
inline constexpr double kMinimumWeightSum = 1e-9;
inline constexpr double kMinimumSeparation = 1.3;
inline constexpr std::size_t kNullReplicates = 40;
inline constexpr std::size_t kMinimumValidNullReplicates = 32;
inline constexpr std::size_t kCoarseSeeds = 10;
inline constexpr double kModalConfidence = 0.7;
inline constexpr double kMinimumModalAssignmentAri = 0.8;
inline constexpr double kHiddenHeterogeneityFraction = 0.30;

// Raw M1 points first materialize binary32 ML_raw / 255.0F.
// Historical Python assignment then reads RegionWriter fixed-setprecision(4)
// CSV values. Both are deliberately distinct from the SAM-spec uncertainty
// interval [N/256, (N+1)/256) retained by P2.
[[nodiscard]] double ml_raw_to_frozen_m1_point(std::uint8_t ml_raw) noexcept;
[[nodiscard]] double historical_matrix_point_round4(double raw_point);
[[nodiscard]] double ml_raw_to_historical_matrix_point_round4(std::uint8_t ml_raw);
[[nodiscard]] Matrix historical_methylation_matrix_round4(const Matrix& raw_point_methylation);
[[nodiscard]] Matrix historical_distance_matrix_round6(const Matrix& raw_distance);

[[nodiscard]] std::uint32_t stable_site_seed(std::string_view sample, std::string_view contig, std::uint64_t position1,
                                             std::int64_t offset);

// NumPy 1.23.5 PCG64 compatibility for integer entropy accepted by
// np.random.Generator(np.random.PCG64(seed)). The class intentionally exposes
// only the operations frozen by P3 vectors.
class NumpyPcg64 {
   public:
    explicit NumpyPcg64(std::uint64_t seed);

    [[nodiscard]] std::uint64_t next_u64() noexcept;
    [[nodiscard]] std::uint32_t next_u32() noexcept;
    [[nodiscard]] std::vector<std::size_t> permutation(std::size_t size);

   private:
    __extension__ typedef unsigned __int128 Uint128;

    [[nodiscard]] std::uint32_t bounded_u32(std::uint32_t maximum_inclusive) noexcept;

    Uint128 state_{0};
    Uint128 increment_{0};
    bool has_cached_u32_{false};
    std::uint32_t cached_u32_{0};
};

[[nodiscard]] Matrix bernoulli_distance(const Matrix& methylation);
[[nodiscard]] std::vector<std::size_t> peel_complete(const Matrix& distance);

struct LinkageRow {
    std::size_t first;
    std::size_t second;
    double distance;
    std::size_t cluster_size;
};

// Frozen SciPy-1.13.1 average-linkage semantics, including nearest-neighbor
// chain tie preference, stable distance sorting and union-find relabeling.
[[nodiscard]] std::vector<LinkageRow> average_linkage(const Matrix& distance);
[[nodiscard]] std::vector<int> flat_clusters_maxclust(const std::vector<LinkageRow>& linkage, std::size_t leaves,
                                                      std::size_t maximum_clusters);

[[nodiscard]] double percentile_linear(std::vector<double> values, double percentile);
[[nodiscard]] double adjusted_rand_index(const std::vector<std::string>& lhs, const std::vector<std::string>& rhs);
[[nodiscard]] std::string partition_digest(const std::vector<std::string>& stable_keys,
                                           const std::vector<std::string>& labels);

enum class M1ReadsetStatus {
    kInsufficientAltReads,
    kIncompleteDistanceBelowMinimum,
    kPrimitiveParityReady,
    kFullClusteringReady,
};

struct M1Preparation {
    M1ReadsetStatus status{M1ReadsetStatus::kInsufficientAltReads};
    Matrix distance;
    std::vector<std::size_t> retained_indices;
    std::vector<LinkageRow> linkage;
    bool full_clustering_abstained{true};
};

// This first-stage state machine proves input/distance/peel/linkage parity but
// explicitly abstains from recursive null clustering and final assignments.
[[nodiscard]] M1Preparation prepare_minimal_clustering(const Matrix& methylation);

enum class SplitFailure {
    kNone,
    kBelowSeparationMinimumOrUndefined,
    kInsufficientValidNull,
    kNotAboveNullThreshold,
    kEmpiricalPAboveAlpha,
};

struct SplitDecision {
    std::optional<double> observed_between_within;
    double null_percentile{95.0};
    std::size_t null_replicates_requested{kNullReplicates};
    std::size_t minimum_valid_null{kMinimumValidNullReplicates};
    std::size_t valid_null_replicates{0};
    std::optional<std::size_t> exceedance;
    std::optional<double> null_threshold;
    std::optional<double> empirical_p;
    bool passed{false};
    SplitFailure failure{SplitFailure::kBelowSeparationMinimumOrUndefined};
};

[[nodiscard]] SplitDecision evaluate_split(std::optional<double> observed_between_within,
                                           const std::vector<double>& valid_null_ratios, double null_percentile,
                                           std::optional<double> empirical_alpha = std::nullopt);

struct ForcedSilhouetteSplit {
    std::size_t groups{0};
    std::optional<double> silhouette;
    std::vector<int> labels;
};

[[nodiscard]] ForcedSilhouetteSplit forced_silhouette_split(const Matrix& distance);

struct SplitTrace {
    std::size_t node_size{0};
    std::size_t first_child_size{0};
    std::size_t second_child_size{0};
    SplitDecision decision;
};

// Complete governed replay payload for one recursive clustering run. Keeping
// every seed's labels and split trace is intentional: m1_assignments.jsonl
// must permit an independent validator to replay the ten coarse seeds and the
// fine run without consulting producer-private state.
struct M1RunEvidence {
    std::size_t seed_order{0};
    std::uint64_t seed{0};
    std::size_t group_count{0};
    std::vector<std::string> labels;
    std::vector<SplitTrace> split_trace;
};

struct M1Options {
    std::uint64_t base_seed{20260622};
    std::size_t coarse_seeds{kCoarseSeeds};
    std::size_t null_replicates{kNullReplicates};
    double minimum_valid_null_fraction{0.8};
    std::optional<double> empirical_alpha;
    std::size_t workers{1};
};

struct M1Analysis {
    std::size_t coarse_groups{0};
    double modal_fraction{0.0};
    std::size_t fine_groups{0};
    std::size_t other_count{0};
    std::size_t outlier_count{0};
    bool unstable{false};
    std::size_t minimum_seed_groups{0};
    std::size_t maximum_seed_groups{0};
    std::vector<std::size_t> seed_group_counts;
    std::size_t modal_assignment_pair_count{0};
    double modal_assignment_ari_median{1.0};
    double modal_assignment_ari_minimum{1.0};
    bool all_modal_assignment_pairs_ari_at_least_0_8{true};
    bool hidden_heterogeneity{false};
    bool stable_null_multigroup{false};
    std::size_t representative_seed_order{0};
    std::vector<M1RunEvidence> coarse_runs;
    M1RunEvidence fine_run;
    std::vector<std::string> coarse_labels;
    std::vector<std::string> fine_labels;
    std::vector<SplitTrace> representative_coarse_trace;
    std::vector<SplitTrace> fine_trace;
    std::string partition_sha256;
    std::string trace_sha256;
};

// Historical-order compatibility entry point. Independent seed runs may execute
// in parallel, but result collection follows frozen seed order.
[[nodiscard]] M1Analysis analyze_phylo(const Matrix& distance, const Matrix& methylation,
                                       const M1Options& options = {});

// Production-facing deterministic entry point: inputs are first sorted by
// stable key, so scheduling and upstream row order cannot affect assignments or
// semantic digests. Returned labels follow sorted_keys.
[[nodiscard]] M1Analysis analyze_phylo_canonical(const Matrix& distance, const Matrix& methylation,
                                                 const std::vector<std::string>& stable_keys,
                                                 const M1Options& options = {});

struct FullM1Result {
    M1ReadsetStatus status{M1ReadsetStatus::kInsufficientAltReads};
    Matrix distance;
    std::vector<std::size_t> retained_indices;
    std::vector<std::string> retained_keys;
    ForcedSilhouetteSplit forced;
    std::optional<M1Analysis> analysis;
};

// Computes BERNOULLI distance from the supplied representation. Callers must
// select raw-point or historical-round4 explicitly before invoking this API.
[[nodiscard]] FullM1Result run_full_m1(const Matrix& methylation, const std::vector<std::string>& stable_keys,
                                       const M1Options& options = {});

// Exact legacy assignment replay: observed BERNOULLI distances are computed
// from raw binary32 points and serialized to six decimals, while column-null
// methylation is independently serialized to four decimals.
[[nodiscard]] FullM1Result run_historical_m1(const Matrix& raw_point_methylation,
                                             const std::vector<std::string>& stable_keys,
                                             const M1Options& options = {});

// For bounded replay of already serialized historical artifacts. The observed
// distance and null methylation representations remain explicit.
[[nodiscard]] FullM1Result run_full_m1_with_observed_distance(const Matrix& observed_distance,
                                                              const Matrix& null_methylation,
                                                              const std::vector<std::string>& stable_keys,
                                                              const M1Options& options = {});

}  // namespace longlineage::m1
