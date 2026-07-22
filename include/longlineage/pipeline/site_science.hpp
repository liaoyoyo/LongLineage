// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/m1/science.hpp"
#include "longlineage/m2/eligibility.hpp"
#include "longlineage/pipeline/block_science.hpp"
#include "longlineage/pipeline/site_matrix.hpp"

namespace longlineage::pipeline {

enum class M1Representation {
    // Production sensitivity: no legacy CSV serialization boundaries.
    kRawBinary32Point,
    // Exact historical comparison: observed distance round6, null matrix
    // round4. This must never be silently substituted for the raw endpoint.
    kHistoricalObservedRound6NullRound4,
};

struct SiteScienceOptions {
    M1Representation m1_representation{M1Representation::kHistoricalObservedRound6NullRound4};
    m1::M1Options m1_options{};
};

struct M1ReadAssignment {
    std::string read_id;
    std::size_t block_read_index = 0;
    std::size_t matrix_row_index = 0;
    std::size_t retained_m1_row_index = 0;
    std::size_t called_cpg_count = 0;
    std::string coarse_label;
    std::string fine_label;
    bool core = false;
};

struct SiteM2Science {
    std::optional<m2::EightAxisResult> axes;
    m2::M2Decision decision;
    std::size_t core_read_count = 0;
    std::size_t minimum_core_group_size = 0;
    double phase_anchored_fraction_core = 0.0;
    bool phase_anchored_robust = false;
};

struct SiteScienceResult {
    SiteMethylationMatrix matrix;
    m1::FullM1Result m1;
    std::vector<M1ReadAssignment> assignments;
    SiteM2Science m2;
    std::string semantic_sha256;
};

// Frozen HP-family mapping used by the M2 categorical axis. Unknown or missing
// tokens are deliberately mapped to "untagged", matching the governed table.
[[nodiscard]] std::string_view hp_family_for_token(std::string_view hp_token) noexcept;

// Runs the complete per-site M1/M2 decision path from truth-blind block
// evidence. The matrix is always built from raw binary32 ML/255 points; the
// chosen M1 representation controls only the explicit clustering boundary.
[[nodiscard]] ParseResult<SiteScienceResult> run_site_science(const BlockScienceEvidence& block,
                                                              const FocalSiteEvidence& focal,
                                                              const SiteScienceOptions& options = {});

}  // namespace longlineage::pipeline
