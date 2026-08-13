// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "longlineage/solver/topology_types.hpp"

namespace longlineage::solver {

struct TerminalSubsetDpOptions {
    std::size_t maximum_terminal_groups = 12;
    std::uint64_t maximum_state_cells = 64ULL * 1024ULL * 1024ULL;
};

struct TerminalSubsetDpResult {
    ExactObjectiveState objective_state = ExactObjectiveState::kAbstainNotIdentifiable;
    ExactKernelReason reason = ExactKernelReason::kMalformedProblem;
    std::optional<std::uint32_t> objective_h;
    std::uint64_t state_cells = 0;
    std::string message;
};

// Directed group-terminal subset DP. It proves only h*: it deliberately
// carries no candidate-family or rank fields.
TerminalSubsetDpResult solve_terminal_subset_objective(const ExactTopologyProblem& problem,
                                                       const TerminalSubsetDpOptions& options = {});

}  // namespace longlineage::solver
