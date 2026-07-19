// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/io/mm_ml.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/io/alignment.hpp"

namespace longlineage {
namespace {

[[nodiscard]] bool is_integer_aux_type(char type) noexcept {
    return type == 'c' || type == 'C' || type == 's' || type == 'S' || type == 'i' || type == 'I';
}

[[nodiscard]] ParseResult<std::optional<std::uint64_t>> parse_mn(const bam1_t& alignment) {
    errno = 0;
    std::uint8_t* tag = bam_aux_get(&alignment, "MN");
    if (tag == nullptr) {
        if (errno == ENOENT) {
            return ParseResult<std::optional<std::uint64_t>>::success(std::nullopt);
        }
        return ParseResult<std::optional<std::uint64_t>>::failure(ParseReason::kMalformedValue,
                                                                  "Malformed MN auxiliary tag");
    }
    if (!is_integer_aux_type(bam_aux_type(tag))) {
        return ParseResult<std::optional<std::uint64_t>>::failure(ParseReason::kMalformedValue,
                                                                  "MN must be a typed integer scalar");
    }
    errno = 0;
    const std::int64_t value = bam_aux2i(tag);
    if (errno != 0 || value < 0) {
        return ParseResult<std::optional<std::uint64_t>>::failure(ParseReason::kMalformedValue,
                                                                  "MN must be a non-negative integer");
    }
    return ParseResult<std::optional<std::uint64_t>>::success(static_cast<std::uint64_t>(value));
}

[[nodiscard]] ParseResult<std::vector<std::uint8_t>> parse_ml(const bam1_t& alignment) {
    errno = 0;
    std::uint8_t* tag = bam_aux_get(&alignment, "ML");
    if (tag == nullptr) {
        return ParseResult<std::vector<std::uint8_t>>::failure(
            errno == ENOENT ? ParseReason::kMissingRequiredField : ParseReason::kMalformedValue,
            errno == ENOENT ? "Required uppercase ML auxiliary tag is absent" : "Malformed ML auxiliary tag");
    }
    if (bam_aux_type(tag) != 'B' || tag[1] != 'C') {
        return ParseResult<std::vector<std::uint8_t>>::failure(ParseReason::kMalformedValue,
                                                               "ML must have exact typed-array form B:C");
    }
    errno = 0;
    const std::uint32_t length = bam_auxB_len(tag);
    if (errno != 0) {
        return ParseResult<std::vector<std::uint8_t>>::failure(ParseReason::kMalformedValue,
                                                               "Malformed ML B:C array length");
    }
    std::vector<std::uint8_t> values;
    values.reserve(length);
    for (std::uint32_t index = 0; index < length; ++index) {
        errno = 0;
        const std::int64_t value = bam_auxB2i(tag, index);
        if (errno != 0 || value < 0 || value > std::numeric_limits<std::uint8_t>::max()) {
            return ParseResult<std::vector<std::uint8_t>>::failure(ParseReason::kMalformedValue,
                                                                   "ML B:C element is outside uint8");
        }
        values.push_back(static_cast<std::uint8_t>(value));
    }
    return values.empty() ? ParseResult<std::vector<std::uint8_t>>::success_empty(std::move(values))
                          : ParseResult<std::vector<std::uint8_t>>::success(std::move(values));
}

[[nodiscard]] ParseResult<std::string> parse_mm_string(const bam1_t& alignment) {
    errno = 0;
    std::uint8_t* tag = bam_aux_get(&alignment, "MM");
    if (tag == nullptr) {
        return ParseResult<std::string>::failure(
            errno == ENOENT ? ParseReason::kMissingRequiredField : ParseReason::kMalformedValue,
            errno == ENOENT ? "Required uppercase MM auxiliary tag is absent" : "Malformed MM auxiliary tag");
    }
    if (bam_aux_type(tag) != 'Z') {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue, "MM must have exact scalar type Z");
    }
    errno = 0;
    const char* value = bam_aux2Z(tag);
    if (value == nullptr || errno != 0) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue, "Malformed MM Z string");
    }
    return *value == '\0' ? ParseResult<std::string>::success_empty({})
                          : ParseResult<std::string>::success(std::string(value));
}

[[nodiscard]] ParseResult<std::vector<std::uint64_t>> parse_deltas(std::string_view body) {
    std::vector<std::uint64_t> deltas;
    if (body.empty()) {
        return ParseResult<std::vector<std::uint64_t>>::success_empty(std::move(deltas));
    }
    std::size_t begin = 0;
    while (begin < body.size()) {
        const std::size_t comma = body.find(',', begin);
        const std::size_t end = comma == std::string_view::npos ? body.size() : comma;
        const std::string_view token = body.substr(begin, end - begin);
        if (token.empty()) {
            return ParseResult<std::vector<std::uint64_t>>::failure(ParseReason::kMalformedValue,
                                                                    "MM contains an empty delta");
        }
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
            return ParseResult<std::vector<std::uint64_t>>::failure(ParseReason::kMalformedValue,
                                                                    "MM delta is not an unsigned decimal integer");
        }
        deltas.push_back(value);
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return ParseResult<std::vector<std::uint64_t>>::success(std::move(deltas));
}

