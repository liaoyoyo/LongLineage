// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/io/variant_sites.hpp"
#include "longlineage/pipeline/dataset_plan.hpp"
#include "longlineage/pipeline/dataset_runner.hpp"
#include "longlineage/pipeline/site_science.hpp"

namespace longlineage::pipeline {

struct BlockScienceCensusReceipt {
    std::uint64_t block_sequence = 0;
    std::uint64_t focal_sites = 0;
    std::uint64_t m1_insufficient_alt_reads = 0;
    std::uint64_t m1_incomplete_distance = 0;
    std::uint64_t m1_evaluable = 0;
    std::uint64_t m1_stable = 0;
    std::uint64_t m2_eligible = 0;
    std::uint64_t m2_evaluable_ineligible = 0;
    std::uint64_t m2_axis_indeterminate = 0;
    std::uint64_t m2_group_count_gt10 = 0;
    std::uint64_t partner_pairs = 0;
    std::uint64_t endpoint_a_testable_pairs = 0;
    std::uint64_t exact_identifiable_pairs = 0;
    std::uint64_t exact_family_pairs = 0;
    std::string semantic_sha256;
    double input_seconds = 0.0;
    double m1_m2_seconds = 0.0;
    double cooccurrence_seconds = 0.0;
};

struct DatasetScienceCensusOptions {
    std::size_t workers = 1;
    std::size_t first_block = 0;
    // Zero means every remaining block.
    std::size_t block_count = 0;
    std::size_t task_queue_capacity_bytes = 16U * 1024U * 1024U;
    M1Representation representation{M1Representation::kHistoricalObservedRound6NullRound4};
};

struct DatasetScienceCensusReceipt {
    std::size_t workers = 0;
    std::size_t first_block = 0;
    std::size_t block_count = 0;
    M1Representation representation{M1Representation::kHistoricalObservedRound6NullRound4};
    std::uint64_t focal_sites = 0;
    std::uint64_t m1_insufficient_alt_reads = 0;
    std::uint64_t m1_incomplete_distance = 0;
    std::uint64_t m1_evaluable = 0;
    std::uint64_t m1_stable = 0;
    std::uint64_t m2_eligible = 0;
    std::uint64_t m2_evaluable_ineligible = 0;
    std::uint64_t m2_axis_indeterminate = 0;
    std::uint64_t m2_group_count_gt10 = 0;
    std::uint64_t partner_pairs = 0;
    std::uint64_t endpoint_a_testable_pairs = 0;
    std::uint64_t exact_identifiable_pairs = 0;
    std::uint64_t exact_family_pairs = 0;
    std::string semantic_sha256;
    double handle_open_seconds = 0.0;
    double execution_seconds = 0.0;
    std::vector<BlockScienceCensusReceipt> blocks;
};

// Complete M1/M2/co-occurrence pre-FDR census. This is a scientific execution
// and timing endpoint, not a publication endpoint: it deliberately does not
// publish global q-values, conditional discoveries, topology or artifacts.
[[nodiscard]] ParseResult<DatasetScienceCensusReceipt> run_dataset_science_census(
    const DatasetReadPaths& paths, const VariantSiteSet& variants, const DatasetExecutionPlan& plan,
    const DatasetScienceCensusOptions& options = {});

}  // namespace longlineage::pipeline
