// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/io/block_reader.hpp"
#include "longlineage/io/cigar_projection.hpp"
#include "longlineage/io/sidecar.hpp"
#include "longlineage/io/variant_sites.hpp"

namespace longlineage::pipeline {

struct JoinedReadEvidence {
    std::string read_id;
    ReadProjectionIdentity projection;
    std::uint16_t alignment_flag = 0;
    std::string cigar_blake2b64;
    std::string typed_aux_sha256_without_rg;
    std::uint64_t query_length = 0;
    std::optional<std::uint64_t> mn_value;
    LatestTags latest_tags;
    std::uint32_t raw_alignment_occurrences = 0;
    std::uint32_t sidecar_identity_occurrences = 0;
    std::vector<ProjectedAlleleCall> allele_calls;
    std::vector<ProjectedMethylationCall> methylation_calls;
};

struct FocalSiteEvidence {
    VariantSite site;
    // Indices into BlockScienceEvidence::reads. Only reads whose reference
    // interval covers the focal Position1 are admitted.
    std::vector<std::size_t> covering_read_indices;
};

struct BlockScienceCounters {
    std::uint64_t raw_records_after_filter = 0;
    std::uint64_t unique_read_projections = 0;
    std::uint64_t rg_only_duplicate_occurrences = 0;
    std::uint64_t sidecar_rows_fetched = 0;
    std::uint64_t sidecar_rows_eligible = 0;
    std::uint64_t exact_sidecar_joins = 0;
    std::uint64_t projected_marker_calls = 0;
    std::uint64_t decoded_methylation_calls = 0;
    std::uint64_t admitted_reference_cpg_calls = 0;
    std::uint64_t rejected_non_cpg_calls = 0;
};

struct BlockScienceEvidence {
    std::uint64_t block_sequence = 0;
    std::vector<VariantSite> markers;
    std::vector<JoinedReadEvidence> reads;
    std::vector<FocalSiteEvidence> focal_sites;
    BlockScienceCounters counters;

    [[nodiscard]] std::size_t logical_retained_bytes() const noexcept;
};

struct BlockScienceOptions {
    std::uint8_t minimum_base_quality = 20;
    bool require_sidecar_occurrence_conservation = true;
};

// Builds the truth-blind block evidence in one pass over each retained read.
//
// ordered_markers must contain every PASS sSNV in block.fetch_interval,
// including partner markers outside block.focal_sites. reference_context must
// cover block.fetch_interval plus one trailing reference base when available,
// so C+m? candidates can be admitted only after an explicit C/G CpG check.
//
// Duplicate raw alignments collapse only when FullAlignmentIdentity is exact;
// a shared projection with any non-RG identity difference fails closed.
[[nodiscard]] ParseResult<BlockScienceEvidence> build_block_science_evidence(
    const AlignmentBlock& block, const std::vector<VariantSite>& ordered_markers, const BlockReadBatch& raw_batch,
    const SidecarLookup& sidecar_lookup, Interval0 reference_context_interval, std::string_view reference_context,
    const BlockScienceOptions& options = {});

[[nodiscard]] const ProjectedAlleleCall* find_allele_call(const JoinedReadEvidence& read,
                                                          std::uint64_t site_order) noexcept;

// Hashes a canonical truth-blind logical representation. The digest excludes
// compression, worker identity, timings and filesystem paths, so it is suitable
// for 1/24/40-worker semantic determinism checks.
[[nodiscard]] ParseResult<std::string> block_science_semantic_sha256(const BlockScienceEvidence& block);

}  // namespace longlineage::pipeline
