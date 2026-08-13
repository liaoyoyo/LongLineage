// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/pipeline/dataset_runner.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "longlineage/common/digest.hpp"
#include "longlineage/io/reference_reader.hpp"
#include "longlineage/io/sidecar.hpp"
#include "longlineage/pipeline/site_matrix.hpp"
#include "longlineage/runtime/ordered_thread_pool.hpp"

namespace longlineage::pipeline {
namespace {

constexpr std::size_t kMaximumWorkers = 64;
constexpr std::size_t kTaskChargeBytes = 256;

[[nodiscard]] double elapsed_seconds(std::chrono::steady_clock::time_point begin) noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

template <typename Integer>
void append_integer(std::string& output, Integer value) {
    char buffer[32]{};
    const auto encoded = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (encoded.ec != std::errc{}) {
        throw std::runtime_error("cannot canonicalize dataset integer");
    }
    output.append(buffer, encoded.ptr);
}

void append_field(std::string& output, std::string_view value) {
    append_integer(output, value.size());
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

template <typename Integer>
[[nodiscard]] bool checked_add(Integer& destination, Integer value) noexcept {
    if (value > std::numeric_limits<Integer>::max() - destination) {
        return false;
    }
    destination += value;
    return true;
}

[[nodiscard]] ParseResult<bool> add_read_counters(BlockReadCounters& destination, const BlockReadCounters& value) {
    if (!checked_add(destination.iterator_records, value.iterator_records) ||
        !checked_add(destination.excluded_flag, value.excluded_flag) ||
        !checked_add(destination.excluded_mapq, value.excluded_mapq) ||
        !checked_add(destination.excluded_query_length, value.excluded_query_length) ||
        !checked_add(destination.excluded_missing_mm_ml, value.excluded_missing_mm_ml) ||
        !checked_add(destination.retained_records, value.retained_records)) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          "dataset read conservation counter overflows uint64");
    }
    return ParseResult<bool>::success(true);
}

[[nodiscard]] ParseResult<bool> add_science_counters(BlockScienceCounters& destination,
                                                     const BlockScienceCounters& value) {
    if (!checked_add(destination.raw_records_after_filter, value.raw_records_after_filter) ||
        !checked_add(destination.unique_read_projections, value.unique_read_projections) ||
        !checked_add(destination.rg_only_duplicate_occurrences, value.rg_only_duplicate_occurrences) ||
        !checked_add(destination.sidecar_rows_fetched, value.sidecar_rows_fetched) ||
        !checked_add(destination.sidecar_rows_eligible, value.sidecar_rows_eligible) ||
        !checked_add(destination.exact_sidecar_joins, value.exact_sidecar_joins) ||
        !checked_add(destination.projected_marker_calls, value.projected_marker_calls) ||
        !checked_add(destination.decoded_methylation_calls, value.decoded_methylation_calls) ||
        !checked_add(destination.admitted_reference_cpg_calls, value.admitted_reference_cpg_calls) ||
        !checked_add(destination.rejected_non_cpg_calls, value.rejected_non_cpg_calls)) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          "dataset science conservation counter overflows uint64");
    }
    return ParseResult<bool>::success(true);
}

[[nodiscard]] std::string block_error(std::uint64_t sequence, std::string_view stage, std::string_view detail) {
    return "block " + std::to_string(sequence) + " " + std::string(stage) + " failed: " + std::string(detail);
}

}  // namespace

struct DatasetBlockReader::Impl {
    std::unique_ptr<IndexedBamBlockReader> bam;
    std::unique_ptr<IndexedSidecarReader> sidecar;
    std::unique_ptr<IndexedReferenceReader> reference;
};

DatasetBlockReader::DatasetBlockReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DatasetBlockReader::~DatasetBlockReader() = default;
DatasetBlockReader::DatasetBlockReader(DatasetBlockReader&&) noexcept = default;
DatasetBlockReader& DatasetBlockReader::operator=(DatasetBlockReader&&) noexcept = default;

