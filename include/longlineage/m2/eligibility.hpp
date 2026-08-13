// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace longlineage::m2 {

inline constexpr std::size_t kPermutations = 499;
inline constexpr double kPermutationPFloor = 1.0 / static_cast<double>(kPermutations + 1);
inline constexpr double kPMaximumExclusive = 0.05;
inline constexpr double kTargetPower = 0.80;
inline constexpr double kPowerAlpha = 0.05;
inline constexpr std::size_t kMinimumGroupN = 5;
inline constexpr std::size_t kMaximumGroups = 10;

enum class AxisKind {
    kCategorical,
    kContinuous,
};

[[nodiscard]] double categorical_power(std::size_t n, std::size_t groups, std::size_t planning_levels,
                                       double effect_threshold);
[[nodiscard]] double continuous_power(std::size_t n, std::size_t groups, double effect_threshold);
[[nodiscard]] std::size_t minimum_n_for_target_power(AxisKind kind, std::size_t groups, std::size_t planning_levels,
                                                     double effect_threshold);

enum class AxisStatus {
    kAlignedEffectAndPermutationPPass,
    kIndeterminateEffectAboveThresholdWithoutP,
    kIndeterminateInsufficientInformation,
    kIndeterminateAxisStatisticMissing,
    kNotAlignedConstantAxis,
    kNotAlignedEffectBelowThresholdAdequatePower,
};

[[nodiscard]] std::string_view to_string(AxisStatus status) noexcept;

struct AxisInput {
    AxisKind kind{AxisKind::kContinuous};
    std::size_t n{0};
    std::size_t groups{0};
    std::size_t minimum_observed_group_n{0};
    std::size_t observed_category_levels{0};
    std::size_t planning_category_levels{0};
    double effect_threshold{0.0};
    std::optional<double> effect;
    std::optional<double> permutation_p;
    bool declared_aligned{false};
};

struct AxisDecision {
    AxisStatus status{AxisStatus::kIndeterminateAxisStatisticMissing};
    std::optional<double> power_at_effect_threshold;
    std::optional<std::size_t> minimum_n;
    bool recomputed_aligned{false};
    bool adequate_information_for_non_alignment{false};
    bool positive_alignment_overrides_power{false};
};

[[nodiscard]] AxisDecision classify_axis(const AxisInput& input);

struct AssociationResult {
    std::optional<double> effect;
    std::optional<double> permutation_p;
    std::size_t n{0};
    std::size_t observed_levels{0};
    std::size_t observed_groups{0};
    std::size_t exceedance{0};
    bool aligned{false};
    std::string trace_sha256;
};

[[nodiscard]] AssociationResult categorical_permutation_association(const std::vector<std::string>& values,
                                                                    const std::vector<std::string>& labels,
                                                                    std::uint64_t seed,
                                                                    std::size_t permutations = kPermutations);
[[nodiscard]] AssociationResult continuous_permutation_association(const std::vector<double>& values,
                                                                   const std::vector<std::string>& labels,
                                                                   std::uint64_t seed,
                                                                   std::size_t permutations = kPermutations);

struct EightAxisInput {
    std::vector<std::string> stable_keys;
    std::vector<std::string> labels;
    std::vector<std::string> hp_exact;
    std::vector<std::string> hp_family;
    std::vector<std::string> strand;
    std::vector<double> start;
    std::vector<double> end;
    std::vector<double> length;
    std::vector<double> mapq;
    std::vector<double> cpg_called;
    std::uint32_t base_seed{0};
};

struct NamedAxisEvidence {
    std::string name;
    AxisKind kind{AxisKind::kContinuous};
    AssociationResult association;
    AxisDecision decision;
};

struct EightAxisResult {
    std::vector<NamedAxisEvidence> axes;
    bool hp_axis_confound{false};
    bool technical_axis_confound{false};
    bool axis_indeterminate{false};
    bool residual_unexplained{false};
    std::string trace_sha256;
};

// All eight arrays must be complete and aligned to core labels. Missing values
// are supported by the individual association functions for parity testing,
// but the integrated M2 gate rejects count drift fail-closed.
[[nodiscard]] EightAxisResult evaluate_eight_axes(const EightAxisInput& input);

enum class M2Status {
    kNotRun,
    kNotEvaluable,
    kFail,
    kPass,
};

enum class M2Reason {
    kM1NotFlagged,
    kGroupCountExceedsPlanningMaximum,
    kAxisIndeterminate,
    kHpAxisConfound,
    kTechnicalAxisConfound,
    kNotPhaseAnchoredRobust,
    kAllMeasuredAxesDeterminateNoAlignedConfound,
};

[[nodiscard]] std::string_view to_string(M2Status status) noexcept;
[[nodiscard]] std::string_view to_string(M2Reason reason) noexcept;

struct M2PrecedenceInput {
    bool m1_flagged{false};
    std::size_t group_count{0};
    bool axis_indeterminate{false};
    bool hp_axis_confound{false};
    bool technical_axis_confound{false};
    bool residual_unexplained{false};
    bool phase_anchored_robust{false};
};

struct M2Decision {
    M2Status status{M2Status::kNotRun};
    M2Reason reason{M2Reason::kM1NotFlagged};
    bool evaluable{false};
    bool eligible{false};
};

[[nodiscard]] M2Decision evaluate_precedence(const M2PrecedenceInput& input);

}  // namespace longlineage::m2
