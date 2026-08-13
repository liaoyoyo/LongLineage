// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "longlineage/artifact/run_root.hpp"

namespace longlineage::validation {

struct ValidationCheck {
    std::string check_id;
    bool passed = false;
    std::string detail;
};

struct ArtifactValidationOptions {
    std::filesystem::path repo_root;
    std::filesystem::path run_root;
    std::filesystem::path validator_executable;
    bool write_validation_receipt = true;
    // Required for canonical DATASET_GATE validation. The independent
    // validator re-hashes the manifest and every locked input.
    std::filesystem::path manifest_path;
};

struct PublicationFileSnapshot {
    std::string relative_path;
    std::uint64_t size_bytes{0};
    std::string physical_sha256;
};

struct ArtifactValidationReport {
    bool all_pass = false;
    std::string validation_profile = "DATASET_GATE";
    bool production_claim_allowed = false;
    std::string run_id;
    std::string producer_receipt_sha256;
    std::string producer_executable_sha256;
    std::string validator_executable_sha256;
    std::string validation_receipt_sha256;
    std::string producer_hostname;
    std::string producer_kernel_release;
    std::string validator_hostname;
    std::string validator_kernel_release;
    std::string input_mount_identity_sha256;
    std::string input_snapshot_before_sha256;
    std::string input_snapshot_after_sha256;
    std::string schema_catalog_sha256;
    std::string science_parameters_sha256;
    std::string validated_at;
    std::vector<ValidationCheck> checks;
    bool scientific_conservation_replayed = false;
    bool input_content_replayed = false;
    bool validation_receipt_written = false;
    bool publication_snapshot_captured = false;
    std::vector<PublicationFileSnapshot> publication_snapshot;
    std::filesystem::path validation_receipt_path;
};

struct DatasetGateFinalizeOptions {
    ArtifactValidationOptions validation;
    std::filesystem::path output_base;
    // Test/recovery hook: complete the atomic directory rename but deliberately
    // leave the final root without run_receipt.json.
    bool stop_after_atomic_rename = false;
    // Deterministic fault-injection hook. Production callers leave this empty.
    std::function<void(const std::filesystem::path&)> before_publication_replay_for_test;
};

struct DatasetGateFinalizeReport {
    ArtifactValidationReport validation;
    artifact::RunRootResult publication;
    artifact::RunRootInspection inspection;
    bool dataset_gate_frozen = false;
};

// Independently replays immutable producer artifacts without linking producer
// science or solver kernels. Unknown formats or underspecified contracts fail.
class ArtifactValidator final {
   public:
    static ArtifactValidationReport validate(const ArtifactValidationOptions& options);
    static DatasetGateFinalizeReport validate_and_freeze(const DatasetGateFinalizeOptions& options);
};

std::string render_report_json(const ArtifactValidationReport& report);
std::string render_finalize_report_json(const DatasetGateFinalizeReport& report);

}  // namespace longlineage::validation
