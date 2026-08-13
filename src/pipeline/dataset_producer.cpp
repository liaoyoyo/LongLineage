// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/pipeline/dataset_producer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"
#include "longlineage/cooccurrence/joint_signature.hpp"
#include "longlineage/cooccurrence/site_cooccurrence.hpp"
#include "longlineage/runtime/byte_bounded_reorder_sink.hpp"
#include "longlineage/runtime/ordered_thread_pool.hpp"
#include "longlineage/solver/evidence_builder.hpp"
#include "longlineage/solver/topology_record.hpp"
#include "longlineage/solver/topology_router.hpp"
#include "longlineage/solver/vertex_set_ranker.hpp"

namespace longlineage::pipeline {
namespace {

using artifact::DatasetArtifactWriteReceipt;
using artifact::IndexedBgzfJsonlWriter;
using artifact::IndexedBgzfTsvWriter;
using artifact::IndexedLlmWriter;
using cooccurrence::JointSignatureResult;
using cooccurrence::PairInference;

constexpr std::size_t kMaximumWorkers = 46;
constexpr std::size_t kTaskChargeBytes = 256;

[[nodiscard]] std::uint64_t process_thread_count() {
    std::ifstream input("/proc/self/status");
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("Threads:", 0) != 0U) {
            continue;
        }
        std::istringstream parser(line.substr(8));
        std::uint64_t observed = 0;
        if (parser >> observed && observed > 0U) {
            return observed;
        }
        break;
    }
    throw std::runtime_error("cannot observe process thread count");
}

const std::vector<std::string> kSiteReadsHeader{"dataset_order",
                                                "dataset_id",
                                                "site_order",
                                                "chrom",
                                                "pos1",
                                                "ref",
                                                "alt",
                                                "read_id",
                                                "alignment_flag",
                                                "cigar_b2",
                                                "typed_aux_sha256_without_rg",
                                                "start0",
                                                "end0",
                                                "mapq",
                                                "strand",
                                                "query_length",
                                                "allele_call",
                                                "baseq",
                                                "latest_hp",
                                                "latest_ps",
                                                "latest_tag_source",
                                                "full_identity_count",
                                                "projection_join_status"};

const std::vector<std::string> kMethylCallsHeader{
    "dataset_order",  "site_order", "read_id",           "cpg_order",         "query_pos0",     "reference_pos1",
    "canonical_base", "mod_code",   "mm_strand",         "mm_skip_mode",      "mm_group_index", "ml_index",
    "mn_value",       "ml_raw",     "probability_lower", "probability_upper", "skip_semantics"};

const std::vector<std::string> kM1SitesHeader{"dataset_order",
                                              "dataset_id",
                                              "site_order",
                                              "chrom",
                                              "pos1",
                                              "ref",
                                              "alt",
                                              "analysis_status",
                                              "reason",
                                              "n_alt_joined",
                                              "n_alt_after_peel",
                                              "distance_matrix_sha256",
                                              "coarse_split_trace_sha256",
                                              "coarse_k",
                                              "fine_split_trace_sha256",
                                              "fine_k",
                                              "non_germline_groups",
                                              "modal_fraction",
                                              "modal_assignment_ari_min",
                                              "modal_assignment_sha256",
                                              "rng_trace_sha256",
                                              "stable",
                                              "hp_exact_axis",
                                              "hp_family_axis",
                                              "strand_axis",
                                              "start_axis",
                                              "end_axis",
                                              "length_axis",
                                              "mapq_axis",
                                              "cpg_called_axis"};

const std::vector<std::string> kCooccurrencePairsHeader{"dataset_order",
                                                        "dataset_id",
                                                        "focal_site_order",
                                                        "chrom",
                                                        "focal_pos1",
                                                        "focal_ref",
                                                        "focal_alt",
                                                        "partner_site_order",
                                                        "partner_pos1",
                                                        "partner_ref",
                                                        "partner_alt",
                                                        "distance_bp",
                                                        "group_count",
                                                        "group_allele_counts_json",
                                                        "group_allele_counts_sha256",
                                                        "n_informative",
                                                        "min_group_n",
                                                        "ref_n",
                                                        "alt_n",
                                                        "exact_state_count",
                                                        "exact_status",
                                                        "family_status",
                                                        "p_exact",
                                                        "fdr_family_id",
                                                        "fdr_family_size",
                                                        "q_global_bh",
                                                        "q_global_by",
                                                        "cramers_v",
                                                        "delta_af",
                                                        "effect_gate_pass",
                                                        "exact_bh_discovery",
                                                        "exact_by_discovery",
                                                        "conditional_valid_permutations",
                                                        "conditional_exceedance",
                                                        "conditional_p",
                                                        "conditional_status",
                                                        "formal_pair_by_confirmed",
                                                        "callability_status",
                                                        "callability_q_global_by",
                                                        "callability_cramers_v",
                                                        "pair_read_count",
                                                        "ra_complete_read_count",
                                                        "rr",
                                                        "ra",
                                                        "ro",
                                                        "rx",
                                                        "ar",
                                                        "aa",
                                                        "ao",
                                                        "ax",
                                                        "or",
                                                        "oa",
                                                        "oo",
                                                        "ox",
                                                        "xr",
                                                        "xa",
                                                        "xo",
                                                        "xx",
                                                        "error_ceiling",
                                                        "familywise_confidence",
                                                        "focal_ancestor_p_exact",
                                                        "focal_ancestor_upper_bound",
                                                        "focal_ancestor_status",
                                                        "partner_ancestor_p_exact",
                                                        "partner_ancestor_upper_bound",
                                                        "partner_ancestor_status",
                                                        "branching_p_exact",
                                                        "branching_upper_bound",
                                                        "branching_status",
                                                        "complete_four_state_testable",
                                                        "compatibility_status",
                                                        "compatible_relation_models_json",
                                                        "n_compatible_relation_models",
                                                        "claim_guardrail"};

const std::vector<std::string> kCooccurrenceSitesHeader{"dataset_order",
                                                        "dataset_id",
                                                        "site_order",
                                                        "chrom",
                                                        "pos1",
                                                        "ref",
                                                        "alt",
                                                        "m1_group_count",
                                                        "m2_status",
                                                        "m2_reason",
                                                        "m2_precedence_rank",
                                                        "m2_grid_id",
                                                        "m2_permutations",
                                                        "min_group_size",
                                                        "categorical_v",
                                                        "continuous_eta2",
                                                        "power",
                                                        "partner_universe_size",
                                                        "exact_testable_pairs",
                                                        "global_bh_discoveries",
                                                        "global_by_discoveries",
                                                        "joint_signature_status",
                                                        "joint_partner_orders_json",
                                                        "joint_complete_readset_sha256"};

[[nodiscard]] double elapsed_seconds(std::chrono::steady_clock::time_point begin) noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

[[nodiscard]] bool m2_partition_conserves(const DatasetProductionCounters& counts) noexcept {
    std::uint64_t remaining = counts.m1_stable_assignments;
    for (const std::uint64_t value : {counts.m2_eligible, counts.m2_evaluable_ineligible, counts.m2_axis_indeterminate,
                                      counts.m2_group_count_gt10}) {
        if (value > remaining) {
            return false;
        }
        remaining -= value;
    }
    return remaining == 0U;
}

[[nodiscard]] double nearest_rank_percentile(std::vector<double> values, double probability) {
    if (values.empty() || probability < 0.0 || probability > 1.0) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double rank = std::ceil(probability * static_cast<double>(values.size()));
    const std::size_t index = rank <= 1.0 ? 0U : static_cast<std::size_t>(rank - 1.0);
    return values.at(std::min(index, values.size() - 1U));
}

[[nodiscard]] std::string hash_or_throw(std::string_view bytes) {
    auto digest = longlineage::sha256_hex(bytes);
    if (!digest.ok()) {
        throw std::runtime_error("SHA-256 failed: " + digest.detail);
    }
    return std::move(*digest.value);
}

[[nodiscard]] std::string optional_uint(const std::optional<std::uint64_t>& value) {
    return value.has_value() ? std::to_string(*value) : ".";
}

[[nodiscard]] std::string optional_double(const std::optional<double>& value) {
    return value.has_value() ? artifact::canonical_float64(*value) : ".";
}

[[nodiscard]] std::string boolean(bool value) { return value ? "true" : "false"; }

[[nodiscard]] std::string json_string_array(const std::vector<std::string>& values) {
    std::string output = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        output.append(artifact::canonical_json_quote(values[index]));
    }
    output.push_back(']');
    return output;
}

[[nodiscard]] std::string json_uint_array(const std::vector<std::uint64_t>& values) {
    std::string output = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        output.append(std::to_string(values[index]));
    }
    output.push_back(']');
    return output;
}

[[nodiscard]] std::string group_table_json(const std::vector<cooccurrence::Kx2Row>& table) {
    std::string output = "[";
    for (std::size_t index = 0; index < table.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        output.append("[");
        output.append(std::to_string(table[index][0]));
        output.push_back(',');
        output.append(std::to_string(table[index][1]));
        output.push_back(']');
    }
    output.push_back(']');
    return output;
}

