// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/io/block_reader.hpp"

#include <htslib/hts.h>
#include <htslib/sam.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace longlineage {
namespace {

using GroupKey = std::tuple<std::uint32_t, std::string, std::string>;

[[nodiscard]] GroupKey group_key(const FocalSiteCost& site) {
    return {site.dataset_order, site.dataset_id, site.contig.value()};
}

[[nodiscard]] bool contains_prohibited_text(std::string_view value) {
    return value.empty() || value.find_first_of("\t\r\n") != std::string_view::npos;
}

[[nodiscard]] ParseResult<AlignmentBlock> make_block(const std::vector<FocalSiteCost>& sites, std::size_t begin,
                                                     std::size_t end, std::uint64_t sequence,
                                                     const BlockPlanOptions& options,
                                                     std::uint64_t estimated_alignments) {
    const FocalSiteCost& first = sites[begin];
    const FocalSiteCost& last = sites[end - 1];
    const std::uint64_t focal_begin = first.position.zero_based();
    const std::uint64_t focal_end = last.position.value();
    auto focal_interval = Interval0::from_bounds(focal_begin, focal_end);
    if (!focal_interval.ok()) {
        return ParseResult<AlignmentBlock>::failure(focal_interval.reason, std::move(focal_interval.detail));
    }

    const std::uint64_t fetch_begin = focal_begin > options.halo_bp ? focal_begin - options.halo_bp : 0;
    const std::uint64_t trailing = first.contig_length - focal_end;
    const std::uint64_t fetch_end = options.halo_bp >= trailing ? first.contig_length : focal_end + options.halo_bp;
    auto fetch_interval = Interval0::from_bounds(fetch_begin, fetch_end);
    if (!fetch_interval.ok()) {
        return ParseResult<AlignmentBlock>::failure(fetch_interval.reason, std::move(fetch_interval.detail));
    }

    std::vector<FocalSiteCost> focal_sites(sites.begin() + static_cast<std::ptrdiff_t>(begin),
                                           sites.begin() + static_cast<std::ptrdiff_t>(end));
    return ParseResult<AlignmentBlock>::success(AlignmentBlock{
        sequence,
        first.dataset_order,
        first.dataset_id,
        first.contig,
        first.contig_length,
        options.halo_bp,
        first.vcf_record_order,
        last.vcf_record_order,
        std::move(*focal_interval.value),
        std::move(*fetch_interval.value),
        estimated_alignments,
        std::move(focal_sites),
    });
}

[[nodiscard]] ParseResult<bool> has_required_tag(const bam1_t& record, const char tag[2]) {
    errno = 0;
    if (bam_aux_get(&record, tag) != nullptr) {
        return ParseResult<bool>::success(true);
    }
    if (errno == ENOENT) {
        return ParseResult<bool>::success(false);
    }
    return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                      "BAM auxiliary data is malformed while locating tag " + std::string(tag, 2));
}

[[nodiscard]] ParseResult<std::uint64_t> validate_cigar_consumption(const bam1_t& record) {
    if (record.core.n_cigar == 0) {
        return ParseResult<std::uint64_t>::failure(ParseReason::kMalformedValue,
                                                   "mapped block record is missing CIGAR operations");
    }
    if (record.core.n_cigar > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return ParseResult<std::uint64_t>::failure(ParseReason::kUnsupportedValue,
                                                   "CIGAR operation count exceeds HTSlib's signed API range");
    }
    const std::uint32_t* cigar = bam_get_cigar(&record);
    const int cigar_count = static_cast<int>(record.core.n_cigar);
    const hts_pos_t query_length = bam_cigar2qlen(cigar_count, cigar);
    const hts_pos_t reference_length = bam_cigar2rlen(cigar_count, cigar);
    if (query_length < 0 || reference_length <= 0 || query_length != record.core.l_qseq) {
        return ParseResult<std::uint64_t>::failure(
            ParseReason::kMalformedValue,
            "CIGAR query/reference consumption is inconsistent with the stored alignment sequence");
    }
    return ParseResult<std::uint64_t>::success(static_cast<std::uint64_t>(query_length));
}

[[nodiscard]] std::size_t saturating_add(std::size_t lhs, std::size_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::numeric_limits<std::size_t>::max();
    }
    return lhs + rhs;
}

[[nodiscard]] std::size_t saturating_multiply(std::size_t lhs, std::size_t rhs) noexcept {
    if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
        return std::numeric_limits<std::size_t>::max();
    }
    return lhs * rhs;
}