ParseResult<std::unique_ptr<DatasetBlockReader>> DatasetBlockReader::open(const DatasetReadPaths& paths) {
    auto bam = IndexedBamBlockReader::open(paths.bam, paths.bam_index);
    if (!bam.ok()) {
        return ParseResult<std::unique_ptr<DatasetBlockReader>>::failure(bam.reason,
                                                                         "worker BAM open failed: " + bam.detail);
    }
    auto sidecar = IndexedSidecarReader::open(paths.latest_hp_ps_sidecar, paths.latest_hp_ps_sidecar_index);
    if (!sidecar.ok()) {
        return ParseResult<std::unique_ptr<DatasetBlockReader>>::failure(
            sidecar.reason, "worker sidecar open failed: " + sidecar.detail);
    }
    auto reference = IndexedReferenceReader::open(paths.reference_fasta, paths.reference_fai);
    if (!reference.ok()) {
        return ParseResult<std::unique_ptr<DatasetBlockReader>>::failure(
            reference.reason, "worker reference open failed: " + reference.detail);
    }
    auto impl = std::make_unique<Impl>();
    impl->bam = std::move(*bam.value);
    impl->sidecar = std::move(*sidecar.value);
    impl->reference = std::move(*reference.value);
    return ParseResult<std::unique_ptr<DatasetBlockReader>>::success(
        std::unique_ptr<DatasetBlockReader>(new DatasetBlockReader(std::move(impl))));
}

ParseResult<DatasetBlockLoad> DatasetBlockReader::read(const PlannedDatasetBlock& block,
                                                       const VariantSiteSet& variants) {
    if (impl_ == nullptr || impl_->bam == nullptr || impl_->sidecar == nullptr || impl_->reference == nullptr) {
        return ParseResult<DatasetBlockLoad>::failure(ParseReason::kIoError, "dataset block reader is not open");
    }
    if (block.marker_begin >= block.marker_end || block.marker_end > variants.sites.size()) {
        return ParseResult<DatasetBlockLoad>::failure(ParseReason::kMalformedValue,
                                                      "planned marker range is empty or outside the variant set");
    }

    DatasetBlockLoad output;
    const auto bam_begin = std::chrono::steady_clock::now();
    auto raw = impl_->bam->read_block(block.alignment);
    output.bam_seconds = elapsed_seconds(bam_begin);
    if (!raw.ok()) {
        return ParseResult<DatasetBlockLoad>::failure(raw.reason,
                                                      block_error(block.alignment.sequence, "BAM", raw.detail));
    }
    output.read_counters = raw.value->counters;
    output.raw_logical_bytes = raw.value->logical_retained_bytes();

    const auto sidecar_begin = std::chrono::steady_clock::now();
    auto lookup = impl_->sidecar->fetch(block.alignment.contig, block.alignment.fetch_interval);
    output.sidecar_seconds = elapsed_seconds(sidecar_begin);
    if (!lookup.ok()) {
        return ParseResult<DatasetBlockLoad>::failure(lookup.reason,
                                                      block_error(block.alignment.sequence, "sidecar", lookup.detail));
    }

    const std::uint64_t context_end = block.alignment.fetch_interval.end() < block.alignment.contig_length
                                          ? block.alignment.fetch_interval.end() + 1
                                          : block.alignment.fetch_interval.end();
    auto context_interval = Interval0::from_bounds(block.alignment.fetch_interval.begin(), context_end);
    if (!context_interval.ok()) {
        return ParseResult<DatasetBlockLoad>::failure(
            context_interval.reason,
            block_error(block.alignment.sequence, "reference interval", context_interval.detail));
    }
    const auto reference_begin = std::chrono::steady_clock::now();
    auto reference = impl_->reference->fetch(block.alignment.contig, *context_interval.value);
    output.reference_seconds = elapsed_seconds(reference_begin);
    if (!reference.ok()) {
        return ParseResult<DatasetBlockLoad>::failure(
            reference.reason, block_error(block.alignment.sequence, "reference", reference.detail));
    }

    std::vector<VariantSite> markers(variants.sites.begin() + static_cast<std::ptrdiff_t>(block.marker_begin),
                                     variants.sites.begin() + static_cast<std::ptrdiff_t>(block.marker_end));
    const auto projection_begin = std::chrono::steady_clock::now();
    auto evidence = build_block_science_evidence(block.alignment, markers, *raw.value, *lookup.value,
                                                 *context_interval.value, *reference.value);
    output.projection_seconds = elapsed_seconds(projection_begin);
    if (!evidence.ok()) {
        return ParseResult<DatasetBlockLoad>::failure(
            evidence.reason, block_error(block.alignment.sequence, "science projection", evidence.detail));
    }
    output.evidence_logical_bytes = evidence.value->logical_retained_bytes();
    output.evidence = std::move(*evidence.value);
    return output.evidence.reads.empty() ? ParseResult<DatasetBlockLoad>::success_empty(std::move(output))
                                         : ParseResult<DatasetBlockLoad>::success(std::move(output));
}