[[nodiscard]] std::string split_failure(m1::SplitFailure failure) {
    switch (failure) {
        case m1::SplitFailure::kNone:
            return "null";
        case m1::SplitFailure::kBelowSeparationMinimumOrUndefined:
            return "\"BELOW_SEPARATION_MINIMUM_OR_UNDEFINED\"";
        case m1::SplitFailure::kInsufficientValidNull:
            return "\"INSUFFICIENT_VALID_NULL\"";
        case m1::SplitFailure::kNotAboveNullThreshold:
            return "\"NOT_ABOVE_NULL_THRESHOLD\"";
        case m1::SplitFailure::kEmpiricalPAboveAlpha:
            return "\"EMPIRICAL_P_ABOVE_ALPHA\"";
    }
    throw std::logic_error("unknown M1 split failure");
}

[[nodiscard]] std::string json_optional_double(const std::optional<double>& value) {
    return value.has_value() ? artifact::canonical_float64(*value) : "null";
}

[[nodiscard]] std::string json_optional_size(const std::optional<std::size_t>& value) {
    return value.has_value() ? std::to_string(*value) : "null";
}

[[nodiscard]] std::string split_trace_json(const std::vector<m1::SplitTrace>& trace) {
    std::string output = "[";
    for (std::size_t index = 0; index < trace.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        const auto& split = trace[index];
        const auto& decision = split.decision;
        output.append("{\"node_order\":");
        output.append(std::to_string(index));
        output.append(",\"n_node\":");
        output.append(std::to_string(split.node_size));
        output.append(",\"child_sizes\":[");
        output.append(std::to_string(split.first_child_size));
        output.push_back(',');
        output.append(std::to_string(split.second_child_size));
        output.append("],\"observed_between_within\":");
        output.append(json_optional_double(decision.observed_between_within));
        output.append(",\"null_percentile\":");
        output.append(artifact::canonical_float64(decision.null_percentile));
        output.append(",\"null_replicates_requested\":");
        output.append(std::to_string(decision.null_replicates_requested));
        output.append(",\"minimum_valid_null\":");
        output.append(std::to_string(decision.minimum_valid_null));
        output.append(",\"n_valid_null\":");
        output.append(std::to_string(decision.valid_null_replicates));
        output.append(",\"exceedance\":");
        output.append(json_optional_size(decision.exceedance));
        output.append(",\"null_threshold\":");
        output.append(json_optional_double(decision.null_threshold));
        output.append(",\"empirical_p\":");
        output.append(json_optional_double(decision.empirical_p));
        output.append(",\"passed\":");
        output.append(boolean(decision.passed));
        output.append(",\"failure\":");
        output.append(split_failure(decision.failure));
        output.push_back('}');
    }
    output.push_back(']');
    return output;
}

[[nodiscard]] std::string trace_sha256(const std::vector<m1::SplitTrace>& trace) {
    return hash_or_throw(split_trace_json(trace));
}

[[nodiscard]] std::string m1_run_json(const m1::M1RunEvidence& run, const std::vector<std::string>& retained_keys) {
    if (run.labels.size() != retained_keys.size()) {
        throw std::runtime_error("M1 run label/read cardinality drift");
    }
    std::string output = "{\"seed_order\":";
    output.append(std::to_string(run.seed_order));
    output.append(",\"seed_u64\":");
    output.append(artifact::canonical_json_quote(std::to_string(run.seed)));
    output.append(",\"group_count\":");
    output.append(std::to_string(run.group_count));
    output.append(",\"labels\":");
    output.append(json_string_array(run.labels));
    output.append(",\"partition_sha256\":");
    output.append(artifact::canonical_json_quote(m1::partition_digest(retained_keys, run.labels)));
    output.append(",\"split_trace\":");
    output.append(split_trace_json(run.split_trace));
    output.push_back('}');
    return output;
}

[[nodiscard]] std::string readset_sha256(const std::vector<std::string>& read_ids) {
    std::string canonical = "longlineage.m1_readset\t1.0.0\n";
    for (const auto& read_id : read_ids) {
        canonical.append(read_id);
        canonical.push_back('\n');
    }
    return hash_or_throw(canonical);
}

[[nodiscard]] std::string m1_assignment_json(const VariantSite& site, const m1::FullM1Result& result) {
    if (!result.analysis.has_value()) {
        throw std::invalid_argument("cannot serialize absent M1 assignment");
    }
    const auto& analysis = *result.analysis;
    if (analysis.coarse_runs.size() != m1::kCoarseSeeds) {
        throw std::runtime_error("M1 assignment does not contain ten coarse runs");
    }
    std::string output =
        "{\"schema_name\":\"longlineage.m1_assignment\","
        "\"schema_version\":\"1.0.0\",\"dataset_order\":";
    output.append(std::to_string(site.dataset_order));
    output.append(",\"site_order\":");
    output.append(std::to_string(site.site_order));
    output.append(",\"read_ids\":");
    output.append(json_string_array(result.retained_keys));
    output.append(",\"labels\":");
    output.append(json_string_array(analysis.coarse_labels));
    output.append(",\"readset_sha256\":");
    output.append(artifact::canonical_json_quote(readset_sha256(result.retained_keys)));
    output.append(",\"partition_sha256\":");
    output.append(artifact::canonical_json_quote(analysis.partition_sha256));
    output.append(",\"representative_seed_order\":");
    output.append(std::to_string(analysis.representative_seed_order));
    output.append(",\"coarse_runs\":[");
    for (std::size_t index = 0; index < analysis.coarse_runs.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        output.append(m1_run_json(analysis.coarse_runs[index], result.retained_keys));
    }
    output.append("],\"fine_run\":");
    output.append(m1_run_json(analysis.fine_run, result.retained_keys));
    output.push_back('}');
    return output;
}

[[nodiscard]] std::string matrix_sha256(const m1::Matrix& matrix) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "longlineage.bernoulli_distance\t1.0.0\n" << matrix.size() << '\n';
    for (const auto& row : matrix) {
        if (row.size() != matrix.size()) {
            throw std::invalid_argument("M1 distance matrix is not square");
        }
        for (const double value : row) {
            stream << std::hexfloat << value << ',';
        }
        stream << '\n';
    }
    return hash_or_throw(stream.str());
}

[[nodiscard]] std::string axis_status(const m2::AxisDecision& decision) {
    switch (decision.status) {
        case m2::AxisStatus::kAlignedEffectAndPermutationPPass:
            return "FAIL";
        case m2::AxisStatus::kIndeterminateEffectAboveThresholdWithoutP:
        case m2::AxisStatus::kIndeterminateInsufficientInformation:
        case m2::AxisStatus::kIndeterminateAxisStatisticMissing:
            return "INDETERMINATE";
        case m2::AxisStatus::kNotAlignedConstantAxis:
        case m2::AxisStatus::kNotAlignedEffectBelowThresholdAdequatePower:
            return "PASS";
    }
    throw std::logic_error("unknown M2 axis status");
}

[[nodiscard]] cooccurrence::JointMarkerCall joint_marker_call(AlleleCall call) noexcept {
    switch (call) {
        case AlleleCall::kReference:
            return cooccurrence::JointMarkerCall::kReference;
        case AlleleCall::kAlternate:
            return cooccurrence::JointMarkerCall::kAlternate;
        case AlleleCall::kOther:
            return cooccurrence::JointMarkerCall::kOther;
        case AlleleCall::kUnobservable:
            return cooccurrence::JointMarkerCall::kNoCall;
    }
    return cooccurrence::JointMarkerCall::kNoCall;
}

[[nodiscard]] std::string phase_set_token(const std::optional<std::uint64_t>& phase_set) {
    return phase_set.has_value() ? std::to_string(*phase_set) : "missing";
}

[[nodiscard]] JointSignatureResult build_joint_signature_evidence(
    const BlockScienceEvidence& block, const FocalSiteEvidence& focal, const SiteScienceResult& science,
    const cooccurrence::SiteCooccurrenceResult& site_pairs) {
    if (!science.m2.decision.eligible) {
        return {};
    }

    std::vector<cooccurrence::JointPartnerCandidate> candidates;
    candidates.reserve(site_pairs.pairs.size());
    for (const auto& pair : site_pairs.pairs) {
        candidates.push_back({pair.partner.position.value(), pair.partner.reference, pair.partner.alternate,
                              pair.endpoint_a.n_informative});
    }

    std::vector<cooccurrence::JointCoreRead> reads;
    reads.reserve(science.assignments.size());
    for (const auto& assignment : science.assignments) {
        if (!assignment.core) {
            continue;
        }
        const auto& read = block.reads.at(assignment.block_read_index);
        cooccurrence::JointCoreRead row;
        row.stable_key = assignment.read_id;
        row.label = assignment.coarse_label;
        row.hp_family = std::string(hp_family_for_token(read.latest_tags.hp));
        row.phase_set = phase_set_token(read.latest_tags.ps);
        row.strand = std::string(1, to_char(read.projection.strand));
        row.marker_calls.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            const auto marker = std::find_if(block.markers.begin(), block.markers.end(), [&](const VariantSite& site) {
                return site.position.value() == candidate.position1 && site.reference == candidate.reference &&
                       site.alternate == candidate.alternate;
            });
            if (marker == block.markers.end()) {
                throw std::runtime_error("joint-signature partner is absent from block markers");
            }
            const ProjectedAlleleCall* call = find_allele_call(read, marker->site_order);
            row.marker_calls.push_back({candidate.position1, call == nullptr ? cooccurrence::JointMarkerCall::kNoCall
                                                                             : joint_marker_call(call->call)});
        }
        reads.push_back(std::move(row));
    }

    auto evaluated = cooccurrence::evaluate_joint_signature(
        focal.site.position.value(), candidates, reads,
        m1::stable_site_seed(focal.site.dataset_id, focal.site.contig.value(), focal.site.position.value(), 30000));
    if (!evaluated.ok() || !evaluated.value.has_value()) {
        throw std::runtime_error("joint-signature evaluation failed: " + evaluated.detail);
    }
    return std::move(*evaluated.value);
}