[[nodiscard]] std::size_t decoded_read_logical_bytes(const DecodedBlockRead& read) noexcept {
    std::size_t bytes = sizeof(DecodedBlockRead);
    bytes = saturating_add(bytes, read.identity.projection.qname.size());
    bytes = saturating_add(bytes, read.identity.projection.contig.value().size());
    bytes = saturating_add(bytes, read.identity.raw_qname.size());
    bytes = saturating_add(bytes, read.identity.cigar.size());
    bytes = saturating_add(bytes, read.identity.sam_core_sha256.size());
    bytes = saturating_add(bytes, read.identity.typed_aux_canonical.size());
    bytes = saturating_add(bytes, read.sequence_reference_orientation.size());
    bytes = saturating_add(bytes, read.base_qualities.size());
    bytes = saturating_add(bytes, read.mm_ml.mm.size());
    bytes = saturating_add(bytes, read.mm_ml.ml.size());
    return saturating_add(bytes, saturating_multiply(read.mm_ml.calls.size(), sizeof(MethylationCall)));
}

[[nodiscard]] ParseResult<bool> validate_block_contract(const AlignmentBlock& block) {
    if (block.focal_sites.empty()) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue, "alignment block has no focal sites");
    }
    if (block.focal_sites.size() > 4096 || block.estimated_alignments > 250000) {
        return ParseResult<bool>::failure(
            ParseReason::kUnsupportedValue,
            "alignment block exceeds the frozen 4096-site or 250000-estimated-alignment production ceiling");
    }
    if (contains_prohibited_text(block.dataset_id) || block.contig_length == 0) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          "alignment block dataset/contig length identity is malformed");
    }

    const FocalSiteCost& first = block.focal_sites.front();
    const FocalSiteCost& last = block.focal_sites.back();
    if (block.dataset_order != first.dataset_order || block.dataset_id != first.dataset_id ||
        block.contig != first.contig || block.contig_length != first.contig_length ||
        block.first_vcf_record_order != first.vcf_record_order ||
        block.last_vcf_record_order != last.vcf_record_order) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          "alignment block summary identity/order differs from its focal sites");
    }

    std::uint64_t estimated_alignments = 0;
    for (std::size_t index = 0; index < block.focal_sites.size(); ++index) {
        const FocalSiteCost& focal = block.focal_sites[index];
        if (focal.dataset_order != block.dataset_order || focal.dataset_id != block.dataset_id ||
            focal.contig != block.contig || focal.contig_length != block.contig_length ||
            focal.position.value() > block.contig_length) {
            return ParseResult<bool>::failure(
                ParseReason::kMalformedValue,
                "alignment block contains a focal site from a different dataset/contig contract");
        }
        if (index > 0) {
            const FocalSiteCost& previous = block.focal_sites[index - 1];
            if (focal.vcf_record_order <= previous.vcf_record_order || !(previous.position < focal.position)) {
                return ParseResult<bool>::failure(
                    ParseReason::kMalformedValue,
                    "alignment block focal record order and positions must both increase strictly");
            }
        }
        if (focal.estimated_alignments > std::numeric_limits<std::uint64_t>::max() - estimated_alignments) {
            return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                              "alignment block estimated-alignment sum overflows uint64");
        }
        estimated_alignments += focal.estimated_alignments;
    }
    if (estimated_alignments != block.estimated_alignments) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          "alignment block estimated-alignment total is not conserved");
    }

    const std::uint64_t expected_focal_begin = first.position.zero_based();
    const std::uint64_t expected_focal_end = last.position.value();
    if (block.focal_interval.begin() != expected_focal_begin || block.focal_interval.end() != expected_focal_end) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          "alignment block focal interval does not exactly span its focal sites");
    }
    const std::uint64_t expected_fetch_begin =
        expected_focal_begin > block.halo_bp ? expected_focal_begin - block.halo_bp : 0;
    const std::uint64_t trailing = block.contig_length - expected_focal_end;
    const std::uint64_t expected_fetch_end =
        block.halo_bp >= trailing ? block.contig_length : expected_focal_end + block.halo_bp;
    if (block.fetch_interval.begin() != expected_fetch_begin || block.fetch_interval.end() != expected_fetch_end) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          "alignment block fetch interval differs from its clipped halo contract");
    }
    return ParseResult<bool>::success(true);
}

}  // namespace

