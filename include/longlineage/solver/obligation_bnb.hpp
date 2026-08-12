// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "longlineage/solver/topology_types.hpp"

namespace longlineage::solver {

struct ObligationBnbOptions {
    // Zero means no limit. A family limit never becomes FAMILY_COMPLETE:
    // search continues to certify h*, partial candidates are then withheld.
    std::size_t maximum_complete_family_size = 0;

    // Zero means no limit. If this limit is reached, neither objective nor
    // family is published because unvisited branches can still improve h*.
    std::uint64_t maximum_search_nodes = 0;

    // Optional upper bound seeded before the first node is expanded. The
    // default sentinel (max) preserves the historical blind search where the
    // incumbent only tightens after a first feasible solution is stumbled on.
    //
    // Seeding is admissible ONLY with a certified optimum (e.g. the objective
    // subset DP): the bound prunes exclusively branches whose lower_bound
    // already exceeds it, so no optimal solution can be discarded. The search
    // is NOT told the seed is optimal — a branch able to beat it is still
    // explored, so a wrong seed remains observable:
    //   * seed too low  -> no solution recorded -> solution_found() == false
    //   * seed too high -> incumbent drops below seed -> caller sees mismatch
    // Callers must compare the returned objective_h against the seed.
    std::size_t seed_incumbent = std::numeric_limits<std::size_t>::max();
};

// Exact recurrence-allowed solver for bit_count <= 12. It dynamically derives
// unsatisfied group and predecessor obligations, removes dominated supersets,
// propagates singletons, and exhausts every branch with lower_bound <= incumbent.
ExactStructuralResult solve_obligation_bnb(const ExactTopologyProblem& problem,
                                           const ObligationBnbOptions& options = {});

}  // namespace longlineage::solver