struct SitePostRecord {
    explicit SitePostRecord(VariantSite site_value) : site(std::move(site_value)) {}

    VariantSite site;
    std::size_t block_index{0};
    std::size_t pair_begin{0};
    std::size_t pair_end{0};
    std::optional<std::size_t> m1_group_count;
    bool m1_stable{false};
    m2::M2Decision m2_decision;
    std::optional<std::size_t> minimum_group_size;
    std::optional<double> categorical_v;
    std::optional<double> continuous_eta2;
    std::optional<double> minimum_power;
    std::uint64_t partner_universe_size{0};
    JointSignatureResult joint_signature;
    bool joint_pass{false};
    std::vector<std::uint64_t> selected_partner_site_orders;
};

struct SiteBlockProduct {
    std::size_t focal_index{0};
    SiteScienceResult science;
    cooccurrence::SiteCooccurrenceResult cooccurrence;
    JointSignatureResult joint_signature;
};

struct BlockProductionPayload {
    std::size_t block_index{0};
    DatasetBlockLoad loaded;
    std::vector<SiteBlockProduct> sites;
    std::size_t charge_bytes{1};

    [[nodiscard]] std::size_t retained_bytes() const noexcept { return charge_bytes; }
};

[[nodiscard]] std::size_t estimate_payload_bytes(const BlockProductionPayload& payload) noexcept {
    std::size_t bytes = payload.loaded.evidence.logical_retained_bytes();
    for (const auto& site : payload.sites) {
        for (const auto& row : site.science.matrix.values) {
            bytes += row.size() * (sizeof(std::optional<double>) + 1U);
        }
        bytes += site.science.assignments.size() * sizeof(M1ReadAssignment);
        if (site.science.m1.analysis.has_value()) {
            const auto& analysis = *site.science.m1.analysis;
            for (const auto& run : analysis.coarse_runs) {
                bytes += run.labels.size() * 48U;
                bytes += run.split_trace.size() * sizeof(m1::SplitTrace);
            }
            bytes += analysis.fine_run.labels.size() * 48U;
        }
        for (const auto& pair : site.cooccurrence.pairs) {
            bytes += sizeof(pair);
            bytes += pair.endpoint_a.groups.size() * 32U;
            bytes += pair.endpoint_a.table.size() * sizeof(cooccurrence::Kx2Row);
            bytes += pair.conditional_payload.labels.size() * 96U;
        }
    }
    return std::max<std::size_t>(bytes, 1U);
}

struct ArtifactStreamState {
    IndexedBgzfTsvWriter site_reads;
    IndexedBgzfTsvWriter methyl_calls;
    IndexedLlmWriter bernoulli;
    IndexedBgzfTsvWriter m1_sites;
    IndexedBgzfJsonlWriter m1_assignments;
    DatasetProductionCounters counters;
    std::vector<PairInference> pairs;
    std::vector<SitePostRecord> sites;
    double emit_seconds{0.0};

    ArtifactStreamState(const std::filesystem::path& root, const std::string& run_id, int threads)
        : site_reads(root / "site_reads.tsv.bgz", root / "indexes/site_reads.site_index.tsv.bgz", "site_reads",
                     "longlineage.site_reads", "1.0.0", run_id, kSiteReadsHeader, 1),
          methyl_calls(root / "methyl_calls.tsv.bgz", root / "indexes/methyl_calls.site_index.tsv.bgz", "methyl_calls",
                       "longlineage.methyl_calls", "1.0.0", run_id, kMethylCallsHeader, threads),
          bernoulli(root / "bernoulli_upper.llm.bgz", root / "indexes/bernoulli_upper.site_index.tsv.bgz", run_id, 1),
          m1_sites(root / "m1_sites.tsv.bgz", root / "indexes/m1_sites.site_index.tsv.bgz", "m1_sites",
                   "longlineage.m1_sites", "1.0.0", run_id, kM1SitesHeader, 1),
          m1_assignments(root / "m1_assignments.jsonl.bgz", root / "indexes/m1_assignments.site_index.tsv.bgz",
                         "m1_assignments", "longlineage.m1_assignment", "1.0.0", run_id, 1) {
        // `threads` is the complete BGZF worker budget, not a per-file
        // multiplier. The five streaming artifacts are open concurrently, so
        // only the dominant methyl_calls stream may own that budget. The
        // other streams compress synchronously. Later pair/site/topology
        // writers are opened sequentially and safely reuse the same budget.
    }
};

[[nodiscard]] std::pair<std::string, std::string> m1_status_reason(m1::M1ReadsetStatus status) {
    switch (status) {
        case m1::M1ReadsetStatus::kInsufficientAltReads:
            return {"INSUFFICIENT_ALT_READS", "INSUFFICIENT_MATRIX_JOINED_FOCAL_ALT_READS"};
        case m1::M1ReadsetStatus::kIncompleteDistanceBelowMinimum:
            return {"INCOMPLETE_DISTANCE_BELOW_MINIMUM", "INCOMPLETE_DISTANCE_BELOW_MINIMUM"};
        case m1::M1ReadsetStatus::kFullClusteringReady:
            return {"EVALUABLE", "NONE"};
        case m1::M1ReadsetStatus::kPrimitiveParityReady:
            break;
    }
    throw std::runtime_error("production site returned primitive-only M1 state");
}

[[nodiscard]] std::vector<std::string> m1_axis_fields(const SiteScienceResult& science) {
    if (!science.m1.analysis.has_value() || !science.m1.analysis->stable_null_multigroup) {
        return std::vector<std::string>(8U, "NOT_RUN_UNSTABLE");
    }
    if (!science.m2.axes.has_value()) {
        return std::vector<std::string>(8U, "INDETERMINATE");
    }
    if (science.m2.axes->axes.size() != 8U) {
        throw std::runtime_error("stable M2 axis result does not contain eight axes");
    }
    std::vector<std::string> output;
    output.reserve(8U);
    for (const auto& axis : science.m2.axes->axes) {
        output.push_back(axis_status(axis.decision));
    }
    return output;
}

void write_site_reads(ArtifactStreamState& stream, const BlockScienceEvidence& block, const FocalSiteEvidence& focal) {
    std::vector<std::size_t> order = focal.covering_read_indices;
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return block.reads.at(lhs).read_id < block.reads.at(rhs).read_id;
    });
    for (const std::size_t read_index : order) {
        const auto& read = block.reads.at(read_index);
        const ProjectedAlleleCall* call = find_allele_call(read, focal.site.site_order);
        if (call == nullptr) {
            throw std::runtime_error("focal covering read lacks serialized allele call");
        }
        const std::uint64_t occurrences = read.raw_alignment_occurrences;
        if (occurrences == 0U) {
            throw std::runtime_error("joined read has zero full-identity occurrences");
        }
        stream.site_reads.write_row(focal.site.dataset_order, focal.site.site_order,
                                    {std::to_string(focal.site.dataset_order),
                                     focal.site.dataset_id,
                                     std::to_string(focal.site.site_order),
                                     focal.site.contig.value(),
                                     std::to_string(focal.site.position.value()),
                                     std::string(1, focal.site.reference),
                                     std::string(1, focal.site.alternate),
                                     read.read_id,
                                     std::to_string(read.alignment_flag),
                                     read.cigar_blake2b64,
                                     read.typed_aux_sha256_without_rg,
                                     std::to_string(read.projection.reference_interval.begin()),
                                     std::to_string(read.projection.reference_interval.end()),
                                     std::to_string(read.projection.mapq),
                                     std::string(1, to_char(read.projection.strand)),
                                     std::to_string(read.query_length),
                                     std::string(1, to_char(call->call)),
                                     call->base_quality.has_value() ? std::to_string(*call->base_quality) : ".",
                                     read.latest_tags.hp,
                                     optional_uint(read.latest_tags.ps),
                                     "RAW_ALL_PRODUCTION_SIDECAR_V2",
                                     std::to_string(occurrences),
                                     occurrences == 1U ? "EXACT_UNIQUE" : "RG_ONLY_COLLAPSED"});
        ++stream.counters.site_read_rows;
        ++stream.counters.raw_expected;
        stream.counters.raw_matched += occurrences;
        stream.counters.raw_rg_only_duplicate_occurrences += occurrences - 1U;
        stream.counters.latest_tag_exact_joins += occurrences == 1U;
    }
}

