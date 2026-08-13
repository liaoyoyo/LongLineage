// SPDX-License-Identifier: GPL-3.0-only

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "longlineage/cooccurrence/site_cooccurrence.hpp"
#include "longlineage/cooccurrence/statistics.hpp"

namespace {

using longlineage::AlleleCall;
using longlineage::cooccurrence::FixedErrorGateStatus;
using longlineage::cooccurrence::JointAlleleCounts;
using longlineage::cooccurrence::RelationCompatibilityStatus;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_near(double observed, double expected, double tolerance, const std::string& message) {
    if (!std::isfinite(observed) || std::abs(observed - expected) > tolerance) {
        throw std::runtime_error(message + ": observed=" + std::to_string(observed) +
                                 " expected=" + std::to_string(expected));
    }
}

void add(JointAlleleCounts& counts, AlleleCall focal, AlleleCall partner, std::uint64_t occurrences) {
    for (std::uint64_t index = 0; index < occurrences; ++index) {
        counts.add(focal, partner);
    }
}

JointAlleleCounts ra_counts(std::uint64_t rr, std::uint64_t ar, std::uint64_t ra, std::uint64_t aa) {
    JointAlleleCounts counts;
    add(counts, AlleleCall::kReference, AlleleCall::kReference, rr);
    add(counts, AlleleCall::kAlternate, AlleleCall::kReference, ar);
    add(counts, AlleleCall::kReference, AlleleCall::kAlternate, ra);
    add(counts, AlleleCall::kAlternate, AlleleCall::kAlternate, aa);
    return counts;
}

longlineage::VariantSite variant(std::uint64_t site_order, std::uint64_t position_value) {
    auto chromosome = longlineage::ContigId::from_string("chrSynthetic");
    auto position = longlineage::Position1::from_value(position_value);
    check(chromosome.ok() && position.ok(), "cannot build synthetic variant identity");
    return longlineage::VariantSite{0,      "synthetic",     site_order, site_order, *chromosome.value,
                                    100000, *position.value, 'A',        'C'};
}

void test_group_allele_summary() {
    const std::vector<std::string> labels{"g1", "g1", "g1", "g1", "g1", "g1", "g2", "g2", "g2", "g2", "g2", "g2", "g2"};
    const std::vector<AlleleCall> calls{AlleleCall::kReference,   AlleleCall::kReference, AlleleCall::kReference,
                                        AlleleCall::kAlternate,   AlleleCall::kOther,     AlleleCall::kUnobservable,
                                        AlleleCall::kReference,   AlleleCall::kAlternate, AlleleCall::kAlternate,
                                        AlleleCall::kAlternate,   AlleleCall::kAlternate, AlleleCall::kOther,
                                        AlleleCall::kUnobservable};
    const auto summary = longlineage::cooccurrence::summarize_group_allele_association(labels, calls);
    check(!summary.testable && summary.groups == std::vector<std::string>({"g1", "g2"}),
          "group-allele testability/group order drift");
    check(summary.table == std::vector<longlineage::cooccurrence::Kx2Row>({{{3, 1}}, {{1, 4}}}) &&
              summary.n_informative == 9,
          "group-allele O/X exclusion drift");

    const auto relaxed = longlineage::cooccurrence::summarize_group_allele_association(labels, calls, 9, 3, 2);
    check(relaxed.testable && relaxed.minimum_group_n == 4 && relaxed.ref_n == 4 && relaxed.alt_n == 5,
          "group-allele conservation drift");
    check_near(*relaxed.delta_alt_fraction, 0.55, 1e-15, "group-allele delta ALT fraction drift");
    check_near(*relaxed.cramers_v, 0.55, 1e-15, "group-allele Cramer's V drift");
}

void test_binary_category_callability_p_values() {
    const std::vector<std::string> two_group_labels{
        "g1", "g1", "g1", "g1", "g1", "g1", "g2", "g2", "g2", "g2", "g2", "g2",
    };
    const std::vector<bool> two_group_categories{
        false, false, false, false, false, true, false, false, true, true, true, true,
    };
    const auto fisher = longlineage::cooccurrence::summarize_binary_category_association(
        two_group_labels, two_group_categories, 10, 3, 1);
    check(fisher.summary.testable && fisher.analytic_method == "FISHER_EXACT_2X2" && fisher.p_analytic.has_value(),
          "2x2 callability Fisher exact was not produced");
    check_near(*fisher.p_analytic, 0.24242424242424246, 2e-13, "2x2 callability Fisher exact parity drift");

    std::vector<std::string> three_group_labels = two_group_labels;
    three_group_labels.insert(three_group_labels.end(), 6, "g3");
    std::vector<bool> three_group_categories = two_group_categories;
    three_group_categories.insert(three_group_categories.end(), {false, false, false, true, true, true});
    const auto pearson = longlineage::cooccurrence::summarize_binary_category_association(
        three_group_labels, three_group_categories, 10, 3, 1);
    check(pearson.summary.testable && pearson.analytic_method == "PEARSON_CHI_SQUARE_UNCORRECTED" &&
              pearson.p_analytic.has_value(),
          "Kx2 callability Pearson statistic was not produced");
    check_near(*pearson.p_analytic, 0.20700755268115262, 2e-13, "Kx2 callability Pearson chi-square parity drift");

    bool threw = false;
    try {
        (void)longlineage::cooccurrence::summarize_binary_category_association({"g1"}, {false, true});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "callability length mismatch did not fail closed");
}

void test_four_state_python_frozen_vectors() {
    // Frozen from ssnv_cooccurrence_lib.py SHA-256
    // 3d41023b03f1f047312011f8513b685bf7ffe25f5d0795d253bc9013b9ec88ff.
    const auto focal = longlineage::cooccurrence::summarize_four_state_relations(ra_counts(100, 10, 0, 210));
    check(focal.compatibility == RelationCompatibilityStatus::kFocalAncestorCompatible &&
              focal.compatible_models.size() == 1 && focal.minimum_zero_violation_depth == 203,
          "focal-ancestor frozen decision drift");
    check(focal.focal_ancestor.status == FixedErrorGateStatus::kCompatibleWithFixedErrorCeiling &&
              focal.partner_ancestor.status == FixedErrorGateStatus::kViolatesFixedErrorCeiling,
          "focal-ancestor gate status drift");
    check_near(*focal.focal_ancestor.upper_bound, 0.019308043946793904, 2e-13,
               "focal zero-violation upper bound drift");
    check_near(*focal.partner_ancestor.p_exact_greater, 0.013933810637240761, 2e-13, "partner violation exact p drift");

    const auto partner = longlineage::cooccurrence::summarize_four_state_relations(ra_counts(100, 0, 10, 210));
    check(partner.compatibility == RelationCompatibilityStatus::kPartnerAncestorCompatible,
          "partner-ancestor frozen decision drift");

    const auto branching = longlineage::cooccurrence::summarize_four_state_relations(ra_counts(100, 210, 210, 0));
    check(branching.compatibility == RelationCompatibilityStatus::kBranchingCompatible,
          "branching frozen decision drift");
    check_near(*branching.branching.upper_bound, 0.009701077424999873, 2e-13,
               "branching zero-violation upper bound drift");

    const auto multiple = longlineage::cooccurrence::summarize_four_state_relations(ra_counts(100, 3, 3, 1000));
    check(multiple.compatibility == RelationCompatibilityStatus::kMultipleModelsCompatible &&
              multiple.compatible_models.size() == 2,
          "multiple-model frozen decision drift");
    check_near(*multiple.focal_ancestor.upper_bound, 0.009282780128478572, 2e-13,
               "multiple-model beta upper bound drift");

    const auto incompatible = longlineage::cooccurrence::summarize_four_state_relations(ra_counts(25, 25, 25, 25));
    check(incompatible.compatibility == RelationCompatibilityStatus::kIncompatibleOrComplex,
          "incompatible frozen decision drift");
    check_near(*incompatible.focal_ancestor.p_exact_greater, 2.610833638819803e-29, 1e-39,
               "incompatible exact p drift");

    const auto insufficient = longlineage::cooccurrence::summarize_four_state_relations(ra_counts(2, 1, 1, 1));
    check(!insufficient.complete_four_state_testable &&
              insufficient.compatibility == RelationCompatibilityStatus::kNotIdentifiableInsufficientFourStateDepth,
          "insufficient four-state depth did not fail closed");
}

void test_noncallable_cells_are_conserved() {
    JointAlleleCounts counts = ra_counts(1, 2, 3, 4);
    counts.add(AlleleCall::kReference, AlleleCall::kOther);
    counts.add(AlleleCall::kAlternate, AlleleCall::kUnobservable);
    counts.add(AlleleCall::kOther, AlleleCall::kReference);
    counts.add(AlleleCall::kUnobservable, AlleleCall::kAlternate);
    const auto summary = longlineage::cooccurrence::summarize_four_state_relations(counts);
    check(summary.counts.total() == 14 && summary.called_ra_depth == 10,
          "O/X cells were collapsed into the four-state R/A depth");
}

void test_global_fdr_conditional_and_formal_pair_gate() {
    longlineage::cooccurrence::PairInference pair{variant(10, 100), variant(11, 120)};
    pair.family_status = longlineage::cooccurrence::PairFamilyStatus::kEligibleM2ExactFamily;
    pair.endpoint_a.groups = {"g1", "g2"};
    pair.endpoint_a.table = {{{10, 0}}, {{0, 10}}};
    pair.endpoint_a.n_informative = 20;
    pair.endpoint_a.minimum_group_n = 10;
    pair.endpoint_a.ref_n = 10;
    pair.endpoint_a.alt_n = 10;
    pair.endpoint_a.testable = true;
    pair.endpoint_a.reason = "OK";
    pair.endpoint_a.cramers_v = 1.0;
    pair.endpoint_a.delta_alt_fraction = 1.0;
    pair.exact.status = longlineage::cooccurrence::ExactStateStatus::kExactEnumerated;
    pair.exact.p_value = 0.0001;
    pair.exact.identifiable = true;
    pair.callability.summary.testable = false;
    pair.noncallable_core_reads = 0;
    for (std::size_t index = 0; index < 20; ++index) {
        pair.conditional_payload.labels.push_back(index < 10 ? "g1" : "g2");
        pair.conditional_payload.categories.push_back(index < 10 ? AlleleCall::kReference : AlleleCall::kAlternate);
        pair.conditional_payload.strata.push_back("HP1-side|PS=7|strand=+");
    }
    std::vector<longlineage::cooccurrence::PairInference> pairs;
    pairs.push_back(std::move(pair));
    auto fdr = longlineage::cooccurrence::finalize_global_pair_families(pairs);
    check(fdr.ok(), fdr.detail);
    check(pairs[0].fdr_family_size == 1 && pairs[0].exact_bh_discovery && pairs[0].exact_by_discovery &&
              pairs[0].effect_gate_pass &&
              pairs[0].callability_status == longlineage::cooccurrence::CallabilityStatus::kPassAllCoreReadsCallable,
          "global endpoint-A/callability gates drift");
    auto conditional = longlineage::cooccurrence::run_conditional_pair_sensitivity(pairs);
    check(conditional.ok(), conditional.detail);
    check(pairs[0].conditional.permutable && pairs[0].conditional.permutations == 999 &&
              pairs[0].conditional.p_value.has_value() && *pairs[0].conditional.p_value == 0.001 &&
              pairs[0].conditional.exceedance == 0 && pairs[0].conditional.strata == 1 &&
              pairs[0].conditional.exchangeable_strata == 1 &&
              pairs[0].conditional.informative_exchangeable_strata == 1 && pairs[0].formal_pair_by_confirmed &&
              pairs[0].semantic_sha256.size() == 64,
          "conditional sensitivity/formal pair gate drift");
}

}  // namespace

int main() {
    try {
        test_group_allele_summary();
        test_binary_category_callability_p_values();
        test_four_state_python_frozen_vectors();
        test_noncallable_cells_are_conserved();
        test_global_fdr_conditional_and_formal_pair_gate();
        std::cout << "PASS test_cooccurrence_full: Kx2 summary, 16-cell conservation, "
                     "Bonferroni fixed-error relations\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL test_cooccurrence_full: " << error.what() << '\n';
        return 1;
    }
}
