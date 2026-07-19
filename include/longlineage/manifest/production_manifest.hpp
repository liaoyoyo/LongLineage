// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/common/parse_result.hpp"

namespace longlineage {

enum class FileRole {
    kRawBam,
    kRawBamIndex,
    kPassBiallelicSsnvVcf,
    kPassBiallelicSsnvVcfIndex,
    kLatestHpPsSidecar,
    kLatestHpPsSidecarIndex,
    kReferenceFasta,
    kReferenceFai,
};

enum class AuthorityProfile {
    kProductionSevenDataset,
    kSynthetic,
};

[[nodiscard]] std::string_view to_string(AuthorityProfile profile) noexcept;
[[nodiscard]] ParseResult<AuthorityProfile> parse_authority_profile(std::string_view value);
[[nodiscard]] std::string_view to_string(FileRole role) noexcept;
[[nodiscard]] ParseResult<FileRole> parse_file_role(std::string_view value);

struct LockedFile {
    FileRole role;
    std::filesystem::path path;
    std::uint64_t size_bytes;
    std::string sha256;
};

struct DatasetManifest {
    std::string dataset_id;
    std::uint32_t dataset_order;
    std::vector<LockedFile> files;
};

struct RuntimeManifest {
    std::uint32_t compute_workers;
    std::uint32_t writer_threads;
    std::uint32_t coordinator_slots;
    std::uint64_t buffer_bytes;
    std::uint32_t max_focal_sites_per_block;
    std::uint32_t max_estimated_alignments_per_block;
    std::uint32_t halo_bp;
};

struct ContractBindings {
    std::string science_parameters_sha256;
    std::string schema_catalog_sha256;
    std::string status_reason_registry_sha256;
    std::string type_registry_sha256;
    std::string transform_registry_sha256;
    std::string authority_manifest_sha256;
    std::string source_to_target_manifest_sha256;
    std::string production_input_authority_sha256;
    std::string schema_id_registry_sha256;
    std::string release_attestation_sha256;
};

struct ProductionManifest {
    std::string schema_name;
    std::string schema_version;
    std::string run_id;
    AuthorityProfile authority_profile;
    std::filesystem::path output_root;
    std::vector<DatasetManifest> datasets;
    RuntimeManifest runtime;
    ContractBindings contract_bindings;
};

struct FileLockCheck {
    std::uint64_t observed_size_bytes;
    std::string observed_sha256;
};

// Both entry points reject duplicate JSON keys and recursively reject truth-bearing
// keys or string values before interpreting any production field.
[[nodiscard]] ParseResult<ProductionManifest> parse_production_manifest_json(std::string_view json);
[[nodiscard]] ParseResult<ProductionManifest> load_production_manifest(const std::filesystem::path& path);

[[nodiscard]] ParseResult<FileLockCheck> verify_locked_file(const LockedFile& locked_file);

}  // namespace longlineage
