// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <htslib/sam.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/common/types.hpp"

namespace longlineage {

enum class Strand : char {
    kForward = '+',
    kReverse = '-',
};

[[nodiscard]] constexpr char to_char(Strand strand) noexcept { return static_cast<char>(strand); }

struct ReadProjectionIdentity {
    std::string qname;
    ContigId contig;
    Interval0 reference_interval;
    std::uint8_t mapq;
    Strand strand;

    friend bool operator==(const ReadProjectionIdentity& lhs, const ReadProjectionIdentity& rhs) noexcept {
        return std::tie(lhs.qname, lhs.contig, lhs.reference_interval, lhs.mapq, lhs.strand) ==
               std::tie(rhs.qname, rhs.contig, rhs.reference_interval, rhs.mapq, rhs.strand);
    }
    friend bool operator!=(const ReadProjectionIdentity& lhs, const ReadProjectionIdentity& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const ReadProjectionIdentity& lhs, const ReadProjectionIdentity& rhs) noexcept {
        return std::tie(lhs.qname, lhs.contig, lhs.reference_interval, lhs.mapq, lhs.strand) <
               std::tie(rhs.qname, rhs.contig, rhs.reference_interval, rhs.mapq, rhs.strand);
    }
};

struct FullAlignmentIdentity {
    ReadProjectionIdentity projection;
    std::string raw_qname;
    std::uint16_t flag;
    std::string cigar;
    // SHA-256 of the canonical serialization of every SAM core field used by
    // the frozen duplicate-equivalence contract. This includes mate fields,
    // SEQ and QUAL; projection identity alone is intentionally insufficient.
    std::string sam_core_sha256;
    std::string typed_aux_canonical;

    friend bool operator==(const FullAlignmentIdentity& lhs, const FullAlignmentIdentity& rhs) noexcept {
        return std::tie(lhs.projection, lhs.raw_qname, lhs.flag, lhs.cigar, lhs.sam_core_sha256,
                        lhs.typed_aux_canonical) == std::tie(rhs.projection, rhs.raw_qname, rhs.flag, rhs.cigar,
                                                             rhs.sam_core_sha256, rhs.typed_aux_canonical);
    }
    friend bool operator!=(const FullAlignmentIdentity& lhs, const FullAlignmentIdentity& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const FullAlignmentIdentity& lhs, const FullAlignmentIdentity& rhs) noexcept {
        return std::tie(lhs.projection, lhs.raw_qname, lhs.flag, lhs.cigar, lhs.sam_core_sha256,
                        lhs.typed_aux_canonical) < std::tie(rhs.projection, rhs.raw_qname, rhs.flag, rhs.cigar,
                                                            rhs.sam_core_sha256, rhs.typed_aux_canonical);
    }
};

[[nodiscard]] std::string projected_read_name(std::string_view raw_qname, std::uint16_t flag);
[[nodiscard]] ParseResult<std::string> cigar_string(const bam1_t& alignment);

// Canonical form is ordered by tag, retains duplicate occurrences in encounter
// order, and retains the exact scalar/B-array type. RG is excluded by default
// so RG-only duplicate occurrences can collapse.
[[nodiscard]] ParseResult<std::string> canonicalize_typed_aux(const bam1_t& alignment,
                                                              const std::vector<std::string>& excluded_tags = {"RG"});

// Canonicalizes every field in the SAM core duplicate-equivalence contract:
// QNAME, FLAG, RNAME/TID, POS/END, MAPQ, CIGAR, RNEXT/PNEXT, TLEN, SEQ and QUAL.
// Missing QUAL is represented distinctly from a byte vector.
[[nodiscard]] ParseResult<std::string> canonicalize_sam_core(const bam1_t& alignment, const sam_hdr_t& header);

[[nodiscard]] ParseResult<FullAlignmentIdentity> build_full_alignment_identity(
    const bam1_t& alignment, const sam_hdr_t& header, const std::vector<std::string>& excluded_tags = {"RG"});

[[nodiscard]] ParseResult<std::string> decode_bam_sequence(const bam1_t& alignment);

}  // namespace longlineage
