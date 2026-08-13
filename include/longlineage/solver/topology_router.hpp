// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "longlineage/solver/obligation_bnb.hpp"
#include "longlineage/solver/small_q_oracle.hpp"
#include "longlineage/solver/terminal_subset_dp.hpp"

namespace longlineage::solver {

enum class ExactSolverRoute {
    kSmallQOracleDifferentialBnb,
    kBitsetObligationBnb,
    kAbstain,
};

struct TopologyRouterOptions {
    ObligationBnbOptions bnb;
    TerminalSubsetDpOptions dp;
    bool run_objective_dp_crosscheck = true;
};

struct TopologyRouterResult {
    ExactSolverRoute route = ExactSolverRoute::kAbstain;
    ExactStructuralResult structural;
    std::vector<std::size_t> active_loci;
    bool small_q_oracle_checked = false;
    bool small_q_oracle_match = false;
    bool objective_dp_checked = false;
    bool objective_dp_match = false;
    std::string message;
};

// Normalize R/A/X patterns onto active ALT bits, route m<=12 to the exact B&B,
// and require an exhaustive-oracle differential match when m<=4.
TopologyRouterResult solve_topology_exact(const TopologyProblem& problem, const TopologyRouterOptions& options = {});

}  // namespace longlineage::solver
