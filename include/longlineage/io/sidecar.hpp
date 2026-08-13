// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/io/alignment.hpp"

namespace longlineage {

inline constexpr std::string_view kLatestTagSidecarHeader = "#CHROM\tSTART0\tEND0\tQNAME\tFLAG\tMAPQ\tCIGAR_B2\tHP\tPS";

enum class LatestTagSourceRole {
    kAuthoritativeLatestHpPsSidecar,
};

struct LatestTags {
    std::string hp;
    std::optional<std::uint64_t> ps;
    LatestTagSourceRole source_role = LatestTagSourceRole::kAuthoritativeLatestHpPsSidecar;

    friend bool operator==(const LatestTags& lhs, const LatestTags& rhs) noexcept {
        return lhs.hp == rhs.hp && lhs.ps == rhs.ps && lhs.source_role == rhs.source_role;
    }
    friend bool operator!=(const LatestTags& lhs, const LatestTags& rhs) noexcept { return !(lhs == rhs); }
};

struct SidecarFullIdentity {
    std::string raw_qname;
    ContigId contig;
    Interval0 reference_interval;
    std::uint16_t flag;
    std::string cigar_blake2b64;

    friend bool operator==(const SidecarFullIdentity& lhs, const SidecarFullIdentity& rhs) noexcept {
        return std::tie(lhs.raw_qname, lhs.contig, lhs.reference_interval, lhs.flag, lhs.cigar_blake2b64) ==
               std::tie(rhs.raw_qname, rhs.contig, rhs.reference_interval, rhs.flag, rhs.cigar_blake2b64);
    }
    friend bool operator!=(const SidecarFullIdentity& lhs, const SidecarFullIdentity& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const SidecarFullIdentity& lhs, const SidecarFullIdentity& rhs) noexcept {
        return std::tie(lhs.raw_qname, lhs.contig, lhs.reference_interval, lhs.flag, lhs.cigar_blake2b64) <
               std::tie(rhs.raw_qname, rhs.contig, rhs.reference_interval, rhs.flag, rhs.cigar_blake2b64);
    }
};

struct SidecarRecord {
    ReadProjectionIdentity projection;
    SidecarFullIdentity full_identity;
    LatestTags latest_tags;
};

enum class JoinStatus {
    kExactMatch,
    kError,
};

enum class JoinReason {
    kNone,
    kMissingProjection,
    kFullIdentityMismatch,
    kConflictingTags,
    kMultipleFullIdentities,
};

[[nodiscard]] constexpr std::string_view to_string(JoinStatus status) noexcept {
    return status == JoinStatus::kExactMatch ? "EXACT_MATCH" : "ERROR";
}

[[nodiscard]] constexpr std::string_view to_string(JoinReason reason) noexcept {
    switch (reason) {
        case JoinReason::kNone:
            return "NONE";
        case JoinReason::kMissingProjection:
            return "MISSING_PROJECTION";
        case JoinReason::kFullIdentityMismatch:
            return "FULL_IDENTITY_MISMATCH";
        case JoinReason::kConflictingTags:
            return "CONFLICTING_TAGS";
        case JoinReason::kMultipleFullIdentities:
            return "MULTIPLE_FULL_IDENTITIES";
    }
    return "CONFLICTING_TAGS";
}

struct LatestTagJoinResult {
    std::optional<LatestTags> tags;
    JoinStatus status;
    JoinReason reason;
    std::string detail;
    std::uint64_t exact_identity_occurrences = 0;
};

class SidecarLookup {
   public:
    [[nodiscard]] ParseResult<std::uint64_t> add(const SidecarRecord& record);
    [[nodiscard]] LatestTagJoinResult join(const ReadProjectionIdentity& projection,
                                           const SidecarFullIdentity& expected_full_identity) const;

    [[nodiscard]] std::uint64_t rows_fetched() const noexcept { return rows_fetched_; }
    [[nodiscard]] std::uint64_t rows_eligible() const noexcept { return rows_eligible_; }
    [[nodiscard]] std::size_t projection_count() const noexcept { return records_.size(); }

   private:
    struct ExactEntry {
        LatestTags tags;
        std::uint64_t occurrences;
    };
    std::map<ReadProjectionIdentity, std::map<SidecarFullIdentity, ExactEntry>> records_;
    std::uint64_t rows_fetched_ = 0;
    std::uint64_t rows_eligible_ = 0;
};

[[nodiscard]] ParseResult<SidecarRecord> parse_sidecar_row(std::string_view line);
[[nodiscard]] SidecarFullIdentity sidecar_identity_from_alignment(const FullAlignmentIdentity& identity);

// One worker owns one persistent sidecar stream and its explicitly named Tabix
// index. Instances are move-only and must not be shared between worker threads.
class IndexedSidecarReader final {
   public:
    [[nodiscard]] static ParseResult<std::unique_ptr<IndexedSidecarReader>> open(
        const std::filesystem::path& sidecar_path, const std::filesystem::path& explicit_index_path);

    ~IndexedSidecarReader();
    IndexedSidecarReader(IndexedSidecarReader&&) noexcept;
    IndexedSidecarReader& operator=(IndexedSidecarReader&&) noexcept;
    IndexedSidecarReader(const IndexedSidecarReader&) = delete;
    IndexedSidecarReader& operator=(const IndexedSidecarReader&) = delete;

    [[nodiscard]] ParseResult<SidecarLookup> fetch(const ContigId& contig, Interval0 interval);
    [[nodiscard]] std::uint64_t fetch_invocations() const noexcept;

   private:
    struct Impl;
    explicit IndexedSidecarReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Opens an explicit Tabix index and returns all eligible records overlapping
// the half-open query interval. Empty overlap is OK_EMPTY, never ERROR.
// Prefer IndexedSidecarReader for repeated production queries.
[[nodiscard]] ParseResult<SidecarLookup> fetch_sidecar_lookup(const std::string& sidecar_path,
                                                              const std::string& index_path, const ContigId& contig,
                                                              Interval0 interval);

}  // namespace longlineage