ParseResult<std::vector<AlignmentBlock>> plan_alignment_blocks(const std::vector<FocalSiteCost>& ordered_sites,
                                                               const BlockPlanOptions& options) {
    if (options.max_focal_sites == 0 || options.max_estimated_alignments == 0) {
        return ParseResult<std::vector<AlignmentBlock>>::failure(
            ParseReason::kMalformedValue, "block site and estimated-alignment ceilings must both be positive");
    }
    if (ordered_sites.empty()) {
        return ParseResult<std::vector<AlignmentBlock>>::success_empty({});
    }

    std::set<GroupKey> completed_groups;
    std::map<std::uint32_t, std::string> dataset_id_by_order;
    std::map<std::string, std::uint32_t> dataset_order_by_id;
    for (std::size_t index = 0; index < ordered_sites.size(); ++index) {
        const FocalSiteCost& site = ordered_sites[index];
        if (contains_prohibited_text(site.dataset_id)) {
            return ParseResult<std::vector<AlignmentBlock>>::failure(
                ParseReason::kMalformedValue, "dataset_id must be non-empty and contain no TSV/newline characters");
        }
        if (site.contig_length == 0 || site.position.value() > site.contig_length) {
            return ParseResult<std::vector<AlignmentBlock>>::failure(
                ParseReason::kMalformedValue, "focal position lies outside the declared positive contig length");
        }
        if (site.estimated_alignments > options.max_estimated_alignments) {
            return ParseResult<std::vector<AlignmentBlock>>::failure(
                ParseReason::kUnsupportedValue,
                "single focal-site estimate exceeds the frozen per-block ceiling and cannot be silently split");
        }
        const auto order_binding = dataset_id_by_order.emplace(site.dataset_order, site.dataset_id);
        if (!order_binding.second && order_binding.first->second != site.dataset_id) {
            return ParseResult<std::vector<AlignmentBlock>>::failure(
                ParseReason::kMalformedValue, "one dataset_order is bound to multiple dataset IDs");
        }
        const auto id_binding = dataset_order_by_id.emplace(site.dataset_id, site.dataset_order);
        if (!id_binding.second && id_binding.first->second != site.dataset_order) {
            return ParseResult<std::vector<AlignmentBlock>>::failure(
                ParseReason::kMalformedValue, "one dataset ID is bound to multiple dataset_order values");
        }
        if (index == 0) {
            continue;
        }

        const FocalSiteCost& previous = ordered_sites[index - 1];
        if (site.dataset_order < previous.dataset_order ||
            (site.dataset_order == previous.dataset_order && site.dataset_id != previous.dataset_id)) {
            return ParseResult<std::vector<AlignmentBlock>>::failure(
                ParseReason::kMalformedValue, "dataset order/id is not a stable one-to-one frozen ordering");
        }
        if (site.dataset_order == previous.dataset_order && site.vcf_record_order <= previous.vcf_record_order) {
            return ParseResult<std::vector<AlignmentBlock>>::failure(
                ParseReason::kMalformedValue, "VCF record order must increase strictly within each dataset");
        }

        const GroupKey previous_group = group_key(previous);
        const GroupKey current_group = group_key(site);
        if (current_group == previous_group) {
            if (site.contig_length != previous.contig_length || !(previous.position < site.position)) {
                return ParseResult<std::vector<AlignmentBlock>>::failure(
                    ParseReason::kMalformedValue,
                    "positions must increase strictly and contig length must remain fixed within a group");
            }
        } else {
            completed_groups.insert(previous_group);
            if (completed_groups.count(current_group) != 0U) {
                return ParseResult<std::vector<AlignmentBlock>>::failure(
                    ParseReason::kMalformedValue, "a dataset/contig group recurs after its frozen segment ended");
            }
        }
    }

    std::vector<AlignmentBlock> blocks;
    std::size_t block_begin = 0;
    std::uint64_t block_cost = 0;
    const auto flush = [&](std::size_t block_end, std::uint64_t cost,
                           std::vector<AlignmentBlock>& destination) -> ParseResult<bool> {
        auto block = make_block(ordered_sites, block_begin, block_end, destination.size(), options, cost);
        if (!block.ok()) {
            return ParseResult<bool>::failure(block.reason, std::move(block.detail));
        }
        destination.push_back(std::move(*block.value));
        return ParseResult<bool>::success(true);
    };

    for (std::size_t index = 0; index < ordered_sites.size(); ++index) {
        const FocalSiteCost& site = ordered_sites[index];
        const bool group_changed = index > block_begin && group_key(site) != group_key(ordered_sites[index - 1]);
        const bool site_ceiling_reached = index - block_begin >= options.max_focal_sites;
        const bool cost_would_overflow = site.estimated_alignments > options.max_estimated_alignments - block_cost;
        if (index > block_begin && (group_changed || site_ceiling_reached || cost_would_overflow)) {
            auto flushed = flush(index, block_cost, blocks);
            if (!flushed.ok()) {
                return ParseResult<std::vector<AlignmentBlock>>::failure(flushed.reason, std::move(flushed.detail));
            }
            block_begin = index;
            block_cost = 0;
        }
        block_cost += site.estimated_alignments;
    }
    auto flushed = flush(ordered_sites.size(), block_cost, blocks);
    if (!flushed.ok()) {
        return ParseResult<std::vector<AlignmentBlock>>::failure(flushed.reason, std::move(flushed.detail));
    }
    return ParseResult<std::vector<AlignmentBlock>>::success(std::move(blocks));
}

