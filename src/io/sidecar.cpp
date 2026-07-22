// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/io/sidecar.hpp"

#include <htslib/hts.h>
#include <htslib/kstring.h>
#include <htslib/tbx.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"

namespace longlineage {
namespace {

constexpr std::uint16_t kExcludedFlags = BAM_FUNMAP | BAM_FSECONDARY | BAM_FSUPPLEMENTARY | BAM_FDUP;

using IteratorPointer = std::unique_ptr<hts_itr_t, decltype(&hts_itr_destroy)>;

[[nodiscard]] std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t tab = line.find('\t', begin);
        fields.push_back(line.substr(begin, tab == std::string_view::npos ? line.size() - begin : tab - begin));
        if (tab == std::string_view::npos) {
            break;
        }
        begin = tab + 1;
    }
    return fields;
}

template <typename Integer>
[[nodiscard]] bool parse_unsigned(std::string_view text, Integer& value) {
    static_assert(std::numeric_limits<Integer>::is_integer && !std::numeric_limits<Integer>::is_signed);
    if (text.empty()) {
        return false;
    }
    Integer parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] bool parse_ps(std::string_view text, std::optional<std::uint64_t>& ps) {
    if (text == ".") {
        ps = std::nullopt;
        return true;
    }
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    ps = parsed;
    return true;
}

[[nodiscard]] bool is_lower_hex_16(std::string_view value) {
    return value.size() == 16 && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
           });
}

}  // namespace

ParseResult<std::uint64_t> SidecarLookup::add(const SidecarRecord& record) {
    ++rows_fetched_;
    if ((record.full_identity.flag & kExcludedFlags) != 0) {
        return ParseResult<std::uint64_t>::success(rows_eligible_);
    }
    ++rows_eligible_;
    auto& by_full_identity = records_[record.projection];
    const auto existing = by_full_identity.find(record.full_identity);
    if (existing == by_full_identity.end()) {
        by_full_identity.emplace(record.full_identity, ExactEntry{record.latest_tags, 1});
        return ParseResult<std::uint64_t>::success(rows_eligible_);
    }
    if (existing->second.tags != record.latest_tags) {
        return ParseResult<std::uint64_t>::failure(ParseReason::kMalformedValue,
                                                   "Conflicting HP/PS for one exact sidecar identity");
    }
    ++existing->second.occurrences;
    return ParseResult<std::uint64_t>::success(rows_eligible_);
}

LatestTagJoinResult SidecarLookup::join(const ReadProjectionIdentity& projection,
                                        const SidecarFullIdentity& expected_full_identity) const {
    const auto projection_it = records_.find(projection);
    if (projection_it == records_.end()) {
        return LatestTagJoinResult{std::nullopt, JoinStatus::kError, JoinReason::kMissingProjection,
                                   "Authoritative latest HP/PS sidecar lacks the read projection", 0};
    }
    if (projection_it->second.size() != 1) {
        return LatestTagJoinResult{std::nullopt, JoinStatus::kError, JoinReason::kMultipleFullIdentities,
                                   "Projection maps to multiple distinct sidecar full identities", 0};
    }
    const auto exact_it = projection_it->second.find(expected_full_identity);
    if (exact_it == projection_it->second.end()) {
        return LatestTagJoinResult{std::nullopt, JoinStatus::kError, JoinReason::kFullIdentityMismatch,
                                   "Projection exists but its exact FLAG/CIGAR identity does not match", 0};
    }
    return LatestTagJoinResult{
        exact_it->second.tags, JoinStatus::kExactMatch, JoinReason::kNone, {}, exact_it->second.occurrences};
}