[[nodiscard]] ParseResult<std::string> reverse_complement(std::string_view sequence) {
    const auto complement = [](char base) -> char {
        switch (base) {
            case 'A':
                return 'T';
            case 'C':
                return 'G';
            case 'G':
                return 'C';
            case 'T':
                return 'A';
            case 'M':
                return 'K';
            case 'R':
                return 'Y';
            case 'S':
                return 'S';
            case 'V':
                return 'B';
            case 'W':
                return 'W';
            case 'Y':
                return 'R';
            case 'H':
                return 'D';
            case 'K':
                return 'M';
            case 'D':
                return 'H';
            case 'B':
                return 'V';
            case 'N':
                return 'N';
            default:
                return '\0';
        }
    };

    std::string output;
    output.reserve(sequence.size());
    for (auto iterator = sequence.rbegin(); iterator != sequence.rend(); ++iterator) {
        char base = *iterator;
        if (base >= 'a' && base <= 'z') {
            base = static_cast<char>(base - ('a' - 'A'));
        }
        const char complemented = complement(base);
        if (complemented == '\0') {
            return ParseResult<std::string>::failure(
                ParseReason::kUnsupportedValue, "BAM sequence contains a base outside the IUPAC complement vocabulary");
        }
        output.push_back(complemented);
    }
    return ParseResult<std::string>::success(std::move(output));
}

}  // namespace

ParseResult<MmMlMnTags> parse_frozen_cm_unknown(std::string_view sequence_as_sequenced, std::string_view mm,
                                                const std::vector<std::uint8_t>& ml, std::optional<std::uint64_t> mn) {
    if (mn.has_value() && *mn != sequence_as_sequenced.size()) {
        return ParseResult<MmMlMnTags>::failure(ParseReason::kUnsupportedValue,
                                                "MN does not match the current encoded query-sequence length");
    }
    if (mm.empty() || mm.back() != ';') {
        return ParseResult<MmMlMnTags>::failure(ParseReason::kMalformedValue,
                                                "MM must contain semicolon-terminated modification groups");
    }

    constexpr std::string_view kTargetGroup = "C+m?";
    constexpr std::string_view kOffsetOnlyGroup = "C+h?";
    std::optional<std::uint32_t> target_group_index;
    std::uint32_t target_ml_offset = 0;
    std::vector<std::uint64_t> target_deltas;
    std::uint64_t total_call_count = 0;
    std::set<std::string_view> observed_groups;
    std::size_t group_begin = 0;
    std::uint64_t group_index = 0;
    while (group_begin < mm.size()) {
        const std::size_t group_end = mm.find(';', group_begin);
        if (group_end == std::string_view::npos) {
            return ParseResult<MmMlMnTags>::failure(ParseReason::kMalformedValue,
                                                    "MM modification group lacks a semicolon terminator");
        }
        const std::string_view group = mm.substr(group_begin, group_end - group_begin);
        if (group.empty()) {
            return ParseResult<MmMlMnTags>::failure(ParseReason::kMalformedValue,
                                                    "MM contains an empty modification group");
        }
        const std::size_t comma = group.find(',');
        const std::string_view group_code = comma == std::string_view::npos ? group : group.substr(0, comma);
        if (group_code != kTargetGroup && group_code != kOffsetOnlyGroup) {
            return ParseResult<MmMlMnTags>::failure(ParseReason::kUnsupportedValue,
                                                    "v1 accepts C+m? and offset-only C+h? MM groups");
        }
        if (!observed_groups.insert(group_code).second) {
            return ParseResult<MmMlMnTags>::failure(ParseReason::kUnsupportedValue,
                                                    "v1 accepts at most one group for each supported MM code");
        }
        if (group_index > std::numeric_limits<std::uint32_t>::max()) {
            return ParseResult<MmMlMnTags>::failure(ParseReason::kUnsupportedValue, "MM group index exceeds uint32");
        }

        std::vector<std::uint64_t> group_deltas;
        if (comma != std::string_view::npos) {
            const std::string_view delta_body = group.substr(comma + 1);
            if (delta_body.empty() || delta_body.back() == ',') {
                return ParseResult<MmMlMnTags>::failure(ParseReason::kMalformedValue,
                                                        "MM group contains an empty delta");
            }
            auto parsed_deltas = parse_deltas(delta_body);
            if (!parsed_deltas.ok()) {
                return ParseResult<MmMlMnTags>::failure(parsed_deltas.reason, std::move(parsed_deltas.detail));
            }
            group_deltas = std::move(*parsed_deltas.value);
        }
        if (group_deltas.size() > std::numeric_limits<std::uint64_t>::max() - total_call_count) {
            return ParseResult<MmMlMnTags>::failure(ParseReason::kMalformedValue, "MM call count overflows uint64");
        }
        if (group_code == kTargetGroup) {
            if (total_call_count > std::numeric_limits<std::uint32_t>::max()) {
                return ParseResult<MmMlMnTags>::failure(ParseReason::kUnsupportedValue,
                                                        "Target C+m? ML offset exceeds uint32");
            }
            target_group_index = static_cast<std::uint32_t>(group_index);
            target_ml_offset = static_cast<std::uint32_t>(total_call_count);
            target_deltas = group_deltas;
        }
        total_call_count += group_deltas.size();
        ++group_index;
        group_begin = group_end + 1;
    }
    if (!target_group_index.has_value()) {
        return ParseResult<MmMlMnTags>::failure(ParseReason::kUnsupportedValue,
                                                "MM does not contain the required frozen C+m? group");
    }
    if (total_call_count != ml.size()) {
        return ParseResult<MmMlMnTags>::failure(ParseReason::kMalformedValue,
                                                "ML element count does not equal the total MM call count");
    }

    std::vector<std::uint64_t> cytosines;
    cytosines.reserve(sequence_as_sequenced.size());
    for (std::size_t index = 0; index < sequence_as_sequenced.size(); ++index) {
        char base = sequence_as_sequenced[index];
        if (base >= 'a' && base <= 'z') {
            base = static_cast<char>(base - ('a' - 'A'));
        }
        if (base == 'C') {
            cytosines.push_back(index);
        }
    }

    std::vector<MethylationCall> calls;
    calls.reserve(target_deltas.size());
    std::uint64_t canonical_cursor = 0;
    for (std::size_t index = 0; index < target_deltas.size(); ++index) {
        const std::uint64_t delta = target_deltas[index];
        if (delta > std::numeric_limits<std::uint64_t>::max() - canonical_cursor) {
            return ParseResult<MmMlMnTags>::failure(ParseReason::kMalformedValue, "MM delta cursor overflows");
        }
        canonical_cursor += delta;
        if (canonical_cursor >= cytosines.size()) {
            return ParseResult<MmMlMnTags>::failure(
                ParseReason::kMalformedValue, "MM delta projects beyond the encoded sequence's canonical cytosines");
        }
        const std::uint64_t raw_ml_index = static_cast<std::uint64_t>(target_ml_offset) + index;
        if (raw_ml_index > std::numeric_limits<std::uint32_t>::max()) {
            return ParseResult<MmMlMnTags>::failure(ParseReason::kUnsupportedValue, "C+m? ML index exceeds uint32");
        }
        const std::uint8_t raw = ml[raw_ml_index];
        calls.push_back(MethylationCall{cytosines[canonical_cursor], *target_group_index,
                                        static_cast<std::uint32_t>(raw_ml_index), raw, static_cast<double>(raw) / 256.0,
                                        (static_cast<double>(raw) + 1.0) / 256.0, MmSkipSemantics::kUnknown});
        ++canonical_cursor;
    }

    MmMlMnTags tags{std::string(mm), ml, mn, std::move(calls)};
    return tags.calls.empty() ? ParseResult<MmMlMnTags>::success_empty(std::move(tags))
                              : ParseResult<MmMlMnTags>::success(std::move(tags));
}

