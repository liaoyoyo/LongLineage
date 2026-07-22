// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/pipeline/dataset_plan.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace longlineage::pipeline {
namespace {

using GroupKey = std::tuple<std::uint32_t, std::string, std::string>;

[[nodiscard]] GroupKey group_key(const VariantSite& site) {
    return {site.dataset_order, site.dataset_id, site.contig.value()};
}

[[nodiscard]] ParseResult<AlignmentBlock> make_alignment_block(const std::vector<VariantSite>& sites, std::size_t begin,
                                                               std::size_t end, std::uint64_t sequence,
                                                               const DatasetPlanOptions& options) {
    if (begin >= end || end > sites.size()) {
        return ParseResult<AlignmentBlock>::failure(ParseReason::kMalformedValue,
                                                    "dataset plan block range is empty or invalid");
    }
    const VariantSite& first = sites[begin];
    const VariantSite& last = sites[end - 1];
    auto focal_interval = Interval0::from_bounds(first.position.zero_based(), last.position.value());
    if (!focal_interval.ok()) {
        return ParseResult<AlignmentBlock>::failure(focal_interval.reason, std::move(focal_interval.detail));
    }
    const std::uint64_t fetch_begin =
        first.position.zero_based() > options.halo_bp ? first.position.zero_based() - options.halo_bp : 0;
    const std::uint64_t trailing = first.contig_length - last.position.value();
    const std::uint64_t fetch_end =
        options.halo_bp >= trailing ? first.contig_length : last.position.value() + options.halo_bp;
    auto fetch_interval = Interval0::from_bounds(fetch_begin, fetch_end);
    if (!fetch_interval.ok()) {
        return ParseResult<AlignmentBlock>::failure(fetch_interval.reason, std::move(fetch_interval.detail));
    }
    std::vector<FocalSiteCost> focal_sites;
    focal_sites.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        const VariantSite& site = sites[index];
        focal_sites.push_back(FocalSiteCost{
            site.dataset_order,
            site.dataset_id,
            site.vcf_record_order,
            site.contig,
            site.position,
            options.planning_units_per_site,
            site.contig_length,
        });
    }
    const std::uint64_t count = static_cast<std::uint64_t>(end - begin);
    if (options.planning_units_per_site > std::numeric_limits<std::uint64_t>::max() / count) {
        return ParseResult<AlignmentBlock>::failure(ParseReason::kMalformedValue,
                                                    "dataset block planning-unit total overflows");
    }
    return ParseResult<AlignmentBlock>::success(AlignmentBlock{
        sequence,
        first.dataset_order,
        first.dataset_id,
        first.contig,
        first.contig_length,
        options.halo_bp,
        first.vcf_record_order,
        last.vcf_record_order,
        *focal_interval.value,
        *fetch_interval.value,
        options.planning_units_per_site * count,
        std::move(focal_sites),
    });
}

}  // namespace

