// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/solver/topology_router.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace longlineage::solver {
namespace {

struct NormalizationResult {
    bool valid = false;
    bool within_exact_boundary = false;
    ExactTopologyProblem problem;
    std::vector<std::size_t> active_loci;
    std::string message;
};

bool is_full_symbol(char symbol) noexcept { return symbol == 'R' || symbol == 'A'; }

bool is_partial_symbol(char symbol) noexcept { return symbol == 'R' || symbol == 'A' || symbol == 'X'; }

HypercubeVertex compress_pattern(const std::string& pattern, const std::vector<std::size_t>& active_loci) {
    HypercubeVertex vertex = 0;
    for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
        if (pattern[active_loci[bit]] == 'A') {
            vertex = static_cast<HypercubeVertex>(vertex | static_cast<HypercubeVertex>(HypercubeVertex{1} << bit));
        }
    }
    return vertex;
}

bool compatible(const std::string& partial, HypercubeVertex vertex, const std::vector<std::size_t>& active_loci) {
    for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
        const char symbol = partial[active_loci[bit]];
        const bool alternate = (vertex & static_cast<HypercubeVertex>(HypercubeVertex{1} << bit)) != 0;
        if ((symbol == 'A' && !alternate) || (symbol == 'R' && alternate)) {
            return false;
        }
    }
    return true;
}

NormalizationResult normalize(const TopologyProblem& input) {
    NormalizationResult result;
    if (input.k == 0 || input.k > 64) {
        result.message = "k must be in [1,64]";
        return result;
    }
    for (const std::string& pattern : input.full_patterns) {
        if (pattern.size() != input.k || !std::all_of(pattern.begin(), pattern.end(), is_full_symbol)) {
            result.message = "full pattern has wrong length or a symbol outside R/A";
            return result;
        }
    }
    for (const std::string& pattern : input.partial_patterns) {
        if (pattern.size() != input.k || !std::all_of(pattern.begin(), pattern.end(), is_partial_symbol)) {
            result.message = "partial pattern has wrong length or a symbol outside R/A/X";
            return result;
        }
    }

    for (std::size_t locus = 0; locus < input.k; ++locus) {
        bool has_alternate = false;
        for (const std::string& pattern : input.full_patterns) {
            has_alternate = has_alternate || pattern[locus] == 'A';
        }
        for (const std::string& pattern : input.partial_patterns) {
            has_alternate = has_alternate || pattern[locus] == 'A';
        }
        if (has_alternate) {
            result.active_loci.push_back(locus);
        }
    }

    result.valid = true;
    if (result.active_loci.size() > kMaximumExactHypercubeBits) {
        result.message = "active ALT dimension exceeds the exact 12-bit kernel boundary";
        return result;
    }
    result.within_exact_boundary = true;
    result.problem.bit_count = result.active_loci.size();
    result.problem.mandatory_vertices.reserve(input.full_patterns.size());
    for (const std::string& pattern : input.full_patterns) {
        result.problem.mandatory_vertices.push_back(compress_pattern(pattern, result.active_loci));
    }
    std::sort(result.problem.mandatory_vertices.begin(), result.problem.mandatory_vertices.end());
    result.problem.mandatory_vertices.erase(
        std::unique(result.problem.mandatory_vertices.begin(), result.problem.mandatory_vertices.end()),
        result.problem.mandatory_vertices.end());

    const std::size_t vertex_count = std::size_t{1} << result.active_loci.size();
    result.problem.terminal_groups.reserve(input.partial_patterns.size());
    for (const std::string& partial : input.partial_patterns) {
        std::vector<HypercubeVertex> group;
        for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
            const auto typed_vertex = static_cast<HypercubeVertex>(vertex);
            if (compatible(partial, typed_vertex, result.active_loci)) {
                group.push_back(typed_vertex);
            }
        }
        if (group.empty()) {
            result.valid = false;
            result.within_exact_boundary = false;
            result.message = "partial pattern has an empty active-subcube domain";
            return result;
        }
        result.problem.terminal_groups.push_back(std::move(group));
    }
    result.message = "R/A/X input normalized onto active ALT bits";
    return result;
}

HypercubeVertex compress_expanded_vertex(std::uint64_t expanded, const std::vector<std::size_t>& active_loci) {
    HypercubeVertex compressed = 0;
    for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
        const std::uint64_t original_mask = std::uint64_t{1} << active_loci[bit];
        if ((expanded & original_mask) != 0) {
            compressed =
                static_cast<HypercubeVertex>(compressed | static_cast<HypercubeVertex>(HypercubeVertex{1} << bit));
        }
    }
    return compressed;
}