void write_methyl_calls(ArtifactStreamState& stream, const BlockScienceEvidence& block,
                        const SiteScienceResult& science, const VariantSite& site) {
    std::vector<std::size_t> alt_rows = science.matrix.alt_row_indices;
    std::sort(alt_rows.begin(), alt_rows.end(), [&](std::size_t lhs, std::size_t rhs) {
        return block.reads.at(science.matrix.read_indices.at(lhs)).read_id <
               block.reads.at(science.matrix.read_indices.at(rhs)).read_id;
    });
    for (const std::size_t row : alt_rows) {
        const auto& read = block.reads.at(science.matrix.read_indices.at(row));
        std::uint32_t cpg_order = 0;
        std::size_t call_index = 0;
        for (std::size_t column = 0; column < science.matrix.cpg_positions.size(); ++column) {
            if (!science.matrix.values.at(row).at(column).has_value()) {
                continue;
            }
            const Position1 position = science.matrix.cpg_positions[column];
            while (call_index < read.methylation_calls.size() &&
                   read.methylation_calls[call_index].candidate_cpg_position < position) {
                ++call_index;
            }
            if (call_index == read.methylation_calls.size() ||
                read.methylation_calls[call_index].candidate_cpg_position != position) {
                throw std::runtime_error("matrix CpG lacks projected methylation metadata");
            }
            const auto& call = read.methylation_calls[call_index];
            const double expected_lower = static_cast<double>(call.ml_raw) / 256.0;
            const double expected_upper = call.ml_raw == 255U ? 1.0 : static_cast<double>(call.ml_raw + 1U) / 256.0;
            if (call.probability_lower != expected_lower || call.probability_upper != expected_upper) {
                throw std::runtime_error("projected ML interval differs from frozen contract");
            }
            const auto probability = artifact::canonical_ml_probability_interval(call.ml_raw);
            stream.methyl_calls.write_row(
                site.dataset_order, site.site_order,
                {std::to_string(site.dataset_order), std::to_string(site.site_order), read.read_id,
                 std::to_string(cpg_order), std::to_string(call.query_pos0_as_sequenced),
                 std::to_string(position.value()), "C", "m", "+", "QUESTION", std::to_string(call.mm_group_index),
                 std::to_string(call.ml_index), optional_uint(read.mn_value), std::to_string(call.ml_raw),
                 std::string(probability.first), std::string(probability.second),
                 std::string(to_string(call.skip_semantics))});
            ++call_index;
            ++cpg_order;
            ++stream.counters.methyl_call_rows;
        }
    }
}

void write_m1_site(ArtifactStreamState& stream, const VariantSite& site, const SiteScienceResult& science) {
    const auto [status, reason] = m1_status_reason(science.m1.status);
    const std::size_t joined = science.matrix.alt_row_indices.size();
    const std::size_t peeled = science.m1.retained_indices.size();
    const bool evaluable = science.m1.analysis.has_value();
    const auto axes = m1_axis_fields(science);
    const auto& analysis = science.m1.analysis;
    stream.m1_sites.write_row(site.dataset_order, site.site_order,
                              {std::to_string(site.dataset_order),
                               site.dataset_id,
                               std::to_string(site.site_order),
                               site.contig.value(),
                               std::to_string(site.position.value()),
                               std::string(1, site.reference),
                               std::string(1, site.alternate),
                               status,
                               reason,
                               std::to_string(joined),
                               std::to_string(peeled),
                               joined >= m1::kMinimumReads ? matrix_sha256(science.m1.distance) : ".",
                               evaluable ? trace_sha256(analysis->representative_coarse_trace) : ".",
                               evaluable ? std::to_string(analysis->coarse_groups) : ".",
                               evaluable ? trace_sha256(analysis->fine_trace) : ".",
                               evaluable ? std::to_string(analysis->fine_groups) : ".",
                               evaluable ? std::to_string(analysis->coarse_groups) : ".",
                               evaluable ? artifact::canonical_float64(analysis->modal_fraction) : ".",
                               evaluable ? artifact::canonical_float64(analysis->modal_assignment_ari_minimum) : ".",
                               evaluable ? analysis->partition_sha256 : ".",
                               evaluable ? analysis->trace_sha256 : ".",
                               boolean(evaluable && analysis->stable_null_multigroup),
                               axes[0],
                               axes[1],
                               axes[2],
                               axes[3],
                               axes[4],
                               axes[5],
                               axes[6],
                               axes[7]});

    ++stream.counters.site_keys;
    switch (science.m1.status) {
        case m1::M1ReadsetStatus::kInsufficientAltReads:
            ++stream.counters.m1_insufficient_alt_reads;
            break;
        case m1::M1ReadsetStatus::kIncompleteDistanceBelowMinimum:
            ++stream.counters.m1_incomplete_distance;
            break;
        case m1::M1ReadsetStatus::kFullClusteringReady:
            ++stream.counters.m1_evaluable;
            break;
        case m1::M1ReadsetStatus::kPrimitiveParityReady:
            throw std::runtime_error("primitive M1 state reached producer");
    }
    if (evaluable) {
        stream.m1_assignments.write_record(site.dataset_order, site.site_order, m1_assignment_json(site, science.m1));
        if (analysis->stable_null_multigroup) {
            ++stream.counters.m1_stable_assignments;
        }
    }
}

SitePostRecord make_site_post(const VariantSite& site, std::size_t block_index, const SiteScienceResult& science,
                              const cooccurrence::SiteCooccurrenceResult& cooccurrence,
                              JointSignatureResult joint_signature, std::size_t pair_begin, std::size_t pair_end) {
    SitePostRecord post(site);
    post.block_index = block_index;
    post.pair_begin = pair_begin;
    post.pair_end = pair_end;
    if (science.m1.analysis.has_value()) {
        post.m1_group_count = science.m1.analysis->coarse_groups;
        post.m1_stable = science.m1.analysis->stable_null_multigroup;
    }
    post.m2_decision = science.m2.decision;
    if (science.m2.core_read_count != 0U) {
        post.minimum_group_size = science.m2.minimum_core_group_size;
    }
    if (science.m2.axes.has_value()) {
        for (const auto& axis : science.m2.axes->axes) {
            if (axis.association.effect.has_value()) {
                if (axis.kind == m2::AxisKind::kCategorical) {
                    post.categorical_v = std::max(post.categorical_v.value_or(0.0), *axis.association.effect);
                } else {
                    post.continuous_eta2 = std::max(post.continuous_eta2.value_or(0.0), *axis.association.effect);
                }
            }
            if (axis.decision.power_at_effect_threshold.has_value()) {
                post.minimum_power =
                    std::min(post.minimum_power.value_or(1.0), *axis.decision.power_at_effect_threshold);
            }
        }
    }
    post.partner_universe_size = cooccurrence.partner_universe_size;
    post.joint_signature = std::move(joint_signature);
    return post;
}

void emit_block(ArtifactStreamState& stream, BlockProductionPayload&& payload) {
    const auto begin = std::chrono::steady_clock::now();
    for (auto& site : payload.sites) {
        const auto& focal = payload.loaded.evidence.focal_sites.at(site.focal_index);
        write_site_reads(stream, payload.loaded.evidence, focal);
        write_methyl_calls(stream, payload.loaded.evidence, site.science, focal.site);
        if (site.science.matrix.alt_row_indices.size() >= m1::kMinimumReads) {
            stream.bernoulli.write_frame(focal.site.dataset_order, focal.site.site_order, site.science.m1.distance);
        }
        write_m1_site(stream, focal.site, site.science);
        const std::size_t pair_begin = stream.pairs.size();
        for (auto& pair : site.cooccurrence.pairs) {
            stream.pairs.push_back(std::move(pair));
        }
        const std::size_t pair_end = stream.pairs.size();
        stream.sites.push_back(make_site_post(focal.site, payload.block_index, site.science, site.cooccurrence,
                                              std::move(site.joint_signature), pair_begin, pair_end));

        switch (site.science.m2.decision.status) {
            case m2::M2Status::kPass:
                ++stream.counters.m2_eligible;
                break;
            case m2::M2Status::kFail:
                ++stream.counters.m2_evaluable_ineligible;
                break;
            case m2::M2Status::kNotEvaluable:
                if (site.science.m2.decision.reason == m2::M2Reason::kGroupCountExceedsPlanningMaximum) {
                    ++stream.counters.m2_group_count_gt10;
                } else if (site.science.m2.decision.reason == m2::M2Reason::kAxisIndeterminate) {
                    ++stream.counters.m2_axis_indeterminate;
                } else {
                    throw std::runtime_error("NOT_EVALUABLE M2 decision has unsupported reason");
                }
                break;
            case m2::M2Status::kNotRun:
                break;
        }
    }
    stream.emit_seconds += elapsed_seconds(begin);
}

