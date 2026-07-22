// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/common/types.hpp"
#include "longlineage/io/alignment.hpp"
#include "longlineage/io/mm_ml.hpp"

namespace longlineage {

struct SsnvMarker {
    std::uint64_t site_order;
    Position1 position;
    char reference;
    char alternate;
};

struct ProjectedAlleleCall {
    std::uint64_t site_order;
    Position1 position;
    AlleleCall call;
    std::optional<std::uint64_t> query_pos0;
    std::optional<std::uint8_t> base_quality;
};

struct ProjectedMethylationCall {
    // Candidate genomic C coordinate. The caller must still verify the
    // reference CpG context (C,G) before admitting this observation.
    Position1 candidate_cpg_position;
    std::uint64_t query_pos0_as_sequenced;
    std::uint64_t query_pos0_reference_orientation;
    std::uint32_t mm_group_index;
    std::uint32_t ml_index;
    std::uint8_t ml_raw;
    double probability_lower;
    double probability_upper;
    MmSkipSemantics skip_semantics;
    double point_probability_raw_div_255;
};

struct ReadEvidenceProjection {
    std::vector<ProjectedAlleleCall> allele_calls;
    std::vector<ProjectedMethylationCall> methylation_calls;
};

// Projects every requested sSNV and every decoded C+m? call in one CIGAR
// traversal. sequence_reference_orientation and qualities are BAM SEQ/QUAL
// order. MM query positions are in original as-sequenced order and are
// converted exactly once for reverse alignments.
//
// Markers must be strictly increasing and unique. M/I/D/N/S/H/P/=/X are all
// handled explicitly. A deletion, reference skip, absent query base, missing
// quality (255), or BQ below the threshold yields X; O is never collapsed to R.
[[nodiscard]] ParseResult<ReadEvidenceProjection> project_read_evidence(
    Interval0 reference_interval, Strand strand, std::string_view cigar,
    std::string_view sequence_reference_orientation, const std::vector<std::uint8_t>& base_qualities,
    const MmMlMnTags& mm_ml, const std::vector<SsnvMarker>& ordered_markers, std::uint8_t minimum_base_quality = 20);

}  // namespace longlineage