std::uint64_t DatasetBlockReader::bam_fetch_invocations() const noexcept {
    return impl_ == nullptr || impl_->bam == nullptr ? 0 : impl_->bam->fetch_invocations();
}

std::uint64_t DatasetBlockReader::sidecar_fetch_invocations() const noexcept {
    return impl_ == nullptr || impl_->sidecar == nullptr ? 0 : impl_->sidecar->fetch_invocations();
}

std::uint64_t DatasetBlockReader::reference_fetch_invocations() const noexcept {
    return impl_ == nullptr || impl_->reference == nullptr ? 0 : impl_->reference->fetch_invocations();
}

std::size_t BlockExtractionReceipt::retained_bytes() const noexcept {
    return sizeof(BlockExtractionReceipt) + contig.size() + block_semantic_sha256.size();
}

ParseResult<std::string> dataset_extraction_semantic_sha256(const std::vector<BlockExtractionReceipt>& ordered_blocks,
                                                            std::size_t first_block) {
    try {
        std::string canonical;
        canonical.reserve(ordered_blocks.size() * 256U + 128U);
        canonical.append("longlineage.dataset_extraction\t1.0.0\n");
        append_integer(canonical, first_block);
        canonical.push_back('|');
        append_integer(canonical, ordered_blocks.size());
        canonical.push_back('\n');
        for (std::size_t index = 0; index < ordered_blocks.size(); ++index) {
            const BlockExtractionReceipt& block = ordered_blocks[index];
            if (block.block_sequence != first_block + index || block.block_semantic_sha256.size() != 64U) {
                return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                         "ordered block receipt sequence or digest is malformed");
            }
            append_integer(canonical, block.block_sequence);
            canonical.push_back('|');
            append_field(canonical, block.contig);
            append_integer(canonical, block.focal_sites);
            canonical.push_back('|');
            append_integer(canonical, block.markers);
            canonical.push_back('|');
            append_integer(canonical, block.read_counters.iterator_records);
            canonical.push_back('|');
            append_integer(canonical, block.read_counters.retained_records);
            canonical.push_back('|');
            append_integer(canonical, block.science_counters.unique_read_projections);
            canonical.push_back('|');
            append_integer(canonical, block.science_counters.rg_only_duplicate_occurrences);
            canonical.push_back('|');
            append_integer(canonical, block.science_counters.exact_sidecar_joins);
            canonical.push_back('|');
            append_integer(canonical, block.science_counters.projected_marker_calls);
            canonical.push_back('|');
            append_integer(canonical, block.science_counters.admitted_reference_cpg_calls);
            canonical.push_back('|');
            append_integer(canonical, block.matrix_rows);
            canonical.push_back('|');
            append_integer(canonical, block.alt_rows);
            canonical.push_back('|');
            append_integer(canonical, block.matrix_cells);
            canonical.push_back('|');
            append_field(canonical, block.block_semantic_sha256);
            canonical.push_back('\n');
        }
        return sha256_hex(canonical);
    } catch (const std::exception& error) {
        return ParseResult<std::string>::failure(ParseReason::kIoError,
                                                 std::string("cannot build dataset semantic digest: ") + error.what());
    }
}

