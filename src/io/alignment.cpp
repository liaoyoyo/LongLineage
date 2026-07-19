// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/io/alignment.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace longlineage {
namespace {

[[nodiscard]] std::string hex(const std::uint8_t* bytes, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        output[index * 2] = kHex[bytes[index] >> 4U];
        output[index * 2 + 1] = kHex[bytes[index] & 0x0fU];
    }
    return output;
}

[[nodiscard]] bool is_excluded(std::string_view tag, const std::set<std::string>& exclusions) {
    return exclusions.count(std::string(tag)) != 0;
}

}  // namespace

std::string projected_read_name(std::string_view raw_qname, std::uint16_t flag) {
    std::string projected(raw_qname);
    if ((flag & BAM_FPAIRED) != 0) {
        if ((flag & BAM_FREAD1) != 0) {
            projected.append("/1");
        } else if ((flag & BAM_FREAD2) != 0) {
            projected.append("/2");
        }
    }
    return projected;
}

ParseResult<std::string> cigar_string(const bam1_t& alignment) {
    if (alignment.core.n_cigar == 0) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "Mapped production alignment must contain a CIGAR");
    }
    const std::uint32_t* cigar = bam_get_cigar(&alignment);
    std::string output;
    output.reserve(static_cast<std::size_t>(alignment.core.n_cigar) * 4);
    for (std::uint32_t index = 0; index < alignment.core.n_cigar; ++index) {
        const std::uint32_t length = bam_cigar_oplen(cigar[index]);
        const char operation = bam_cigar_opchr(cigar[index]);
        if (length == 0 || operation == '?' || std::string_view("MIDNSHP=X").find(operation) == std::string::npos) {
            return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                     "Alignment contains a malformed CIGAR operation");
        }
        output.append(std::to_string(length));
        output.push_back(operation);
    }
    return ParseResult<std::string>::success(std::move(output));
}

ParseResult<std::string> canonicalize_typed_aux(const bam1_t& alignment,
                                                const std::vector<std::string>& excluded_tags) {
    std::set<std::string> exclusions;
    for (const std::string& tag : excluded_tags) {
        if (tag.size() != 2) {
            return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                     "Excluded auxiliary tag must have exactly two characters");
        }
        exclusions.insert(tag);
    }

    std::map<std::string, std::vector<std::string>> canonical_by_tag;
    errno = 0;
    std::uint8_t* field = bam_aux_first(&alignment);
    if (field == nullptr) {
        if (errno == ENOENT) {
            return ParseResult<std::string>::success_empty({});
        }
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "Cannot locate the first typed auxiliary field");
    }

    while (field != nullptr) {
        const char* tag_bytes = bam_aux_tag(field);
        const std::string tag(tag_bytes, 2);

        errno = 0;
        std::uint8_t* next = bam_aux_next(&alignment, field);
        if (next == nullptr && errno != ENOENT) {
            return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                     "Malformed typed auxiliary field: " + tag);
        }
        const std::uint8_t* field_end =
            next != nullptr ? next - 2 : alignment.data + static_cast<std::ptrdiff_t>(alignment.l_data);
        if (field_end <= field) {
            return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                     "Malformed typed auxiliary field boundary: " + tag);
        }

        if (!is_excluded(tag, exclusions)) {
            const std::size_t typed_size = static_cast<std::size_t>(field_end - field);
            std::string encoded;
            encoded.reserve(2 + 1 + 20 + 1 + typed_size * 2);
            encoded.append(tag);
            encoded.push_back(':');
            encoded.append(std::to_string(typed_size));
            encoded.push_back(':');
            encoded.append(hex(field, typed_size));
            canonical_by_tag[tag].push_back(std::move(encoded));
        }
        field = next;
    }

    std::string canonical;
    for (const auto& entry : canonical_by_tag) {
        for (const auto& occurrence : entry.second) {
            canonical.append(occurrence);
            canonical.push_back('\n');
        }
    }
    return canonical.empty() ? ParseResult<std::string>::success_empty(std::move(canonical))
                             : ParseResult<std::string>::success(std::move(canonical));
}

