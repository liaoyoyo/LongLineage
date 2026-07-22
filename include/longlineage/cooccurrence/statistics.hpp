// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/common/types.hpp"

namespace longlineage::cooccurrence {

inline constexpr std::uint64_t kExactStateSpaceCeiling = 250000;

enum class ExactStateStatus {
    kExactEnumerated,
    kNotIdentifiableStateSpaceLimit,
    kNotIdentifiableDegenerateTable,
};

[[nodiscard]] std::string_view to_string(ExactStateStatus status) noexcept;

struct ExactKx2Result {
    ExactStateStatus status{ExactStateStatus::kNotIdentifiableDegenerateTable};
    std::optional<double> p_value;
    std::optional<std::uint64_t> state_space_size;
    std::optional<std::uint64_t> state_space_lower_bound;
    std::optional<double> observed_table_probability;
    std::uint64_t state_space_ceiling{kExactStateSpaceCeiling};
    bool identifiable{false};
};

using Kx2Row = std::array<std::uint64_t, 2>;

[[nodiscard]] ExactKx2Result fisher_freeman_halton_kx2(const std::vector<Kx2Row>& table,
                                                       std::uint64_t state_space_ceiling = kExactStateSpaceCeiling);

[[nodiscard]] std::vector<std::optional<double>> benjamini_hochberg(const std::vector<std::optional<double>>& p_values);
[[nodiscard]] std::vector<std::optional<double>> benjamini_yekutieli(
    const std::vector<std::optional<double>>& p_values);

class JointAlleleCounts {
   public:
    void add(AlleleCall focal, AlleleCall partner);

    [[nodiscard]] std::uint64_t at(AlleleCall focal, AlleleCall partner) const noexcept;
    [[nodiscard]] std::uint64_t total() const noexcept;
    [[nodiscard]] const std::array<std::uint64_t, 16>& row_major_cells() const noexcept { return cells_; }
    [[nodiscard]] bool conservation_holds(std::uint64_t expected_pairs) const noexcept;

   private:
    [[nodiscard]] static std::size_t index(AlleleCall focal, AlleleCall partner) noexcept;

    std::array<std::uint64_t, 16> cells_{};
};

enum class CallabilityStatus {
    kPassAllCoreReadsCallable,
    kPassNoStrongDifferentialCallability,
    kFailDifferentialCallabilitySignal,
    kNotIdentifiableDifferentialCallability,
    kNotIdentifiableMissingCallabilityStatistic,
};

[[nodiscard]] std::string_view to_string(CallabilityStatus status) noexcept;

struct CallabilityInput {
    std::uint64_t noncallable_core_reads{0};
    bool exact_testable{false};
    std::optional<double> by_q;
    std::optional<double> cramers_v;
};

// Frozen precedence: no noncallable -> not testable -> missing statistic ->
// BY<=0.05 or V>=0.30 -> fail for differential callability signal.
[[nodiscard]] CallabilityStatus evaluate_callability(const CallabilityInput& input);

struct GroupAlleleAssociation {
    std::vector<std::string> groups;
    std::vector<Kx2Row> table;
    std::uint64_t n_informative{0};
    std::uint64_t minimum_group_n{0};
    std::uint64_t ref_n{0};
    std::uint64_t alt_n{0};
    bool testable{false};
    std::string reason{"INSUFFICIENT_JOINT_INFORMATION"};
    std::optional<double> cramers_v;
    std::optional<double> delta_alt_fraction;
};

// O/X calls remain outside the Kx2 effect endpoint, but callers must retain
// them separately in JointAlleleCounts for callability and four-state output.
[[nodiscard]] GroupAlleleAssociation summarize_group_allele_association(const std::vector<std::string>& labels,
                                                                        const std::vector<AlleleCall>& partner_calls,
                                                                        std::uint64_t minimum_total = 10,
                                                                        std::uint64_t minimum_group = 3,
                                                                        std::uint64_t minimum_allele = 3);

struct BinaryCategoryAssociation {
    GroupAlleleAssociation summary;
    std::optional<double> p_analytic;
    std::string analytic_method;
};

// Historical callability guardrail: a two-group table uses a two-sided Fisher
// exact p-value; K>2 uses the uncorrected Pearson chi-square survival
// probability. This descriptive test enters a separate global callability FDR
// family and is not the endpoint-A formal fixed-margin test.
[[nodiscard]] BinaryCategoryAssociation summarize_binary_category_association(const std::vector<std::string>& labels,
                                                                              const std::vector<bool>& second_category,
                                                                              std::uint64_t minimum_total = 10,
                                                                              std::uint64_t minimum_group = 3,
                                                                              std::uint64_t minimum_category = 3);

enum class FixedErrorGateStatus {
    kCompatibleWithFixedErrorCeiling,
    kViolatesFixedErrorCeiling,
    kNotIdentifiableLowPrecision,
    kNotIdentifiableNoDenominator,
};

[[nodiscard]] std::string_view to_string(FixedErrorGateStatus status) noexcept;

struct FixedErrorGateResult {
    std::uint64_t violations{0};
    std::uint64_t denominator{0};
    std::optional<double> rate;
    std::optional<double> p_exact_greater;
    std::optional<double> upper_bound;
    double threshold{0.02};
    double confidence{0.0};
    FixedErrorGateStatus status{FixedErrorGateStatus::kNotIdentifiableNoDenominator};
};

enum class RelationModel {
    kFocalAncestor,
    kPartnerAncestor,
    kBranching,
};

[[nodiscard]] std::string_view to_string(RelationModel model) noexcept;

enum class RelationCompatibilityStatus {
    kFocalAncestorCompatible,
    kPartnerAncestorCompatible,
    kBranchingCompatible,
    kMultipleModelsCompatible,
    kIncompatibleOrComplex,
    kNotIdentifiableInsufficientFourStateDepth,
    kNotIdentifiableFixedErrorCeiling,
};

[[nodiscard]] std::string_view to_string(RelationCompatibilityStatus status) noexcept;

struct FourStateSummary {
    JointAlleleCounts counts;
    std::uint64_t called_ra_depth{0};
    std::uint64_t focal_ref{0};
    std::uint64_t focal_alt{0};
    std::uint64_t partner_ref{0};
    std::uint64_t partner_alt{0};
    FixedErrorGateResult focal_ancestor;
    FixedErrorGateResult partner_ancestor;
    FixedErrorGateResult branching;
    double error_ceiling{0.02};
    double per_relation_confidence{0.0};
    double familywise_confidence{0.95};
    std::uint64_t minimum_zero_violation_depth{0};
    bool complete_four_state_testable{false};
    RelationCompatibilityStatus compatibility{RelationCompatibilityStatus::kNotIdentifiableInsufficientFourStateDepth};
    std::vector<RelationModel> compatible_models;
};

// Implements the Bonferroni-adjusted three-relation fixed-error sensitivity.
// A compatible model is not a claim of cellular ancestry or unique order.
[[nodiscard]] FourStateSummary summarize_four_state_relations(const JointAlleleCounts& counts,
                                                              double error_ceiling = 0.02,
                                                              double familywise_confidence = 0.95);

}  // namespace longlineage::cooccurrence
