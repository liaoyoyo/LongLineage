// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "longlineage/compat/regional_topology.hpp"

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

longlineage::ContigId contig(const std::string& value) {
    auto parsed = longlineage::ContigId::from_string(value);
    check(parsed.ok(), parsed.detail);
    return *parsed.value;
}

longlineage::Position1 position(std::uint64_t value) {
    auto parsed = longlineage::Position1::from_value(value);
    check(parsed.ok(), parsed.detail);
    return *parsed.value;
}

longlineage::VariantSite site(std::uint64_t order, std::string chromosome, std::uint64_t position1) {
    return longlineage::VariantSite{0,         "synthetic",         order, order, contig(chromosome),
                                    1'000'000, position(position1), 'C',   'T'};
}

void test_grouping_boundary_transitivity_and_densest_tie() {
    longlineage::VariantSiteSet variants;
    variants.sites = {
        site(0, "chr1", 100),     site(1, "chr1", 50'100),  site(2, "chr1", 100'100),
        site(3, "chr1", 150'101), site(4, "chr1", 300'000),
    };
    variants.census.selected_scope = variants.sites.size();
    auto plan = longlineage::compat::make_python_v2_regional_plan(variants);
    check(plan.ok(), plan.detail);
    check(plan.value->regions.size() == 1, "50 kb transitive component or 50,001 split drifted");
    check(plan.value->regions.front().selected_sites.size() == 3 && plan.value->regions.front().span == 100'000,
          "transitive component membership drifted");
    check(plan.value->census.positional_singletons == 2, "split singletons were not counted");

    longlineage::VariantSiteSet capped;
    for (std::uint64_t index = 0; index < 9; ++index) {
        capped.sites.push_back(site(index, "chr2", 1'000 + index * 10));
    }
    capped.census.selected_scope = capped.sites.size();
    auto capped_plan = longlineage::compat::make_python_v2_regional_plan(capped);
    check(capped_plan.ok(), capped_plan.detail);
    const auto& region = capped_plan.value->regions.front();
    check(region.pre_cap_site_count == 9 && region.selected_sites.front().position.value() == 1'000 &&
              region.selected_sites.back().position.value() == 1'070,
          "densest-eight tie did not choose the first window");
}

void test_hp_family_prefix_contract() {
    using longlineage::compat::python_v2_hp_family;
    check(python_v2_hp_family("1") == "1" && python_v2_hp_family("1-1") == "1" && python_v2_hp_family("1-any") == "1" &&
              python_v2_hp_family("2-2") == "2" && python_v2_hp_family("3") == "3" && python_v2_hp_family("4") == "4" &&
              python_v2_hp_family(".") == "none" && python_v2_hp_family("") == "none",
          "Python-v2 HP prefix family contract drifted");
}

void test_first_hcc_unit_golden() {
    longlineage::compat::LegacySolverInput input;
    input.site_count = 2;
    input.supported_full_patterns = {{"AA", 6}};
    input.supported_subread_patterns = {{"AA", 6}, {"XA", 44}, {"XR", 5}};
    auto solved = longlineage::compat::solve_python_v2_legacy(input);
    check(solved.ok(), solved.detail);
    check(solved.value->classification == "ambiguous_structure" && solved.value->n_hidden == 1 &&
              solved.value->n_trees == 2 && solved.value->n_feasible_node_sets == 2 && !solved.value->capped,
          "frozen translated HCC golden unit solver result drifted");
}

void test_reference_only_and_minread_fail_closed() {
    longlineage::compat::LegacySolverInput reference;
    reference.site_count = 2;
    reference.supported_subread_patterns = {{"RX", 4}};
    auto solved = longlineage::compat::solve_python_v2_legacy(reference);
    check(solved.ok(), solved.detail);
    check(solved.value->classification == "determined" && solved.value->n_hidden == 0 && solved.value->n_trees == 1 &&
              !longlineage::compat::pattern_is_mutation_bearing(reference),
          "reference-only legacy unit drifted");
    check(longlineage::compat::python_v2_unit_role("1", false) == "reference_only_control" &&
              longlineage::compat::python_v2_unit_role("2", true) == "primary_mutation_lineage" &&
              longlineage::compat::python_v2_unit_role("3", true) == "unresolved_H3_auxiliary" &&
              longlineage::compat::python_v2_unit_role("none", false) == "unphased_auxiliary",
          "unit-role precedence drifted");

    reference.supported_subread_patterns = {{"RX", 2}};
    auto rejected = longlineage::compat::solve_python_v2_legacy(reference);
    check(!rejected.ok(), "below-MINREAD pattern was accepted by solver");
}

void test_cpython39_capped_fallback_golden() {
    longlineage::compat::LegacySolverInput first;
    first.site_count = 8;
    first.supported_full_patterns = {{"AAAAAAAA", 70}, {"AAAAAARA", 3}, {"ARAAAAAA", 4}, {"RRRRRRRR", 8}};
    first.supported_subread_patterns = {
        {"AAAAAAAA", 70}, {"AAAAAARA", 3}, {"AAAAAAXA", 4}, {"ARAAAAAA", 4}, {"RRRRRRRR", 8}};
    auto first_result = longlineage::compat::solve_python_v2_legacy(first);
    check(first_result.ok(), first_result.detail);
    check(first_result.value->capped && first_result.value->n_hidden == 7 && first_result.value->n_trees == 4,
          "CPython 3.9 capped fallback drifted for synthetic golden A");

    longlineage::compat::LegacySolverInput second;
    second.site_count = 8;
    second.supported_full_patterns = {{"AAAAAAAA", 34}, {"AAAAARAA", 5}, {"AARAAAAA", 5}, {"ARRRRRRR", 3}};
    second.supported_subread_patterns = {{"AAAAAAAA", 34}, {"AAAAARAA", 5}, {"AARAAAAA", 5}, {"AAXXXXXX", 5},
                                         {"ARRRRRRR", 3},  {"XAAAAAAA", 4}, {"XXAAAAAA", 4}, {"XXXXXAAA", 6}};
    auto second_result = longlineage::compat::solve_python_v2_legacy(second);
    check(second_result.ok(), second_result.detail);
    check(second_result.value->capped && second_result.value->n_hidden == 9 && second_result.value->n_trees == 4,
          "CPython 3.9 capped fallback drifted for synthetic golden B: " +
              std::to_string(second_result.value->n_hidden) + "/" + std::to_string(second_result.value->n_trees));
}

void test_h2009_near_budget_solver_regression() {
    longlineage::compat::LegacySolverInput input;
    input.site_count = 8;
    input.supported_full_patterns = {{"AARRAAAR", 48}, {"RRRRRRRA", 24}, {"RRRRRRRR", 73}};
    input.supported_subread_patterns = {
        {"AARRAAAR", 48}, {"RRRRRRRA", 24}, {"RRRRRRRR", 73}, {"RRRRRRRX", 3}, {"RRRRRRXX", 8},
        {"RXXXXXXX", 4},  {"XXXXXAAR", 3},  {"XXXXXRRR", 5},  {"XXXXXXAR", 4}, {"XXXXXXRR", 8},
    };
    auto solved = longlineage::compat::solve_python_v2_legacy(input);
    check(solved.ok(), solved.detail);
    check(!solved.value->capped && solved.value->cap_reason.empty() &&
              solved.value->classification == "ambiguous_structure" && solved.value->n_hidden == 4 &&
              solved.value->n_trees == 120 && solved.value->n_feasible_node_sets == 120 &&
              solved.value->n_recurrence_free_node_sets == 120 && solved.value->n_recurrent_node_sets == 0,
          "H2009 C(45,4)=148995 near-budget solver result drifted: " + std::to_string(solved.value->n_hidden) + "/" +
              std::to_string(solved.value->n_trees));
}

}  // namespace

int main() {
    try {
        test_grouping_boundary_transitivity_and_densest_tie();
        test_hp_family_prefix_contract();
        test_first_hcc_unit_golden();
        test_reference_only_and_minread_fail_closed();
        test_cpython39_capped_fallback_golden();
        test_h2009_near_budget_solver_regression();
        std::cout << "regional_compat: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "regional_compat: FAIL: " << error.what() << '\n';
        return 1;
    }
}