[[nodiscard]] std::string exact_status(const PairInference& pair) {
    if (!pair.endpoint_a.testable) {
        return "NOT_IDENTIFIABLE_ENDPOINT_A_NOT_TESTABLE";
    }
    switch (pair.exact.status) {
        case cooccurrence::ExactStateStatus::kExactEnumerated:
            return "EXACT_IDENTIFIABLE";
        case cooccurrence::ExactStateStatus::kNotIdentifiableStateSpaceLimit:
            return "NOT_IDENTIFIABLE_STATE_SPACE_LIMIT";
        case cooccurrence::ExactStateStatus::kNotIdentifiableDegenerateTable:
            return "NOT_IDENTIFIABLE_ENDPOINT_A_NOT_TESTABLE";
    }
    throw std::logic_error("unknown endpoint-A exact status");
}

[[nodiscard]] std::uint64_t exact_state_count(const PairInference& pair) {
    if (pair.exact.state_space_size.has_value()) {
        return *pair.exact.state_space_size;
    }
    if (pair.exact.state_space_lower_bound.has_value()) {
        return *pair.exact.state_space_lower_bound;
    }
    return 0U;
}

[[nodiscard]] std::string relation_models_json(const std::vector<cooccurrence::RelationModel>& models) {
    std::vector<std::string> values;
    values.reserve(models.size());
    for (const auto model : models) {
        values.emplace_back(cooccurrence::to_string(model));
    }
    return json_string_array(values);
}

[[nodiscard]] std::vector<std::string> pair_row(const PairInference& pair) {
    const std::string table = group_table_json(pair.endpoint_a.table);
    const auto& cells = pair.endpoint_b.counts.row_major_cells();
    const bool in_fdr_family = pair.family_status == cooccurrence::PairFamilyStatus::kEligibleM2ExactFamily &&
                               pair.exact.identifiable && pair.exact.p_value.has_value();
    const std::string fdr_id = in_fdr_family ? "GLOBAL_M2_ELIGIBLE_ENDPOINT_A_EXACT_V1" : ".";
    const std::string fdr_size = in_fdr_family ? std::to_string(pair.fdr_family_size) : ".";
    const std::string compatible = relation_models_json(pair.endpoint_b.compatible_models);
    std::vector<std::string> row{std::to_string(pair.focal.dataset_order),
                                 pair.focal.dataset_id,
                                 std::to_string(pair.focal.site_order),
                                 pair.focal.contig.value(),
                                 std::to_string(pair.focal.position.value()),
                                 std::string(1, pair.focal.reference),
                                 std::string(1, pair.focal.alternate),
                                 std::to_string(pair.partner.site_order),
                                 std::to_string(pair.partner.position.value()),
                                 std::string(1, pair.partner.reference),
                                 std::string(1, pair.partner.alternate),
                                 std::to_string(pair.distance_bp),
                                 std::to_string(pair.endpoint_a.table.size()),
                                 table,
                                 hash_or_throw(table),
                                 std::to_string(pair.endpoint_a.n_informative),
                                 std::to_string(pair.endpoint_a.minimum_group_n),
                                 std::to_string(pair.endpoint_a.ref_n),
                                 std::to_string(pair.endpoint_a.alt_n),
                                 std::to_string(exact_state_count(pair)),
                                 exact_status(pair),
                                 std::string(cooccurrence::to_string(pair.family_status)),
                                 optional_double(pair.exact.p_value),
                                 fdr_id,
                                 fdr_size,
                                 optional_double(pair.q_global_bh),
                                 optional_double(pair.q_global_by),
                                 optional_double(pair.endpoint_a.cramers_v),
                                 optional_double(pair.endpoint_a.delta_alt_fraction),
                                 boolean(pair.effect_gate_pass),
                                 boolean(pair.exact_bh_discovery),
                                 boolean(pair.exact_by_discovery),
                                 std::to_string(pair.conditional.permutations),
                                 pair.conditional.permutable ? std::to_string(pair.conditional.exceedance) : ".",
                                 optional_double(pair.conditional.p_value),
                                 std::string(cooccurrence::to_string(pair.conditional.status)),
                                 boolean(pair.formal_pair_by_confirmed),
                                 std::string(cooccurrence::to_string(pair.callability_status)),
                                 optional_double(pair.callability_q_global_by),
                                 optional_double(pair.callability.summary.cramers_v),
                                 std::to_string(pair.pair_read_count),
                                 std::to_string(pair.endpoint_b.called_ra_depth)};
    row.reserve(kCooccurrencePairsHeader.size());
    for (const std::uint64_t cell : cells) {
        row.push_back(std::to_string(cell));
    }
    row.insert(row.end(), {artifact::canonical_float64(pair.endpoint_b.error_ceiling),
                           artifact::canonical_float64(pair.endpoint_b.familywise_confidence),
                           optional_double(pair.endpoint_b.focal_ancestor.p_exact_greater),
                           optional_double(pair.endpoint_b.focal_ancestor.upper_bound),
                           std::string(cooccurrence::to_string(pair.endpoint_b.focal_ancestor.status)),
                           optional_double(pair.endpoint_b.partner_ancestor.p_exact_greater),
                           optional_double(pair.endpoint_b.partner_ancestor.upper_bound),
                           std::string(cooccurrence::to_string(pair.endpoint_b.partner_ancestor.status)),
                           optional_double(pair.endpoint_b.branching.p_exact_greater),
                           optional_double(pair.endpoint_b.branching.upper_bound),
                           std::string(cooccurrence::to_string(pair.endpoint_b.branching.status)),
                           boolean(pair.endpoint_b.complete_four_state_testable),
                           std::string(cooccurrence::to_string(pair.endpoint_b.compatibility)), compatible,
                           std::to_string(pair.endpoint_b.compatible_models.size()),
                           "COMPATIBILITY_ONLY_NOT_ANCESTRY_OR_TEMPORAL_ORDER"});
    if (row.size() != kCooccurrencePairsHeader.size()) {
        throw std::logic_error("co-occurrence pair row/header cardinality drift");
    }
    return row;
}

[[nodiscard]] std::uint32_t m2_precedence_rank(m2::M2Reason reason) {
    switch (reason) {
        case m2::M2Reason::kM1NotFlagged:
            return 0U;
        case m2::M2Reason::kGroupCountExceedsPlanningMaximum:
            return 1U;
        case m2::M2Reason::kAxisIndeterminate:
            return 2U;
        case m2::M2Reason::kHpAxisConfound:
            return 3U;
        case m2::M2Reason::kTechnicalAxisConfound:
            return 4U;
        case m2::M2Reason::kNotPhaseAnchoredRobust:
            return 5U;
        case m2::M2Reason::kAllMeasuredAxesDeterminateNoAlignedConfound:
            return 6U;
    }
    throw std::logic_error("unknown M2 precedence reason");
}

void finalize_site_pair_counts(const SitePostRecord& site, const std::vector<PairInference>& pairs,
                               std::uint64_t& exact, std::uint64_t& bh, std::uint64_t& by, std::uint64_t& formal) {
    exact = 0U;
    bh = 0U;
    by = 0U;
    formal = 0U;
    for (std::size_t index = site.pair_begin; index < site.pair_end; ++index) {
        const auto& pair = pairs.at(index);
        exact += pair.exact.identifiable;
        bh += pair.exact_bh_discovery;
        by += pair.exact_by_discovery;
        formal += pair.formal_pair_by_confirmed;
    }
}

[[nodiscard]] std::string stable_site_key(const VariantSite& site) {
    return std::to_string(site.dataset_order) + "|" + site.dataset_id + "|" + site.contig.value() + "|" +
           std::to_string(site.site_order) + "|" + std::to_string(site.position.value()) + "|" +
           std::string(1, site.reference) + "|" + std::string(1, site.alternate);
}

void finalize_joint_topology_gates(std::vector<SitePostRecord>& sites, const std::vector<PairInference>& pairs) {
    std::vector<cooccurrence::JointGlobalFdrInput> fdr_inputs;
    fdr_inputs.reserve(sites.size());
    for (const auto& site : sites) {
        fdr_inputs.push_back({stable_site_key(site.site), site.m2_decision.eligible,
                              site.m2_decision.eligible && site.joint_signature.association.testable,
                              site.m2_decision.eligible && site.joint_signature.conditional.permutable,
                              site.m2_decision.eligible ? site.joint_signature.conditional.permutations : 0U,
                              site.m2_decision.eligible ? site.joint_signature.conditional.p_value : std::nullopt});
    }
    auto global = cooccurrence::apply_joint_signature_global_fdr(fdr_inputs);
    if (!global.ok() || !global.value.has_value()) {
        throw std::runtime_error("joint-signature global FDR failed: " + global.detail);
    }
    if (!global.value->all_permutation_evidence_valid) {
        throw std::runtime_error("joint-signature global FDR rejected malformed permutation evidence");
    }
    std::map<std::string, cooccurrence::JointGlobalFdrRow> rows;
    for (auto& row : global.value->rows) {
        rows.emplace(row.stable_site_key, std::move(row));
    }

    for (auto& site : sites) {
        const std::string key = stable_site_key(site.site);
        const auto fdr = rows.find(key);
        if (fdr == rows.end()) {
            throw std::runtime_error("joint-signature site lacks global FDR row");
        }
        cooccurrence::JointTopologyGateInput input;
        input.stable_site_key = key;
        input.m2_screen_eligible = site.m2_decision.eligible;
        input.global_fdr = fdr->second;
        if (site.m2_decision.eligible) {
            input.partner_evidence.reserve(site.pair_end - site.pair_begin);
            for (std::size_t index = site.pair_begin; index < site.pair_end; ++index) {
                const auto& pair = pairs.at(index);
                input.partner_evidence.push_back(
                    {pair.partner.position.value(), pair.partner.site_order, pair.formal_pair_by_confirmed});
            }
        }
        auto finalized = cooccurrence::finalize_joint_signature_topology_gate(site.joint_signature, input);
        if (!finalized.ok()) {
            throw std::runtime_error("joint-signature topology gate failed: " + finalized.detail);
        }
        site.joint_pass = site.joint_signature.passed;
        site.selected_partner_site_orders = site.joint_signature.selected_partner_site_orders;
    }
}

