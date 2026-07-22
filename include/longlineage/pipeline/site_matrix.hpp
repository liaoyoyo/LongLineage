// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/pipeline/block_science.hpp"

namespace longlineage::pipeline {

enum class M1PointMode {
    // New in-memory contract: reproduce the historical binary32 ML/255
    // computation but do not serialize and parse it.
    kFloat32RawDiv255,
    // Backward-comparison contract: additionally reproduce the legacy
    // methylation.csv fixed four-decimal serialization boundary.
    kLegacyCsvRound4,
};

struct SiteMethylationMatrix {
    std::uint64_t site_order = 0;
    std::vector<std::size_t> read_indices;
    std::vector<std::size_t> alt_row_indices;
    std::vector<Position1> cpg_positions;
    std::vector<std::vector<std::optional<double>>> values;
    std::vector<std::size_t> called_cpg_counts;
};

// Reconstructs the site-specific sparse matrix without materializing per-site
// CSV files. R/A reads define the legacy matrix row/column universe; O/X are
// retained in site_reads/co-occurrence evidence but cannot enter M1.
//
// The CpG window is the clipped, inclusive focal +/- halo interval represented
// as a zero-based half-open Interval0. Duplicate read/CpG observations fail
// closed.
[[nodiscard]] ParseResult<SiteMethylationMatrix> build_site_methylation_matrix(
    const BlockScienceEvidence& block, const FocalSiteEvidence& focal, std::uint64_t halo_bp = 5000,
    M1PointMode point_mode = M1PointMode::kFloat32RawDiv255);

[[nodiscard]] double m1_point_from_ml(std::uint8_t ml_raw, M1PointMode mode) noexcept;

}  // namespace longlineage::pipeline
