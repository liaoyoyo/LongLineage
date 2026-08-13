// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/io/reference_reader.hpp"

#include <htslib/faidx.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace longlineage {

struct IndexedReferenceReader::Impl {
    faidx_t* index = nullptr;
    std::uint64_t fetch_invocations = 0;

    ~Impl() {
        if (index != nullptr) {
            fai_destroy(index);
        }
    }
};

IndexedReferenceReader::IndexedReferenceReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
IndexedReferenceReader::~IndexedReferenceReader() = default;
IndexedReferenceReader::IndexedReferenceReader(IndexedReferenceReader&&) noexcept = default;
IndexedReferenceReader& IndexedReferenceReader::operator=(IndexedReferenceReader&&) noexcept = default;

ParseResult<std::unique_ptr<IndexedReferenceReader>> IndexedReferenceReader::open(
    const std::filesystem::path& fasta_path, const std::filesystem::path& explicit_fai_path) {
    const std::string fasta_name = fasta_path.string();
    const std::string fai_name = explicit_fai_path.string();
    if (fasta_name.empty() || fai_name.empty()) {
        return ParseResult<std::unique_ptr<IndexedReferenceReader>>::failure(
            ParseReason::kMissingRequiredField, "FASTA and explicit FAI paths must both be non-empty");
    }
    auto impl = std::make_unique<Impl>();
    impl->index = fai_load3(fasta_name.c_str(), fai_name.c_str(), nullptr, 0);
    if (impl->index == nullptr) {
        return ParseResult<std::unique_ptr<IndexedReferenceReader>>::failure(
            ParseReason::kIndexError, "Cannot open FASTA with the explicitly named FAI");
    }
    return ParseResult<std::unique_ptr<IndexedReferenceReader>>::success(
        std::unique_ptr<IndexedReferenceReader>(new IndexedReferenceReader(std::move(impl))));
}

ParseResult<std::uint64_t> IndexedReferenceReader::contig_length(const ContigId& contig) const {
    if (impl_ == nullptr || impl_->index == nullptr) {
        return ParseResult<std::uint64_t>::failure(ParseReason::kIoError, "Reference reader is not open");
    }
    const hts_pos_t observed = faidx_seq_len64(impl_->index, contig.value().c_str());
    if (observed <= 0) {
        return ParseResult<std::uint64_t>::failure(ParseReason::kIndexError,
                                                   "Reference contig is absent or has non-positive length");
    }
    return ParseResult<std::uint64_t>::success(static_cast<std::uint64_t>(observed));
}

ParseResult<std::string> IndexedReferenceReader::fetch(const ContigId& contig, Interval0 interval) {
    if (impl_ == nullptr || impl_->index == nullptr) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, "Reference reader is not open");
    }
    const auto length = contig_length(contig);
    if (!length.ok()) {
        return ParseResult<std::string>::failure(length.reason, length.detail);
    }
    if (interval.end() > *length.value || interval.begin() > static_cast<std::uint64_t>(HTS_POS_MAX) ||
        interval.end() - 1 > static_cast<std::uint64_t>(HTS_POS_MAX)) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "Reference interval lies outside the indexed contig");
    }
    hts_pos_t observed_length = 0;
    char* raw = faidx_fetch_seq64(impl_->index, contig.value().c_str(), static_cast<hts_pos_t>(interval.begin()),
                                  static_cast<hts_pos_t>(interval.end() - 1), &observed_length);
    ++impl_->fetch_invocations;
    if (raw == nullptr || observed_length < 0) {
        std::free(raw);
        return ParseResult<std::string>::failure(ParseReason::kIoError, "FASTA interval fetch failed");
    }
    if (static_cast<std::uint64_t>(observed_length) != interval.size()) {
        std::free(raw);
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                 "FASTA fetch length differs from the requested interval");
    }
    std::string sequence(raw, static_cast<std::size_t>(observed_length));
    std::free(raw);
    std::transform(sequence.begin(), sequence.end(), sequence.begin(),
                   [](char base) { return static_cast<char>(std::toupper(static_cast<unsigned char>(base))); });
    // Preserve the canonical DNA IUPAC symbol instead of guessing one of its
    // possible bases. Downstream CpG admission requires an exact C followed by
    // an exact G, so an ambiguity symbol can never fabricate a CpG.
    const auto invalid = std::find_if(sequence.begin(), sequence.end(), [](char base) {
        return std::string_view("ACGTRYSWKMBDHVN").find(base) == std::string_view::npos;
    });
    if (invalid != sequence.end()) {
        const auto offset = static_cast<std::uint64_t>(std::distance(sequence.begin(), invalid));
        const auto byte = static_cast<unsigned int>(static_cast<unsigned char>(*invalid));
        return ParseResult<std::string>::failure(
            ParseReason::kUnsupportedValue, "Reference contains a base outside the DNA IUPAC vocabulary at " +
                                                contig.value() + ":" + std::to_string(interval.begin() + offset + 1U) +
                                                " (byte_decimal=" + std::to_string(byte) + ")");
    }
    return ParseResult<std::string>::success(std::move(sequence));
}

ParseResult<char> IndexedReferenceReader::base(const ContigId& contig, Position1 position) {
    if (position.zero_based() == std::numeric_limits<std::uint64_t>::max()) {
        return ParseResult<char>::failure(ParseReason::kMalformedValue, "Reference position overflows");
    }
    const auto interval = Interval0::from_bounds(position.zero_based(), position.value());
    if (!interval.ok()) {
        return ParseResult<char>::failure(interval.reason, interval.detail);
    }
    auto sequence = fetch(contig, *interval.value);
    if (!sequence.ok()) {
        return ParseResult<char>::failure(sequence.reason, std::move(sequence.detail));
    }
    if (sequence.value->size() != 1) {
        return ParseResult<char>::failure(ParseReason::kMalformedValue, "Single-base FASTA fetch was not singular");
    }
    return ParseResult<char>::success(sequence.value->front());
}

std::uint64_t IndexedReferenceReader::fetch_invocations() const noexcept {
    return impl_ == nullptr ? 0 : impl_->fetch_invocations;
}

}  // namespace longlineage
