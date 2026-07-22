// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "longlineage/artifact/dataset_artifacts.hpp"
#include "longlineage/common/parse_result.hpp"
#include "longlineage/io/variant_sites.hpp"
#include "longlineage/pipeline/dataset_plan.hpp"
#include "longlineage/pipeline/dataset_runner.hpp"
#include "longlineage/pipeline/site_science.hpp"
#include "longlineage/pipeline/topology_membership.hpp"

namespace longlineage::pipeline {

struct DatasetProductionOptions {
    std::size_t workers{1};
    std::size_t first_block{0};
    // Zero means every remaining block.
    std::size_t block_count{0};
    std::size_t task_queue_capacity_bytes{16U * 1024U * 1024U};
    std::size_t reorder_capacity_bytes{8ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t maximum_block_payload_bytes{512ULL * 1024ULL * 1024ULL};
    int bgzf_compression_threads{1};
    M1Representation representation{M1Representation::kHistoricalObservedRound6NullRound4};
    std::string task_type{"B"};
    std::string completeness{"FULL"};
    std::string site_population{"FROZEN_PASS_BIALLELIC_SSNV_AUTOSOMES_CHR1_TO_CHR22"};
};

struct DatasetProductionCounters {
    std::uint64_t site_keys{0};
    std::uint64_t site_read_rows{0};
    std::uint64_t methyl_call_rows{0};
    std::uint64_t m1_evaluable{0};
    std::uint64_t m1_insufficient_alt_reads{0};
    std::uint64_t m1_incomplete_distance{0};
    std::uint64_t m1_stable_assignments{0};
    std::uint64_t latest_tag_exact_joins{0};
    std::uint64_t raw_expected{0};
    std::uint64_t raw_matched{0};
    std::uint64_t raw_rg_only_duplicate_occurrences{0};
    std::uint64_t m2_eligible{0};
    std::uint64_t m2_evaluable_ineligible{0};
    std::uint64_t m2_axis_indeterminate{0};
    std::uint64_t m2_group_count_gt10{0};
    std::uint64_t cooccurrence_pairs{0};
    std::uint64_t exact_testable_pairs{0};
    std::uint64_t global_bh_discoveries{0};
    std::uint64_t global_by_discoveries{0};
    std::uint64_t formal_pair_by_confirmed{0};
    std::uint64_t topology_primary_hp_units{0};
    std::uint64_t topology_regions{0};
    std::uint64_t topology_fully_complete_regions{0};
    std::uint64_t topology_incomplete_regions{0};
    std::uint64_t topology_incomplete_units_with_winner{0};
};

struct DatasetProductionTiming {
    double handle_open_seconds{0.0};
    double block_execution_seconds{0.0};
    double artifact_stream_seconds{0.0};
    double global_pair_seconds{0.0};
    double topology_second_pass_seconds{0.0};
    double finalization_seconds{0.0};
    double total_seconds{0.0};
    double queue_wait_seconds{0.0};
    double reorder_wait_seconds{0.0};
    double task_latency_p50_seconds{0.0};
    double task_latency_p95_seconds{0.0};
    double task_latency_p99_seconds{0.0};
    double task_latency_max_seconds{0.0};
    std::size_t peak_reorder_bytes{0};
};

struct DatasetProductionReceipt {
    std::string run_id;
    std::filesystem::path staging_root;
    std::size_t workers{0};
    std::size_t first_block{0};
    std::size_t block_count{0};
    bool complete_plan{false};
    std::uint64_t peak_process_threads{0};
    DatasetProductionCounters counters;
    DatasetProductionTiming timing;
    TopologyMembershipFunnel topology_funnel;
    std::vector<artifact::DatasetArtifactWriteReceipt> artifacts;
    std::string semantic_sha256;
};

// Produces the complete nine-artifact scientific dataset inside an already
// exclusive staging root. It never creates validation/run receipts and never
// publishes or freezes the run: those transitions belong exclusively to the
// independent validator.
[[nodiscard]] ParseResult<DatasetProductionReceipt> produce_dataset_scientific_artifacts(
    const DatasetReadPaths& paths, const VariantSiteSet& variants, const DatasetExecutionPlan& plan,
    const std::filesystem::path& staging_root, std::string run_id, const DatasetProductionOptions& options = {});

}  // namespace longlineage::pipeline
