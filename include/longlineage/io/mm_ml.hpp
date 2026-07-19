// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <htslib/sam.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/common/parse_result.hpp"

namespace longlineage {

enum class MmSkipSemantics {
    kUnknown,
};

[[nodiscard]] constexpr std::string_view to_string(MmSkipSemantics semantics) noexcept {
    switch (semantics) {
        case MmSkipSemantics::kUnknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

struct MethylationCall {
    std::uint64_t query_pos0;
    std::uint32_t mm_group_index;
    std::uint32_t ml_index;
    std::uint8_t ml_raw;
    double probability_lower;
    double probability_upper;
    MmSkipSemantics skip_semantics;
};

struct MmMlMnTags {
    std::string mm;
    std::vector<std::uint8_t> ml;
    std::optional<std::uint64_t> mn;
    std::vector<MethylationCall> calls;
};

// The standalone parser consumes the query sequence in the original
// as-sequenced 5' orientation. It accepts exactly one C+m? target group and
// permits C+h? groups solely to account for their exact ML offsets.
[[nodiscard]] ParseResult<MmMlMnTags> parse_frozen_cm_unknown(std::string_view sequence_as_sequenced,
                                                              std::string_view mm, const std::vector<std::uint8_t>& ml,
                                                              std::optional<std::uint64_t> mn);

// Only uppercase MM/ML/MN are authoritative. Missing uppercase tags never fall
// back to legacy/lowercase tags.
[[nodiscard]] ParseResult<MmMlMnTags> parse_mm_ml_mn(const bam1_t& alignment);

// Reuses an already decoded BAM sequence in reference/alignment orientation.
// Reverse alignments are converted to the original as-sequenced orientation
// exactly once before applying the frozen MM delta semantics.
[[nodiscard]] ParseResult<MmMlMnTags> parse_mm_ml_mn(const bam1_t& alignment,
                                                     std::string_view decoded_bam_sequence_reference_orientation);

}  // namespace longlineage