[[nodiscard]] std::vector<std::string> cooccurrence_site_row(const SitePostRecord& site,
                                                             const std::vector<PairInference>& pairs) {
    std::uint64_t exact = 0;
    std::uint64_t bh = 0;
    std::uint64_t by = 0;
    std::uint64_t formal = 0;
    finalize_site_pair_counts(site, pairs, exact, bh, by, formal);
    static_cast<void>(formal);
    const bool joint_pass = site.joint_pass;
    return {
        std::to_string(site.site.dataset_order),
        site.site.dataset_id,
        std::to_string(site.site.site_order),
        site.site.contig.value(),
        std::to_string(site.site.position.value()),
        std::string(1, site.site.reference),
        std::string(1, site.site.alternate),
        site.m1_group_count.has_value() ? std::to_string(*site.m1_group_count) : ".",
        std::string(m2::to_string(site.m2_decision.status)),
        std::string(m2::to_string(site.m2_decision.reason)),
        std::to_string(m2_precedence_rank(site.m2_decision.reason)),
        "M2_GRID_499_MIN5_MAX10_POWER080_V030_ETA014_V1",
        std::to_string(m2::kPermutations),
        site.minimum_group_size.has_value() ? std::to_string(*site.minimum_group_size) : ".",
        optional_double(site.categorical_v),
        optional_double(site.continuous_eta2),
        optional_double(site.minimum_power),
        std::to_string(site.pair_end - site.pair_begin),
        std::to_string(exact),
        std::to_string(bh),
        std::to_string(by),
        joint_pass ? "PASS" : "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE",
        joint_pass ? json_uint_array(site.selected_partner_site_orders) : ".",
        joint_pass ? site.joint_signature.complete_readset_sha256 : ".",
    };
}

[[nodiscard]] TopologyLocusIdentity topology_locus(const VariantSite& site) {
    return TopologyLocusIdentity{site.dataset_order, site.dataset_id, site.contig,   site.site_order,
                                 site.position,      site.reference,  site.alternate};
}

[[nodiscard]] std::vector<TopologySiteCandidate> topology_candidates(const std::vector<SitePostRecord>& sites,
                                                                     const std::vector<PairInference>& pairs) {
    std::vector<TopologySiteCandidate> output;
    output.reserve(sites.size());
    for (const auto& site : sites) {
        TopologySiteCandidate candidate(topology_locus(site.site));
        candidate.m2_eligible = site.m2_decision.eligible;
        if (site.joint_pass) {
            candidate.joint_signature_state = JointSignatureState::kPass;
            candidate.joint_complete_readset_sha256 = site.joint_signature.complete_readset_sha256;
            for (const std::uint64_t order : site.selected_partner_site_orders) {
                const auto pair = std::find_if(pairs.begin() + static_cast<std::ptrdiff_t>(site.pair_begin),
                                               pairs.begin() + static_cast<std::ptrdiff_t>(site.pair_end),
                                               [order](const PairInference& candidate_pair) {
                                                   return candidate_pair.partner.site_order == order;
                                               });
                if (pair == pairs.begin() + static_cast<std::ptrdiff_t>(site.pair_end)) {
                    throw std::runtime_error("joint signature selected an absent partner");
                }
                candidate.selected_partners.push_back(topology_locus(pair->partner));
            }
        } else {
            candidate.joint_signature_state =
                site.m2_decision.eligible ? JointSignatureState::kFail : JointSignatureState::kNotEvaluated;
        }
        output.push_back(std::move(candidate));
    }
    return output;
}

struct TopologyArtifactResult {
    DatasetArtifactWriteReceipt receipt;
    TopologyMembershipFunnel funnel;
};

[[nodiscard]] TopologyArtifactResult write_topology_units(const DatasetReadPaths& paths, const VariantSiteSet& variants,
                                                          const DatasetExecutionPlan& execution_plan,
                                                          const std::filesystem::path& root, const std::string& run_id,
                                                          int compression_threads,
                                                          const std::vector<SitePostRecord>& sites,
                                                          const std::vector<PairInference>& pairs,
                                                          DatasetProductionCounters& counters) {
    auto membership = build_topology_membership_plan(topology_candidates(sites, pairs));
    if (!membership.ok() || !membership.value.has_value()) {
        throw std::runtime_error("topology membership failed: " + membership.detail);
    }

    IndexedBgzfJsonlWriter writer(root / "topology_units.jsonl.bgz", root / "indexes/topology_units.site_index.tsv.bgz",
                                  "topology_units", "longlineage.topology_unit", "2.0.0", run_id, compression_threads);

    std::map<std::uint64_t, std::size_t> block_by_focal;
    for (const auto& site : sites) {
        if (!block_by_focal.emplace(site.site.site_order, site.block_index).second) {
            throw std::runtime_error("duplicate focal site while mapping topology blocks");
        }
    }

    std::unique_ptr<DatasetBlockReader> reader;
    std::optional<std::size_t> loaded_block_index;
    DatasetBlockLoad loaded;
    if (!membership.value->units.empty()) {
        auto opened = DatasetBlockReader::open(paths);
        if (!opened.ok() || !opened.value.has_value()) {
            throw std::runtime_error("topology second-pass input open failed: " + opened.detail);
        }
        reader = std::move(*opened.value);
    }

    for (const auto& unit : membership.value->units) {
        const auto block = block_by_focal.find(unit.focal.site_order);
        if (block == block_by_focal.end() || block->second >= execution_plan.blocks.size()) {
            throw std::runtime_error("topology unit focal lacks an execution block");
        }
        if (!loaded_block_index.has_value() || *loaded_block_index != block->second) {
            auto next = reader->read(execution_plan.blocks[block->second], variants);
            if (!next.ok() || !next.value.has_value()) {
                throw std::runtime_error("topology second-pass block read failed: " + next.detail);
            }
            loaded = std::move(*next.value);
            loaded_block_index = block->second;
        }

        auto adapted = build_topology_read_patterns(loaded.evidence, unit);
        if (!adapted.ok() || !adapted.value.has_value()) {
            throw std::runtime_error("topology read-pattern adaptation failed: " + adapted.detail);
        }
        const auto evidence = solver::build_topology_evidence(adapted.value->observations);
        const auto topology = solver::solve_topology_exact(evidence.structural_problem);
        const auto ranking = solver::rank_complete_vertex_sets(evidence, topology.structural);
        const solver::ParentEdgeEndpointRequest edge_request{};
        const auto edge_endpoint = solver::evaluate_ranked_parent_edges(evidence.active_loci.size(),
                                                                        topology.structural, ranking, edge_request);
        const solver::TopologyRecordIdentity identity{unit.focal.dataset_order, unit.focal.dataset_id, unit.unit_order,
                                                      unit.unit_id};
        const auto record = solver::serialize_topology_unit_v2(identity, evidence, topology, ranking, edge_endpoint);
        if (!record.valid || record.canonical_json.empty()) {
            throw std::runtime_error("topology serialization failed: " + record.message);
        }
        writer.write_record(unit.focal.dataset_order, unit.unit_order, record.canonical_json);

        ++counters.topology_primary_hp_units;
        ++counters.topology_regions;
        if (topology.structural.family_state == solver::ExactFamilyState::kFamilyComplete) {
            ++counters.topology_fully_complete_regions;
        } else {
            ++counters.topology_incomplete_regions;
            if (ranking.state == solver::VertexSetRankingState::kRankingComplete) {
                ++counters.topology_incomplete_units_with_winner;
            }
        }
    }

    TopologyArtifactResult result;
    result.receipt = writer.close();
    result.funnel = membership.value->funnel;
    return result;
}

