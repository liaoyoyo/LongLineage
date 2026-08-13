// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/cooccurrence/joint_signature.hpp"

namespace {

using longlineage::ParseReason;
using longlineage::cooccurrence::JointAssociationStatus;
using longlineage::cooccurrence::JointConditionalStatus;
using longlineage::cooccurrence::JointCoreRead;
using longlineage::cooccurrence::JointGlobalFdrInput;
using longlineage::cooccurrence::JointGlobalFdrResult;
using longlineage::cooccurrence::JointGlobalFdrStatus;
using longlineage::cooccurrence::JointMarkerCall;
using longlineage::cooccurrence::JointMarkerObservation;
using longlineage::cooccurrence::JointPartnerCandidate;
using longlineage::cooccurrence::JointPartnerFormalEvidence;
using longlineage::cooccurrence::JointSignatureResult;
using longlineage::cooccurrence::JointTopologyGateInput;
using longlineage::cooccurrence::JointTopologyGateStatus;

struct Counts {
    std::size_t oracle_cases{0};
    std::size_t top_partner_cases{0};
    std::size_t fdr_rows{0};
    std::size_t topology_gate_cases{0};
    std::size_t invariance_cases{0};
    std::size_t negative_cases{0};
};

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_near(double observed, double expected, double tolerance, const std::string& message) {
    if (!std::isfinite(observed) || !std::isfinite(expected) || std::abs(observed - expected) > tolerance) {
        throw std::runtime_error(message + ": observed=" + std::to_string(observed) +
                                 " expected=" + std::to_string(expected));
    }
}

std::string read_key(std::size_t index) {
    std::ostringstream stream;
    stream << std::hex << std::setw(64) << std::setfill('0') << index;
    return stream.str();
}

JointCoreRead make_read(std::size_t index, std::string label, std::string hp_family, std::string phase_set,
                        std::string strand, std::vector<std::pair<std::uint64_t, JointMarkerCall>> calls) {
    JointCoreRead read;
    read.stable_key = read_key(index);
    read.label = std::move(label);
    read.hp_family = std::move(hp_family);
    read.phase_set = std::move(phase_set);
    read.strand = std::move(strand);
    for (const auto& [position, call] : calls) {
        read.marker_calls.push_back({position, call});
    }
    return read;
}

std::vector<JointPartnerCandidate> two_partners() { return {{1100, 'A', 'G', 24}, {1120, 'C', 'T', 24}}; }

std::vector<JointCoreRead> positive_reads(bool include_incomplete = true) {
    std::vector<JointCoreRead> reads;
    for (std::size_t index = 0; index < 24; ++index) {
        const bool first_group = index < 12;
        const JointMarkerCall call = first_group ? JointMarkerCall::kReference : JointMarkerCall::kAlternate;
        reads.push_back(make_read(index, first_group ? "g1" : "g2", index % 2 == 0 ? "HP1-side" : "HP2-side", "101",
                                  "+", {{1100, call}, {1120, call}}));
    }
    if (include_incomplete) {
        reads.push_back(make_read(24, "g1", "HP1-side", "101", "+",
                                  {{1100, JointMarkerCall::kReference}, {1120, JointMarkerCall::kNoCall}}));
    }
    return reads;
}

JointSignatureResult require_result(const longlineage::ParseResult<JointSignatureResult>& result,
                                    std::string_view context) {
    check(result.ok() && result.value.has_value(), std::string(context) + " unexpectedly failed: " + result.detail);
    return *result.value;
}

void test_top_partner_ranking(Counts& counts) {
    const std::vector<JointPartnerCandidate> candidates = {
        {90, 'A', 'C', 10},
        {110, 'G', 'T', 10},
        {95, 'C', 'G', 11},
        {105, 'T', 'A', 11},
    };
    auto ranked = longlineage::cooccurrence::select_top_joint_partners(100, candidates, 3);
    check(ranked.ok() && ranked.value.has_value(), "top-partner ranking failed");
    check(ranked.value->size() == 3 && (*ranked.value)[0].position1 == 95 && (*ranked.value)[1].position1 == 105 &&
              (*ranked.value)[2].position1 == 90,
          "top-partner coverage/distance/position precedence drift");
    ++counts.top_partner_cases;

    std::vector<JointPartnerCandidate> reversed = candidates;
    std::reverse(reversed.begin(), reversed.end());
    const auto repeated = longlineage::cooccurrence::select_top_joint_partners(100, reversed, 3);
    check(repeated.ok() && repeated.value.has_value(), "reversed top-partner ranking failed");
    for (std::size_t index = 0; index < ranked.value->size(); ++index) {
        check((*ranked.value)[index].position1 == (*repeated.value)[index].position1,
              "top-partner input-order invariance drift");
    }
    ++counts.invariance_cases;
}

void test_historical_positive_oracle(Counts& counts, std::string& positive_trace, std::string& complete_readset_trace) {
    const auto candidates = two_partners();
    const auto reads = positive_reads();
    const JointSignatureResult observed = require_result(
        longlineage::cooccurrence::evaluate_joint_signature(1000, candidates, reads, 29), "historical positive oracle");
    check(observed.top_partners.size() == 2 && observed.top_partners[0].position1 == 1100 &&
              observed.top_partners[1].position1 == 1120,
          "historical positive top markers drift");
    check(observed.n_core_reads == 25 && observed.n_complete_reads == 24,
          "complete-read filtering no longer excludes O/X");
    check(observed.association.testable && observed.association.status == JointAssociationStatus::kOk &&
              observed.association.groups == std::vector<std::string>({"g1", "g2"}) &&
              observed.association.categories == std::vector<std::string>({"AA", "RR"}) &&
              observed.association.table == std::vector<std::vector<std::uint64_t>>({{0, 12}, {12, 0}}),
          "historical positive categorical table drift");
    check(observed.association.cramers_v.has_value(), "historical positive Cramer's V missing");
    check_near(*observed.association.cramers_v, 1.0, 1e-15, "historical positive Cramer's V drift");
    check(observed.conditional.status == JointConditionalStatus::kPermutable && observed.conditional.permutable &&
              observed.conditional.permutations == 999 && observed.conditional.exceedance == 0 &&
              observed.conditional.strata == 2 && observed.conditional.exchangeable_strata == 2 &&
              observed.conditional.informative_exchangeable_strata == 2 && observed.conditional.p_value.has_value(),
          "historical positive conditional permutation state drift");
    check_near(*observed.conditional.p_value, 0.001, 1e-15, "historical positive conditional permutation p drift");
    check(observed.conditional_sensitivity_pass && !observed.postselection_fdr_calibrated &&
              observed.effect_supported_positions == std::vector<std::uint64_t>({1100, 1120}),
          "same-complete-read effect gate drift");
    check(observed.marker_support.size() == 2, "positive marker support cardinality drift");
    for (const auto& marker : observed.marker_support) {
        check(marker.testable && marker.effect_gate_pass && marker.cramers_v.has_value() &&
                  marker.delta_alt_fraction.has_value(),
              "positive same-readset marker support became non-testable");
        check_near(*marker.cramers_v, 1.0, 1e-15, "positive marker Cramer's V drift");
        check_near(*marker.delta_alt_fraction, 1.0, 1e-15, "positive marker delta drift");
    }
    check(observed.signature_counts == std::vector<std::pair<std::string, std::size_t>>({{"AA", 12}, {"RR", 12}}),
          "positive signature counts drift");
    check(observed.complete_readset_sha256.size() == 64, "positive complete-readset SHA missing");
    check(observed.complete_readset_sha256 == "f9a34ec03d93a5c1dcce24160fe5eb9d121090f43e286505f8ad72af170c2672",
          "positive complete-readset frozen digest drift");
    complete_readset_trace = observed.complete_readset_sha256;
    check(observed.semantic_sha256.size() == 64, "positive semantic SHA missing");
    check(observed.semantic_sha256 == "9a4fab2a6f90dee4bd3cc345d969168d8c7d1ddbf2697ef301126c722a36b4f4",
          "positive frozen semantic trace drift");
    positive_trace = observed.semantic_sha256;
    ++counts.oracle_cases;

    std::vector<JointPartnerCandidate> reversed_candidates = candidates;
    std::vector<JointCoreRead> reversed_reads = reads;
    std::reverse(reversed_candidates.begin(), reversed_candidates.end());
    std::reverse(reversed_reads.begin(), reversed_reads.end());
    const JointSignatureResult reversed = require_result(
        longlineage::cooccurrence::evaluate_joint_signature(1000, reversed_candidates, reversed_reads, 29),
        "reversed positive oracle");
    check(reversed.semantic_sha256 == observed.semantic_sha256 &&
              reversed.effect_supported_positions == observed.effect_supported_positions &&
              reversed.association.table == observed.association.table,
          "joint-signature stable-key/input-order invariance drift");
    ++counts.invariance_cases;

    const JointSignatureResult repeated = require_result(
        longlineage::cooccurrence::evaluate_joint_signature(1000, candidates, reads, 29), "repeated positive oracle");
    check(repeated.semantic_sha256 == observed.semantic_sha256,
          "joint-signature repeated PCG64 trace is not deterministic");
    ++counts.invariance_cases;
}

void test_balanced_null_oracle(Counts& counts) {
    std::vector<JointCoreRead> reads;
    for (std::size_t index = 0; index < 20; ++index) {
        const std::size_t within_group = index % 10;
        const JointMarkerCall call = within_group < 5 ? JointMarkerCall::kReference : JointMarkerCall::kAlternate;
        reads.push_back(make_read(index, index < 10 ? "g1" : "g2", "mixed", "1", "+", {{1100, call}, {1120, call}}));
    }
    const JointSignatureResult observed = require_result(
        longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), reads, 41), "balanced null oracle");
    check(observed.association.testable && observed.association.cramers_v.has_value() &&
              observed.association.table == std::vector<std::vector<std::uint64_t>>({{5, 5}, {5, 5}}),
          "balanced null table drift");
    check_near(*observed.association.cramers_v, 0.0, 1e-15, "balanced null Cramer's V drift");
    check(observed.conditional.permutable && observed.conditional.exceedance == 999 &&
              observed.conditional.p_value.has_value(),
          "balanced null conditional state drift");
    check_near(*observed.conditional.p_value, 1.0, 1e-15, "balanced null permutation p drift");
    check(!observed.conditional_sensitivity_pass && observed.effect_supported_positions.empty(),
          "balanced null incorrectly passed a scientific effect gate");
    ++counts.oracle_cases;
}

