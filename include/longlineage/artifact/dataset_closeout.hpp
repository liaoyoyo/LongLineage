// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "longlineage/artifact/dataset_artifacts.hpp"
#include "longlineage/common/parse_result.hpp"

namespace longlineage::artifact {

struct ArtifactInputBinding {
    std::string source_kind;
    std::string source_id;
    std::string digest_kind;
    std::string sha256;
};

struct InputMountIdentity {
    std::string dataset_id;
    std::string role;
    std::filesystem::path canonical_path;
    std::string mount_source;
    std::string filesystem_type;
    bool readonly{false};
    std::string mount_options_sha256;
};

struct ProducerExecutableIdentity {
    std::string version;
    std::string git_commit;
    std::string executable_sha256;
    std::string compiler;
    std::string htslib_version{"1.18"};
};

struct ProducerPerformance {
    double wall_seconds{0.0};
    double user_seconds{0.0};
    double system_seconds{0.0};
    std::uint64_t memory_peak_bytes{0};
    std::uint64_t oom_events{0};
    std::uint64_t io_read_bytes{0};
    std::uint64_t io_write_bytes{0};
    std::uint64_t major_page_faults{0};
    std::uint64_t minor_page_faults{0};
    std::uint64_t peak_threads{1};
    double queue_wait_seconds{0.0};
    double reorder_wait_seconds{0.0};
    double task_latency_p50_seconds{0.0};
    double task_latency_p95_seconds{0.0};
    double task_latency_p99_seconds{0.0};
    double task_latency_max_seconds{0.0};
    std::uint64_t logical_records{0};
    std::uint64_t logical_bytes{0};
    std::uint64_t final_file_count{0};
    std::uint64_t transient_file_count{0};
    std::string cache_condition{"UNKNOWN"};
};

struct DatasetCloseoutOptions {
    std::filesystem::path repo_root;
    std::filesystem::path staging_root;
    std::string run_id;
    ProducerExecutableIdentity executable;
    std::string producer_hostname;
    std::string producer_kernel_release;
    std::vector<InputMountIdentity> input_mount_identity;
    std::vector<ArtifactInputBinding> manifest_inputs;
    std::string manifest_sha256;
    std::string input_snapshot_before_sha256;
    std::string input_snapshot_after_sha256;
    std::string input_lock_sha256;
    std::string phase_ledger_sha256;
    ProducerPerformance performance;
    std::string finished_at;
};

struct DatasetCloseoutReceipt {
    std::vector<DatasetArtifactWriteReceipt> artifacts;
    std::filesystem::path producer_receipt_path;
    std::string producer_receipt_sha256;
    std::filesystem::path checksums_path;
    std::string checksums_sha256;
};

// Closes the producer side of the dataset-gate transaction. The returned
// staging root is still RUNNING/READY_FOR_VALIDATION: this function never
// writes a validation receipt or run receipt and never publishes the run.
[[nodiscard]] ParseResult<DatasetCloseoutReceipt> write_dataset_producer_closeout(
    const std::vector<DatasetArtifactWriteReceipt>& scientific_artifacts, const DatasetCloseoutOptions& options);

}  // namespace longlineage::artifact
