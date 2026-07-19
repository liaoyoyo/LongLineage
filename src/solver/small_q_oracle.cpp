// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/solver/small_q_oracle.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace longlineage::solver {
namespace {

constexpr std::size_t kMaximumExhaustiveQ = 4;
constexpr std::size_t kMaximumRepresentableLoci = 64;

bool is_full_symbol(char symbol) noexcept { return symbol == 'R' || symbol == 'A'; }

bool is_partial_symbol(char symbol) noexcept { return symbol == 'R' || symbol == 'A' || symbol == 'X'; }

SmallQOracleResult invalid(TopologyReason reason, std::string message) {
    SmallQOracleResult result;
    result.objective_status = TopologyObjectiveStatus::kAbstainNotIdentifiable;
    result.family_status = TopologyFamilyStatus::kAbstainNotIdentifiable;
    result.reason = reason;
    result.message = std::move(message);
    return result;
}

std::uint32_t popcount(std::uint32_t value) noexcept {
    std::uint32_t count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

std::uint64_t expand_vertex(std::uint32_t compressed, const std::vector<std::size_t>& active_loci) {
    std::uint64_t expanded = 0;
    for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
        if ((compressed & (std::uint32_t{1} << bit)) != 0) {
            expanded |= std::uint64_t{1} << active_loci[bit];
        }
    }
    return expanded;
}

std::uint32_t compress_pattern(const std::string& pattern, const std::vector<std::size_t>& active_loci) {
    std::uint32_t compressed = 0;
    for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
        if (pattern[active_loci[bit]] == 'A') {
            compressed |= std::uint32_t{1} << bit;
        }
    }
    return compressed;
}

bool compatible(const std::string& partial, std::uint32_t vertex,
                const std::vector<std::size_t>& active_loci) noexcept {
    for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
        const char symbol = partial[active_loci[bit]];
        const bool alt = (vertex & (std::uint32_t{1} << bit)) != 0;
        if ((symbol == 'A' && !alt) || (symbol == 'R' && alt)) {
            return false;
        }
    }
    return true;
}

bool is_feasible(std::uint32_t selection, std::uint32_t vertex_count, std::uint32_t mandatory_selection,
                 const std::vector<std::string>& partial_patterns,
                 const std::vector<std::size_t>& active_loci) noexcept {
    if ((selection & mandatory_selection) != mandatory_selection) {
        return false;
    }

    for (std::uint32_t vertex = 1; vertex < vertex_count; ++vertex) {
        if ((selection & (std::uint32_t{1} << vertex)) == 0) {
            continue;
        }
        bool has_parent = false;
        for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
            if ((vertex & (std::uint32_t{1} << bit)) == 0) {
                continue;
            }
            const std::uint32_t parent = vertex ^ (std::uint32_t{1} << bit);
            if ((selection & (std::uint32_t{1} << parent)) != 0) {
                has_parent = true;
                break;
            }
        }
        if (!has_parent) {
            return false;
        }
    }

    for (const std::string& partial : partial_patterns) {
        bool hit = false;
        for (std::uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
            if ((selection & (std::uint32_t{1} << vertex)) != 0 && compatible(partial, vertex, active_loci)) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    return true;
}

MinimumVertexSet materialize_family(std::uint32_t selection, std::uint32_t vertex_count,
                                    const std::vector<std::size_t>& active_loci) {
    MinimumVertexSet family;
    std::set<std::uint32_t> selected;
    for (std::uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
        if ((selection & (std::uint32_t{1} << vertex)) != 0) {
            selected.insert(vertex);
            family.vertices.push_back(expand_vertex(vertex, active_loci));
        }
    }

    family.tree_count = 1;
    for (std::uint32_t vertex : selected) {
        if (vertex == 0) {
            continue;
        }
        LegalParentChoices choices;
        choices.vertex = expand_vertex(vertex, active_loci);
        for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
            if ((vertex & (std::uint32_t{1} << bit)) == 0) {
                continue;
            }
            const std::uint32_t parent = vertex ^ (std::uint32_t{1} << bit);
            if (selected.count(parent) != 0) {
                choices.parents.push_back(expand_vertex(parent, active_loci));
            }
        }
        std::sort(choices.parents.begin(), choices.parents.end());
        if (choices.parents.empty()) {
            throw std::logic_error("feasible family contains a vertex without a legal parent");
        }
        family.legal_parent_count += static_cast<std::uint64_t>(choices.parents.size());
        if (family.tree_count > std::numeric_limits<std::uint64_t>::max() / choices.parents.size()) {
            throw std::overflow_error("legal parent mapping count overflow");
        }
        family.tree_count *= static_cast<std::uint64_t>(choices.parents.size());
        family.legal_parents.push_back(std::move(choices));
    }
    return family;
}

}  // namespace

const char* to_string(TopologyObjectiveStatus status) noexcept {
    switch (status) {
        case TopologyObjectiveStatus::kObjectiveCertified:
            return "OBJECTIVE_CERTIFIED";
        case TopologyObjectiveStatus::kAbstainDependencyUnavailable:
            return "ABSTAIN_DEPENDENCY_UNAVAILABLE";
        case TopologyObjectiveStatus::kAbstainKernelNotVerified:
            return "ABSTAIN_KERNEL_NOT_VERIFIED";
        case TopologyObjectiveStatus::kAbstainNotIdentifiable:
            return "ABSTAIN_NOT_IDENTIFIABLE";
    }
    return "UNKNOWN";
}