void test_nonexchangeable_oracle(Counts& counts) {
    std::vector<JointCoreRead> reads;
    for (std::size_t index = 0; index < 12; ++index) {
        const bool first_group = index < 6;
        const JointMarkerCall call = first_group ? JointMarkerCall::kReference : JointMarkerCall::kAlternate;
        reads.push_back(make_read(index, first_group ? "g1" : "g2", first_group ? "s1" : "s2", "1", "+",
                                  {{1100, call}, {1120, call}}));
    }
    const JointSignatureResult observed = require_result(
        longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), reads, 43), "nonexchangeable oracle");
    check(observed.association.testable &&
              observed.conditional.status == JointConditionalStatus::kNotIdentifiableNonexchangeable &&
              !observed.conditional.permutable && !observed.conditional.p_value.has_value() &&
              observed.conditional.strata == 2 && observed.conditional.exchangeable_strata == 0 &&
              observed.conditional.informative_exchangeable_strata == 0,
          "nonexchangeable stratum did not fail closed");
    ++counts.oracle_cases;
}

void test_intermediate_permutation_oracle(Counts& counts) {
    struct StratumCounts {
        std::string stratum;
        std::size_t g1_reference;
        std::size_t g1_alternate;
        std::size_t g2_reference;
        std::size_t g2_alternate;
    };
    const std::vector<StratumCounts> specifications = {
        {"s1", 5, 1, 3, 3},
        {"s2", 4, 2, 2, 4},
    };
    std::vector<JointCoreRead> reads;
    std::size_t read_index = 0;
    const auto append_reads = [&](const std::string& label, const std::string& stratum, std::size_t count,
                                  JointMarkerCall call) {
        for (std::size_t offset = 0; offset < count; ++offset) {
            reads.push_back(make_read(read_index++, label, stratum, "1", "+", {{1100, call}, {1120, call}}));
        }
    };
    for (const auto& specification : specifications) {
        append_reads("g1", specification.stratum, specification.g1_reference, JointMarkerCall::kReference);
        append_reads("g1", specification.stratum, specification.g1_alternate, JointMarkerCall::kAlternate);
        append_reads("g2", specification.stratum, specification.g2_reference, JointMarkerCall::kReference);
        append_reads("g2", specification.stratum, specification.g2_alternate, JointMarkerCall::kAlternate);
    }
    const JointSignatureResult observed =
        require_result(longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), reads, 73),
                       "intermediate permutation oracle");
    check(observed.association.table == std::vector<std::vector<std::uint64_t>>({{3, 9}, {7, 5}}) &&
              observed.association.cramers_v.has_value(),
          "intermediate permutation contingency table drift");
    check_near(*observed.association.cramers_v, 0.3380617018914066, 2e-15, "intermediate permutation Cramer's V drift");
    check(observed.conditional.permutable && observed.conditional.permutations == 999 &&
              observed.conditional.exceedance == 213 && observed.conditional.p_value.has_value(),
          "intermediate PCG64 permutation census drift");
    check_near(*observed.conditional.p_value, 0.214, 1e-15, "intermediate PCG64 permutation p drift");
    ++counts.oracle_cases;
}

