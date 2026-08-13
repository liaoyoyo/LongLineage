// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace longlineage::validation {

struct RegionalCompatValidationCheck {
    std::string check_id;
    bool passed = false;
    std::string detail;
};

struct RegionalCompatValidationOptions {
    std::filesystem::path bundle_root;
    std::filesystem::path validator_executable;
    bool write_frozen_outputs = true;
    std::filesystem::path repository_root;
};

struct RegionalCompatValidationReport {
    bool all_pass = false;
    bool validation_receipt_written = false;
    bool frozen_marker_written = false;
    std::string schema_version;
    std::string run_id;
    std::string profile_id;
    std::string dataset_id;
    std::uint64_t dataset_order = 0;
    std::string source_authority_profile;
    std::string source_authority_sha256;
    std::string producer_receipt_sha256;
    std::string semantic_sha256;
    std::string validator_executable_sha256;
    std::string validation_receipt_sha256;
    std::uint64_t region_rows = 0;
    std::uint64_t unit_rows = 0;
    std::uint64_t pattern_rows = 0;
    std::filesystem::path validation_receipt_path;
    std::filesystem::path frozen_marker_path;
    std::vector<RegionalCompatValidationCheck> checks;
};

// This validator deliberately does not depend on producer parsing, grouping or
// topology kernels. It replays the closed regional compatibility artifact
// contract directly from immutable bytes and fails on any unknown file or row.
class RegionalCompatValidator final {
   public:
    static RegionalCompatValidationReport validate_and_freeze(const RegionalCompatValidationOptions& options);
};

std::string render_regional_compat_validation_report_json(const RegionalCompatValidationReport& report);

}  // namespace longlineage::validation
