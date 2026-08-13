// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/pipeline/block_science.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "longlineage/common/digest.hpp"

namespace longlineage::pipeline {
namespace {

[[nodiscard]] std::size_t saturating_add(std::size_t lhs, std::size_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::numeric_limits<std::size_t>::max();
    }
    return lhs + rhs;
}

[[nodiscard]] bool marker_contract_holds(const AlignmentBlock& block,
                                         const std::vector<VariantSite>& markers) noexcept {
    for (std::size_t index = 0; index < markers.size(); ++index) {
        const VariantSite& marker = markers[index];
        if (marker.dataset_order != block.dataset_order || marker.dataset_id != block.dataset_id ||
            marker.contig != block.contig || !block.fetch_interval.contains(marker.position)) {
            return false;
        }
        if (index > 0 &&
            (!(markers[index - 1].position < marker.position) || markers[index - 1].site_order >= marker.site_order)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ParseResult<std::vector<SsnvMarker>> make_projection_markers(const std::vector<VariantSite>& markers) {
    std::vector<SsnvMarker> output;
    output.reserve(markers.size());
    for (const VariantSite& marker : markers) {
        output.push_back(SsnvMarker{marker.site_order, marker.position, marker.reference, marker.alternate});
    }
    return ParseResult<std::vector<SsnvMarker>>::success(std::move(output));
}

[[nodiscard]] bool is_reference_cpg(const ProjectedMethylationCall& call, Interval0 context_interval,
                                    std::string_view context) noexcept {
    const std::uint64_t c_zero = call.candidate_cpg_position.zero_based();
    if (c_zero < context_interval.begin() || c_zero >= context_interval.end()) {
        return false;
    }
    const std::uint64_t relative = c_zero - context_interval.begin();
    if (relative >= context.size() || relative + 1 >= context.size()) {
        return false;
    }
    return context[static_cast<std::size_t>(relative)] == 'C' && context[static_cast<std::size_t>(relative + 1)] == 'G';
}

[[nodiscard]] ParseResult<std::uint32_t> checked_occurrences(std::size_t value, std::string_view source) {
    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        return ParseResult<std::uint32_t>::failure(ParseReason::kMalformedValue,
                                                   std::string(source) + " occurrence count is zero or exceeds uint32");
    }
    return ParseResult<std::uint32_t>::success(static_cast<std::uint32_t>(value));
}

template <typename Integer>
void append_integer(std::string& output, Integer value) {
    char buffer[32]{};
    const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (encoded.ec != std::errc{}) {
        throw std::runtime_error("cannot canonicalize block integer");
    }
    output.append(buffer, encoded.ptr);
}

void append_field(std::string& output, std::string_view value) {
    append_integer(output, value.size());
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

}  // namespace

std::size_t BlockScienceEvidence::logical_retained_bytes() const noexcept {
    std::size_t bytes = sizeof(BlockScienceEvidence);
    bytes = saturating_add(bytes, markers.size() * sizeof(VariantSite));
    bytes = saturating_add(bytes, focal_sites.size() * sizeof(FocalSiteEvidence));
    for (const VariantSite& marker : markers) {
        bytes = saturating_add(bytes, marker.dataset_id.size());
        bytes = saturating_add(bytes, marker.contig.value().size());
    }
    for (const FocalSiteEvidence& focal : focal_sites) {
        bytes = saturating_add(bytes, focal.covering_read_indices.size() * sizeof(std::size_t));
    }
    for (const JoinedReadEvidence& read : reads) {
        bytes = saturating_add(bytes, sizeof(JoinedReadEvidence));
        bytes = saturating_add(bytes, read.read_id.size());
        bytes = saturating_add(bytes, read.projection.qname.size());
        bytes = saturating_add(bytes, read.projection.contig.value().size());
        bytes = saturating_add(bytes, read.cigar_blake2b64.size());
        bytes = saturating_add(bytes, read.typed_aux_sha256_without_rg.size());
        bytes = saturating_add(bytes, read.latest_tags.hp.size());
        bytes = saturating_add(bytes, read.allele_calls.size() * sizeof(ProjectedAlleleCall));
        bytes = saturating_add(bytes, read.methylation_calls.size() * sizeof(ProjectedMethylationCall));
    }
    return bytes;
}

const ProjectedAlleleCall* find_allele_call(const JoinedReadEvidence& read, std::uint64_t site_order) noexcept {
    const auto found = std::lower_bound(
        read.allele_calls.begin(), read.allele_calls.end(), site_order,
        [](const ProjectedAlleleCall& call, std::uint64_t requested) { return call.site_order < requested; });
    return found != read.allele_calls.end() && found->site_order == site_order ? &*found : nullptr;
}

ParseResult<std::string> block_science_semantic_sha256(const BlockScienceEvidence& block) {
    try {
        std::string canonical;
        canonical.reserve(block.logical_retained_bytes() / 2);
        canonical.append("longlineage.block_science\t1.0.0\n");
        append_integer(canonical, block.block_sequence);
        canonical.push_back('\n');
        for (const VariantSite& marker : block.markers) {
            canonical.push_back('V');
            append_integer(canonical, marker.dataset_order);
            canonical.push_back('|');
            append_field(canonical, marker.dataset_id);
            append_integer(canonical, marker.site_order);
            canonical.push_back('|');
            append_field(canonical, marker.contig.value());
            append_integer(canonical, marker.position.value());
            canonical.push_back('|');
            canonical.push_back(marker.reference);
            canonical.push_back('|');
            canonical.push_back(marker.alternate);
            canonical.push_back('\n');
        }
        for (const JoinedReadEvidence& read : block.reads) {
            canonical.push_back('R');
            append_field(canonical, read.read_id);
            append_field(canonical, read.projection.contig.value());
            append_integer(canonical, read.projection.reference_interval.begin());
            canonical.push_back('|');
            append_integer(canonical, read.projection.reference_interval.end());
            canonical.push_back('|');
            append_integer(canonical, read.projection.mapq);
            canonical.push_back('|');
            canonical.push_back(to_char(read.projection.strand));
            canonical.push_back('|');
            append_integer(canonical, read.alignment_flag);
            canonical.push_back('|');
            append_field(canonical, read.cigar_blake2b64);
            append_field(canonical, read.typed_aux_sha256_without_rg);
            append_integer(canonical, read.query_length);
            canonical.push_back('|');
            if (read.mn_value.has_value()) {
                append_integer(canonical, *read.mn_value);
            } else {
                canonical.push_back('.');
            }
            canonical.push_back('|');
            append_field(canonical, read.latest_tags.hp);
            if (read.latest_tags.ps.has_value()) {
                append_integer(canonical, *read.latest_tags.ps);
            } else {
                canonical.push_back('.');
            }
            canonical.push_back('|');
            append_integer(canonical, read.raw_alignment_occurrences);
            canonical.push_back('|');
            append_integer(canonical, read.sidecar_identity_occurrences);
            canonical.push_back('\n');
            for (const ProjectedAlleleCall& call : read.allele_calls) {
                canonical.push_back('A');
                append_integer(canonical, call.site_order);
                canonical.push_back('|');
                canonical.push_back(to_char(call.call));
                canonical.push_back('|');
                if (call.base_quality.has_value()) {
                    append_integer(canonical, *call.base_quality);
                } else {
                    canonical.push_back('.');
                }
                canonical.push_back('\n');
            }
            for (const ProjectedMethylationCall& call : read.methylation_calls) {
                canonical.push_back('M');
                append_integer(canonical, call.candidate_cpg_position.value());
                canonical.push_back('|');
                append_integer(canonical, call.query_pos0_as_sequenced);
                canonical.push_back('|');
                append_integer(canonical, call.query_pos0_reference_orientation);
                canonical.push_back('|');
                append_integer(canonical, call.mm_group_index);
                canonical.push_back('|');
                append_integer(canonical, call.ml_index);
                canonical.push_back('|');
                append_integer(canonical, call.ml_raw);
                canonical.push_back('|');
                canonical.append(to_string(call.skip_semantics));
                canonical.push_back('\n');
            }
        }
        for (const FocalSiteEvidence& focal : block.focal_sites) {
            canonical.push_back('F');
            append_integer(canonical, focal.site.site_order);
            canonical.push_back('|');
            for (const std::size_t read_index : focal.covering_read_indices) {
                append_integer(canonical, read_index);
                canonical.push_back(',');
            }
            canonical.push_back('\n');
        }
        return sha256_hex(canonical);
    } catch (const std::exception& error) {
        return ParseResult<std::string>::failure(ParseReason::kIoError,
                                                 std::string("cannot build block semantic digest: ") + error.what());
    }
}

ParseResult<BlockScienceEvidence> build_block_science_evidence(
    const AlignmentBlock& block, const std::vector<VariantSite>& ordered_markers, const BlockReadBatch& raw_batch,
    const SidecarLookup& sidecar_lookup, Interval0 reference_context_interval, std::string_view reference_context,
    const BlockScienceOptions& options) {
    if (raw_batch.block_sequence != block.sequence) {
        return ParseResult<BlockScienceEvidence>::failure(
            ParseReason::kMalformedValue, "BAM batch sequence differs from the alignment block sequence");
    }
    if (options.minimum_base_quality != 20) {
        return ParseResult<BlockScienceEvidence>::failure(ParseReason::kUnsupportedValue,
                                                          "v1 block science minimum base quality is frozen at 20");
    }
    if (reference_context.size() != reference_context_interval.size() ||
        reference_context_interval.begin() > block.fetch_interval.begin() ||
        reference_context_interval.end() < block.fetch_interval.end()) {
        return ParseResult<BlockScienceEvidence>::failure(
            ParseReason::kMalformedValue, "reference context does not cover the complete block fetch interval");
    }
    if (!marker_contract_holds(block, ordered_markers)) {
        return ParseResult<BlockScienceEvidence>::failure(ParseReason::kMalformedValue,
                                                          "block marker identity/order/fetch membership is malformed");
    }
    for (const FocalSiteCost& focal : block.focal_sites) {
        const auto found =
            std::find_if(ordered_markers.begin(), ordered_markers.end(), [&focal](const VariantSite& marker) {
                return marker.vcf_record_order == focal.vcf_record_order && marker.position == focal.position;
            });
        if (found == ordered_markers.end()) {
            return ParseResult<BlockScienceEvidence>::failure(
                ParseReason::kMalformedValue, "a focal block site is absent from the projection marker universe");
        }
    }
    auto projection_markers = make_projection_markers(ordered_markers);
    if (!projection_markers.ok()) {
        return ParseResult<BlockScienceEvidence>::failure(projection_markers.reason,
                                                          std::move(projection_markers.detail));
    }

    std::map<ReadProjectionIdentity, std::vector<const DecodedBlockRead*>> by_projection;
    for (const DecodedBlockRead& read : raw_batch.reads) {
        by_projection[read.identity.projection].push_back(&read);
    }

    BlockScienceEvidence output;
    output.block_sequence = block.sequence;
    output.markers = ordered_markers;
    output.counters.raw_records_after_filter = raw_batch.reads.size();
    output.counters.sidecar_rows_fetched = sidecar_lookup.rows_fetched();
    output.counters.sidecar_rows_eligible = sidecar_lookup.rows_eligible();
    output.reads.reserve(by_projection.size());
    std::map<std::string, ReadProjectionIdentity> read_id_bindings;

    for (const auto& [projection, occurrences] : by_projection) {
        const DecodedBlockRead& representative = *occurrences.front();
        for (const DecodedBlockRead* occurrence : occurrences) {
            if (occurrence == nullptr || occurrence->identity != representative.identity ||
                occurrence->sequence_reference_orientation != representative.sequence_reference_orientation ||
                occurrence->base_qualities != representative.base_qualities ||
                occurrence->mm_ml.mm != representative.mm_ml.mm || occurrence->mm_ml.ml != representative.mm_ml.ml ||
                occurrence->mm_ml.mn != representative.mm_ml.mn) {
                return ParseResult<BlockScienceEvidence>::failure(
                    ParseReason::kMalformedValue, "one read projection has non-equivalent duplicate BAM records");
            }
        }
        auto raw_occurrences = checked_occurrences(occurrences.size(), "raw BAM identity");
        if (!raw_occurrences.ok()) {
            return ParseResult<BlockScienceEvidence>::failure(raw_occurrences.reason,
                                                              std::move(raw_occurrences.detail));
        }

        const SidecarFullIdentity sidecar_identity = sidecar_identity_from_alignment(representative.identity);
        const LatestTagJoinResult joined = sidecar_lookup.join(projection, sidecar_identity);
        if (joined.status != JoinStatus::kExactMatch || !joined.tags.has_value()) {
            return ParseResult<BlockScienceEvidence>::failure(
                ParseReason::kMalformedValue, "authoritative sidecar exact join failed: " +
                                                  std::string(to_string(joined.reason)) + ": " + joined.detail);
        }
        if (joined.exact_identity_occurrences == 0 ||
            joined.exact_identity_occurrences > std::numeric_limits<std::uint32_t>::max()) {
            return ParseResult<BlockScienceEvidence>::failure(
                ParseReason::kMalformedValue, "sidecar exact identity occurrence count is outside uint32");
        }
        if (options.require_sidecar_occurrence_conservation &&
            joined.exact_identity_occurrences != *raw_occurrences.value) {
            return ParseResult<BlockScienceEvidence>::failure(
                ParseReason::kMalformedValue, "raw BAM and authoritative sidecar exact identity occurrences differ");
        }

        auto projected =
            project_read_evidence(projection.reference_interval, projection.strand, representative.identity.cigar,
                                  representative.sequence_reference_orientation, representative.base_qualities,
                                  representative.mm_ml, *projection_markers.value, options.minimum_base_quality);
        if (!projected.ok()) {
            return ParseResult<BlockScienceEvidence>::failure(projected.reason, std::move(projected.detail));
        }
        if (projected.value->allele_calls.size() != ordered_markers.size()) {
            return ParseResult<BlockScienceEvidence>::failure(
                ParseReason::kMalformedValue, "one-pass CIGAR projection did not emit exactly one call per marker");
        }

        std::vector<ProjectedMethylationCall> admitted_methylation;
        admitted_methylation.reserve(projected.value->methylation_calls.size());
        for (const ProjectedMethylationCall& call : projected.value->methylation_calls) {
            if (!block.fetch_interval.contains(call.candidate_cpg_position)) {
                continue;
            }
            if (is_reference_cpg(call, reference_context_interval, reference_context)) {
                admitted_methylation.push_back(call);
                ++output.counters.admitted_reference_cpg_calls;
            } else {
                ++output.counters.rejected_non_cpg_calls;
            }
        }
        std::sort(admitted_methylation.begin(), admitted_methylation.end(),
                  [](const ProjectedMethylationCall& lhs, const ProjectedMethylationCall& rhs) {
                      return lhs.candidate_cpg_position < rhs.candidate_cpg_position;
                  });
        for (std::size_t index = 1; index < admitted_methylation.size(); ++index) {
            if (admitted_methylation[index - 1].candidate_cpg_position ==
                admitted_methylation[index].candidate_cpg_position) {
                return ParseResult<BlockScienceEvidence>::failure(ParseReason::kMalformedValue,
                                                                  "one read has duplicate admitted calls at one CpG");
            }
        }

        auto read_id = sha256_hex(projection.qname);
        auto typed_aux_sha = sha256_hex(representative.identity.typed_aux_canonical);
        if (!read_id.ok() || !typed_aux_sha.ok()) {
            return ParseResult<BlockScienceEvidence>::failure(
                ParseReason::kIoError, "OpenSSL failed while hashing read or typed auxiliary identity");
        }
        const auto prior = read_id_bindings.emplace(*read_id.value, projection);
        if (!prior.second && prior.first->second != projection) {
            return ParseResult<BlockScienceEvidence>::failure(ParseReason::kMalformedValue,
                                                              "opaque read ID is bound to multiple read projections");
        }

        output.counters.rg_only_duplicate_occurrences += static_cast<std::uint64_t>(*raw_occurrences.value) - 1;
        ++output.counters.exact_sidecar_joins;
        output.counters.projected_marker_calls += projected.value->allele_calls.size();
        output.counters.decoded_methylation_calls += projected.value->methylation_calls.size();
        output.reads.push_back(JoinedReadEvidence{
            std::move(*read_id.value),
            projection,
            representative.identity.flag,
            sidecar_identity.cigar_blake2b64,
            std::move(*typed_aux_sha.value),
            representative.sequence_reference_orientation.size(),
            representative.mm_ml.mn,
            *joined.tags,
            *raw_occurrences.value,
            static_cast<std::uint32_t>(joined.exact_identity_occurrences),
            std::move(projected.value->allele_calls),
            std::move(admitted_methylation),
        });
    }
    output.counters.unique_read_projections = output.reads.size();

    output.focal_sites.reserve(block.focal_sites.size());
    for (const FocalSiteCost& focal : block.focal_sites) {
        const auto marker =
            std::find_if(output.markers.begin(), output.markers.end(), [&focal](const VariantSite& candidate) {
                return candidate.vcf_record_order == focal.vcf_record_order && candidate.position == focal.position;
            });
        if (marker == output.markers.end()) {
            return ParseResult<BlockScienceEvidence>::failure(
                ParseReason::kMalformedValue, "focal-to-marker mapping disappeared during block construction");
        }
        FocalSiteEvidence site{*marker, {}};
        for (std::size_t read_index = 0; read_index < output.reads.size(); ++read_index) {
            const JoinedReadEvidence& read = output.reads[read_index];
            if (read.projection.reference_interval.contains(marker->position)) {
                const ProjectedAlleleCall* call = find_allele_call(read, marker->site_order);
                if (call == nullptr || call->position != marker->position) {
                    return ParseResult<BlockScienceEvidence>::failure(
                        ParseReason::kMalformedValue, "covering read lacks its focal allele projection");
                }
                site.covering_read_indices.push_back(read_index);
            }
        }
        output.focal_sites.push_back(std::move(site));
    }

    return output.reads.empty() ? ParseResult<BlockScienceEvidence>::success_empty(std::move(output))
                                : ParseResult<BlockScienceEvidence>::success(std::move(output));
}

}  // namespace longlineage::pipeline