void test_one_marker_effect_support(Counts& counts) {
    std::vector<JointCoreRead> reads;
    for (std::size_t index = 0; index < 20; ++index) {
        const bool first_group = index < 10;
        reads.push_back(make_read(index, first_group ? "g1" : "g2", index % 2 == 0 ? "HP1-side" : "HP2-side", "101",
                                  "+",
                                  {{1100, first_group ? JointMarkerCall::kReference : JointMarkerCall::kAlternate},
                                   {1120, JointMarkerCall::kReference}}));
    }
    const JointSignatureResult observed =
        require_result(longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), reads, 29),
                       "one-marker effect oracle");
    check(observed.association.testable && observed.conditional_sensitivity_pass &&
              observed.effect_supported_positions == std::vector<std::uint64_t>({1100}) &&
              observed.marker_support[0].effect_gate_pass && !observed.marker_support[1].effect_gate_pass &&
              observed.marker_support[1].status == JointAssociationStatus::kMarkerAlleleBelowMinimum,
          "same-complete-read per-marker effect gate failed to reject the constant marker");
    ++counts.oracle_cases;
}

void test_not_testable_boundaries(Counts& counts) {
    auto one_partner = two_partners();
    one_partner.resize(1);
    const JointSignatureResult one = require_result(
        longlineage::cooccurrence::evaluate_joint_signature(1000, one_partner, positive_reads(false), 29),
        "one-partner boundary");
    check(!one.association.testable && one.association.status == JointAssociationStatus::kFewerThanTwoTopMarkers &&
              one.association.table.empty() &&
              one.conditional.status == JointConditionalStatus::kNotRunJointSignatureNotTestable,
          "fewer-than-two top markers did not fail closed");

    std::vector<JointCoreRead> rare_categories;
    const std::vector<std::pair<JointMarkerCall, JointMarkerCall>> patterns = {
        {JointMarkerCall::kReference, JointMarkerCall::kReference},
        {JointMarkerCall::kReference, JointMarkerCall::kAlternate},
        {JointMarkerCall::kAlternate, JointMarkerCall::kReference},
        {JointMarkerCall::kAlternate, JointMarkerCall::kAlternate},
        {JointMarkerCall::kReference, JointMarkerCall::kReference},
        {JointMarkerCall::kReference, JointMarkerCall::kAlternate},
    };
    for (std::size_t index = 0; index < 12; ++index) {
        const auto pattern = patterns[index % patterns.size()];
        rare_categories.push_back(make_read(index, index < 6 ? "g1" : "g2", "mixed", "1", "+",
                                            {{1100, pattern.first}, {1120, pattern.second}}));
    }
    const JointSignatureResult rare =
        require_result(longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), rare_categories, 41),
                       "rare-category boundary");
    check(!rare.association.testable && rare.association.status == JointAssociationStatus::kCategoryBelowMinimum &&
              rare.conditional.status == JointConditionalStatus::kNotRunJointSignatureNotTestable,
          "rare joint category did not fail closed");
    counts.oracle_cases += 2;
}