[[nodiscard]] std::string summary_json(const std::string& run_id, const std::string& dataset_id,
                                       const DatasetProductionOptions& options,
                                       const DatasetProductionCounters& counts) {
    std::string output =
        "{\"schema_name\":\"longlineage.summary\","
        "\"schema_version\":\"2.0.0\",\"run_id\":";
    output.append(artifact::canonical_json_quote(run_id));
    output.append(",\"scope\":{\"task_type\":");
    output.append(artifact::canonical_json_quote(options.task_type));
    output.append(",\"completeness\":");
    output.append(artifact::canonical_json_quote(options.completeness));
    output.append(",\"dataset_count\":1,\"dataset_ids\":[");
    output.append(artifact::canonical_json_quote(dataset_id));
    output.append("],\"site_population\":");
    output.append(artifact::canonical_json_quote(options.site_population));
    output.append(",\"m1_representation\":");
    output.append(artifact::canonical_json_quote(options.representation == M1Representation::kRawBinary32Point
                                                     ? "RAW_BINARY32_POINT"
                                                     : "HISTORICAL_OBSERVED_ROUND6_NULL_ROUND4"));
    output.append("},\"counts\":{");
    const std::vector<std::pair<std::string, std::uint64_t>> fields{
        {"site_keys", counts.site_keys},
        {"site_keys_missing", 0},
        {"site_keys_extra", 0},
        {"site_keys_duplicate", 0},
        {"m1_evaluable", counts.m1_evaluable},
        {"m1_insufficient_alt_reads", counts.m1_insufficient_alt_reads},
        {"m1_incomplete_distance", counts.m1_incomplete_distance},
        {"m1_stable_assignments", counts.m1_stable_assignments},
        {"latest_tag_exact_joins", counts.latest_tag_exact_joins},
        {"latest_tag_missing", 0},
        {"latest_tag_conflict", 0},
        {"latest_tag_multimatch", 0},
        {"m2_eligible", counts.m2_eligible},
        {"m2_evaluable_ineligible", counts.m2_evaluable_ineligible},
        {"m2_axis_indeterminate", counts.m2_axis_indeterminate},
        {"m2_group_count_gt10", counts.m2_group_count_gt10},
        {"raw_expected", counts.raw_expected},
        {"raw_matched", counts.raw_matched},
        {"raw_rg_only_duplicate_occurrences", counts.raw_rg_only_duplicate_occurrences},
        {"topology_primary_hp_units", counts.topology_primary_hp_units},
        {"topology_regions", counts.topology_regions},
        {"topology_fully_complete_regions", counts.topology_fully_complete_regions},
        {"topology_incomplete_regions", counts.topology_incomplete_regions},
        {"topology_incomplete_units_with_winner", counts.topology_incomplete_units_with_winner},
    };
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        output.append(artifact::canonical_json_quote(fields[index].first));
        output.push_back(':');
        output.append(std::to_string(fields[index].second));
    }
    output.append(
        "},\"phase_status_scope\":"
        "\"RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER\","
        "\"phase_status\":{\"P0\":\"VERIFIED\","
        "\"P1\":\"VERIFIED\",\"P2\":\"VERIFIED\","
        "\"P3\":\"VERIFIED\",\"P4\":\"VERIFIED\","
        "\"P5\":\"VERIFIED\",\"P6\":\"IN_PROGRESS\","
        "\"P7\":\"NOT_STARTED\",\"P8\":\"NOT_STARTED\"}}");
    return output;
}

[[nodiscard]] std::string production_digest(const std::vector<DatasetArtifactWriteReceipt>& artifacts,
                                            const DatasetProductionCounters& counters,
                                            const TopologyMembershipFunnel& funnel) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "longlineage.dataset_production\t1.0.0\n";
    for (const auto& artifact : artifacts) {
        stream << artifact.artifact_id << '\t' << artifact.logical_rows << '\t' << artifact.semantic_sha256 << '\n';
    }
    stream << counters.site_keys << '\t' << counters.site_read_rows << '\t' << counters.methyl_call_rows << '\t'
           << counters.m1_evaluable << '\t' << counters.m1_stable_assignments << '\t' << counters.m2_eligible << '\t'
           << counters.cooccurrence_pairs << '\t' << counters.exact_testable_pairs << '\t'
           << counters.global_by_discoveries << '\t' << counters.formal_pair_by_confirmed << '\t'
           << counters.topology_regions << '\n'
           << funnel.candidates_total << '\t' << funnel.instantiated_units << '\t'
           << funnel.not_instantiated_m2_ineligible << '\t' << funnel.not_instantiated_joint_signature_fail << '\t'
           << funnel.not_instantiated_joint_signature_not_evaluated << '\n';
    return hash_or_throw(stream.str());
}

}  // namespace

