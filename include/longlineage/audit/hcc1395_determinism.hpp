// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>

namespace longlineage::audit {

struct Hcc1395DeterminismAuditOptions {
    std::filesystem::path w24_run_root;
    std::filesystem::path w24_manifest;
    std::filesystem::path w40_run_root;
    std::filesystem::path w40_manifest;
    std::filesystem::path historical_m1_tsv_gz;
    std::string historical_m1_sha256;
    std::filesystem::path output_receipt;
};

struct Hcc1395DeterminismAuditResult {
    bool ok{false};
    std::string receipt_json;
    std::string error_code;
    std::string detail;
};

// This independent audit reads only frozen output artifacts, receipts, manifests
// and the explicitly bound historical M1 projection source. It never opens BAM,
// VCF, sidecar or reference inputs and does not link producer science kernels.
[[nodiscard]] Hcc1395DeterminismAuditResult run_hcc1395_determinism_audit(
    const Hcc1395DeterminismAuditOptions& options) noexcept;

}  // namespace longlineage::audit