std::size_t BlockReadBatch::logical_retained_bytes() const noexcept {
    std::size_t bytes = sizeof(BlockReadBatch);
    for (const DecodedBlockRead& read : reads) {
        bytes = saturating_add(bytes, decoded_read_logical_bytes(read));
    }
    return bytes;
}

struct IndexedBamBlockReader::Impl {
    samFile* bam = nullptr;
    sam_hdr_t* header = nullptr;
    hts_idx_t* index = nullptr;
    std::uint64_t fetch_invocations = 0;

    ~Impl() {
        if (index != nullptr) {
            hts_idx_destroy(index);
        }
        if (header != nullptr) {
            sam_hdr_destroy(header);
        }
        if (bam != nullptr) {
            sam_close(bam);
        }
    }
};

IndexedBamBlockReader::IndexedBamBlockReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

IndexedBamBlockReader::~IndexedBamBlockReader() = default;
IndexedBamBlockReader::IndexedBamBlockReader(IndexedBamBlockReader&&) noexcept = default;
IndexedBamBlockReader& IndexedBamBlockReader::operator=(IndexedBamBlockReader&&) noexcept = default;

ParseResult<std::unique_ptr<IndexedBamBlockReader>> IndexedBamBlockReader::open(
    const std::filesystem::path& bam_path, const std::filesystem::path& explicit_index_path) {
    auto impl = std::make_unique<Impl>();
    const std::string bam_name = bam_path.string();
    const std::string index_name = explicit_index_path.string();
    if (bam_name.empty() || index_name.empty()) {
        return ParseResult<std::unique_ptr<IndexedBamBlockReader>>::failure(
            ParseReason::kMissingRequiredField, "BAM and explicit index paths must both be non-empty");
    }
    impl->bam = sam_open(bam_name.c_str(), "rb");
    if (impl->bam == nullptr) {
        return ParseResult<std::unique_ptr<IndexedBamBlockReader>>::failure(ParseReason::kIoError,
                                                                            "cannot open BAM input: " + bam_name);
    }
    const htsFormat* input_format = hts_get_format(impl->bam);
    if (input_format == nullptr || input_format->format != ::bam) {
        return ParseResult<std::unique_ptr<IndexedBamBlockReader>>::failure(
            ParseReason::kUnsupportedValue, "indexed block reader accepts physical BAM only: " + bam_name);
    }
    impl->header = sam_hdr_read(impl->bam);
    if (impl->header == nullptr) {
        return ParseResult<std::unique_ptr<IndexedBamBlockReader>>::failure(ParseReason::kMalformedValue,
                                                                            "cannot read BAM header: " + bam_name);
    }
    impl->index = sam_index_load2(impl->bam, bam_name.c_str(), index_name.c_str());
    if (impl->index == nullptr) {
        return ParseResult<std::unique_ptr<IndexedBamBlockReader>>::failure(
            ParseReason::kIndexError, "cannot load the explicitly named BAM index: " + index_name);
    }
    return ParseResult<std::unique_ptr<IndexedBamBlockReader>>::success(
        std::unique_ptr<IndexedBamBlockReader>(new IndexedBamBlockReader(std::move(impl))));
}

