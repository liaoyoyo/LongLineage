// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/pipeline/dataset_science_runner.hpp"

#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "longlineage/common/digest.hpp"
#include "longlineage/cooccurrence/site_cooccurrence.hpp"
#include "longlineage/runtime/ordered_thread_pool.hpp"

namespace longlineage::pipeline {
namespace {

constexpr std::size_t kMaximumWorkers = 64;
constexpr std::size_t kTaskChargeBytes = 256;

[[nodiscard]] double elapsed_seconds(std::chrono::steady_clock::time_point begin) noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

template <typename Integer>
void checked_add(Integer& destination, Integer value, const char* field) {
    if (value > std::numeric_limits<Integer>::max() - destination) {
        throw std::overflow_error(std::string(field) + " overflows");
    }
    destination += value;
}

template <typename Receipt>
[[nodiscard]] bool m2_partition_conserves(const Receipt& receipt) noexcept {
    std::uint64_t remaining = receipt.m1_stable;
    for (const std::uint64_t value : {receipt.m2_eligible, receipt.m2_evaluable_ineligible,
                                      receipt.m2_axis_indeterminate, receipt.m2_group_count_gt10}) {
        if (value > remaining) {
            return false;
        }
        remaining -= value;
    }
    return remaining == 0U;
}

void add_receipt(DatasetScienceCensusReceipt& destination, const BlockScienceCensusReceipt& source) {
    if (source.m1_stable > source.m1_evaluable || !m2_partition_conserves(source)) {
        throw std::runtime_error("block M2 status partition does not conserve stable M1 sites");
    }
    checked_add(destination.focal_sites, source.focal_sites, "focal_sites");
    checked_add(destination.m1_insufficient_alt_reads, source.m1_insufficient_alt_reads, "m1_insufficient_alt_reads");
    checked_add(destination.m1_incomplete_distance, source.m1_incomplete_distance, "m1_incomplete_distance");
    checked_add(destination.m1_evaluable, source.m1_evaluable, "m1_evaluable");
    checked_add(destination.m1_stable, source.m1_stable, "m1_stable");
    checked_add(destination.m2_eligible, source.m2_eligible, "m2_eligible");
    checked_add(destination.m2_evaluable_ineligible, source.m2_evaluable_ineligible, "m2_evaluable_ineligible");
    checked_add(destination.m2_axis_indeterminate, source.m2_axis_indeterminate, "m2_axis_indeterminate");
    checked_add(destination.m2_group_count_gt10, source.m2_group_count_gt10, "m2_group_count_gt10");
    checked_add(destination.partner_pairs, source.partner_pairs, "partner_pairs");
    checked_add(destination.endpoint_a_testable_pairs, source.endpoint_a_testable_pairs, "endpoint_a_testable_pairs");
    checked_add(destination.exact_identifiable_pairs, source.exact_identifiable_pairs, "exact_identifiable_pairs");
    checked_add(destination.exact_family_pairs, source.exact_family_pairs, "exact_family_pairs");
}

[[nodiscard]] ParseResult<std::string> census_digest(const std::vector<BlockScienceCensusReceipt>& blocks,
                                                     std::size_t first_block, M1Representation representation) {
    try {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << "longlineage.dataset_science_census\t1.0.0\n"
               << first_block << '\t' << blocks.size() << '\t'
               << (representation == M1Representation::kRawBinary32Point ? "RAW_BINARY32_POINT"
                                                                         : "HISTORICAL_OBSERVED_ROUND6_NULL_ROUND4")
               << '\n';
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            if (blocks[index].block_sequence != first_block + index || blocks[index].semantic_sha256.size() != 64) {
                return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                         "science census block order/digest is malformed");
            }
            const auto& block = blocks[index];
            stream << block.block_sequence << '\t' << block.focal_sites << '\t' << block.m1_insufficient_alt_reads
                   << '\t' << block.m1_incomplete_distance << '\t' << block.m1_evaluable << '\t' << block.m1_stable
                   << '\t' << block.m2_eligible << '\t' << block.m2_evaluable_ineligible << '\t'
                   << block.m2_axis_indeterminate << '\t' << block.m2_group_count_gt10 << '\t' << block.partner_pairs
                   << '\t' << block.endpoint_a_testable_pairs << '\t' << block.exact_identifiable_pairs << '\t'
                   << block.exact_family_pairs << '\t' << block.semantic_sha256 << '\n';
        }
        return sha256_hex(stream.str());
    } catch (const std::exception& error) {
        return ParseResult<std::string>::failure(ParseReason::kIoError,
                                                 std::string("cannot hash science census: ") + error.what());
    }
}

}  // namespace