std::vector<JointGlobalFdrInput> fdr_inputs() {
    return {
        {"site-01", true, true, true, 999, 0.001},       {"site-02", true, true, true, 999, 0.005},
        {"site-03", true, true, true, 999, 0.020},       {"site-04", true, true, true, 999, 0.200},
        {"site-05", false, true, true, 999, 0.001},      {"site-06", true, false, true, 999, 0.001},
        {"site-07", true, true, false, 0, std::nullopt}, {"site-08", true, true, true, 999, 0.0015},
    };
}

JointGlobalFdrResult require_fdr(const longlineage::ParseResult<JointGlobalFdrResult>& result,
                                 std::string_view context) {
    check(result.ok() && result.value.has_value(), std::string(context) + " unexpectedly failed: " + result.detail);
    return *result.value;
}

void test_global_fdr_oracle(Counts& counts, std::string& fdr_trace) {
    const auto inputs = fdr_inputs();
    const JointGlobalFdrResult observed =
        require_fdr(longlineage::cooccurrence::apply_joint_signature_global_fdr(inputs), "global FDR oracle");
    check(observed.rows.size() == 8 && observed.family_size == 4 && observed.bh_discoveries == 3 &&
              observed.by_discoveries == 2 && !observed.all_permutation_evidence_valid,
          "global FDR family/discovery census drift");
    const std::vector<double> expected_bh = {0.004, 0.010, 0.02666666666666667, 0.200};
    const std::vector<double> expected_by = {0.008333333333333335, 0.020833333333333336, 0.055555555555555566,
                                             0.41666666666666674};
    for (std::size_t index = 0; index < 4; ++index) {
        const auto& row = observed.rows[index];
        check(row.status == JointGlobalFdrStatus::kEligibleJointSignatureGlobalFdrFamily &&
                  row.q_global_bh.has_value() && row.q_global_by.has_value(),
              "eligible global FDR row lost adjusted evidence");
        check_near(*row.q_global_bh, expected_bh[index], 2e-15, "global BH q drift");
        check_near(*row.q_global_by, expected_by[index], 2e-15, "global BY q drift");
    }
    check(observed.rows[4].status == JointGlobalFdrStatus::kIneligibleM2Screen &&
              observed.rows[5].status == JointGlobalFdrStatus::kEligibleJointSignatureNotTestable &&
              observed.rows[6].status == JointGlobalFdrStatus::kEligibleJointSignatureNotPermutable &&
              observed.rows[7].status == JointGlobalFdrStatus::kInvalidPermutationEvidence,
          "global FDR fail-closed status precedence drift");
    fdr_trace = observed.semantic_sha256;
    check(fdr_trace == "daef4675f35dc17779bf93a0b362838938d3355fc5997ee6586f2e200a4a140b",
          "global FDR frozen semantic trace drift");
    counts.fdr_rows += observed.rows.size();
    ++counts.oracle_cases;

    std::vector<JointGlobalFdrInput> reversed = inputs;
    std::reverse(reversed.begin(), reversed.end());
    const JointGlobalFdrResult repeated = require_fdr(
        longlineage::cooccurrence::apply_joint_signature_global_fdr(reversed), "reversed global FDR oracle");
    check(repeated.semantic_sha256 == observed.semantic_sha256, "global FDR stable-site input-order invariance drift");
    ++counts.invariance_cases;

    const JointGlobalFdrResult ties = require_fdr(longlineage::cooccurrence::apply_joint_signature_global_fdr({
                                                      {"zeta", true, true, true, 999, 0.010},
                                                      {"alpha", true, true, true, 999, 0.010},
                                                      {"middle", true, true, true, 999, 0.010},
                                                  }),
                                                  "global tied-p oracle");
    check(ties.family_size == 3 && ties.rows[0].stable_site_key == "alpha" &&
              ties.rows[1].stable_site_key == "middle" && ties.rows[2].stable_site_key == "zeta",
          "global tied-p canonical stable-key order drift");
    for (const auto& row : ties.rows) {
        check(row.q_global_bh.has_value() && row.q_global_by.has_value(), "global tied-p row lost adjusted evidence");
        check_near(*row.q_global_bh, 0.010, 1e-15, "global tied-p BH drift");
        check_near(*row.q_global_by, 0.018333333333333333, 2e-15, "global tied-p BY drift");
    }
    counts.fdr_rows += ties.rows.size();
    ++counts.oracle_cases;
}

