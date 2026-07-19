// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace longlineage::solver {

enum class TopologyObjectiveStatus {
    kObjectiveCertified,
    kAbstainDependencyUnavailable,
    kAbstainKernelNotVerified,
    kAbstainNotIdentifiable,
};

enum class TopologyFamilyStatus {
    kFamilyComplete,
    kFamilyIncompleteCap,
    kFamilyIncompleteDeadline,
    kAbstainDependencyUnavailable,
    kAbstainKernelNotVerified,
    kAbstainNotIdentifiable,
};

enum class TopologyReason {
    kNone,
    kMaxFamilySizeReached,
    kDeadlineReached,
    kSolverRouteNotVerified,
    kMalformedValue,
    kUnsupportedValue,
};

const char* to_string(TopologyObjectiveStatus status) noexcept;
const char* to_string(TopologyFamilyStatus status) noexcept;
const char* to_string(TopologyReason reason) noexcept;

struct TopologyProblem {
    std::size_t k = 0;
    std::vector<std::string> full_patterns;
    std::vector<std::string> partial_patterns;
};

struct LegalParentChoices {
    std::uint64_t vertex = 0;
    std::vector<std::uint64_t> parents;
};

struct MinimumVertexSet {
    // Original-locus bit masks, sorted ascending. Root state 0 is included.
    std::vector<std::uint64_t> vertices;
    std::vector<LegalParentChoices> legal_parents;
    std::uint64_t legal_parent_count = 0;
    std::uint64_t tree_count = 0;
};

struct SmallQOracleResult {
    TopologyObjectiveStatus objective_status = TopologyObjectiveStatus::kAbstainNotIdentifiable;
    TopologyFamilyStatus family_status = TopologyFamilyStatus::kAbstainNotIdentifiable;
    TopologyReason reason = TopologyReason::kNone;
    std::optional<std::uint32_t> minimum_hidden_vertices;
    std::vector<std::size_t> active_loci;
    std::vector<MinimumVertexSet> minimum_family;

    // Structural evidence may expose a winner only when both the complete
    // minimum vertex family and its legal parent mapping are unique. Incomplete,
    // abstaining, vertex-set-tied, or parent-mapping-tied results leave this null.
    std::optional<std::size_t> winner_index;
    std::string message;
};

// Exhaustive structural oracle for the observed-ALT compressed hypercube.
//
// q is the number of loci with at least one observed ALT across full and partial
// patterns. q <= 4 is exhaustively enumerated. This is a bounded P5 oracle, not
// the production bitset B&B/HiGHS router. Until that kernel is independently
// verified, q > 4 deliberately abstains with ABSTAIN_KERNEL_NOT_VERIFIED and
// SOLVER_ROUTE_NOT_VERIFIED; it never emits a partial family or winner.
SmallQOracleResult solve_small_q_exact(const TopologyProblem& problem);

}  // namespace longlineage::solver
