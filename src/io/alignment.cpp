// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/io/alignment.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "longlineage/common/digest.hpp"

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

void append_field(std::string& output, std::string_view name, std::string_view value) {
    output.append(name);
    output.push_back(':');
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back('\n');
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

ParseResult<std::string> canonicalize_sam_core(const bam1_t& alignment, const sam_hdr_t& header) {
    if ((alignment.core.flag & BAM_FUNMAP) != 0 || alignment.core.tid < 0 || alignment.core.pos < 0) {
        return ParseResult<std::string>::failure(ParseReason::kUnsupportedValue,
                                                 "Unmapped alignment has no production SAM-core identity");
    }
    if (alignment.core.tid >= sam_hdr_nref(&header)) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "Alignment reference ID is absent from the SAM header");
    }
    const char* reference_name = sam_hdr_tid2name(&header, alignment.core.tid);
    const char* raw_qname = bam_get_qname(&alignment);
    if (reference_name == nullptr || raw_qname == nullptr || *raw_qname == '\0') {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "SAM-core QNAME/RNAME identity is malformed");
    }
    const hts_pos_t raw_end = bam_endpos(&alignment);
    if (raw_end < alignment.core.pos) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "SAM-core computed alignment end precedes start");
    }
    auto cigar = cigar_string(alignment);
    if (!cigar.ok()) {
        return ParseResult<std::string>::failure(cigar.reason, std::move(cigar.detail));
    }
    auto sequence = decode_bam_sequence(alignment);
    if (!sequence.ok()) {
        return ParseResult<std::string>::failure(sequence.reason, std::move(sequence.detail));
    }
    if (alignment.core.l_qseq < 0) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue, "SAM-core query length is negative");
    }
    const std::size_t query_length = static_cast<std::size_t>(alignment.core.l_qseq);
    const std::uint8_t* raw_qualities = query_length == 0 ? nullptr : bam_get_qual(&alignment);
    if (query_length != 0 && raw_qualities == nullptr) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue, "SAM-core QUAL bytes are absent");
    }
    const bool missing_qualities =
        query_length != 0 && std::all_of(raw_qualities, raw_qualities + query_length, [](std::uint8_t value) noexcept {
            return value == std::numeric_limits<std::uint8_t>::max();
        });

    std::string canonical;
    canonical.reserve(sequence.value->size() * 3 + cigar.value->size() + 256);
    append_field(canonical, "QNAME", raw_qname);
    append_field(canonical, "FLAG", std::to_string(alignment.core.flag));
    append_field(canonical, "TID", std::to_string(alignment.core.tid));
    append_field(canonical, "RNAME", reference_name);
    append_field(canonical, "POS0", std::to_string(alignment.core.pos));
    append_field(canonical, "END0", std::to_string(raw_end));
    append_field(canonical, "MAPQ", std::to_string(alignment.core.qual));
    append_field(canonical, "CIGAR", *cigar.value);
    append_field(canonical, "MTID", std::to_string(alignment.core.mtid));
    append_field(canonical, "MPOS0", std::to_string(alignment.core.mpos));
    append_field(canonical, "TLEN", std::to_string(alignment.core.isize));
    append_field(canonical, "SEQ", *sequence.value);
    append_field(canonical, "QUAL",
                 query_length == 0   ? std::string_view{}
                 : missing_qualities ? std::string_view{"*"}
                                     : std::string_view{hex(raw_qualities, query_length)});
    return ParseResult<std::string>::success(std::move(canonical));
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
    auto sam_core = canonicalize_sam_core(alignment, header);
    if (!sam_core.ok()) {
        return ParseResult<FullAlignmentIdentity>::failure(sam_core.reason, std::move(sam_core.detail));
    }
    auto sam_core_sha256 = sha256_hex(*sam_core.value);
    if (!sam_core_sha256.ok()) {
        return ParseResult<FullAlignmentIdentity>::failure(sam_core_sha256.reason, std::move(sam_core_sha256.detail));
    }
    const std::uint16_t flag = alignment.core.flag;
    ReadProjectionIdentity projection{projected_read_name(raw_qname, flag), std::move(*contig.value),
                                      std::move(*interval.value), alignment.core.qual,
                                      (flag & BAM_FREVERSE) != 0 ? Strand::kReverse : Strand::kForward};
    return ParseResult<FullAlignmentIdentity>::success(
        FullAlignmentIdentity{std::move(projection), raw_qname, flag, std::move(*cigar.value),
                              std::move(*sam_core_sha256.value), std::move(*typed_aux.value)});
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
