// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/solver/parent_mapping.hpp"

#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <limits>
#include <set>
#include <sstream>

namespace longlineage::solver {
namespace {

using boost::multiprecision::cpp_int;

std::string decimal(const cpp_int& value) {
    std::ostringstream output;
    output << value;
    return output.str();
}

}  // namespace

ExactParentMappingSummary summarize_exact_parent_mappings(std::size_t bit_count,
                                                          const std::vector<HypercubeVertex>& vertices) {
    ExactParentMappingSummary result;
    if (bit_count > kMaximumExactHypercubeBits) {
        result.message = "bit_count exceeds the exact 12-bit kernel boundary";
        return result;
    }

    const std::size_t vertex_count = std::size_t{1} << bit_count;
    if (vertices.empty() || vertices.front() != 0 || !std::is_sorted(vertices.begin(), vertices.end()) ||
        std::adjacent_find(vertices.begin(), vertices.end()) != vertices.end()) {
        result.message = "vertex set must be sorted, unique and include root 0";
        return result;
    }
    if (std::any_of(vertices.begin(), vertices.end(), [vertex_count](HypercubeVertex vertex) {
            return static_cast<std::size_t>(vertex) >= vertex_count;
        })) {
        result.message = "vertex lies outside the declared hypercube";
        return result;
    }

    const std::set<HypercubeVertex> selected(vertices.begin(), vertices.end());
    cpp_int legal_parent_count = 0;
    cpp_int tree_count = 1;
    result.legal_parents.reserve(vertices.size() - 1);

    for (HypercubeVertex vertex : vertices) {
        if (vertex == 0) {
            continue;
        }
        ExactLegalParentChoices choices;
        choices.vertex = vertex;
        for (std::size_t bit = 0; bit < bit_count; ++bit) {
            const HypercubeVertex bit_mask = static_cast<HypercubeVertex>(HypercubeVertex{1} << bit);
            if ((vertex & bit_mask) == 0) {
                continue;
            }
            const HypercubeVertex parent = static_cast<HypercubeVertex>(vertex ^ bit_mask);
            if (selected.count(parent) != 0) {
                choices.parents.push_back(parent);
            }
        }
        std::sort(choices.parents.begin(), choices.parents.end());
        if (choices.parents.empty()) {
            result.legal_parents.clear();
            result.message = "vertex set is not rooted: a non-root vertex has no selected Hamming-1 predecessor";
            return result;
        }
        legal_parent_count += choices.parents.size();
        tree_count *= choices.parents.size();
        result.legal_parents.push_back(std::move(choices));
    }

    result.valid = true;
    result.legal_parent_count = decimal(legal_parent_count);
    result.tree_count = decimal(tree_count);
    result.message = "exact factorized legal-parent count";
    return result;
}

}  // namespace longlineage::solver