const char* to_string(TopologyFamilyStatus status) noexcept {
    switch (status) {
        case TopologyFamilyStatus::kFamilyComplete:
            return "FAMILY_COMPLETE";
        case TopologyFamilyStatus::kFamilyIncompleteCap:
            return "FAMILY_INCOMPLETE_CAP";
        case TopologyFamilyStatus::kFamilyIncompleteDeadline:
            return "FAMILY_INCOMPLETE_DEADLINE";
        case TopologyFamilyStatus::kAbstainDependencyUnavailable:
            return "ABSTAIN_DEPENDENCY_UNAVAILABLE";
        case TopologyFamilyStatus::kAbstainKernelNotVerified:
            return "ABSTAIN_KERNEL_NOT_VERIFIED";
        case TopologyFamilyStatus::kAbstainNotIdentifiable:
            return "ABSTAIN_NOT_IDENTIFIABLE";
    }
    return "UNKNOWN";
}

const char* to_string(TopologyReason reason) noexcept {
    switch (reason) {
        case TopologyReason::kNone:
            return "NONE";
        case TopologyReason::kMaxFamilySizeReached:
            return "MAX_FAMILY_SIZE_REACHED";
        case TopologyReason::kDeadlineReached:
            return "DEADLINE_REACHED";
        case TopologyReason::kSolverRouteNotVerified:
            return "SOLVER_ROUTE_NOT_VERIFIED";
        case TopologyReason::kMalformedValue:
            return "MALFORMED_VALUE";
        case TopologyReason::kUnsupportedValue:
            return "UNSUPPORTED_VALUE";
    }
    return "UNKNOWN";
}

SmallQOracleResult solve_small_q_exact(const TopologyProblem& problem) {
    if (problem.k == 0 || problem.k > kMaximumRepresentableLoci) {
        return invalid(TopologyReason::kUnsupportedValue, "k must be in [1,64]");
    }
    for (const std::string& pattern : problem.full_patterns) {
        if (pattern.size() != problem.k) {
            return invalid(TopologyReason::kMalformedValue, "full pattern length differs from k");
        }
        if (!std::all_of(pattern.begin(), pattern.end(), is_full_symbol)) {
            return invalid(TopologyReason::kMalformedValue, "full pattern contains a symbol outside R/A");
        }
    }
    for (const std::string& pattern : problem.partial_patterns) {
        if (pattern.size() != problem.k) {
            return invalid(TopologyReason::kMalformedValue, "partial pattern length differs from k");
        }
        if (!std::all_of(pattern.begin(), pattern.end(), is_partial_symbol)) {
            return invalid(TopologyReason::kMalformedValue, "partial pattern contains a symbol outside R/A/X");
        }
    }

    std::vector<std::size_t> active_loci;
    active_loci.reserve(problem.k);
    for (std::size_t locus = 0; locus < problem.k; ++locus) {
        bool observed_alt = false;
        for (const std::string& pattern : problem.full_patterns) {
            observed_alt = observed_alt || pattern[locus] == 'A';
        }
        for (const std::string& pattern : problem.partial_patterns) {
            observed_alt = observed_alt || pattern[locus] == 'A';
        }
        if (observed_alt) {
            active_loci.push_back(locus);
        }
    }

    if (active_loci.size() > kMaximumExhaustiveQ) {
        SmallQOracleResult result;
        result.objective_status = TopologyObjectiveStatus::kAbstainKernelNotVerified;
        result.family_status = TopologyFamilyStatus::kAbstainKernelNotVerified;
        result.reason = TopologyReason::kSolverRouteNotVerified;
        result.active_loci = std::move(active_loci);
        result.message = "P5 incomplete: q > 4 bitset B&B/direct-HiGHS router is not independently verified";
        return result;
    }

    const std::uint32_t vertex_count = std::uint32_t{1} << active_loci.size();
    std::uint32_t mandatory_selection = 1;  // Root is always mandatory.
    for (const std::string& pattern : problem.full_patterns) {
        const std::uint32_t vertex = compress_pattern(pattern, active_loci);
        mandatory_selection |= std::uint32_t{1} << vertex;
    }

    const std::uint32_t selection_count = std::uint32_t{1} << vertex_count;
    std::uint32_t minimum_hidden = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> optimal_selections;
    for (std::uint32_t selection = 0; selection < selection_count; ++selection) {
        if (!is_feasible(selection, vertex_count, mandatory_selection, problem.partial_patterns, active_loci)) {
            continue;
        }
        const std::uint32_t hidden = popcount(selection & ~mandatory_selection);
        if (hidden < minimum_hidden) {
            minimum_hidden = hidden;
            optimal_selections.clear();
        }
        if (hidden == minimum_hidden) {
            optimal_selections.push_back(selection);
        }
    }

    if (optimal_selections.empty()) {
        return invalid(TopologyReason::kMalformedValue,
                       "no feasible rooted family exists for the declared pattern constraints");
    }

    SmallQOracleResult result;
    result.objective_status = TopologyObjectiveStatus::kObjectiveCertified;
    result.family_status = TopologyFamilyStatus::kFamilyComplete;
    result.reason = TopologyReason::kNone;
    result.minimum_hidden_vertices = minimum_hidden;
    result.active_loci = std::move(active_loci);
    result.minimum_family.reserve(optimal_selections.size());
    for (std::uint32_t selection : optimal_selections) {
        result.minimum_family.push_back(materialize_family(selection, vertex_count, result.active_loci));
    }
    std::sort(
        result.minimum_family.begin(), result.minimum_family.end(),
        [](const MinimumVertexSet& left, const MinimumVertexSet& right) { return left.vertices < right.vertices; });
    if (result.minimum_family.size() == 1 && result.minimum_family.front().tree_count == 1) {
        result.winner_index = 0;
    }
    result.message = "complete exhaustive q<=4 family";
    return result;
}

}  // namespace longlineage::solver
