// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/pipeline/site_matrix.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace longlineage::pipeline {
namespace {

[[nodiscard]] ParseResult<Interval0> focal_window(const VariantSite& site, std::uint64_t halo_bp) {
    const std::uint64_t zero = site.position.zero_based();
    const std::uint64_t begin = zero > halo_bp ? zero - halo_bp : 0;
    const std::uint64_t trailing = site.contig_length - site.position.value();
    const std::uint64_t end = halo_bp >= trailing ? site.contig_length : site.position.value() + halo_bp;
    return Interval0::from_bounds(begin, end);
}

}  // namespace

double m1_point_from_ml(std::uint8_t ml_raw, M1PointMode mode) noexcept {
    const float legacy_binary32 = static_cast<float>(ml_raw) / static_cast<float>(255.0F);
    const double point = static_cast<double>(legacy_binary32);
    if (mode == M1PointMode::kLegacyCsvRound4) {
        return std::round(point * 10000.0) / 10000.0;
    }
    return point;
}

ParseResult<SiteMethylationMatrix> build_site_methylation_matrix(const BlockScienceEvidence& block,
                                                                 const FocalSiteEvidence& focal, std::uint64_t halo_bp,
                                                                 M1PointMode point_mode) {
    if (halo_bp != 5000) {
        return ParseResult<SiteMethylationMatrix>::failure(ParseReason::kUnsupportedValue,
                                                           "v1 M1 focal methylation halo is frozen at 5000 bp");
    }
    const auto block_focal =
        std::find_if(block.focal_sites.begin(), block.focal_sites.end(), [&focal](const FocalSiteEvidence& candidate) {
            return candidate.site.dataset_order == focal.site.dataset_order &&
                   candidate.site.site_order == focal.site.site_order;
        });
    if (block_focal == block.focal_sites.end() || block_focal->site.contig != focal.site.contig ||
        block_focal->site.position != focal.site.position) {
        return ParseResult<SiteMethylationMatrix>::failure(ParseReason::kMalformedValue,
                                                           "requested focal site is absent from its block evidence");
    }
    auto window = focal_window(focal.site, halo_bp);
    if (!window.ok()) {
        return ParseResult<SiteMethylationMatrix>::failure(window.reason, std::move(window.detail));
    }

    SiteMethylationMatrix output;
    output.site_order = focal.site.site_order;
    std::set<std::size_t> observed_read_indices;
    std::set<std::uint64_t> cpg_values;
    for (const std::size_t read_index : focal.covering_read_indices) {
        if (read_index >= block.reads.size() || !observed_read_indices.insert(read_index).second) {
            return ParseResult<SiteMethylationMatrix>::failure(
                ParseReason::kMalformedValue, "focal covering-read index is out of range or duplicated");
        }
        const JoinedReadEvidence& read = block.reads[read_index];
        const ProjectedAlleleCall* focal_call = find_allele_call(read, focal.site.site_order);
        if (focal_call == nullptr) {
            return ParseResult<SiteMethylationMatrix>::failure(ParseReason::kMalformedValue,
                                                               "covering read lacks a focal sSNV call");
        }
        if (focal_call->call != AlleleCall::kReference && focal_call->call != AlleleCall::kAlternate) {
            continue;
        }
        const std::size_t matrix_row = output.read_indices.size();
        output.read_indices.push_back(read_index);
        if (focal_call->call == AlleleCall::kAlternate) {
            output.alt_row_indices.push_back(matrix_row);
        }
        for (const ProjectedMethylationCall& call : read.methylation_calls) {
            if (window.value->contains(call.candidate_cpg_position)) {
                cpg_values.insert(call.candidate_cpg_position.value());
            }
        }
    }
    output.cpg_positions.reserve(cpg_values.size());
    for (const std::uint64_t value : cpg_values) {
        auto position = Position1::from_value(value);
        if (!position.ok()) {
            return ParseResult<SiteMethylationMatrix>::failure(position.reason, std::move(position.detail));
        }
        output.cpg_positions.push_back(*position.value);
    }

    std::map<std::uint64_t, std::size_t> column_by_position;
    for (std::size_t column = 0; column < output.cpg_positions.size(); ++column) {
        column_by_position.emplace(output.cpg_positions[column].value(), column);
    }
    output.values.assign(output.read_indices.size(),
                         std::vector<std::optional<double>>(output.cpg_positions.size(), std::nullopt));
    output.called_cpg_counts.assign(output.read_indices.size(), 0);
    for (std::size_t row = 0; row < output.read_indices.size(); ++row) {
        const JoinedReadEvidence& read = block.reads[output.read_indices[row]];
        for (const ProjectedMethylationCall& call : read.methylation_calls) {
            const auto column = column_by_position.find(call.candidate_cpg_position.value());
            if (column == column_by_position.end()) {
                continue;
            }
            auto& cell = output.values[row][column->second];
            if (cell.has_value()) {
                return ParseResult<SiteMethylationMatrix>::failure(
                    ParseReason::kMalformedValue, "one read contributes multiple M1 values at one CpG");
            }
            cell = m1_point_from_ml(call.ml_raw, point_mode);
            ++output.called_cpg_counts[row];
        }
    }
    return output.read_indices.empty() ? ParseResult<SiteMethylationMatrix>::success_empty(std::move(output))
                                       : ParseResult<SiteMethylationMatrix>::success(std::move(output));
}

}  // namespace longlineage::pipeline
