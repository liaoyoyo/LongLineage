// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/common/types.hpp"
#include "longlineage/io/alignment.hpp"
#include "longlineage/io/mm_ml.hpp"

namespace longlineage {

struct FocalSiteCost {
    std::uint32_t dataset_order;
    std::string dataset_id;
    std::uint64_t vcf_record_order;
    ContigId contig;
    Position1 position;
    std::uint64_t estimated_alignments;
    std::uint64_t contig_length;
};

struct BlockPlanOptions {
    std::size_t max_focal_sites = 4096;
    std::uint64_t max_estimated_alignments = 250000;
    std::uint64_t halo_bp = 5000;
};

struct AlignmentBlock {
    std::uint64_t sequence;
    std::uint32_t dataset_order;
    std::string dataset_id;
    ContigId contig;
    std::uint64_t contig_length;
    std::uint64_t halo_bp;
    std::uint64_t first_vcf_record_order;
    std::uint64_t last_vcf_record_order;
    Interval0 focal_interval;
    Interval0 fetch_interval;
    std::uint64_t estimated_alignments;
    std::vector<FocalSiteCost> focal_sites;
};

// Input order is authoritative. The planner may split but never reorder,
// discard, or duplicate a focal site. A single-site estimate above the cap is
// rejected because v1 has no scientifically frozen within-site sharding rule.
[[nodiscard]] ParseResult<std::vector<AlignmentBlock>> plan_alignment_blocks(
    const std::vector<FocalSiteCost>& ordered_sites, const BlockPlanOptions& options = {});

struct BlockReadPolicy {
    std::uint8_t minimum_mapq = 20;
    std::uint64_t minimum_query_length = 1000;
    bool require_mm_ml = true;
};

struct BlockReadCounters {
    std::uint64_t iterator_records = 0;
    std::uint64_t excluded_flag = 0;
    std::uint64_t excluded_mapq = 0;
    std::uint64_t excluded_query_length = 0;
    std::uint64_t excluded_missing_mm_ml = 0;
    std::uint64_t retained_records = 0;
};

struct DecodedBlockRead {
    FullAlignmentIdentity identity;
    std::string sequence_reference_orientation;
    std::vector<std::uint8_t> base_qualities;
    MmMlMnTags mm_ml;
};

struct BlockReadBatch {
    std::uint64_t block_sequence = 0;
    BlockReadCounters counters;
    std::vector<DecodedBlockRead> reads;

    // Deterministic logical retained-byte charge for admission accounting.
    // This is not a claim about allocator-specific resident memory.
    [[nodiscard]] std::size_t logical_retained_bytes() const noexcept;
};

// One instance owns one BAM stream, header, and explicitly named index.
// Instances are move-only and must not be shared between worker threads.
class IndexedBamBlockReader final {
   public:
    [[nodiscard]] static ParseResult<std::unique_ptr<IndexedBamBlockReader>> open(
        const std::filesystem::path& bam_path, const std::filesystem::path& explicit_index_path);

    ~IndexedBamBlockReader();
    IndexedBamBlockReader(IndexedBamBlockReader&&) noexcept;
    IndexedBamBlockReader& operator=(IndexedBamBlockReader&&) noexcept;
    IndexedBamBlockReader(const IndexedBamBlockReader&) = delete;
    IndexedBamBlockReader& operator=(const IndexedBamBlockReader&) = delete;

    [[nodiscard]] ParseResult<BlockReadBatch> read_block(const AlignmentBlock& block,
                                                         const BlockReadPolicy& policy = {});
    [[nodiscard]] std::uint64_t fetch_invocations() const noexcept;

   private:
    struct Impl;
    explicit IndexedBamBlockReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace longlineage