ParseResult<SidecarRecord> parse_sidecar_row(std::string_view line) {
    if (line.empty()) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kMalformedValue, "Sidecar row must not be empty");
    }
    if (line.back() == '\r' || line.find('\n') != std::string_view::npos || line.find('\0') != std::string_view::npos) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kMalformedValue,
                                                   "Sidecar row must use LF framing without embedded controls");
    }
    const auto fields = split_tabs(line);
    if (fields.size() != 9) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kMalformedValue,
                                                   "Sidecar row must contain exactly nine tab-separated fields");
    }
    auto contig = ContigId::from_string(std::string(fields[0]));
    if (!contig.ok()) {
        return ParseResult<SidecarRecord>::failure(contig.reason, std::move(contig.detail));
    }
    std::uint64_t start0 = 0;
    std::uint64_t end0 = 0;
    std::uint16_t flag = 0;
    std::uint8_t mapq = 0;
    if (!parse_unsigned(fields[1], start0) || !parse_unsigned(fields[2], end0) || !parse_unsigned(fields[4], flag) ||
        !parse_unsigned(fields[5], mapq)) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kMalformedValue,
                                                   "Sidecar coordinates, FLAG, or MAPQ are malformed");
    }
    auto interval = Interval0::from_bounds(start0, end0);
    if (!interval.ok()) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kMalformedValue,
                                                   "Sidecar alignment interval must be non-empty");
    }
    if (fields[3].empty()) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kMalformedValue, "Sidecar QNAME must not be empty");
    }
    if (!is_lower_hex_16(fields[6])) {
        return ParseResult<SidecarRecord>::failure(
            ParseReason::kMalformedValue, "CIGAR_B2 must be 16 lowercase hex digits from BLAKE2b digest_size=8");
    }
    if (fields[7].empty()) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kMalformedValue, "Sidecar HP must not be empty");
    }
    static const std::set<std::string_view> kSupportedRawHp = {
        ".", "1", "2", "3", "4", "1-1", "2-1", "1-2", "2-2",
    };
    if (kSupportedRawHp.count(fields[7]) == 0) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kUnsupportedValue,
                                                   "Sidecar HP is outside the frozen LongPhase-S vocabulary");
    }
    std::string hp = fields[7] == "." ? "0" : std::string(fields[7]);
    std::optional<std::uint64_t> ps;
    if (!parse_ps(fields[8], ps)) {
        return ParseResult<SidecarRecord>::failure(ParseReason::kMalformedValue,
                                                   "Sidecar PS must be a non-negative integer or '.'");
    }
    const Strand strand = (flag & BAM_FREVERSE) != 0 ? Strand::kReverse : Strand::kForward;
    const std::string raw_qname(fields[3]);
    ReadProjectionIdentity projection{projected_read_name(raw_qname, flag), *contig.value, *interval.value, mapq,
                                      strand};
    SidecarFullIdentity full_identity{raw_qname, std::move(*contig.value), std::move(*interval.value), flag,
                                      std::string(fields[6])};
    return ParseResult<SidecarRecord>::success(
        SidecarRecord{std::move(projection), std::move(full_identity), LatestTags{std::move(hp), ps}});
}

SidecarFullIdentity sidecar_identity_from_alignment(const FullAlignmentIdentity& identity) {
    return SidecarFullIdentity{identity.raw_qname, identity.projection.contig, identity.projection.reference_interval,
                               identity.flag, blake2b_64_hex(identity.cigar)};
}

struct IndexedSidecarReader::Impl {
    htsFile* file = nullptr;
    tbx_t* index = nullptr;
    std::uint64_t fetch_invocations = 0;

    ~Impl() {
        if (index != nullptr) {
            tbx_destroy(index);
        }
        if (file != nullptr) {
            hts_close(file);
        }
    }
};

IndexedSidecarReader::IndexedSidecarReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
IndexedSidecarReader::~IndexedSidecarReader() = default;
IndexedSidecarReader::IndexedSidecarReader(IndexedSidecarReader&&) noexcept = default;
IndexedSidecarReader& IndexedSidecarReader::operator=(IndexedSidecarReader&&) noexcept = default;