ParseResult<BlockReadBatch> IndexedBamBlockReader::read_block(const AlignmentBlock& block,
                                                              const BlockReadPolicy& policy,
                                                              const BlockReadResourceLimits& limits) {
    if (impl_ == nullptr || impl_->bam == nullptr || impl_->header == nullptr || impl_->index == nullptr) {
        return ParseResult<BlockReadBatch>::failure(ParseReason::kIoError, "BAM reader is not open");
    }
    auto block_contract = validate_block_contract(block);
    if (!block_contract.ok()) {
        return ParseResult<BlockReadBatch>::failure(block_contract.reason, std::move(block_contract.detail));
    }
    if (policy.minimum_mapq != 20 || policy.minimum_query_length != 1000 || !policy.require_mm_ml) {
        return ParseResult<BlockReadBatch>::failure(
            ParseReason::kUnsupportedValue,
            "v1 production block-read policy is fixed at MAPQ>=20, query length>=1000, and uppercase MM/ML required");
    }
    if (limits.maximum_filter_eligible_records == 0 ||
        limits.maximum_filter_eligible_records > kMaximumRetainedRecordsPerBlock ||
        limits.maximum_decoded_logical_bytes < sizeof(BlockReadBatch) ||
        limits.maximum_decoded_logical_bytes > kMaximumBlockReadLogicalBytes) {
        return ParseResult<BlockReadBatch>::failure(ParseReason::kUnsupportedValue,
                                                    "block-read resource limits must be positive and cannot exceed "
                                                    "250000 filter-eligible records or 512 MiB decoded logical bytes");
    }
    if (block.fetch_interval.end() > static_cast<std::uint64_t>(std::numeric_limits<hts_pos_t>::max())) {
        return ParseResult<BlockReadBatch>::failure(ParseReason::kUnsupportedValue,
                                                    "block interval exceeds this HTSlib coordinate range");
    }

    const int tid = sam_hdr_name2tid(impl_->header, block.contig.value().c_str());
    if (tid < 0) {
        return ParseResult<BlockReadBatch>::failure(ParseReason::kMalformedValue,
                                                    "block contig is absent from BAM header");
    }
    const hts_pos_t header_length = sam_hdr_tid2len(impl_->header, tid);
    if (header_length <= 0 || static_cast<std::uint64_t>(header_length) != block.contig_length) {
        return ParseResult<BlockReadBatch>::failure(ParseReason::kMalformedValue,
                                                    "block and BAM header contig lengths differ");
    }

    std::unique_ptr<hts_itr_t, decltype(&hts_itr_destroy)> iterator(
        sam_itr_queryi(impl_->index, tid, static_cast<hts_pos_t>(block.fetch_interval.begin()),
                       static_cast<hts_pos_t>(block.fetch_interval.end())),
        &hts_itr_destroy);
    ++impl_->fetch_invocations;
    if (!iterator) {
        return ParseResult<BlockReadBatch>::failure(ParseReason::kIndexError,
                                                    "BAM index cannot create the requested block iterator");
    }
    std::unique_ptr<bam1_t, decltype(&bam_destroy1)> record(bam_init1(), &bam_destroy1);
    if (!record) {
        return ParseResult<BlockReadBatch>::failure(ParseReason::kIoError, "cannot allocate BAM record");
    }

    BlockReadBatch batch;
    batch.block_sequence = block.sequence;
    std::size_t retained_logical_bytes = sizeof(BlockReadBatch);
    int iterator_status = 0;
    while ((iterator_status = sam_itr_next(impl_->bam, iterator.get(), record.get())) >= 0) {
        if (batch.counters.iterator_records == std::numeric_limits<std::uint64_t>::max()) {
            return ParseResult<BlockReadBatch>::failure(ParseReason::kMalformedValue,
                                                        "indexed BAM iterator record counter overflows uint64");
        }
        ++batch.counters.iterator_records;
        if (record->core.tid != tid || record->core.pos < 0) {
            return ParseResult<BlockReadBatch>::failure(ParseReason::kMalformedValue,
                                                        "indexed iterator returned a record outside the block contig");
        }
        const hts_pos_t end = bam_endpos(record.get());
        if (end <= record->core.pos || static_cast<std::uint64_t>(record->core.pos) >= block.fetch_interval.end() ||
            static_cast<std::uint64_t>(end) <= block.fetch_interval.begin()) {
            return ParseResult<BlockReadBatch>::failure(
                ParseReason::kMalformedValue, "indexed iterator returned a non-overlapping or empty-span record");
        }

        constexpr std::uint16_t kExcludedFlags = BAM_FUNMAP | BAM_FSECONDARY | BAM_FSUPPLEMENTARY | BAM_FDUP;
        if ((record->core.flag & kExcludedFlags) != 0) {
            ++batch.counters.excluded_flag;
            continue;
        }
        if ((record->core.flag & BAM_FPAIRED) != 0) {
            const bool read1 = (record->core.flag & BAM_FREAD1) != 0;
            const bool read2 = (record->core.flag & BAM_FREAD2) != 0;
            if (read1 == read2) {
                return ParseResult<BlockReadBatch>::failure(
                    ParseReason::kMalformedValue, "paired alignment must identify exactly one of READ1 or READ2");
            }
        }
        if (record->core.qual < policy.minimum_mapq) {
            ++batch.counters.excluded_mapq;
            continue;
        }
        if (record->core.l_qseq < 0) {
            return ParseResult<BlockReadBatch>::failure(ParseReason::kMalformedValue, "BAM query length is negative");
        }
        if (static_cast<std::uint64_t>(record->core.l_qseq) < policy.minimum_query_length) {
            ++batch.counters.excluded_query_length;
            continue;
        }
        auto mm_present = has_required_tag(*record, "MM");
        if (!mm_present.ok()) {
            return ParseResult<BlockReadBatch>::failure(mm_present.reason, std::move(mm_present.detail));
        }
        auto ml_present = has_required_tag(*record, "ML");
        if (!ml_present.ok()) {
            return ParseResult<BlockReadBatch>::failure(ml_present.reason, std::move(ml_present.detail));
        }
        if (!*mm_present.value || !*ml_present.value) {
            ++batch.counters.excluded_missing_mm_ml;
            continue;
        }
        if (batch.counters.retained_records >= limits.maximum_filter_eligible_records) {
            return ParseResult<BlockReadBatch>::failure(
                ParseReason::kUnsupportedValue,
                "POSTFILTER_RECORD_LIMIT: filter-eligible BAM records exceed the configured per-block ceiling");
        }

        auto query_consumption = validate_cigar_consumption(*record);
        if (!query_consumption.ok()) {
            return ParseResult<BlockReadBatch>::failure(query_consumption.reason, std::move(query_consumption.detail));
        }
        auto identity = build_full_alignment_identity(*record, *impl_->header);
        if (!identity.ok()) {
            return ParseResult<BlockReadBatch>::failure(identity.reason, std::move(identity.detail));
        }
        auto sequence = decode_bam_sequence(*record);
        if (!sequence.ok()) {
            return ParseResult<BlockReadBatch>::failure(sequence.reason, std::move(sequence.detail));
        }
        auto mm_ml = parse_mm_ml_mn(*record, *sequence.value);
        if (!mm_ml.ok()) {
            return ParseResult<BlockReadBatch>::failure(mm_ml.reason, std::move(mm_ml.detail));
        }

        std::vector<std::uint8_t> qualities(static_cast<std::size_t>(record->core.l_qseq));
        if (!qualities.empty()) {
            const std::uint8_t* raw_qualities = bam_get_qual(record.get());
            if (raw_qualities == nullptr) {
                return ParseResult<BlockReadBatch>::failure(ParseReason::kMalformedValue,
                                                            "BAM base-quality bytes are absent");
            }
            std::copy(raw_qualities, raw_qualities + qualities.size(), qualities.begin());
        }
        DecodedBlockRead decoded{std::move(*identity.value), std::move(*sequence.value), std::move(qualities),
                                 std::move(*mm_ml.value)};
        const std::size_t read_logical_bytes = decoded_read_logical_bytes(decoded);
        if (read_logical_bytes > limits.maximum_decoded_logical_bytes - retained_logical_bytes) {
            return ParseResult<BlockReadBatch>::failure(ParseReason::kUnsupportedValue,
                                                        "DECODED_LOGICAL_BYTE_LIMIT: decoded BAM evidence exceeds the "
                                                        "configured per-block logical-byte ceiling");
        }
        retained_logical_bytes += read_logical_bytes;
        batch.reads.push_back(std::move(decoded));
        ++batch.counters.retained_records;
    }
    if (iterator_status < -1) {
        return ParseResult<BlockReadBatch>::failure(ParseReason::kIoError,
                                                    "indexed BAM iterator failed before normal end-of-region");
    }
    return batch.reads.empty() ? ParseResult<BlockReadBatch>::success_empty(std::move(batch))
                               : ParseResult<BlockReadBatch>::success(std::move(batch));
}

std::uint64_t IndexedBamBlockReader::fetch_invocations() const noexcept {
    return impl_ == nullptr ? 0 : impl_->fetch_invocations;
}

}  // namespace longlineage
