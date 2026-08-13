// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/cooccurrence/statistics.hpp"
#include "longlineage/io/variant_sites.hpp"
#include "longlineage/pipeline/block_science.hpp"
#include "longlineage/pipeline/site_science.hpp"

namespace longlineage::cooccurrence {

enum class PairFamilyStatus {
    kIneligibleM2Screen,
    kEligibleM2EndpointANotTestable,
    kEligibleM2ExactNotIdentifiable,
    kEligibleM2ExactFamily,
};

[[nodiscard]] std::string_view to_string(PairFamilyStatus status) noexcept;

enum class ConditionalStatus {
    kNotRunNotExactByDiscovery,
    kNotIdentifiableDegenerateTable,
    kNotIdentifiableNonexchangeable,
    kPermutable,
};

[[nodiscard]] std::string_view to_string(ConditionalStatus status) noexcept;

struct ConditionalPermutationPayload {
    std::vector<std::string> labels;
    std::vector<AlleleCall> categories;
    std::vector<std::string> strata;
};

struct ConditionalPermutationResult {
    std::optional<double> p_value;
    std::uint64_t permutations = 0;
    std::uint64_t exceedance = 0;
    std::uint64_t strata = 0;
    std::uint64_t exchangeable_strata = 0;
    std::uint64_t informative_exchangeable_strata = 0;
    bool permutable = false;
    ConditionalStatus status{ConditionalStatus::kNotRunNotExactByDiscovery};
};

struct PairInference {
    PairInference(VariantSite focal_value, VariantSite partner_value)
        : focal(std::move(focal_value)), partner(std::move(partner_value)) {}

    VariantSite focal;
    VariantSite partner;
    std::int64_t distance_bp = 0;
    std::size_t group_count = 0;
    GroupAlleleAssociation endpoint_a;
    ExactKx2Result exact;
    PairFamilyStatus family_status{PairFamilyStatus::kIneligibleM2Screen};
    std::size_t fdr_family_size = 0;
    std::optional<double> q_global_bh;
    std::optional<double> q_global_by;
    bool effect_gate_pass = false;
    bool exact_bh_discovery = false;
    bool exact_by_discovery = false;
    ConditionalPermutationPayload conditional_payload;
    ConditionalPermutationResult conditional;
    BinaryCategoryAssociation callability;
    std::uint64_t noncallable_core_reads = 0;
    std::optional<double> callability_q_global_bh;
    std::optional<double> callability_q_global_by;
    CallabilityStatus callability_status{CallabilityStatus::kNotIdentifiableDifferentialCallability};
    std::uint64_t pair_read_count = 0;
    FourStateSummary endpoint_b;
    bool formal_pair_by_confirmed = false;
    std::string semantic_sha256;
};

struct SiteCooccurrenceResult {
    std::vector<PairInference> pairs;
    std::size_t partner_universe_size = 0;
    std::string semantic_sha256;
};

// Builds every directed focal->partner pair in the complete same-contig,
// inclusive +/-5000 bp frozen marker universe. Pair rows are emitted for every
// stable M1 site; M2 eligibility controls only membership in the formal exact
// FDR family.
[[nodiscard]] ParseResult<SiteCooccurrenceResult> build_site_cooccurrence(const pipeline::BlockScienceEvidence& block,
                                                                          const pipeline::FocalSiteEvidence& focal,
                                                                          const pipeline::SiteScienceResult& science);

// Applies the complete global endpoint-A and callability multiple-testing
// families. Calling this on a subset would change q-values and is forbidden for
// task type B/C production claims.
[[nodiscard]] ParseResult<bool> finalize_global_pair_families(std::vector<PairInference>& pairs);

// Secondary sensitivity: 999 within-stratum label permutations only for
// endpoint-A exact-BY discoveries, followed by the effect and callability
// gates. The deterministic seed matches the frozen SHA-256 separator contract.
[[nodiscard]] ParseResult<bool> run_conditional_pair_sensitivity(std::vector<PairInference>& pairs,
                                                                 std::uint64_t permutations = 999);

}  // namespace longlineage::cooccurrence