ParseResult<MmMlMnTags> parse_mm_ml_mn(const bam1_t& alignment) {
    auto sequence = decode_bam_sequence(alignment);
    if (!sequence.ok()) {
        return ParseResult<MmMlMnTags>::failure(sequence.reason, std::move(sequence.detail));
    }
    return parse_mm_ml_mn(alignment, *sequence.value);
}

ParseResult<MmMlMnTags> parse_mm_ml_mn(const bam1_t& alignment,
                                       std::string_view decoded_bam_sequence_reference_orientation) {
    if (alignment.core.l_qseq < 0 ||
        decoded_bam_sequence_reference_orientation.size() != static_cast<std::size_t>(alignment.core.l_qseq)) {
        return ParseResult<MmMlMnTags>::failure(
            ParseReason::kMalformedValue, "supplied decoded BAM sequence length differs from alignment.core.l_qseq");
    }
    auto mm = parse_mm_string(alignment);
    if (!mm.ok()) {
        return ParseResult<MmMlMnTags>::failure(mm.reason, std::move(mm.detail));
    }
    auto ml = parse_ml(alignment);
    if (!ml.ok()) {
        return ParseResult<MmMlMnTags>::failure(ml.reason, std::move(ml.detail));
    }
    auto mn = parse_mn(alignment);
    if (!mn.ok()) {
        return ParseResult<MmMlMnTags>::failure(mn.reason, std::move(mn.detail));
    }
    std::string sequence(decoded_bam_sequence_reference_orientation);
    if ((alignment.core.flag & BAM_FREVERSE) != 0) {
        auto as_sequenced = reverse_complement(sequence);
        if (!as_sequenced.ok()) {
            return ParseResult<MmMlMnTags>::failure(as_sequenced.reason, std::move(as_sequenced.detail));
        }
        sequence = std::move(*as_sequenced.value);
    }
    return parse_frozen_cm_unknown(sequence, *mm.value, *ml.value, *mn.value);
}

}  // namespace longlineage