ParseResult<std::unique_ptr<IndexedSidecarReader>> IndexedSidecarReader::open(
    const std::filesystem::path& sidecar_path, const std::filesystem::path& explicit_index_path) {
    const std::string sidecar_name = sidecar_path.string();
    const std::string index_name = explicit_index_path.string();
    if (sidecar_name.empty() || index_name.empty()) {
        return ParseResult<std::unique_ptr<IndexedSidecarReader>>::failure(
            ParseReason::kMissingRequiredField, "Sidecar and explicit Tabix index paths must both be non-empty");
    }
    auto impl = std::make_unique<Impl>();
    impl->file = hts_open(sidecar_name.c_str(), "r");
    if (impl->file == nullptr) {
        return ParseResult<std::unique_ptr<IndexedSidecarReader>>::failure(
            ParseReason::kIoError, "Cannot open authoritative latest HP/PS sidecar");
    }
    impl->index = tbx_index_load3(sidecar_name.c_str(), index_name.c_str(), 0);
    if (impl->index == nullptr) {
        return ParseResult<std::unique_ptr<IndexedSidecarReader>>::failure(
            ParseReason::kIndexError, "Cannot open explicit latest HP/PS sidecar Tabix index");
    }
    return ParseResult<std::unique_ptr<IndexedSidecarReader>>::success(
        std::unique_ptr<IndexedSidecarReader>(new IndexedSidecarReader(std::move(impl))));
}

ParseResult<SidecarLookup> IndexedSidecarReader::fetch(const ContigId& contig, Interval0 interval) {
    if (impl_ == nullptr || impl_->file == nullptr || impl_->index == nullptr) {
        return ParseResult<SidecarLookup>::failure(ParseReason::kIoError, "Sidecar reader is not open");
    }
    if (interval.begin() > static_cast<std::uint64_t>(HTS_POS_MAX) ||
        interval.end() > static_cast<std::uint64_t>(HTS_POS_MAX)) {
        return ParseResult<SidecarLookup>::failure(ParseReason::kUnsupportedValue,
                                                   "Sidecar query interval exceeds HTSlib coordinate range");
    }
    const int tid = tbx_name2id(impl_->index, contig.value().c_str());
    SidecarLookup lookup;
    if (tid < 0) {
        return ParseResult<SidecarLookup>::failure(ParseReason::kIndexError,
                                                   "Sidecar Tabix index does not contain requested contig");
    }
    IteratorPointer iterator(tbx_itr_queryi(impl_->index, tid, static_cast<hts_pos_t>(interval.begin()),
                                            static_cast<hts_pos_t>(interval.end())),
                             &hts_itr_destroy);
    ++impl_->fetch_invocations;
    if (!iterator) {
        return ParseResult<SidecarLookup>::failure(ParseReason::kIndexError,
                                                   "Cannot construct sidecar Tabix interval iterator");
    }

    kstring_t line{0, 0, nullptr};
    int result = 0;
    while ((result = tbx_itr_next(impl_->file, impl_->index, iterator.get(), &line)) >= 0) {
        const auto parsed = parse_sidecar_row(std::string_view(line.s, line.l));
        if (!parsed.ok()) {
            std::free(line.s);
            return ParseResult<SidecarLookup>::failure(parsed.reason, parsed.detail);
        }
        const auto added = lookup.add(*parsed.value);
        if (!added.ok()) {
            std::free(line.s);
            return ParseResult<SidecarLookup>::failure(added.reason, added.detail);
        }
    }
    std::free(line.s);
    if (result < -1) {
        return ParseResult<SidecarLookup>::failure(ParseReason::kIoError,
                                                   "Sidecar Tabix iterator reported a read error");
    }
    return lookup.rows_fetched() == 0 ? ParseResult<SidecarLookup>::success_empty(std::move(lookup))
                                      : ParseResult<SidecarLookup>::success(std::move(lookup));
}

std::uint64_t IndexedSidecarReader::fetch_invocations() const noexcept {
    return impl_ == nullptr ? 0 : impl_->fetch_invocations;
}

ParseResult<SidecarLookup> fetch_sidecar_lookup(const std::string& sidecar_path, const std::string& index_path,
                                                const ContigId& contig, Interval0 interval) {
    auto reader = IndexedSidecarReader::open(sidecar_path, index_path);
    if (!reader.ok()) {
        return ParseResult<SidecarLookup>::failure(reader.reason, std::move(reader.detail));
    }
    return (*reader.value)->fetch(contig, interval);
}

}  // namespace longlineage
