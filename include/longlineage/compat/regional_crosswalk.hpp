// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "longlineage/common/parse_result.hpp"

namespace longlineage::compat {

struct RegionalCrosswalkLayer {
    std::uint64_t expected_rows = 0;
    std::uint64_t actual_rows = 0;
    std::uint64_t mismatches = 0;
    std::string expected_sha256;
    std::string actual_sha256;
};

struct RegionalCrosswalkReceipt {
    std::string dataset_id;
    std::filesystem::path cpp_bundle;
    std::filesystem::path python_output_manifest;
    std::filesystem::path python_region_view;
    std::string cpp_summary_sha256;
    std::string cpp_validation_receipt_sha256;
    std::string python_output_manifest_sha256;
    std::string python_region_view_sha256;
    std::string python_authority_sha256;
    RegionalCrosswalkLayer regions;
    RegionalCrosswalkLayer units;
    RegionalCrosswalkLayer patterns;
    bool all_exact = false;
};

// Compares only the descriptive Python-v2 fields shared by the validated C++
// bundle and frozen layered region view. CN/LOH and other post-tree fields are
// intentionally excluded. Both inputs must already carry frozen SHA bindings.
[[nodiscard]] ParseResult<RegionalCrosswalkReceipt> compare_frozen_regional_bundle(
    const std::filesystem::path& repository_root, const std::filesystem::path& cpp_bundle,
    const std::filesystem::path& python_output_manifest);

[[nodiscard]] std::string render_regional_crosswalk_receipt_json(const RegionalCrosswalkReceipt& receipt);

}  // namespace longlineage::compat
