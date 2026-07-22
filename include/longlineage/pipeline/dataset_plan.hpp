// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/io/block_reader.hpp"
#include "longlineage/io/variant_sites.hpp"

namespace longlineage::pipeline {

struct DatasetPlanOptions {
    std::size_t max_focal_sites_per_block = 256;
    std::uint64_t max_estimated_alignments_per_block = 250000;
    std::uint64_t max_focal_span_bp = 250000;
    std::uint64_t halo_bp = 5000;
    // Until a frozen index-density estimator exists, v1 uses a declared
    // deterministic planning unit. Raw iterator hits remain an observed
    // streaming counter; execution bounds the filter-eligible record count
    // and decoded retained bytes instead. The planning unit is not reported
    // as a measured read count.
    std::uint64_t planning_units_per_site = 1;
};

struct PlannedDatasetBlock {
    AlignmentBlock alignment;
    std::size_t marker_begin = 0;
    std::size_t marker_end = 0;
};

struct DatasetExecutionPlan {
    std::vector<PlannedDatasetBlock> blocks;
    std::uint64_t focal_site_count = 0;
    std::uint64_t marker_occurrences = 0;
    std::uint64_t maximum_observed_focal_span_bp = 0;
};

// Splits the already-frozen VariantSite order without reordering or dropping
// sites. Besides the existing site/planning-unit ceilings, the explicit span
// ceiling prevents a sparse group of 4096 sites from expanding one BAM query
// across many megabases. marker_begin/end include every same-contig PASS sSNV
// in the block halo, so partner discovery is complete at +/-5000 bp.
[[nodiscard]] ParseResult<DatasetExecutionPlan> plan_dataset_execution(const VariantSiteSet& variants,
                                                                       const DatasetPlanOptions& options = {});

}  // namespace longlineage::pipeline
