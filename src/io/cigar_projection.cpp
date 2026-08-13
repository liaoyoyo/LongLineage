// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/io/cigar_projection.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace longlineage {
namespace {

struct CigarOperation {
    std::uint64_t length;
    char code;
};

struct OrderedMethylCall {
    std::uint64_t query_pos0_reference_orientation;
    const MethylationCall* call;
};

[[nodiscard]] bool is_snv_base(char base) noexcept {
    if (base >= 'a' && base <= 'z') {
        base = static_cast<char>(base - ('a' - 'A'));
    }
    return base == 'A' || base == 'C' || base == 'G' || base == 'T';
}

[[nodiscard]] ParseResult<std::vector<CigarOperation>> parse_cigar(std::string_view cigar) {
    if (cigar.empty()) {
        return ParseResult<std::vector<CigarOperation>>::failure(ParseReason::kMalformedValue,
                                                                 "CIGAR projection requires a non-empty CIGAR");
    }
    std::vector<CigarOperation> operations;
    std::size_t cursor = 0;
    while (cursor < cigar.size()) {
        const std::size_t digits_begin = cursor;
        while (cursor < cigar.size() && cigar[cursor] >= '0' && cigar[cursor] <= '9') {
            ++cursor;
        }
        if (digits_begin == cursor || cursor == cigar.size()) {
            return ParseResult<std::vector<CigarOperation>>::failure(ParseReason::kMalformedValue,
                                                                     "CIGAR operation lacks length or code");
        }
        std::uint64_t length = 0;
        const auto parsed = std::from_chars(cigar.data() + digits_begin, cigar.data() + cursor, length);
        if (parsed.ec != std::errc{} || parsed.ptr != cigar.data() + cursor || length == 0) {
            return ParseResult<std::vector<CigarOperation>>::failure(ParseReason::kMalformedValue,
                                                                     "CIGAR operation length is malformed");
        }
        const char code = cigar[cursor++];
        if (std::string_view("MIDNSHP=X").find(code) == std::string_view::npos) {
            return ParseResult<std::vector<CigarOperation>>::failure(ParseReason::kMalformedValue,
                                                                     "CIGAR operation code is unsupported");
        }
        operations.push_back(CigarOperation{length, code});
    }
    return ParseResult<std::vector<CigarOperation>>::success(std::move(operations));
}

[[nodiscard]] bool checked_add(std::uint64_t value, std::uint64_t increment, std::uint64_t& output) noexcept {
    if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
        return false;
    }
    output = value + increment;
    return true;
}

void emit_unobservable(const SsnvMarker& marker, std::vector<ProjectedAlleleCall>& output) {
    output.push_back(
        ProjectedAlleleCall{marker.site_order, marker.position, AlleleCall::kUnobservable, std::nullopt, std::nullopt});
}

}  // namespace

