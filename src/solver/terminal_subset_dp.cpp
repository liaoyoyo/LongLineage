// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/solver/terminal_subset_dp.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace longlineage::solver {
namespace {

TerminalSubsetDpResult abstain(ExactObjectiveState state, ExactKernelReason reason, std::string message,
                               std::uint64_t state_cells = 0) {
    TerminalSubsetDpResult result;
    result.objective_state = state;
    result.reason = reason;
    result.state_cells = state_cells;
    result.message = std::move(message);
    return result;
}

bool contains_vertex(const std::vector<HypercubeVertex>& group, HypercubeVertex vertex) {
    return std::find(group.begin(), group.end(), vertex) != group.end();
}

}  // namespace

TerminalSubsetDpResult solve_terminal_subset_objective(const ExactTopologyProblem& problem,
                                                       const TerminalSubsetDpOptions& options) {
    if (problem.bit_count > kMaximumExactHypercubeBits) {
        return abstain(ExactObjectiveState::kAbstainKernelNotVerified, ExactKernelReason::kUnsupportedBitCount,
                       "bit_count exceeds the exact 12-bit kernel boundary");
    }
    const std::size_t vertex_count = std::size_t{1} << problem.bit_count;

    std::vector<bool> mandatory(vertex_count, false);
    mandatory[0] = true;
    for (HypercubeVertex vertex : problem.mandatory_vertices) {
        if (static_cast<std::size_t>(vertex) >= vertex_count) {
            return abstain(ExactObjectiveState::kAbstainNotIdentifiable, ExactKernelReason::kMalformedProblem,
                           "mandatory vertex lies outside the declared hypercube");
        }
        mandatory[static_cast<std::size_t>(vertex)] = true;
    }

    std::vector<std::vector<HypercubeVertex>> terminal_groups;
    terminal_groups.reserve(problem.terminal_groups.size() + problem.mandatory_vertices.size());
    for (const std::vector<HypercubeVertex>& group : problem.terminal_groups) {
        if (group.empty()) {
            return abstain(ExactObjectiveState::kAbstainNotIdentifiable, ExactKernelReason::kMalformedProblem,
                           "terminal group must contain at least one vertex");
        }
        std::vector<HypercubeVertex> canonical = group;
        std::sort(canonical.begin(), canonical.end());
        canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());
        if (std::any_of(canonical.begin(), canonical.end(), [vertex_count](HypercubeVertex vertex) {
                return static_cast<std::size_t>(vertex) >= vertex_count;
            })) {
            return abstain(ExactObjectiveState::kAbstainNotIdentifiable, ExactKernelReason::kMalformedProblem,
                           "terminal group vertex lies outside the declared hypercube");
        }
        terminal_groups.push_back(std::move(canonical));
    }
    for (std::size_t vertex = 1; vertex < vertex_count; ++vertex) {
        if (mandatory[vertex]) {
            terminal_groups.push_back({static_cast<HypercubeVertex>(vertex)});
        }
    }

    const std::size_t terminal_count = terminal_groups.size();
    if (terminal_count > options.maximum_terminal_groups ||
        terminal_count >= std::numeric_limits<std::uint32_t>::digits) {
        return abstain(ExactObjectiveState::kAbstainResourceLimit, ExactKernelReason::kStateSpaceLimitReached,
                       "terminal-group count exceeds the configured subset-DP boundary");
    }
    if (terminal_count == 0) {
        TerminalSubsetDpResult result;
        result.objective_state = ExactObjectiveState::kObjectiveCertified;
        result.reason = ExactKernelReason::kNone;
        result.objective_h = 0;
        result.state_cells = vertex_count;
        result.message = "root-only objective certified";
        return result;
    }

    const std::size_t subset_count = std::size_t{1} << terminal_count;
    if (vertex_count > std::numeric_limits<std::uint64_t>::max() / subset_count) {
        return abstain(ExactObjectiveState::kAbstainResourceLimit, ExactKernelReason::kStateSpaceLimitReached,
                       "subset-DP state-cell count overflow");
    }
    const std::uint64_t state_cells =
        static_cast<std::uint64_t>(vertex_count) * static_cast<std::uint64_t>(subset_count);
    if (state_cells > options.maximum_state_cells ||
        state_cells > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return abstain(ExactObjectiveState::kAbstainResourceLimit, ExactKernelReason::kStateSpaceLimitReached,
                       "subset-DP state-cell limit reached", state_cells);
    }

    const int infinity = std::numeric_limits<int>::max() / 4;
    std::vector<int> dp(static_cast<std::size_t>(state_cells), infinity);
    const auto cell_index = [subset_count](std::size_t vertex, std::size_t subset) {
        return vertex * subset_count + subset;
    };

    // A state dp[v,S] is the minimum hidden-node weight of a directed
    // arborescence rooted at v that hits every group in S. Numeric descending
    // vertex order is a reverse topological order because every child adds one
    // bit and therefore has a larger integer value.
    for (std::size_t vertex = vertex_count; vertex-- > 0;) {
        std::uint32_t coverage = 0;
        const auto typed_vertex = static_cast<HypercubeVertex>(vertex);
        for (std::size_t terminal = 0; terminal < terminal_count; ++terminal) {
            if (contains_vertex(terminal_groups[terminal], typed_vertex)) {
                coverage |= std::uint32_t{1} << terminal;
            }
        }
        const int vertex_weight = mandatory[vertex] ? 0 : 1;

        for (std::size_t subset = 1; subset < subset_count; ++subset) {
            int best = infinity;
            const auto typed_subset = static_cast<std::uint32_t>(subset);
            if ((typed_subset & ~coverage) == 0) {
                best = vertex_weight;
            }

            for (std::size_t bit = 0; bit < problem.bit_count; ++bit) {
                const std::size_t bit_mask = std::size_t{1} << bit;
                if ((vertex & bit_mask) != 0) {
                    continue;
                }
                const std::size_t child = vertex | bit_mask;
                const int child_cost = dp[cell_index(child, subset)];
                if (child_cost != infinity) {
                    best = std::min(best, vertex_weight + child_cost);
                }
            }

            std::size_t left = (subset - 1) & subset;
            while (left != 0) {
                const std::size_t right = subset ^ left;
                if (right != 0) {
                    const int left_cost = dp[cell_index(vertex, left)];
                    const int right_cost = dp[cell_index(vertex, right)];
                    if (left_cost != infinity && right_cost != infinity) {
                        best = std::min(best, left_cost + right_cost - vertex_weight);
                    }
                }
                left = (left - 1) & subset;
            }
            dp[cell_index(vertex, subset)] = best;
        }
    }

    const int objective = dp[cell_index(0, subset_count - 1)];
    if (objective == infinity || objective < 0) {
        return abstain(ExactObjectiveState::kAbstainDifferentialMismatch, ExactKernelReason::kDifferentialMismatch,
                       "subset DP found no finite rooted objective", state_cells);
    }

    TerminalSubsetDpResult result;
    result.objective_state = ExactObjectiveState::kObjectiveCertified;
    result.reason = ExactKernelReason::kNone;
    result.objective_h = static_cast<std::uint32_t>(objective);
    result.state_cells = state_cells;
    result.message = "objective-only directed group-terminal subset DP certified h*";
    return result;
}

}  // namespace longlineage::solver