ParseResult<DatasetExtractionReceipt> run_dataset_extraction(const DatasetReadPaths& paths,
                                                             const VariantSiteSet& variants,
                                                             const DatasetExecutionPlan& plan,
                                                             const DatasetExtractionOptions& options) {
    if (options.workers == 0 || options.workers > kMaximumWorkers ||
        options.task_queue_capacity_bytes < kTaskChargeBytes || plan.blocks.empty() ||
        plan.focal_site_count != variants.sites.size() || options.first_block >= plan.blocks.size()) {
        return ParseResult<DatasetExtractionReceipt>::failure(
            ParseReason::kUnsupportedValue, "dataset extraction options or plan/variant cardinality are invalid");
    }
    const std::size_t remaining = plan.blocks.size() - options.first_block;
    const std::size_t requested_count = options.block_count == 0 ? remaining : options.block_count;
    if (requested_count == 0 || requested_count > remaining) {
        return ParseResult<DatasetExtractionReceipt>::failure(
            ParseReason::kMalformedValue, "requested dataset block range lies outside the execution plan");
    }

    DatasetExtractionReceipt output;
    output.workers = options.workers;
    output.first_block = options.first_block;
    output.block_count = requested_count;

    const auto open_begin = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<DatasetBlockReader>> readers;
    readers.reserve(options.workers);
    for (std::size_t worker = 0; worker < options.workers; ++worker) {
        auto reader = DatasetBlockReader::open(paths);
        if (!reader.ok()) {
            return ParseResult<DatasetExtractionReceipt>::failure(
                reader.reason, "worker " + std::to_string(worker) + " input bundle open failed: " + reader.detail);
        }
        readers.push_back(std::move(*reader.value));
    }
    output.handle_open_seconds = elapsed_seconds(open_begin);

    using runtime::OrderedThreadPool;
    using runtime::PoolStatus;
    OrderedThreadPool<BlockExtractionReceipt> pool(options.workers, options.task_queue_capacity_bytes);
    const auto execution_begin = std::chrono::steady_clock::now();
    for (std::size_t local_index = 0; local_index < requested_count; ++local_index) {
        const std::size_t block_index = options.first_block + local_index;
        const auto submitted = pool.submit_indexed(
            kTaskChargeBytes,
            [block_index, &plan, &variants, &readers](std::size_t worker_index) -> BlockExtractionReceipt {
                const PlannedDatasetBlock& planned = plan.blocks[block_index];
                auto loaded = readers[worker_index]->read(planned, variants);
                if (!loaded.ok()) {
                    throw std::runtime_error(loaded.detail);
                }
                BlockExtractionReceipt receipt;
                receipt.block_sequence = planned.alignment.sequence;
                receipt.contig = planned.alignment.contig.value();
                receipt.focal_sites = planned.alignment.focal_sites.size();
                receipt.markers = planned.marker_end - planned.marker_begin;
                receipt.read_counters = loaded.value->read_counters;
                receipt.science_counters = loaded.value->evidence.counters;
                receipt.raw_logical_bytes = loaded.value->raw_logical_bytes;
                receipt.evidence_logical_bytes = loaded.value->evidence_logical_bytes;
                receipt.bam_seconds = loaded.value->bam_seconds;
                receipt.sidecar_seconds = loaded.value->sidecar_seconds;
                receipt.reference_seconds = loaded.value->reference_seconds;
                receipt.projection_seconds = loaded.value->projection_seconds;

                auto digest = block_science_semantic_sha256(loaded.value->evidence);
                if (!digest.ok()) {
                    throw std::runtime_error(block_error(planned.alignment.sequence, "semantic digest", digest.detail));
                }
                receipt.block_semantic_sha256 = std::move(*digest.value);

                const auto matrix_begin = std::chrono::steady_clock::now();
                for (const FocalSiteEvidence& focal : loaded.value->evidence.focal_sites) {
                    auto matrix = build_site_methylation_matrix(loaded.value->evidence, focal);
                    if (!matrix.ok()) {
                        throw std::runtime_error(block_error(planned.alignment.sequence, "site matrix", matrix.detail));
                    }
                    const std::uint64_t rows = matrix.value->read_indices.size();
                    const std::uint64_t columns = matrix.value->cpg_positions.size();
                    if (!checked_add(receipt.matrix_rows, rows) ||
                        !checked_add(receipt.alt_rows,
                                     static_cast<std::uint64_t>(matrix.value->alt_row_indices.size())) ||
                        (rows != 0 && columns > std::numeric_limits<std::uint64_t>::max() / rows) ||
                        !checked_add(receipt.matrix_cells, rows * columns)) {
                        throw std::runtime_error(block_error(planned.alignment.sequence, "site matrix",
                                                             "matrix conservation counter overflows uint64"));
                    }
                }
                receipt.matrix_seconds = elapsed_seconds(matrix_begin);
                return receipt;
            });
        if (submitted.status != PoolStatus::kSuccess) {
            pool.cancel("dataset task submission failed");
            const auto failed = pool.finish();
            static_cast<void>(failed);
            return ParseResult<DatasetExtractionReceipt>::failure(
                ParseReason::kIoError,
                "dataset task submission failed at block " + std::to_string(block_index) + ": " + submitted.message);
        }
    }
    auto batch = pool.finish();
    output.execution_seconds = elapsed_seconds(execution_begin);
    if (batch.status != PoolStatus::kSuccess) {
        return ParseResult<DatasetExtractionReceipt>::failure(
            ParseReason::kIoError,
            "dataset worker batch failed" +
                (batch.failed_sequence.has_value() ? " at submitted sequence " + std::to_string(*batch.failed_sequence)
                                                   : std::string{}) +
                ": " + batch.message);
    }
    output.peak_task_queue_bytes = batch.peak_queued_bytes;
    output.blocks = std::move(batch.ordered_results);

    for (const BlockExtractionReceipt& receipt : output.blocks) {
        if (!checked_add(output.focal_sites, receipt.focal_sites) || !checked_add(output.markers, receipt.markers) ||
            !checked_add(output.matrix_rows, receipt.matrix_rows) || !checked_add(output.alt_rows, receipt.alt_rows) ||
            !checked_add(output.matrix_cells, receipt.matrix_cells)) {
            return ParseResult<DatasetExtractionReceipt>::failure(ParseReason::kMalformedValue,
                                                                  "dataset aggregate counter overflows uint64");
        }
        auto reads = add_read_counters(output.read_counters, receipt.read_counters);
        auto science = add_science_counters(output.science_counters, receipt.science_counters);
        if (!reads.ok() || !science.ok()) {
            return ParseResult<DatasetExtractionReceipt>::failure(ParseReason::kMalformedValue,
                                                                  !reads.ok() ? reads.detail : science.detail);
        }
        output.maximum_raw_logical_bytes = std::max(output.maximum_raw_logical_bytes, receipt.raw_logical_bytes);
        output.maximum_evidence_logical_bytes =
            std::max(output.maximum_evidence_logical_bytes, receipt.evidence_logical_bytes);
    }
    for (const auto& reader : readers) {
        if (!checked_add(output.bam_fetch_invocations, reader->bam_fetch_invocations()) ||
            !checked_add(output.sidecar_fetch_invocations, reader->sidecar_fetch_invocations()) ||
            !checked_add(output.reference_fetch_invocations, reader->reference_fetch_invocations())) {
            return ParseResult<DatasetExtractionReceipt>::failure(ParseReason::kMalformedValue,
                                                                  "dataset reader invocation counter overflows uint64");
        }
    }
    if (output.bam_fetch_invocations != requested_count || output.sidecar_fetch_invocations != requested_count ||
        output.reference_fetch_invocations != requested_count ||
        output.science_counters.raw_records_after_filter != output.read_counters.retained_records) {
        return ParseResult<DatasetExtractionReceipt>::failure(
            ParseReason::kMalformedValue, "dataset per-block fetch or retained-record conservation failed");
    }
    auto digest = dataset_extraction_semantic_sha256(output.blocks, options.first_block);
    if (!digest.ok()) {
        return ParseResult<DatasetExtractionReceipt>::failure(digest.reason, std::move(digest.detail));
    }
    output.semantic_sha256 = std::move(*digest.value);
    return ParseResult<DatasetExtractionReceipt>::success(std::move(output));
}

}  // namespace longlineage::pipeline
