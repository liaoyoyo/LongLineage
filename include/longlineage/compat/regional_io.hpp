// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/compat/regional_topology.hpp"

namespace longlineage::compat {

struct RegionalCompatibilityOptions {
    std::filesystem::path repository_root;
    std::filesystem::path source_manifest;
    std::string dataset_id;
    std::string run_id;
    std::filesystem::path output_directory;
    std::size_t workers = 1;
    std::size_t queue_capacity_bytes = 64U * 1024U * 1024U;
    std::size_t first_region = 0;
    // Zero means all remaining regions. A non-zero bounded range is always a
    // PARTIAL_PROBE and cannot pass the independent frozen validator.
    std::size_t region_count = 0;
};

struct RegionalCompatibilityReceipt {
    std::filesystem::path output_directory;
    std::string dataset_id;
    std::uint32_t dataset_order = 0;
    std::string semantic_sha256;
    std::uint64_t regions = 0;
    std::uint64_t units = 0;
    std::uint64_t patterns = 0;
    double total_wall_seconds = 0.0;
    double input_sha256_seconds = 0.0;
    double science_wall_seconds = 0.0;
};

// Runs the frozen Python-v2 descriptive endpoint from the source manifest's
// selected truth-isolated raw BAM, PASS VCF and authoritative HP/PS sidecar.
// New runs require an exact production authority or the legacy governed HCC
// gate, use <manifest.run_id>-<dataset_id> under
// <manifest.output_root>/<dataset_id>, verify all eight physical SHA-256 locks,
// and remain READY_FOR_VALIDATION until the independent validator freezes them.
[[nodiscard]] ParseResult<RegionalCompatibilityReceipt> run_regional_compatibility(
    const RegionalCompatibilityOptions& options);

struct RegionalOracleReplayReceipt {
    std::uint64_t regions = 0;
    std::uint64_t units = 0;
    std::uint64_t patterns = 0;
    std::map<std::string, std::uint64_t> all_unit_classes;
    std::map<std::string, std::uint64_t> primary_unit_classes;
    std::string unit_semantic_sha256;
};

// Solver-only replay of frozen Python MLHP parts. This is a pre-BAM probe and
// does not create artifacts or make a production claim.
[[nodiscard]] ParseResult<RegionalOracleReplayReceipt> replay_frozen_mlhp_oracle(
    const std::vector<std::filesystem::path>& ordered_mlhp_parts);

}  // namespace longlineage::compat