std::vector<std::vector<HypercubeVertex>> oracle_family(const SmallQOracleResult& oracle,
                                                        const std::vector<std::size_t>& active_loci) {
    std::vector<std::vector<HypercubeVertex>> family;
    family.reserve(oracle.minimum_family.size());
    for (const MinimumVertexSet& candidate : oracle.minimum_family) {
        std::vector<HypercubeVertex> vertices;
        vertices.reserve(candidate.vertices.size());
        for (std::uint64_t expanded : candidate.vertices) {
            vertices.push_back(compress_expanded_vertex(expanded, active_loci));
        }
        std::sort(vertices.begin(), vertices.end());
        family.push_back(std::move(vertices));
    }
    std::sort(family.begin(), family.end());
    return family;
}

std::vector<std::vector<HypercubeVertex>> structural_family(const ExactStructuralResult& structural) {
    std::vector<std::vector<HypercubeVertex>> family;
    family.reserve(structural.minimum_family.size());
    for (const ExactStructuralCandidate& candidate : structural.minimum_family) {
        family.push_back(candidate.vertices);
    }
    std::sort(family.begin(), family.end());
    return family;
}

void fail_differential(TopologyRouterResult& result, const std::string& message) {
    result.route = ExactSolverRoute::kAbstain;
    result.structural.objective_state = ExactObjectiveState::kAbstainDifferentialMismatch;
    result.structural.family_state = ExactFamilyState::kAbstainDifferentialMismatch;
    result.structural.reason = ExactKernelReason::kDifferentialMismatch;
    result.structural.objective_h.reset();
    result.structural.minimum_family.clear();
    result.structural.objective_search_exhausted = false;
    result.structural.family_enumeration_exhausted = false;
    result.structural.message = message;
    result.message = message;
}

}  // namespace

TopologyRouterResult solve_topology_exact(const TopologyProblem& problem, const TopologyRouterOptions& options) {
    TopologyRouterResult result;
    NormalizationResult normalized = normalize(problem);
    result.active_loci = normalized.active_loci;
    if (!normalized.valid) {
        result.structural.objective_state = ExactObjectiveState::kAbstainNotIdentifiable;
        result.structural.family_state = ExactFamilyState::kAbstainNotIdentifiable;
        result.structural.reason = ExactKernelReason::kMalformedProblem;
        result.structural.message = normalized.message;
        result.message = normalized.message;
        return result;
    }
    if (!normalized.within_exact_boundary) {
        result.structural.objective_state = ExactObjectiveState::kAbstainKernelNotVerified;
        result.structural.family_state = ExactFamilyState::kAbstainKernelNotVerified;
        result.structural.reason = ExactKernelReason::kUnsupportedBitCount;
        result.structural.message = normalized.message;
        result.message = normalized.message;
        return result;
    }

    result.structural = solve_obligation_bnb(normalized.problem, options.bnb);
    result.route = normalized.problem.bit_count <= 4 ? ExactSolverRoute::kSmallQOracleDifferentialBnb
                                                     : ExactSolverRoute::kBitsetObligationBnb;

    if (result.structural.objective_state != ExactObjectiveState::kObjectiveCertified) {
        result.message = "exact B&B abstained before a differential result was available";
        return result;
    }

    if (normalized.problem.bit_count <= 4) {
        result.small_q_oracle_checked = true;
        const SmallQOracleResult oracle = solve_small_q_exact(problem);
        const bool objective_match = oracle.objective_status == TopologyObjectiveStatus::kObjectiveCertified &&
                                     result.structural.objective_state == ExactObjectiveState::kObjectiveCertified &&
                                     oracle.minimum_hidden_vertices == result.structural.objective_h;
        bool family_match = true;
        if (result.structural.family_state == ExactFamilyState::kFamilyComplete) {
            family_match = oracle.family_status == TopologyFamilyStatus::kFamilyComplete &&
                           oracle_family(oracle, result.active_loci) == structural_family(result.structural);
        }
        result.small_q_oracle_match = objective_match && family_match;
        if (!result.small_q_oracle_match) {
            fail_differential(result,
                              "q<=4 exhaustive oracle and obligation B&B disagree; "
                              "all structural outputs withheld");
            return result;
        }
    }

    if (options.run_objective_dp_crosscheck &&
        result.structural.objective_state == ExactObjectiveState::kObjectiveCertified) {
        const TerminalSubsetDpResult dp = solve_terminal_subset_objective(normalized.problem, options.dp);
        if (dp.objective_state == ExactObjectiveState::kObjectiveCertified) {
            result.objective_dp_checked = true;
            result.objective_dp_match = dp.objective_h == result.structural.objective_h;
            if (!result.objective_dp_match) {
                fail_differential(result,
                                  "objective-only subset DP and obligation B&B disagree; "
                                  "all structural outputs withheld");
                return result;
            }
        }
    }

    result.message =
        "exact structural route completed; abundance ranking and edge "
        "selection remain separate endpoints";
    return result;
}

}  // namespace longlineage::solver
