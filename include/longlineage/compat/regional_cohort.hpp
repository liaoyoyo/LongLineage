// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "longlineage/common/parse_result.hpp"

namespace longlineage::compat {

struct RegionalCohortInput {
    std::filesystem::path frozen_bundle;
    std::filesystem::path crosswalk_receipt;
};

struct RegionalCohortRowCounts {
    std::uint64_t regions = 0;
    std::uint64_t units = 0;
    std::uint64_t patterns = 0;
};

struct RegionalCohortTiming {
    double total_wall_seconds = 0.0;
    double input_sha256_seconds = 0.0;
    double science_wall_seconds = 0.0;
    double worker_open_seconds = 0.0;
    double summed_input_seconds = 0.0;
    double summed_solver_seconds = 0.0;
};

struct RegionalCohortPeakRss {
    bool available = false;
    std::optional<std::uint64_t> bytes;
    std::string source;
};

struct RegionalCohortSample {
    std::string dataset_id;
    std::uint32_t dataset_order = 0;
    std::string run_id;
    std::uint32_t workers = 0;
    std::filesystem::path frozen_bundle;
    std::filesystem::path crosswalk_receipt;
    std::string source_authority_profile;
    std::string source_authority_sha256;
    std::filesystem::path source_manifest_path;
    std::string source_manifest_run_id;
    std::string source_manifest_sha256;
    std::string validator_executable_sha256;
    std::string summary_sha256;
    std::string producer_receipt_sha256;
    std::string validation_receipt_sha256;
    std::string frozen_marker_sha256;
    std::string crosswalk_receipt_sha256;
    std::string checksum_manifest_sha256;
    std::string regions_tsv_sha256;
    std::string units_tsv_sha256;
    std::string patterns_tsv_sha256;
    std::string python_output_manifest_sha256;
    std::string python_region_view_sha256;
    std::string python_authority_sha256;
    RegionalCohortRowCounts row_counts;
    std::map<std::string, std::uint64_t> unit_class_census;
    std::map<std::string, std::uint64_t> primary_class_census;
    std::map<std::string, std::uint64_t> region_determinacy_census;
    RegionalCohortTiming timing;
    RegionalCohortPeakRss peak_rss;
};

struct RegionalCohortReceipt {
    std::vector<RegionalCohortSample> samples;
    std::string source_authority_profile;
    std::string source_authority_sha256;
    std::string python_authority_sha256;
    std::string source_set_sha256;
    std::string chart_payload_sha256;
    RegionalCohortRowCounts total_row_counts;
    std::map<std::string, std::uint64_t> total_unit_class_census;
    std::map<std::string, std::uint64_t> total_primary_class_census;
    std::map<std::string, std::uint64_t> total_region_determinacy_census;
    RegionalCohortTiming sequential_timing_totals;
    RegionalCohortPeakRss maximum_peak_rss;
    bool all_sources_validated_frozen = false;
    bool all_crosswalk_receipts_exact = false;
};

// Aggregates frozen output metadata only. The caller must provide exactly seven
// paired inputs in canonical dataset order. This function never opens
// BAM/VCF/sidecar files or recomputes science; it replays regional TSV SHA-256
// values and the class/determinacy census needed for output integrity.
[[nodiscard]] ParseResult<RegionalCohortReceipt> aggregate_frozen_regional_cohort(
    const std::vector<RegionalCohortInput>& inputs);

// Produces deterministic chart-ready JSON. An empty string indicates an
// allocation/encoding failure and must be handled as an error by the caller.
[[nodiscard]] std::string render_regional_cohort_receipt_json(const RegionalCohortReceipt& receipt);

}  // namespace longlineage::compat
