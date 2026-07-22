// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace longlineage::solver {

using HypercubeVertex = std::uint16_t;

constexpr std::size_t kMaximumExactHypercubeBits = 12;
constexpr std::size_t kMaximumExactHypercubeVertices = std::size_t{1} << kMaximumExactHypercubeBits;

enum class ExactObjectiveState {
    kObjectiveCertified,
    kAbstainKernelNotVerified,
    kAbstainNotIdentifiable,
    kAbstainResourceLimit,
    kAbstainDifferentialMismatch,
};

enum class ExactFamilyState {
    kFamilyComplete,
    kFamilyIncompleteCap,
    kAbstainKernelNotVerified,
    kAbstainNotIdentifiable,
    kAbstainResourceLimit,
    kAbstainDifferentialMismatch,
};

enum class ExactKernelReason {
    kNone,
    kMalformedProblem,
    kUnsupportedBitCount,
    kFamilySizeLimitReached,
    kSearchNodeLimitReached,
    kStateSpaceLimitReached,
    kDifferentialMismatch,
    kKernelNotVerified,
};

const char* to_string(ExactObjectiveState state) noexcept;
const char* to_string(ExactFamilyState state) noexcept;
const char* to_string(ExactKernelReason reason) noexcept;

// A normalized recurrence-allowed hypercube problem. Vertex 0 is always added
// to the mandatory set by the exact solvers. Each terminal group is an
// existential obligation: at least one member must be selected.
struct ExactTopologyProblem {
    std::size_t bit_count = 0;
    std::vector<HypercubeVertex> mandatory_vertices;
    std::vector<std::vector<HypercubeVertex>> terminal_groups;
};

struct ExactLegalParentChoices {
    HypercubeVertex vertex = 0;
    std::vector<HypercubeVertex> parents;
};

struct ExactParentMappingSummary {
    bool valid = false;
    std::vector<ExactLegalParentChoices> legal_parents;
    std::string legal_parent_count = "0";
    std::string tree_count = "0";
    std::string message;
};

struct ExactStructuralCandidate {
    std::vector<HypercubeVertex> vertices;
    ExactParentMappingSummary parent_mapping;
};

struct ExactStructuralResult {
    ExactObjectiveState objective_state = ExactObjectiveState::kAbstainNotIdentifiable;
    ExactFamilyState family_state = ExactFamilyState::kAbstainNotIdentifiable;
    ExactKernelReason reason = ExactKernelReason::kMalformedProblem;
    std::optional<std::uint32_t> objective_h;
    std::vector<ExactStructuralCandidate> minimum_family;
    std::uint64_t search_nodes = 0;
    bool objective_search_exhausted = false;
    bool family_enumeration_exhausted = false;
    std::string message;
};

}  // namespace longlineage::solver
