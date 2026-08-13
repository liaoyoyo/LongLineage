// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/pipeline/site_science.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "longlineage/common/digest.hpp"
#include "longlineage/m1/hp_family.hpp"

namespace longlineage::pipeline {
namespace {

[[nodiscard]] bool is_core_label(std::string_view label) noexcept { return label != "other" && label != "outlier"; }

[[nodiscard]] bool is_phase_anchored_token(std::string_view hp) noexcept {
    return hp == "1-1" || hp == "2-1" || hp == "1-2" || hp == "2-2";
}

[[nodiscard]] std::string site_context(const FocalSiteEvidence& focal) {
    return focal.site.dataset_id + ':' + focal.site.contig.value() + ':' + std::to_string(focal.site.position.value());
}

[[nodiscard]] ParseResult<std::string> science_digest(const FocalSiteEvidence& focal, const SiteScienceResult& result,
                                                      M1Representation representation) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "longlineage.site_science\t1.0.0\n"
           << focal.site.dataset_order << '\t' << focal.site.dataset_id << '\t' << focal.site.site_order << '\t'
           << focal.site.contig.value() << '\t' << focal.site.position.value() << '\t' << focal.site.reference << '\t'
           << focal.site.alternate << '\n'
           << "representation="
           << (representation == M1Representation::kRawBinary32Point ? "RAW_BINARY32_POINT"
                                                                     : "HISTORICAL_OBSERVED_ROUND6_NULL_ROUND4")
           << '\n'
           << "m1_status=" << static_cast<int>(result.m1.status) << '\n';
    if (result.m1.analysis.has_value()) {
        const auto& analysis = *result.m1.analysis;
        stream << "coarse_groups=" << analysis.coarse_groups << ";fine_groups=" << analysis.fine_groups
               << ";modal_fraction=" << analysis.modal_fraction << ";stable=" << analysis.stable_null_multigroup
               << ";partition=" << analysis.partition_sha256 << ";trace=" << analysis.trace_sha256 << '\n';
    }
    for (const auto& assignment : result.assignments) {
        stream << assignment.read_id << '\t' << assignment.matrix_row_index << '\t' << assignment.retained_m1_row_index
               << '\t' << assignment.called_cpg_count << '\t' << assignment.coarse_label << '\t'
               << assignment.fine_label << '\t' << assignment.core << '\n';
    }
    stream << "m2_status=" << m2::to_string(result.m2.decision.status)
           << ";reason=" << m2::to_string(result.m2.decision.reason) << ";core=" << result.m2.core_read_count
           << ";minimum_group=" << result.m2.minimum_core_group_size
           << ";phase_fraction=" << result.m2.phase_anchored_fraction_core
           << ";phase_robust=" << result.m2.phase_anchored_robust << '\n';
    if (result.m2.axes.has_value()) {
        stream << "m2_trace=" << result.m2.axes->trace_sha256 << '\n';
    }
    return sha256_hex(stream.str());
}

}  // namespace

std::string_view hp_family_for_token(std::string_view hp_token) noexcept {
    return m1::to_string(m1::hp_family(hp_token));
}