longlineage::cooccurrence::JointGlobalFdrRow one_site_global_row(const JointSignatureResult& joint,
                                                                 std::string stable_site_key) {
    const JointGlobalFdrResult fdr =
        require_fdr(longlineage::cooccurrence::apply_joint_signature_global_fdr(
                        {{stable_site_key, true, joint.association.testable, joint.conditional.permutable,
                          joint.conditional.permutations, joint.conditional.p_value}}),
                    "one-site topology global FDR");
    check(fdr.rows.size() == 1, "one-site topology global FDR cardinality drift");
    return fdr.rows.front();
}

void test_topology_gate_oracle(Counts& counts, std::string& topology_trace) {
    auto candidates = two_partners();
    candidates[0].endpoint_a_n_informative = 24;
    candidates[1].endpoint_a_n_informative = 25;
    JointSignatureResult passing =
        require_result(longlineage::cooccurrence::evaluate_joint_signature(1000, candidates, positive_reads(false), 29),
                       "topology passing joint signature");
    const std::string stable_site_key = "dataset|chr1|1000|A|G";
    JointTopologyGateInput input{
        stable_site_key,
        true,
        one_site_global_row(passing, stable_site_key),
        {{1100, 11, true}, {1120, 12, true}},
    };
    const auto finalized = longlineage::cooccurrence::finalize_joint_signature_topology_gate(passing, input);
    check(finalized.ok() && finalized.value.has_value() && *finalized.value,
          "passing topology gate finalization failed");
    check(passing.topology_gate_evaluated && passing.passed && passing.postselection_fdr_calibrated &&
              passing.topology_gate_status == JointTopologyGateStatus::kPass &&
              passing.selected_partner_site_orders == std::vector<std::uint64_t>({12, 11}) &&
              passing.topology_gate_sha256.size() == 64,
          "formal topology PASS/ranking-order projection drift");
    topology_trace = passing.topology_gate_sha256;
    check(topology_trace == "8bd49c708a99b7086579bb207e9275e3054ee31bf8ff02cea82a5aff1f6313cb",
          "topology gate frozen semantic trace drift");
    ++counts.topology_gate_cases;

    const auto repeated = longlineage::cooccurrence::finalize_joint_signature_topology_gate(passing, input);
    check(!repeated.ok() && repeated.reason == ParseReason::kUnsupportedValue,
          "double topology finalization did not fail closed");
    ++counts.negative_cases;

    JointSignatureResult insufficient_confirmed = require_result(
        longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), positive_reads(false), 29),
        "topology insufficient-confirmed joint signature");
    JointTopologyGateInput insufficient_input{
        stable_site_key,
        true,
        one_site_global_row(insufficient_confirmed, stable_site_key),
        {{1100, 11, true}, {1120, 12, false}},
    };
    const auto insufficient =
        longlineage::cooccurrence::finalize_joint_signature_topology_gate(insufficient_confirmed, insufficient_input);
    check(insufficient.ok() && insufficient.value.has_value() && *insufficient.value &&
              !insufficient_confirmed.passed && insufficient_confirmed.selected_partner_site_orders.empty() &&
              insufficient_confirmed.topology_gate_status == JointTopologyGateStatus::kFailInsufficientConfirmedPairs,
          "topology insufficient-confirmed gate did not fail closed");
    ++counts.topology_gate_cases;

    JointSignatureResult not_evaluated;
    const JointGlobalFdrResult ineligible_fdr =
        require_fdr(longlineage::cooccurrence::apply_joint_signature_global_fdr(
                        {{"ineligible-site", false, false, false, 0, std::nullopt}}),
                    "M2-ineligible topology global FDR");
    JointTopologyGateInput ineligible_input{"ineligible-site", false, ineligible_fdr.rows.front(), {}};
    const auto ineligible =
        longlineage::cooccurrence::finalize_joint_signature_topology_gate(not_evaluated, ineligible_input);
    check(ineligible.ok() && ineligible.value.has_value() && *ineligible.value && !not_evaluated.passed &&
              not_evaluated.topology_gate_evaluated && !not_evaluated.postselection_fdr_calibrated &&
              not_evaluated.semantic_sha256.empty() && not_evaluated.complete_readset_sha256.empty() &&
              not_evaluated.topology_gate_status == JointTopologyGateStatus::kNotEvaluatedM2Ineligible &&
              not_evaluated.topology_gate_sha256.size() == 64,
          "M2-ineligible cheap not-evaluated path drift");
    ++counts.topology_gate_cases;

    JointSignatureResult mismatch = require_result(
        longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), positive_reads(false), 29),
        "topology mismatched-global joint signature");
    JointTopologyGateInput mismatch_input{
        stable_site_key,
        true,
        one_site_global_row(mismatch, stable_site_key),
        {{1100, 11, true}, {1120, 12, true}},
    };
    mismatch_input.global_fdr.stable_site_key = "other-site";
    const auto rejected = longlineage::cooccurrence::finalize_joint_signature_topology_gate(mismatch, mismatch_input);
    check(!rejected.ok() && rejected.reason == ParseReason::kMalformedValue,
          "topology stable-site/global-FDR mismatch did not fail closed");
    ++counts.negative_cases;
}

