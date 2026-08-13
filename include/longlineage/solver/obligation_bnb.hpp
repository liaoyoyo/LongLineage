// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "longlineage/solver/topology_types.hpp"

namespace longlineage::solver {

struct ObligationBnbOptions {
    // Zero means no limit. A family limit never becomes FAMILY_COMPLETE:
    // search continues to certify h*, partial candidates are then withheld.
    std::size_t maximum_complete_family_size = 0;

    // Zero means no limit. If this limit is reached, neither objective nor
    // family is published because unvisited branches can still improve h*.
    std::uint64_t maximum_search_nodes = 0;
};

// Exact recurrence-allowed solver for bit_count <= 12. It dynamically derives
// unsatisfied group and predecessor obligations, removes dominated supersets,
// propagates singletons, and exhausts every branch with lower_bound <= incumbent.
ExactStructuralResult solve_obligation_bnb(const ExactTopologyProblem& problem,
                                           const ObligationBnbOptions& options = {});

}  // namespace longlineage::solver
