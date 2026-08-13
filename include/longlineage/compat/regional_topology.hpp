// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/io/variant_sites.hpp"

namespace longlineage::compat {

inline constexpr std::string_view kRegionalProfileId = "PYTHON_V2_DESCRIPTIVE_REGIONAL";
inline constexpr std::uint64_t kRegionalGapBp = 50'000;
inline constexpr std::size_t kRegionalMaximumSites = 8;
inline constexpr std::uint64_t kRegionalMinimumPatternReads = 3;
inline constexpr std::size_t kLegacyExtraNodeCap = 4;
inline constexpr std::uint64_t kLegacyPerLevelBudget = 150'000;

struct RegionalRegionPlan {
    std::uint64_t region_order = 0;
    std::string region_id;
    std::string chrom;
    std::uint64_t start1 = 0;
    std::uint64_t end1 = 0;
    std::uint64_t span = 0;
    std::size_t pre_cap_site_count = 0;
    std::size_t cap_excluded_site_count = 0;
    std::vector<VariantSite> selected_sites;
};

struct RegionalPlanCensus {
    std::uint64_t scope_sites = 0;
    std::uint64_t positional_singletons = 0;
    std::uint64_t multi_region_count = 0;
    std::uint64_t multi_region_pre_cap_sites = 0;
    std::uint64_t capped_region_count = 0;
    std::uint64_t cap_excluded_sites = 0;
    std::uint64_t retained_sites = 0;
};

struct RegionalPlan {
    std::vector<RegionalRegionPlan> regions;
    RegionalPlanCensus census;
};

// Reproduces frozen Python-v2 grouping: adjacent gap <= 50 kb is transitive,
// total span is unbounded, and a >8-site component contributes only the first
// minimum-span consecutive eight-site window.
[[nodiscard]] ParseResult<RegionalPlan> make_python_v2_regional_plan(const VariantSiteSet& variants);

[[nodiscard]] std::string python_v2_hp_family(std::string_view raw_hp);

struct LegacySolverInput {
    std::size_t site_count = 0;
    std::map<std::string, std::uint64_t> supported_full_patterns;
    std::map<std::string, std::uint64_t> supported_subread_patterns;
};

struct LegacySolverResult {
    std::uint64_t n_hidden = 0;
    std::uint64_t n_trees = 0;
    std::uint64_t n_feasible_node_sets = 0;
    std::uint64_t n_recurrence_free_node_sets = 0;
    std::uint64_t n_recurrent_node_sets = 0;
    bool capped = false;
    bool has_recurrence_free = false;
    bool has_recurrence = false;
    std::string cap_reason;
    std::string classification;
};

// Exact compatibility implementation of the frozen Python solver's node-set
// enumeration and analytical parent-mapping count. Pattern counts have already
// crossed MINREAD and are intentionally not objective weights.
[[nodiscard]] ParseResult<LegacySolverResult> solve_python_v2_legacy(const LegacySolverInput& input);

[[nodiscard]] bool pattern_is_mutation_bearing(const LegacySolverInput& input) noexcept;

[[nodiscard]] std::string python_v2_unit_role(std::string_view family, bool mutation_bearing);

}  // namespace longlineage::compat