ParseResult<DatasetScienceCensusReceipt> run_dataset_science_census(const DatasetReadPaths& paths,
                                                                    const VariantSiteSet& variants,
                                                                    const DatasetExecutionPlan& plan,
                                                                    const DatasetScienceCensusOptions& options) {
    if (options.workers == 0 || options.workers > kMaximumWorkers ||
        options.task_queue_capacity_bytes < kTaskChargeBytes || plan.blocks.empty() ||
        plan.focal_site_count != variants.sites.size() || options.first_block >= plan.blocks.size()) {
        return ParseResult<DatasetScienceCensusReceipt>::failure(
            ParseReason::kUnsupportedValue, "science census options or plan/variant cardinality are invalid");
    }
    const std::size_t remaining = plan.blocks.size() - options.first_block;
    const std::size_t requested = options.block_count == 0 ? remaining : options.block_count;
    if (requested == 0 || requested > remaining) {
        return ParseResult<DatasetScienceCensusReceipt>::failure(
            ParseReason::kMalformedValue, "science census block range lies outside the execution plan");
    }

    DatasetScienceCensusReceipt output;
    output.workers = options.workers;
    output.first_block = options.first_block;
    output.block_count = requested;
    output.representation = options.representation;

    const auto open_begin = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<DatasetBlockReader>> readers;
    readers.reserve(options.workers);
    for (std::size_t worker = 0; worker < options.workers; ++worker) {
        auto reader = DatasetBlockReader::open(paths);
        if (!reader.ok()) {
            return ParseResult<DatasetScienceCensusReceipt>::failure(
                reader.reason, "science worker " + std::to_string(worker) + " input open failed: " + reader.detail);
        }
        readers.push_back(std::move(*reader.value));
    }
    output.handle_open_seconds = elapsed_seconds(open_begin);

    runtime::OrderedThreadPool<BlockScienceCensusReceipt> pool(options.workers, options.task_queue_capacity_bytes);
    const auto execution_begin = std::chrono::steady_clock::now();
    for (std::size_t local = 0; local < requested; ++local) {
        const std::size_t block_index = options.first_block + local;
        const auto submitted = pool.submit_indexed(
            kTaskChargeBytes, [block_index, &readers, &plan, &variants, &options](std::size_t worker) {
                const auto input_begin = std::chrono::steady_clock::now();
                auto loaded = readers[worker]->read(plan.blocks[block_index], variants);
                if (!loaded.ok()) {
                    throw std::runtime_error(loaded.detail);
                }
                BlockScienceCensusReceipt receipt;
                receipt.block_sequence = plan.blocks[block_index].alignment.sequence;
                receipt.focal_sites = loaded.value->evidence.focal_sites.size();
                receipt.input_seconds = elapsed_seconds(input_begin);
                std::ostringstream semantic;
                semantic.imbue(std::locale::classic());
                semantic << "longlineage.block_science_census\t1.0.0\n"
                         << receipt.block_sequence << '\t' << receipt.focal_sites << '\n';
                for (const auto& focal : loaded.value->evidence.focal_sites) {
                    const auto m1_begin = std::chrono::steady_clock::now();
                    SiteScienceOptions site_options;
                    site_options.m1_representation = options.representation;
                    site_options.m1_options.workers = 1;
                    auto science = run_site_science(loaded.value->evidence, focal, site_options);
                    receipt.m1_m2_seconds += elapsed_seconds(m1_begin);
                    if (!science.ok()) {
                        throw std::runtime_error(science.detail);
                    }
                    switch (science.value->m1.status) {
                        case m1::M1ReadsetStatus::kInsufficientAltReads:
                            ++receipt.m1_insufficient_alt_reads;
                            break;
                        case m1::M1ReadsetStatus::kIncompleteDistanceBelowMinimum:
                            ++receipt.m1_incomplete_distance;
                            break;
                        case m1::M1ReadsetStatus::kFullClusteringReady:
                            ++receipt.m1_evaluable;
                            break;
                        case m1::M1ReadsetStatus::kPrimitiveParityReady:
                            throw std::runtime_error("full site science returned primitive-only M1");
                    }
                    if (science.value->m1.analysis.has_value() && science.value->m1.analysis->stable_null_multigroup) {
                        ++receipt.m1_stable;
                    }
                    switch (science.value->m2.decision.status) {
                        case m2::M2Status::kPass:
                            ++receipt.m2_eligible;
                            break;
                        case m2::M2Status::kFail:
                            ++receipt.m2_evaluable_ineligible;
                            break;
                        case m2::M2Status::kNotEvaluable:
                            if (science.value->m2.decision.reason == m2::M2Reason::kGroupCountExceedsPlanningMaximum) {
                                ++receipt.m2_group_count_gt10;
                            } else if (science.value->m2.decision.reason == m2::M2Reason::kAxisIndeterminate) {
                                ++receipt.m2_axis_indeterminate;
                            } else {
                                throw std::runtime_error(
                                    "NOT_EVALUABLE M2 decision has "
                                    "unsupported reason");
                            }
                            break;
                        case m2::M2Status::kNotRun:
                            break;
                    }

                    const auto cooccurrence_begin = std::chrono::steady_clock::now();
                    auto cooccurrence =
                        cooccurrence::build_site_cooccurrence(loaded.value->evidence, focal, *science.value);
                    receipt.cooccurrence_seconds += elapsed_seconds(cooccurrence_begin);
                    if (!cooccurrence.ok()) {
                        throw std::runtime_error(cooccurrence.detail);
                    }
                    checked_add(receipt.partner_pairs, static_cast<std::uint64_t>(cooccurrence.value->pairs.size()),
                                "block partner_pairs");
                    for (const auto& pair : cooccurrence.value->pairs) {
                        receipt.endpoint_a_testable_pairs += pair.endpoint_a.testable;
                        receipt.exact_identifiable_pairs += pair.exact.identifiable;
                        receipt.exact_family_pairs +=
                            pair.family_status == cooccurrence::PairFamilyStatus::kEligibleM2ExactFamily;
                    }
                    semantic << focal.site.site_order << '\t' << science.value->semantic_sha256 << '\t'
                             << cooccurrence.value->semantic_sha256 << '\n';
                }
                auto digest = sha256_hex(semantic.str());
                if (!digest.ok()) {
                    throw std::runtime_error(digest.detail);
                }
                receipt.semantic_sha256 = std::move(*digest.value);
                return receipt;
            });
        if (submitted.status != runtime::PoolStatus::kSuccess) {
            pool.cancel("science census submission failed");
            (void)pool.finish();
            return ParseResult<DatasetScienceCensusReceipt>::failure(
                ParseReason::kIoError, "science census task submission failed: " + submitted.message);
        }
    }
    auto batch = pool.finish();
    output.execution_seconds = elapsed_seconds(execution_begin);
    if (batch.status != runtime::PoolStatus::kSuccess || batch.ordered_results.size() != requested) {
        return ParseResult<DatasetScienceCensusReceipt>::failure(
            ParseReason::kIoError, "science census worker batch failed: " + batch.message);
    }
    output.blocks = std::move(batch.ordered_results);
    try {
        for (const auto& block : output.blocks) {
            add_receipt(output, block);
        }
    } catch (const std::exception& error) {
        return ParseResult<DatasetScienceCensusReceipt>::failure(ParseReason::kMalformedValue, error.what());
    }
    if (output.focal_sites == 0 ||
        output.m1_insufficient_alt_reads + output.m1_incomplete_distance + output.m1_evaluable != output.focal_sites ||
        output.m1_stable > output.m1_evaluable || !m2_partition_conserves(output)) {
        return ParseResult<DatasetScienceCensusReceipt>::failure(ParseReason::kMalformedValue,
                                                                 "science census focal/M1/M2 conservation failed");
    }
    auto digest = census_digest(output.blocks, output.first_block, output.representation);
    if (!digest.ok()) {
        return ParseResult<DatasetScienceCensusReceipt>::failure(digest.reason, std::move(digest.detail));
    }
    output.semantic_sha256 = std::move(*digest.value);
    return ParseResult<DatasetScienceCensusReceipt>::success(std::move(output));
}

}  // namespace longlineage::pipeline