void expect_failure(const longlineage::ParseResult<JointSignatureResult>& result, ParseReason reason, Counts& counts) {
    check(!result.ok() && !result.value.has_value() && result.reason == reason,
          "joint-signature malformed input did not fail closed");
    ++counts.negative_cases;
}

void test_negative_cases(Counts& counts) {
    auto duplicate_partner = two_partners();
    duplicate_partner[1].position1 = duplicate_partner[0].position1;
    expect_failure(longlineage::cooccurrence::evaluate_joint_signature(1000, duplicate_partner, positive_reads(), 29),
                   ParseReason::kIndexError, counts);

    auto focal_partner = two_partners();
    focal_partner[0].position1 = 1000;
    expect_failure(longlineage::cooccurrence::evaluate_joint_signature(1000, focal_partner, positive_reads(), 29),
                   ParseReason::kMalformedValue, counts);

    expect_failure(
        longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), positive_reads(), 29, 3, 998),
        ParseReason::kUnsupportedValue, counts);

    auto duplicate_reads = positive_reads();
    duplicate_reads.back().stable_key = duplicate_reads.front().stable_key;
    expect_failure(longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), duplicate_reads, 29),
                   ParseReason::kIndexError, counts);

    auto missing_call = positive_reads();
    missing_call.front().marker_calls.pop_back();
    expect_failure(longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), missing_call, 29),
                   ParseReason::kMissingRequiredField, counts);

    auto duplicate_call = positive_reads();
    duplicate_call.front().marker_calls.push_back(duplicate_call.front().marker_calls.front());
    expect_failure(longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), duplicate_call, 29),
                   ParseReason::kIndexError, counts);

    auto malformed_read_id = positive_reads();
    malformed_read_id.front().stable_key = std::string(64, 'A');
    expect_failure(longlineage::cooccurrence::evaluate_joint_signature(1000, two_partners(), malformed_read_id, 29),
                   ParseReason::kMalformedValue, counts);

    const auto duplicate_sites = longlineage::cooccurrence::apply_joint_signature_global_fdr({
        {"site", true, true, true, 999, 0.001},
        {"site", true, true, true, 999, 0.002},
    });
    check(!duplicate_sites.ok() && duplicate_sites.reason == ParseReason::kIndexError,
          "duplicate global stable-site key did not fail closed");
    ++counts.negative_cases;
}

}  // namespace