ParseResult<SiteScienceResult> run_site_science(const BlockScienceEvidence& block, const FocalSiteEvidence& focal,
                                                const SiteScienceOptions& options) {
    try {
        auto matrix = build_site_methylation_matrix(block, focal, 5000, M1PointMode::kFloat32RawDiv255);
        if (!matrix.ok()) {
            return ParseResult<SiteScienceResult>::failure(matrix.reason,
                                                           site_context(focal) + " matrix: " + matrix.detail);
        }

        SiteScienceResult output;
        output.matrix = std::move(*matrix.value);
        m1::Matrix alt_methylation;
        std::vector<std::string> alt_keys;
        alt_methylation.reserve(output.matrix.alt_row_indices.size());
        alt_keys.reserve(output.matrix.alt_row_indices.size());
        std::map<std::string, std::pair<std::size_t, std::size_t>> alt_location_by_key;
        for (const std::size_t matrix_row : output.matrix.alt_row_indices) {
            if (matrix_row >= output.matrix.values.size() || matrix_row >= output.matrix.read_indices.size()) {
                return ParseResult<SiteScienceResult>::failure(
                    ParseReason::kMalformedValue, site_context(focal) + " ALT row lies outside the site matrix");
            }
            const std::size_t read_index = output.matrix.read_indices[matrix_row];
            if (read_index >= block.reads.size()) {
                return ParseResult<SiteScienceResult>::failure(
                    ParseReason::kMalformedValue,
                    site_context(focal) + " matrix read index lies outside block evidence");
            }
            const JoinedReadEvidence& read = block.reads[read_index];
            if (!alt_location_by_key.emplace(read.read_id, std::make_pair(matrix_row, read_index)).second) {
                return ParseResult<SiteScienceResult>::failure(ParseReason::kMalformedValue,
                                                               site_context(focal) + " duplicate ALT read ID");
            }
            std::vector<double> row;
            row.reserve(output.matrix.values[matrix_row].size());
            for (const auto& value : output.matrix.values[matrix_row]) {
                row.push_back(value.has_value() ? *value : std::numeric_limits<double>::quiet_NaN());
            }
            alt_methylation.push_back(std::move(row));
            alt_keys.push_back(read.read_id);
        }

        m1::M1Options integrated_m1_options = options.m1_options;
        integrated_m1_options.base_seed =
            m1::stable_site_seed(focal.site.dataset_id, focal.site.contig.value(), focal.site.position.value(), 0);
        if (options.m1_representation == M1Representation::kRawBinary32Point) {
            output.m1 = m1::run_full_m1(alt_methylation, alt_keys, integrated_m1_options);
        } else {
            output.m1 = m1::run_historical_m1(alt_methylation, alt_keys, integrated_m1_options);
        }

        if (output.m1.analysis.has_value()) {
            const auto& analysis = *output.m1.analysis;
            if (output.m1.retained_keys.size() != analysis.coarse_labels.size() ||
                output.m1.retained_keys.size() != analysis.fine_labels.size()) {
                return ParseResult<SiteScienceResult>::failure(ParseReason::kMalformedValue,
                                                               site_context(focal) + " M1 key/label cardinality drift");
            }
            output.assignments.reserve(output.m1.retained_keys.size());
            for (std::size_t retained = 0; retained < output.m1.retained_keys.size(); ++retained) {
                const auto location = alt_location_by_key.find(output.m1.retained_keys[retained]);
                if (location == alt_location_by_key.end()) {
                    return ParseResult<SiteScienceResult>::failure(
                        ParseReason::kMalformedValue,
                        site_context(focal) + " retained M1 key is absent from ALT matrix");
                }
                const std::size_t matrix_row = location->second.first;
                output.assignments.push_back(M1ReadAssignment{
                    location->first,
                    location->second.second,
                    matrix_row,
                    retained,
                    output.matrix.called_cpg_counts.at(matrix_row),
                    analysis.coarse_labels[retained],
                    analysis.fine_labels[retained],
                    is_core_label(analysis.coarse_labels[retained]),
                });
            }
        }

        const bool m1_flagged = output.m1.analysis.has_value() && output.m1.analysis->stable_null_multigroup;
        const std::size_t group_count = output.m1.analysis.has_value() ? output.m1.analysis->coarse_groups : 0;
        if (m1_flagged && group_count <= m2::kMaximumGroups) {
            m2::EightAxisInput axis_input;
            axis_input.base_seed =
                m1::stable_site_seed(focal.site.dataset_id, focal.site.contig.value(), focal.site.position.value(), 0);
            std::map<std::string, std::size_t> core_group_sizes;
            std::size_t phase_anchored = 0;
            for (const auto& assignment : output.assignments) {
                if (!assignment.core) {
                    continue;
                }
                const JoinedReadEvidence& read = block.reads.at(assignment.block_read_index);
                axis_input.stable_keys.push_back(assignment.read_id);
                axis_input.labels.push_back(assignment.coarse_label);
                axis_input.hp_exact.push_back(read.latest_tags.hp);
                axis_input.hp_family.emplace_back(hp_family_for_token(read.latest_tags.hp));
                axis_input.strand.emplace_back(1, to_char(read.projection.strand));
                axis_input.start.push_back(static_cast<double>(read.projection.reference_interval.begin()));
                axis_input.end.push_back(static_cast<double>(read.projection.reference_interval.end()));
                axis_input.length.push_back(static_cast<double>(read.projection.reference_interval.size()));
                axis_input.mapq.push_back(static_cast<double>(read.projection.mapq));
                axis_input.cpg_called.push_back(static_cast<double>(assignment.called_cpg_count));
                ++core_group_sizes[assignment.coarse_label];
                phase_anchored += is_phase_anchored_token(read.latest_tags.hp);
            }
            output.m2.core_read_count = axis_input.labels.size();
            if (core_group_sizes.size() != group_count || output.m2.core_read_count == 0) {
                return ParseResult<SiteScienceResult>::failure(
                    ParseReason::kMalformedValue,
                    site_context(focal) + " M1 coarse group/core assignment conservation drift");
            }
            output.m2.minimum_core_group_size =
                std::min_element(core_group_sizes.begin(), core_group_sizes.end(),
                                 [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; })
                    ->second;
            output.m2.phase_anchored_fraction_core =
                static_cast<double>(phase_anchored) / static_cast<double>(output.m2.core_read_count);
            output.m2.axes = m2::evaluate_eight_axes(axis_input);
            output.m2.phase_anchored_robust = output.m2.axes->residual_unexplained && output.m2.core_read_count >= 10 &&
                                              output.m1.analysis->modal_fraction == 1.0 &&
                                              output.m2.minimum_core_group_size >= 5 &&
                                              output.m2.phase_anchored_fraction_core >= 0.80;
            output.m2.decision = m2::evaluate_precedence(m2::M2PrecedenceInput{
                true,
                group_count,
                output.m2.axes->axis_indeterminate,
                output.m2.axes->hp_axis_confound,
                output.m2.axes->technical_axis_confound,
                output.m2.axes->residual_unexplained,
                output.m2.phase_anchored_robust,
            });
        } else {
            output.m2.decision = m2::evaluate_precedence(m2::M2PrecedenceInput{
                m1_flagged,
                group_count,
                false,
                false,
                false,
                false,
                false,
            });
        }

        auto digest = science_digest(focal, output, options.m1_representation);
        if (!digest.ok()) {
            return ParseResult<SiteScienceResult>::failure(digest.reason,
                                                           site_context(focal) + " semantic digest: " + digest.detail);
        }
        output.semantic_sha256 = std::move(*digest.value);
        return output.assignments.empty() ? ParseResult<SiteScienceResult>::success_empty(std::move(output))
                                          : ParseResult<SiteScienceResult>::success(std::move(output));
    } catch (const std::exception& error) {
        return ParseResult<SiteScienceResult>::failure(ParseReason::kMalformedValue,
                                                       site_context(focal) + " M1/M2 science failed: " + error.what());
    }
}

}  // namespace longlineage::pipeline