ParseResult<DatasetExecutionPlan> plan_dataset_execution(const VariantSiteSet& variants,
                                                         const DatasetPlanOptions& options) {
    if (variants.sites.empty()) {
        return ParseResult<DatasetExecutionPlan>::failure(
            ParseReason::kMissingRequiredField, "dataset execution planning requires at least one variant site");
    }
    if (options.max_focal_sites_per_block == 0 || options.max_estimated_alignments_per_block == 0 ||
        options.max_focal_span_bp == 0 || options.halo_bp != 5000 || options.planning_units_per_site == 0 ||
        options.planning_units_per_site > options.max_estimated_alignments_per_block) {
        return ParseResult<DatasetExecutionPlan>::failure(
            ParseReason::kUnsupportedValue, "dataset plan ceilings are zero or differ from the v1 5000-bp halo");
    }

    const std::vector<VariantSite>& sites = variants.sites;
    std::map<GroupKey, std::pair<std::size_t, std::size_t>> group_ranges;
    for (std::size_t index = 0; index < sites.size(); ++index) {
        const VariantSite& site = sites[index];
        if (site.position.value() > site.contig_length) {
            return ParseResult<DatasetExecutionPlan>::failure(ParseReason::kMalformedValue,
                                                              "variant site lies outside its declared contig length");
        }
        if (index > 0) {
            const VariantSite& previous = sites[index - 1];
            if (site.dataset_order < previous.dataset_order ||
                (group_key(site) == group_key(previous) &&
                 (!(previous.position < site.position) || previous.site_order >= site.site_order ||
                  previous.vcf_record_order >= site.vcf_record_order))) {
                return ParseResult<DatasetExecutionPlan>::failure(
                    ParseReason::kMalformedValue, "variant dataset/group/position/site order is not strictly frozen");
            }
        }
        auto [range, inserted] = group_ranges.emplace(group_key(site), std::pair{index, index + 1});
        if (!inserted) {
            if (range->second.second != index) {
                return ParseResult<DatasetExecutionPlan>::failure(ParseReason::kMalformedValue,
                                                                  "a dataset/contig group recurs after another group");
            }
            range->second.second = index + 1;
        }
    }

    DatasetExecutionPlan output;
    std::size_t begin = 0;
    while (begin < sites.size()) {
        const GroupKey group = group_key(sites[begin]);
        std::size_t end = begin + 1;
        std::uint64_t planning_total = options.planning_units_per_site;
        while (end < sites.size() && group_key(sites[end]) == group) {
            const std::size_t candidate_count = end - begin + 1;
            const std::uint64_t candidate_span = sites[end].position.value() - sites[begin].position.value();
            const bool site_limit = candidate_count > options.max_focal_sites_per_block;
            const bool span_limit = candidate_span > options.max_focal_span_bp;
            const bool planning_limit =
                options.planning_units_per_site > options.max_estimated_alignments_per_block - planning_total;
            if (site_limit || span_limit || planning_limit) {
                break;
            }
            planning_total += options.planning_units_per_site;
            ++end;
        }
        auto alignment = make_alignment_block(sites, begin, end, output.blocks.size(), options);
        if (!alignment.ok()) {
            return ParseResult<DatasetExecutionPlan>::failure(alignment.reason, std::move(alignment.detail));
        }

        const auto group_range = group_ranges.find(group);
        if (group_range == group_ranges.end()) {
            return ParseResult<DatasetExecutionPlan>::failure(ParseReason::kMalformedValue,
                                                              "dataset/contig range disappeared during planning");
        }
        const auto marker_begin = std::lower_bound(
            sites.begin() + static_cast<std::ptrdiff_t>(group_range->second.first),
            sites.begin() + static_cast<std::ptrdiff_t>(group_range->second.second),
            alignment.value->fetch_interval.begin(),
            [](const VariantSite& site, std::uint64_t zero_based) { return site.position.zero_based() < zero_based; });
        const auto marker_end = std::lower_bound(
            marker_begin, sites.begin() + static_cast<std::ptrdiff_t>(group_range->second.second),
            alignment.value->fetch_interval.end(),
            [](const VariantSite& site, std::uint64_t zero_based) { return site.position.zero_based() < zero_based; });
        const std::size_t marker_begin_index = static_cast<std::size_t>(marker_begin - sites.begin());
        const std::size_t marker_end_index = static_cast<std::size_t>(marker_end - sites.begin());
        if (marker_begin_index > begin || marker_end_index < end || marker_begin_index == marker_end_index) {
            return ParseResult<DatasetExecutionPlan>::failure(ParseReason::kMalformedValue,
                                                              "block marker halo does not contain every focal site");
        }
        output.marker_occurrences += marker_end_index - marker_begin_index;
        const std::uint64_t observed_span = sites[end - 1].position.value() - sites[begin].position.value();
        output.maximum_observed_focal_span_bp = std::max(output.maximum_observed_focal_span_bp, observed_span);
        output.blocks.push_back(PlannedDatasetBlock{std::move(*alignment.value), marker_begin_index, marker_end_index});
        begin = end;
    }
    output.focal_site_count = sites.size();
    return ParseResult<DatasetExecutionPlan>::success(std::move(output));
}

}  // namespace longlineage::pipeline
