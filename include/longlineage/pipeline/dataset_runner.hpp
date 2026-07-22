// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/io/block_reader.hpp"
#include "longlineage/io/variant_sites.hpp"
#include "longlineage/pipeline/block_science.hpp"
#include "longlineage/pipeline/dataset_plan.hpp"

namespace longlineage::pipeline {

struct DatasetReadPaths {
    std::filesystem::path bam;
    std::filesystem::path bam_index;
    std::filesystem::path latest_hp_ps_sidecar;
    std::filesystem::path latest_hp_ps_sidecar_index;
    std::filesystem::path reference_fasta;
    std::filesystem::path reference_fai;
};

struct DatasetBlockLoad {
    BlockScienceEvidence evidence;
    BlockReadCounters read_counters;
    std::size_t raw_logical_bytes = 0;
    std::size_t evidence_logical_bytes = 0;
    double bam_seconds = 0.0;
    double sidecar_seconds = 0.0;
    double reference_seconds = 0.0;
    double projection_seconds = 0.0;
};

// One instance owns one independent BAM, sidecar and reference handle bundle.
// It is move-only and must remain bound to exactly one worker thread.
class DatasetBlockReader final {
   public:
    [[nodiscard]] static ParseResult<std::unique_ptr<DatasetBlockReader>> open(const DatasetReadPaths& paths);

    ~DatasetBlockReader();
    DatasetBlockReader(DatasetBlockReader&&) noexcept;
    DatasetBlockReader& operator=(DatasetBlockReader&&) noexcept;
    DatasetBlockReader(const DatasetBlockReader&) = delete;
    DatasetBlockReader& operator=(const DatasetBlockReader&) = delete;

    [[nodiscard]] ParseResult<DatasetBlockLoad> read(const PlannedDatasetBlock& block, const VariantSiteSet& variants);

    [[nodiscard]] std::uint64_t bam_fetch_invocations() const noexcept;
    [[nodiscard]] std::uint64_t sidecar_fetch_invocations() const noexcept;
    [[nodiscard]] std::uint64_t reference_fetch_invocations() const noexcept;

   private:
    struct Impl;
    explicit DatasetBlockReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct BlockExtractionReceipt {
    std::uint64_t block_sequence = 0;
    std::string contig;
    std::uint64_t focal_sites = 0;
    std::uint64_t markers = 0;
    BlockReadCounters read_counters;
    BlockScienceCounters science_counters;
    std::uint64_t matrix_rows = 0;
    std::uint64_t alt_rows = 0;
    std::uint64_t matrix_cells = 0;
    std::size_t raw_logical_bytes = 0;
    std::size_t evidence_logical_bytes = 0;
    std::string block_semantic_sha256;
    double bam_seconds = 0.0;
    double sidecar_seconds = 0.0;
    double reference_seconds = 0.0;
    double projection_seconds = 0.0;
    double matrix_seconds = 0.0;

    [[nodiscard]] std::size_t retained_bytes() const noexcept;
};

struct DatasetExtractionOptions {
    std::size_t workers = 1;
    std::size_t first_block = 0;
    // Zero means every remaining block.
    std::size_t block_count = 0;
    std::size_t task_queue_capacity_bytes = 16U * 1024U * 1024U;
};

struct DatasetExtractionReceipt {
    std::size_t workers = 0;
    std::size_t first_block = 0;
    std::size_t block_count = 0;
    std::uint64_t focal_sites = 0;
    std::uint64_t markers = 0;
    BlockReadCounters read_counters;
    BlockScienceCounters science_counters;
    std::uint64_t matrix_rows = 0;
    std::uint64_t alt_rows = 0;
    std::uint64_t matrix_cells = 0;
    std::size_t maximum_raw_logical_bytes = 0;
    std::size_t maximum_evidence_logical_bytes = 0;
    std::size_t peak_task_queue_bytes = 0;
    std::uint64_t bam_fetch_invocations = 0;
    std::uint64_t sidecar_fetch_invocations = 0;
    std::uint64_t reference_fetch_invocations = 0;
    std::string semantic_sha256;
    double handle_open_seconds = 0.0;
    double execution_seconds = 0.0;
    std::vector<BlockExtractionReceipt> blocks;
};

// The digest excludes worker count, timings, physical paths, queue statistics
// and compression. It binds ordered block semantic digests plus deterministic
// conservation counters and therefore must match across 1/24/40 workers.
[[nodiscard]] ParseResult<std::string> dataset_extraction_semantic_sha256(
    const std::vector<BlockExtractionReceipt>& ordered_blocks, std::size_t first_block);

// Executes a bounded or complete dataset plan. Worker-local HTSlib handles are
// never shared. Results are published in block order, and any block exception
// discards the complete batch receipt rather than publishing a partial result.
[[nodiscard]] ParseResult<DatasetExtractionReceipt> run_dataset_extraction(
    const DatasetReadPaths& paths, const VariantSiteSet& variants, const DatasetExecutionPlan& plan,
    const DatasetExtractionOptions& options = {});

}  // namespace longlineage::pipeline