int main() {
    try {
        Counts counts;
        std::string positive_trace;
        std::string complete_readset_trace;
        std::string fdr_trace;
        std::string topology_trace;
        test_top_partner_ranking(counts);
        test_historical_positive_oracle(counts, positive_trace, complete_readset_trace);
        test_balanced_null_oracle(counts);
        test_nonexchangeable_oracle(counts);
        test_intermediate_permutation_oracle(counts);
        test_one_marker_effect_support(counts);
        test_not_testable_boundaries(counts);
        test_global_fdr_oracle(counts, fdr_trace);
        test_topology_gate_oracle(counts, topology_trace);
        test_negative_cases(counts);
        std::cout << "joint_signature: PASS oracle_cases=" << counts.oracle_cases
                  << " top_partner_cases=" << counts.top_partner_cases << " fdr_rows=" << counts.fdr_rows
                  << " topology_gate_cases=" << counts.topology_gate_cases
                  << " invariance_cases=" << counts.invariance_cases << " negative_cases=" << counts.negative_cases
                  << " positive_trace=" << positive_trace << " complete_readset_trace=" << complete_readset_trace
                  << " fdr_trace=" << fdr_trace << " topology_trace=" << topology_trace
                  << " production_python_calls=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "joint_signature: FAIL " << error.what() << '\n';
        return 1;
    }
}
