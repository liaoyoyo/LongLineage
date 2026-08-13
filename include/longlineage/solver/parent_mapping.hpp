// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <vector>

#include "longlineage/solver/topology_types.hpp"

namespace longlineage::solver {

// Factorize all legal Hamming-1 parent mappings for one fixed vertex set.
// Counts are decimal strings backed by boost::multiprecision::cpp_int, so a
// structurally valid result never overflows a machine integer.
ExactParentMappingSummary summarize_exact_parent_mappings(std::size_t bit_count,
                                                          const std::vector<HypercubeVertex>& vertices);

}  // namespace longlineage::solver
