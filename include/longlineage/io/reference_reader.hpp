// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/common/types.hpp"

namespace longlineage {

// One worker owns one persistent FASTA handle and its explicitly named FAI.
// The reader never creates or replaces an index.
class IndexedReferenceReader final {
   public:
    [[nodiscard]] static ParseResult<std::unique_ptr<IndexedReferenceReader>> open(
        const std::filesystem::path& fasta_path, const std::filesystem::path& explicit_fai_path);

    ~IndexedReferenceReader();
    IndexedReferenceReader(IndexedReferenceReader&&) noexcept;
    IndexedReferenceReader& operator=(IndexedReferenceReader&&) noexcept;
    IndexedReferenceReader(const IndexedReferenceReader&) = delete;
    IndexedReferenceReader& operator=(const IndexedReferenceReader&) = delete;

    [[nodiscard]] ParseResult<std::uint64_t> contig_length(const ContigId& contig) const;
    [[nodiscard]] ParseResult<std::string> fetch(const ContigId& contig, Interval0 interval);
    [[nodiscard]] ParseResult<char> base(const ContigId& contig, Position1 position);
    [[nodiscard]] std::uint64_t fetch_invocations() const noexcept;

   private:
    struct Impl;
    explicit IndexedReferenceReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace longlineage