ParseResult<DatasetProductionReceipt> produce_dataset_scientific_artifacts(
    const DatasetReadPaths& paths, const VariantSiteSet& variants, const DatasetExecutionPlan& plan,
    const std::filesystem::path& staging_root, std::string run_id, const DatasetProductionOptions& options) {
    const auto total_begin = std::chrono::steady_clock::now();
    try {
        if (run_id.empty() || !staging_root.is_absolute() || options.workers == 0U ||
            options.workers > kMaximumWorkers || options.task_queue_capacity_bytes < kTaskChargeBytes ||
            options.maximum_block_payload_bytes == 0U ||
            options.maximum_block_payload_bytes > options.reorder_capacity_bytes ||
            options.bgzf_compression_threads < 1 || plan.blocks.empty() ||
            plan.focal_site_count != variants.sites.size() || variants.sites.empty() ||
            options.first_block >= plan.blocks.size()) {
            return ParseResult<DatasetProductionReceipt>::failure(
                ParseReason::kUnsupportedValue, "dataset production options, path or plan are invalid");
        }
        const std::size_t remaining = plan.blocks.size() - options.first_block;
        const std::size_t requested = options.block_count == 0U ? remaining : options.block_count;
        if (requested == 0U || requested > remaining) {
            return ParseResult<DatasetProductionReceipt>::failure(
                ParseReason::kMalformedValue, "dataset production block range lies outside the plan");
        }
        const bool complete = options.first_block == 0U && requested == plan.blocks.size();
        if (options.completeness == "FULL" && !complete) {
            return ParseResult<DatasetProductionReceipt>::failure(ParseReason::kUnsupportedValue,
                                                                  "FULL dataset production cannot use a block subset");
        }
        if (variants.sites.front().dataset_id.empty()) {
            return ParseResult<DatasetProductionReceipt>::failure(ParseReason::kMissingRequiredField,
                                                                  "variant dataset identity is empty");
        }
        const std::string dataset_id = variants.sites.front().dataset_id;
        for (const auto& site : variants.sites) {
            if (site.dataset_id != dataset_id) {
                return ParseResult<DatasetProductionReceipt>::failure(ParseReason::kMalformedValue,
                                                                      "dataset production received mixed dataset IDs");
            }
        }

        std::filesystem::create_directories(staging_root);
        std::filesystem::create_directories(staging_root / "indexes");
        ArtifactStreamState stream(staging_root, run_id, options.bgzf_compression_threads);
        DatasetProductionReceipt output;
        output.run_id = run_id;
        output.staging_root = staging_root;
        output.workers = options.workers;
        output.first_block = options.first_block;
        output.block_count = requested;
        output.complete_plan = complete;

        const auto open_begin = std::chrono::steady_clock::now();
        std::vector<std::unique_ptr<DatasetBlockReader>> readers;
        readers.reserve(options.workers);
        for (std::size_t worker = 0; worker < options.workers; ++worker) {
            auto reader = DatasetBlockReader::open(paths);
            if (!reader.ok() || !reader.value.has_value()) {
                return ParseResult<DatasetProductionReceipt>::failure(
                    reader.reason,
                    "producer worker " + std::to_string(worker) + " input open failed: " + reader.detail);
            }
            readers.push_back(std::move(*reader.value));
        }
        output.timing.handle_open_seconds = elapsed_seconds(open_begin);

        runtime::ByteBoundedReorderSink<BlockProductionPayload> sink(
            options.reorder_capacity_bytes, options.maximum_block_payload_bytes,
            [&](std::uint64_t sequence, BlockProductionPayload&& payload) {
                if (payload.block_index != options.first_block + sequence) {
                    throw std::runtime_error("producer reorder block identity drift");
                }
                emit_block(stream, std::move(payload));
            });
        runtime::OrderedThreadPool<std::size_t> pool(
            options.workers, options.task_queue_capacity_bytes,
            [&](const std::string& reason) { sink.cancel("producer worker terminated: " + reason); });
        // Both the compute pool and the one active multi-threaded BGZF writer
        // are live here. Observe synchronously so measurement does not add a
        // thread and violate the manifest's process-wide workload ceiling.
        output.peak_process_threads = process_thread_count();

        const auto execution_begin = std::chrono::steady_clock::now();
        std::vector<double> queue_wait_seconds(requested, 0.0);
        std::vector<double> reorder_wait_seconds(requested, 0.0);
        std::vector<double> task_latency_seconds(requested, 0.0);
        for (std::size_t local = 0; local < requested; ++local) {
            const std::size_t block_index = options.first_block + local;
            const auto submitted_at = std::chrono::steady_clock::now();
            const auto submitted = pool.submit_indexed(kTaskChargeBytes, [local, block_index, &readers, &plan,
                                                                          &variants, &options, &sink, submitted_at,
                                                                          &queue_wait_seconds, &reorder_wait_seconds,
                                                                          &task_latency_seconds](std::size_t worker) {
                const auto task_begin = std::chrono::steady_clock::now();
                queue_wait_seconds[local] = std::chrono::duration<double>(task_begin - submitted_at).count();
                auto loaded = readers[worker]->read(plan.blocks[block_index], variants);
                if (!loaded.ok() || !loaded.value.has_value()) {
                    throw std::runtime_error("producer block read failed: " + loaded.detail);
                }
                BlockProductionPayload payload;
                payload.block_index = block_index;
                payload.loaded = std::move(*loaded.value);
                payload.sites.reserve(payload.loaded.evidence.focal_sites.size());
                for (std::size_t focal_index = 0; focal_index < payload.loaded.evidence.focal_sites.size();
                     ++focal_index) {
                    const auto& focal = payload.loaded.evidence.focal_sites[focal_index];
                    SiteScienceOptions science_options;
                    science_options.m1_representation = options.representation;
                    science_options.m1_options.workers = 1;
                    auto science = run_site_science(payload.loaded.evidence, focal, science_options);
                    if (!science.ok() || !science.value.has_value()) {
                        throw std::runtime_error("producer site science failed: " + science.detail);
                    }
                    auto pairs = cooccurrence::build_site_cooccurrence(payload.loaded.evidence, focal, *science.value);
                    if (!pairs.ok() || !pairs.value.has_value()) {
                        throw std::runtime_error("producer co-occurrence failed: " + pairs.detail);
                    }
                    SiteBlockProduct product;
                    product.focal_index = focal_index;
                    product.science = std::move(*science.value);
                    product.cooccurrence = std::move(*pairs.value);
                    product.joint_signature = build_joint_signature_evidence(payload.loaded.evidence, focal,
                                                                             product.science, product.cooccurrence);
                    payload.sites.push_back(std::move(product));
                }
                payload.charge_bytes = estimate_payload_bytes(payload);
                const auto reorder_begin = std::chrono::steady_clock::now();
                const auto published = sink.publish(local, std::move(payload));
                reorder_wait_seconds[local] = elapsed_seconds(reorder_begin);
                task_latency_seconds[local] = elapsed_seconds(task_begin);
                if (published.status != runtime::ReorderSinkStatus::kSuccess) {
                    throw std::runtime_error("producer reorder publish failed: " + published.message);
                }
                return block_index;
            });
            if (submitted.status != runtime::PoolStatus::kSuccess) {
                pool.cancel("producer submission failed");
                sink.cancel("producer submission failed");
                static_cast<void>(pool.finish());
                return ParseResult<DatasetProductionReceipt>::failure(
                    ParseReason::kIoError, "producer task submission failed: " + submitted.message);
            }
        }
        auto batch = pool.finish();
        if (batch.status != runtime::PoolStatus::kSuccess || batch.ordered_results.size() != requested) {
            sink.cancel("producer worker batch failed");
            return ParseResult<DatasetProductionReceipt>::failure(ParseReason::kIoError,
                                                                  "producer worker batch failed: " + batch.message);
        }
        const auto closed = sink.close(static_cast<std::uint64_t>(requested));
        if (closed.status != runtime::ReorderSinkStatus::kSuccess) {
            return ParseResult<DatasetProductionReceipt>::failure(ParseReason::kIoError,
                                                                  "producer reorder close failed: " + closed.message);
        }
        output.timing.block_execution_seconds = elapsed_seconds(execution_begin);
        output.timing.artifact_stream_seconds = stream.emit_seconds;
        output.timing.peak_reorder_bytes = sink.snapshot().peak_retained_bytes;
        output.timing.queue_wait_seconds = std::accumulate(queue_wait_seconds.begin(), queue_wait_seconds.end(), 0.0);
        output.timing.reorder_wait_seconds =
            std::accumulate(reorder_wait_seconds.begin(), reorder_wait_seconds.end(), 0.0);
        output.timing.task_latency_p50_seconds = nearest_rank_percentile(task_latency_seconds, 0.50);
        output.timing.task_latency_p95_seconds = nearest_rank_percentile(task_latency_seconds, 0.95);
        output.timing.task_latency_p99_seconds = nearest_rank_percentile(task_latency_seconds, 0.99);
        output.timing.task_latency_max_seconds =
            *std::max_element(task_latency_seconds.begin(), task_latency_seconds.end());

        if (stream.counters.m1_stable_assignments > stream.counters.m1_evaluable ||
            !m2_partition_conserves(stream.counters)) {
            return ParseResult<DatasetProductionReceipt>::failure(ParseReason::kMalformedValue,
                                                                  "dataset production M2 stable-site partition failed");
        }

        const auto finalization_begin = std::chrono::steady_clock::now();
        output.artifacts.push_back(stream.site_reads.close());
        output.artifacts.push_back(stream.methyl_calls.close());
        output.artifacts.push_back(stream.bernoulli.close());
        output.artifacts.push_back(stream.m1_sites.close());
        output.artifacts.push_back(stream.m1_assignments.close());

        const auto global_begin = std::chrono::steady_clock::now();
        auto fdr = cooccurrence::finalize_global_pair_families(stream.pairs);
        if (!fdr.ok()) {
            return ParseResult<DatasetProductionReceipt>::failure(
                fdr.reason, "global pair-family finalization failed: " + fdr.detail);
        }
        auto conditional = cooccurrence::run_conditional_pair_sensitivity(stream.pairs);
        if (!conditional.ok()) {
            return ParseResult<DatasetProductionReceipt>::failure(
                conditional.reason, "conditional pair sensitivity failed: " + conditional.detail);
        }
        finalize_joint_topology_gates(stream.sites, stream.pairs);
        output.timing.global_pair_seconds = elapsed_seconds(global_begin);

        // HTSlib multi-threaded BGZF requires a queue-draining flush at every
        // indexed group boundary before its virtual offset is stable. Reserve
        // that cost for the dominant methyl stream; the smaller final
        // artifacts retain precise per-row offsets with one writer thread.
        constexpr int kOffsetStableWriterThreads = 1;
        IndexedBgzfTsvWriter pair_writer(staging_root / "cooccurrence_pairs.tsv.bgz",
                                         staging_root / "indexes/cooccurrence_pairs.site_index.tsv.bgz",
                                         "cooccurrence_pairs", "longlineage.cooccurrence_pairs", "1.0.1", run_id,
                                         kCooccurrencePairsHeader, kOffsetStableWriterThreads);
        for (const auto& pair : stream.pairs) {
            pair_writer.write_row(pair.focal.dataset_order, pair.focal.site_order, pair_row(pair));
            ++stream.counters.cooccurrence_pairs;
            stream.counters.exact_testable_pairs += pair.exact.identifiable;
            stream.counters.global_bh_discoveries += pair.exact_bh_discovery;
            stream.counters.global_by_discoveries += pair.exact_by_discovery;
            stream.counters.formal_pair_by_confirmed += pair.formal_pair_by_confirmed;
        }
        output.artifacts.push_back(pair_writer.close());

        IndexedBgzfTsvWriter site_writer(staging_root / "cooccurrence_sites.tsv.bgz",
                                         staging_root / "indexes/cooccurrence_sites.site_index.tsv.bgz",
                                         "cooccurrence_sites", "longlineage.cooccurrence_sites", "1.0.0", run_id,
                                         kCooccurrenceSitesHeader, kOffsetStableWriterThreads);
        for (const auto& site : stream.sites) {
            site_writer.write_row(site.site.dataset_order, site.site.site_order,
                                  cooccurrence_site_row(site, stream.pairs));
        }
        output.artifacts.push_back(site_writer.close());

        const auto topology_begin = std::chrono::steady_clock::now();
        auto topology = write_topology_units(paths, variants, plan, staging_root, run_id, kOffsetStableWriterThreads,
                                             stream.sites, stream.pairs, stream.counters);
        output.timing.topology_second_pass_seconds = elapsed_seconds(topology_begin);
        output.topology_funnel = topology.funnel;
        output.artifacts.push_back(std::move(topology.receipt));

        output.counters = stream.counters;
        output.artifacts.push_back(artifact::write_canonical_json_artifact(
            staging_root / "summary.json", "summary", "longlineage.summary", "2.0.0",
            summary_json(run_id, dataset_id, options, output.counters)));
        output.timing.finalization_seconds = elapsed_seconds(finalization_begin);
        output.semantic_sha256 = production_digest(output.artifacts, output.counters, output.topology_funnel);
        output.timing.total_seconds = elapsed_seconds(total_begin);

        if (output.counters.site_keys !=
                std::accumulate(plan.blocks.begin() + static_cast<std::ptrdiff_t>(options.first_block),
                                plan.blocks.begin() + static_cast<std::ptrdiff_t>(options.first_block + requested),
                                std::uint64_t{0},
                                [](std::uint64_t total, const PlannedDatasetBlock& block) {
                                    return total + block.alignment.focal_sites.size();
                                }) ||
            output.counters.m1_evaluable + output.counters.m1_insufficient_alt_reads +
                    output.counters.m1_incomplete_distance !=
                output.counters.site_keys) {
            return ParseResult<DatasetProductionReceipt>::failure(ParseReason::kMalformedValue,
                                                                  "dataset production site/M1 conservation failed");
        }
        return ParseResult<DatasetProductionReceipt>::success(std::move(output));
    } catch (const std::exception& error) {
        return ParseResult<DatasetProductionReceipt>::failure(
            ParseReason::kIoError, std::string("dataset production failed: ") + error.what());
    }
}

}  // namespace longlineage::pipeline