ParseResult<FullAlignmentIdentity> build_full_alignment_identity(const bam1_t& alignment, const sam_hdr_t& header,
                                                                 const std::vector<std::string>& excluded_tags) {
    if ((alignment.core.flag & BAM_FUNMAP) != 0 || alignment.core.tid < 0 || alignment.core.pos < 0) {
        return ParseResult<FullAlignmentIdentity>::failure(ParseReason::kUnsupportedValue,
                                                           "Unmapped alignment has no production projection identity");
    }
    if (alignment.core.tid >= sam_hdr_nref(&header)) {
        return ParseResult<FullAlignmentIdentity>::failure(ParseReason::kMalformedValue,
                                                           "Alignment reference ID is absent from the SAM header");
    }
    const char* contig_name = sam_hdr_tid2name(&header, alignment.core.tid);
    if (contig_name == nullptr) {
        return ParseResult<FullAlignmentIdentity>::failure(ParseReason::kMalformedValue,
                                                           "Cannot resolve alignment reference ID");
    }
    auto contig = ContigId::from_string(contig_name);
    if (!contig.ok()) {
        return ParseResult<FullAlignmentIdentity>::failure(contig.reason, std::move(contig.detail));
    }
    const auto begin = static_cast<std::uint64_t>(alignment.core.pos);
    const hts_pos_t raw_end = bam_endpos(&alignment);
    if (raw_end < alignment.core.pos) {
        return ParseResult<FullAlignmentIdentity>::failure(ParseReason::kMalformedValue,
                                                           "Alignment end precedes alignment start");
    }
    const auto end = static_cast<std::uint64_t>(raw_end);
    auto interval = Interval0::from_bounds(begin, end);
    if (!interval.ok()) {
        return ParseResult<FullAlignmentIdentity>::failure(interval.reason, std::move(interval.detail));
    }
    const char* raw_qname = bam_get_qname(&alignment);
    if (raw_qname == nullptr || *raw_qname == '\0') {
        return ParseResult<FullAlignmentIdentity>::failure(ParseReason::kMalformedValue,
                                                           "Alignment QNAME must not be empty");
    }
    auto cigar = cigar_string(alignment);
    if (!cigar.ok()) {
        return ParseResult<FullAlignmentIdentity>::failure(cigar.reason, std::move(cigar.detail));
    }
    auto typed_aux = canonicalize_typed_aux(alignment, excluded_tags);
    if (!typed_aux.ok()) {
        return ParseResult<FullAlignmentIdentity>::failure(typed_aux.reason, std::move(typed_aux.detail));
    }
    const std::uint16_t flag = alignment.core.flag;
    ReadProjectionIdentity projection{projected_read_name(raw_qname, flag), std::move(*contig.value),
                                      std::move(*interval.value), alignment.core.qual,
                                      (flag & BAM_FREVERSE) != 0 ? Strand::kReverse : Strand::kForward};
    return ParseResult<FullAlignmentIdentity>::success(FullAlignmentIdentity{
        std::move(projection), raw_qname, flag, std::move(*cigar.value), std::move(*typed_aux.value)});
}

ParseResult<std::string> decode_bam_sequence(const bam1_t& alignment) {
    if (alignment.core.l_qseq < 0) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "Alignment query sequence length is negative");
    }
    if (alignment.core.l_qseq == 0) {
        return ParseResult<std::string>::success_empty({});
    }
    const std::uint8_t* encoded = bam_get_seq(&alignment);
    if (encoded == nullptr) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "Alignment query sequence bytes are absent");
    }
    std::string sequence;
    sequence.reserve(static_cast<std::size_t>(alignment.core.l_qseq));
    for (hts_pos_t index = 0; index < alignment.core.l_qseq; ++index) {
        const char base = seq_nt16_str[bam_seqi(encoded, index)];
        if (base == '=') {
            return ParseResult<std::string>::failure(ParseReason::kUnsupportedValue,
                                                     "Sequence '=' is unsupported for MM projection");
        }
        sequence.push_back(base);
    }
    return ParseResult<std::string>::success(std::move(sequence));
}

}  // namespace longlineage