ParseResult<ReadEvidenceProjection> project_read_evidence(
    Interval0 reference_interval, Strand strand, std::string_view cigar,
    std::string_view sequence_reference_orientation, const std::vector<std::uint8_t>& base_qualities,
    const MmMlMnTags& mm_ml, const std::vector<SsnvMarker>& ordered_markers, std::uint8_t minimum_base_quality) {
    if (sequence_reference_orientation.size() != base_qualities.size()) {
        return ParseResult<ReadEvidenceProjection>::failure(ParseReason::kMalformedValue,
                                                            "BAM SEQ and QUAL lengths differ during CIGAR projection");
    }
    for (std::size_t index = 0; index < ordered_markers.size(); ++index) {
        const auto& marker = ordered_markers[index];
        if (!is_snv_base(marker.reference) || !is_snv_base(marker.alternate) ||
            classify_allele(marker.reference, marker.reference, marker.alternate, true) != AlleleCall::kReference ||
            classify_allele(marker.alternate, marker.reference, marker.alternate, true) != AlleleCall::kAlternate) {
            return ParseResult<ReadEvidenceProjection>::failure(
                ParseReason::kMalformedValue, "sSNV marker must contain two distinct A/C/G/T alleles");
        }
        if (index > 0 && !(ordered_markers[index - 1].position < marker.position)) {
            return ParseResult<ReadEvidenceProjection>::failure(ParseReason::kMalformedValue,
                                                                "sSNV markers must be strictly increasing and unique");
        }
    }

    auto operations = parse_cigar(cigar);
    if (!operations.ok()) {
        return ParseResult<ReadEvidenceProjection>::failure(operations.reason, std::move(operations.detail));
    }

    std::vector<OrderedMethylCall> methyl_calls;
    methyl_calls.reserve(mm_ml.calls.size());
    const std::uint64_t query_length = sequence_reference_orientation.size();
    for (const auto& call : mm_ml.calls) {
        if (call.query_pos0 >= query_length) {
            return ParseResult<ReadEvidenceProjection>::failure(ParseReason::kMalformedValue,
                                                                "MM query position lies outside BAM SEQ");
        }
        const std::uint64_t projected =
            strand == Strand::kReverse ? query_length - 1 - call.query_pos0 : call.query_pos0;
        methyl_calls.push_back(OrderedMethylCall{projected, &call});
    }
    std::sort(methyl_calls.begin(), methyl_calls.end(), [](const OrderedMethylCall& lhs, const OrderedMethylCall& rhs) {
        return lhs.query_pos0_reference_orientation < rhs.query_pos0_reference_orientation;
    });
    for (std::size_t index = 1; index < methyl_calls.size(); ++index) {
        if (methyl_calls[index - 1].query_pos0_reference_orientation ==
            methyl_calls[index].query_pos0_reference_orientation) {
            return ParseResult<ReadEvidenceProjection>::failure(ParseReason::kMalformedValue,
                                                                "C+m? MM calls contain a duplicate query position");
        }
    }

    ReadEvidenceProjection output;
    output.allele_calls.reserve(ordered_markers.size());
    output.methylation_calls.reserve(methyl_calls.size());
    std::size_t marker_index = 0;
    std::size_t methyl_index = 0;
    std::uint64_t query_cursor = 0;
    std::uint64_t reference_cursor = reference_interval.begin();

    while (marker_index < ordered_markers.size() &&
           ordered_markers[marker_index].position.zero_based() < reference_cursor) {
        emit_unobservable(ordered_markers[marker_index++], output.allele_calls);
    }

    for (const auto& operation : *operations.value) {
        const bool consumes_query = std::string_view("MIS=X").find(operation.code) != std::string_view::npos;
        const bool consumes_reference = std::string_view("MDN=X").find(operation.code) != std::string_view::npos;
        std::uint64_t query_end = query_cursor;
        std::uint64_t reference_end = reference_cursor;
        if ((consumes_query && !checked_add(query_cursor, operation.length, query_end)) ||
            (consumes_reference && !checked_add(reference_cursor, operation.length, reference_end))) {
            return ParseResult<ReadEvidenceProjection>::failure(ParseReason::kMalformedValue,
                                                                "CIGAR coordinate consumption overflows");
        }

        if (operation.code == 'M' || operation.code == '=' || operation.code == 'X') {
            while (marker_index < ordered_markers.size() &&
                   ordered_markers[marker_index].position.zero_based() < reference_end) {
                const auto& marker = ordered_markers[marker_index++];
                const std::uint64_t query_position = query_cursor + (marker.position.zero_based() - reference_cursor);
                if (query_position >= sequence_reference_orientation.size() ||
                    query_position >= base_qualities.size()) {
                    return ParseResult<ReadEvidenceProjection>::failure(
                        ParseReason::kMalformedValue, "CIGAR marker projection lies outside BAM SEQ/QUAL");
                }
                const std::uint8_t quality = base_qualities[query_position];
                const bool observable =
                    quality != std::numeric_limits<std::uint8_t>::max() && quality >= minimum_base_quality;
                output.allele_calls.push_back(
                    ProjectedAlleleCall{marker.site_order, marker.position,
                                        classify_allele(sequence_reference_orientation[query_position],
                                                        marker.reference, marker.alternate, observable),
                                        observable ? std::optional<std::uint64_t>(query_position) : std::nullopt,
                                        observable ? std::optional<std::uint8_t>(quality) : std::nullopt});
            }
            while (methyl_index < methyl_calls.size() &&
                   methyl_calls[methyl_index].query_pos0_reference_orientation < query_end) {
                const auto& methyl = methyl_calls[methyl_index++];
                if (methyl.query_pos0_reference_orientation < query_cursor) {
                    return ParseResult<ReadEvidenceProjection>::failure(
                        ParseReason::kMalformedValue, "MM call order regressed during CIGAR projection");
                }
                const std::uint64_t reference_position =
                    reference_cursor + (methyl.query_pos0_reference_orientation - query_cursor);
                const std::uint64_t cpg_position1 =
                    strand == Strand::kReverse ? reference_position : reference_position + 1;
                if (cpg_position1 == 0) {
                    continue;
                }
                auto position = Position1::from_value(cpg_position1);
                if (!position.ok()) {
                    return ParseResult<ReadEvidenceProjection>::failure(position.reason, std::move(position.detail));
                }
                output.methylation_calls.push_back(ProjectedMethylationCall{
                    *position.value, methyl.call->query_pos0, methyl.query_pos0_reference_orientation,
                    methyl.call->mm_group_index, methyl.call->ml_index, methyl.call->ml_raw,
                    methyl.call->probability_lower, methyl.call->probability_upper, methyl.call->skip_semantics,
                    static_cast<double>(methyl.call->ml_raw) / 255.0});
            }
        } else if (operation.code == 'D' || operation.code == 'N') {
            while (marker_index < ordered_markers.size() &&
                   ordered_markers[marker_index].position.zero_based() < reference_end) {
                emit_unobservable(ordered_markers[marker_index++], output.allele_calls);
            }
        } else if (operation.code == 'I' || operation.code == 'S') {
            while (methyl_index < methyl_calls.size() &&
                   methyl_calls[methyl_index].query_pos0_reference_orientation < query_end) {
                ++methyl_index;
            }
        }

        query_cursor = query_end;
        reference_cursor = reference_end;
    }

    if (query_cursor != sequence_reference_orientation.size() || reference_cursor != reference_interval.end()) {
        return ParseResult<ReadEvidenceProjection>::failure(
            ParseReason::kMalformedValue, "CIGAR consumption differs from BAM SEQ length or reference interval");
    }
    while (marker_index < ordered_markers.size()) {
        emit_unobservable(ordered_markers[marker_index++], output.allele_calls);
    }
    if (methyl_index != methyl_calls.size()) {
        return ParseResult<ReadEvidenceProjection>::failure(
            ParseReason::kMalformedValue, "MM call was not consumed by the complete CIGAR query span");
    }
    return output.allele_calls.empty() && output.methylation_calls.empty()
               ? ParseResult<ReadEvidenceProjection>::success_empty(std::move(output))
               : ParseResult<ReadEvidenceProjection>::success(std::move(output));
}

}  // namespace longlineage
