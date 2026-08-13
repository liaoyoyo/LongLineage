// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/solver/obligation_bnb.hpp"

#include <algorithm>
#include <bitset>
#include <limits>
#include <utility>
#include <vector>

#include "longlineage/solver/parent_mapping.hpp"

namespace longlineage::solver {
namespace {

using VertexSet = std::bitset<kMaximumExactHypercubeVertices>;

struct NormalizedProblem {
    std::size_t bit_count = 0;
    std::size_t vertex_count = 0;
    VertexSet mandatory;
    std::vector<VertexSet> terminal_groups;
};

struct ObligationBuild {
    bool feasible = true;
    std::vector<VertexSet> domains;
};

bool is_subset(const VertexSet& left, const VertexSet& right) { return (left & ~right).none(); }

bool set_lexicographic_less(const VertexSet& left, const VertexSet& right, std::size_t vertex_count) {
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (left.test(vertex) != right.test(vertex)) {
            return left.test(vertex);
        }
    }
    return false;
}

std::uint32_t vertex_popcount(HypercubeVertex vertex) noexcept {
    std::uint32_t count = 0;
    while (vertex != 0) {
        vertex = static_cast<HypercubeVertex>(vertex & (vertex - 1));
        ++count;
    }
    return count;
}

VertexSet predecessor_domain(HypercubeVertex vertex, std::size_t bit_count) {
    VertexSet domain;
    for (std::size_t bit = 0; bit < bit_count; ++bit) {
        const HypercubeVertex bit_mask = static_cast<HypercubeVertex>(HypercubeVertex{1} << bit);
        if ((vertex & bit_mask) != 0) {
            domain.set(static_cast<std::size_t>(vertex ^ bit_mask));
        }
    }
    return domain;
}

bool normalize_problem(const ExactTopologyProblem& problem, NormalizedProblem& normalized, std::string& message) {
    if (problem.bit_count > kMaximumExactHypercubeBits) {
        message = "bit_count exceeds the exact 12-bit kernel boundary";
        return false;
    }
    normalized.bit_count = problem.bit_count;
    normalized.vertex_count = std::size_t{1} << problem.bit_count;
    normalized.mandatory.set(0);

    for (HypercubeVertex vertex : problem.mandatory_vertices) {
        if (static_cast<std::size_t>(vertex) >= normalized.vertex_count) {
            message = "mandatory vertex lies outside the declared hypercube";
            return false;
        }
        normalized.mandatory.set(static_cast<std::size_t>(vertex));
    }

    normalized.terminal_groups.reserve(problem.terminal_groups.size());
    for (const std::vector<HypercubeVertex>& group : problem.terminal_groups) {
        if (group.empty()) {
            message = "terminal group must contain at least one vertex";
            return false;
        }
        VertexSet domain;
        for (HypercubeVertex vertex : group) {
            if (static_cast<std::size_t>(vertex) >= normalized.vertex_count) {
                message = "terminal group vertex lies outside the declared hypercube";
                return false;
            }
            domain.set(static_cast<std::size_t>(vertex));
        }
        normalized.terminal_groups.push_back(domain);
    }
    return true;
}

void insert_antichain(std::vector<VertexSet>& antichain, const VertexSet& domain) {
    if (std::any_of(antichain.begin(), antichain.end(),
                    [&domain](const VertexSet& existing) { return is_subset(existing, domain); })) {
        return;
    }
    antichain.erase(std::remove_if(antichain.begin(), antichain.end(),
                                   [&domain](const VertexSet& existing) { return is_subset(domain, existing); }),
                    antichain.end());
    antichain.push_back(domain);
}

ObligationBuild build_obligations(const NormalizedProblem& problem, const VertexSet& selected,
                                  const VertexSet& excluded) {
    ObligationBuild result;
    for (const VertexSet& group : problem.terminal_groups) {
        if ((group & selected).any()) {
            continue;
        }
        const VertexSet available = group & ~excluded;
        if (available.none()) {
            result.feasible = false;
            result.domains.clear();
            return result;
        }
        insert_antichain(result.domains, available);
    }

    for (std::size_t index = 1; index < problem.vertex_count; ++index) {
        if (!selected.test(index)) {
            continue;
        }
        const auto vertex = static_cast<HypercubeVertex>(index);
        const VertexSet predecessors = predecessor_domain(vertex, problem.bit_count);
        if ((predecessors & selected).any()) {
            continue;
        }
        const VertexSet available = predecessors & ~excluded;
        if (available.none()) {
            result.feasible = false;
            result.domains.clear();
            return result;
        }
        insert_antichain(result.domains, available);
    }
    return result;
}

bool propagate_singletons(const NormalizedProblem& problem, VertexSet& selected, const VertexSet& excluded,
                          ObligationBuild& obligations) {
    while (true) {
        obligations = build_obligations(problem, selected, excluded);
        if (!obligations.feasible) {
            return false;
        }
        const auto singleton = std::find_if(obligations.domains.begin(), obligations.domains.end(),
                                            [](const VertexSet& domain) { return domain.count() == 1; });
        if (singleton == obligations.domains.end()) {
            return true;
        }
        for (std::size_t vertex = 0; vertex < problem.vertex_count; ++vertex) {
            if (singleton->test(vertex)) {
                selected.set(vertex);
                break;
            }
        }
    }
}

std::size_t disjoint_obligation_lower_bound(std::vector<VertexSet> domains, std::size_t vertex_count) {
    std::sort(domains.begin(), domains.end(), [vertex_count](const VertexSet& left, const VertexSet& right) {
        if (left.count() != right.count()) {
            return left.count() < right.count();
        }
        return set_lexicographic_less(left, right, vertex_count);
    });
    VertexSet occupied;
    std::size_t disjoint_count = 0;
    for (const VertexSet& domain : domains) {
        if ((domain & occupied).none()) {
            occupied |= domain;
            ++disjoint_count;
        }
    }
    return disjoint_count;
}

std::size_t connection_lower_bound(const NormalizedProblem& problem, const VertexSet& selected) {
    std::size_t maximum_missing = 0;
    for (std::size_t index = 1; index < problem.vertex_count; ++index) {
        if (!selected.test(index)) {
            continue;
        }
        const auto vertex = static_cast<HypercubeVertex>(index);
        if ((predecessor_domain(vertex, problem.bit_count) & selected).any()) {
            continue;
        }

        std::uint32_t minimum_distance = vertex_popcount(vertex);
        HypercubeVertex subset = static_cast<HypercubeVertex>((vertex - 1) & vertex);
        while (true) {
            if (selected.test(static_cast<std::size_t>(subset))) {
                const std::uint32_t distance = vertex_popcount(static_cast<HypercubeVertex>(vertex ^ subset));
                minimum_distance = std::min(minimum_distance, distance);
            }
            if (subset == 0) {
                break;
            }
            subset = static_cast<HypercubeVertex>((subset - 1) & vertex);
        }
        if (minimum_distance > 0) {
            maximum_missing = std::max(maximum_missing, static_cast<std::size_t>(minimum_distance - 1));
        }
    }
    return maximum_missing;
}

std::vector<HypercubeVertex> materialize_vertices(const VertexSet& selected, std::size_t vertex_count) {
    std::vector<HypercubeVertex> vertices;
    vertices.reserve(selected.count());
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (selected.test(vertex)) {
            vertices.push_back(static_cast<HypercubeVertex>(vertex));
        }
    }
    return vertices;
}

class ExactSearch {
   public:
    ExactSearch(const NormalizedProblem& problem, const ObligationBnbOptions& options)
        : problem_(problem), options_(options) {}

    void run() {
        VertexSet selected = problem_.mandatory;
        VertexSet excluded;
        visit(std::move(selected), std::move(excluded));
    }

    bool aborted() const noexcept { return aborted_; }
    bool has_incumbent() const noexcept { return incumbent_ != std::numeric_limits<std::size_t>::max(); }
    std::size_t incumbent() const noexcept { return incumbent_; }
    bool family_overflow() const noexcept { return family_overflow_; }
    std::uint64_t search_nodes() const noexcept { return search_nodes_; }
    std::vector<std::vector<HypercubeVertex>> take_family() {
        std::sort(optimal_family_.begin(), optimal_family_.end());
        optimal_family_.erase(std::unique(optimal_family_.begin(), optimal_family_.end()), optimal_family_.end());
        return std::move(optimal_family_);
    }

   private:
    void record_solution(const VertexSet& selected, std::size_t hidden) {
        if (hidden > incumbent_) {
            return;
        }
        const std::vector<HypercubeVertex> vertices = materialize_vertices(selected, problem_.vertex_count);
        if (hidden < incumbent_) {
            incumbent_ = hidden;
            family_overflow_ = false;
            optimal_family_.clear();
        }
        if (std::find(optimal_family_.begin(), optimal_family_.end(), vertices) != optimal_family_.end()) {
            return;
        }
        if (options_.maximum_complete_family_size != 0 &&
            optimal_family_.size() >= options_.maximum_complete_family_size) {
            family_overflow_ = true;
            return;
        }
        optimal_family_.push_back(vertices);
    }

    void visit(VertexSet selected, VertexSet excluded) {
        if (aborted_) {
            return;
        }
        if (options_.maximum_search_nodes != 0 && search_nodes_ >= options_.maximum_search_nodes) {
            aborted_ = true;
            return;
        }
        ++search_nodes_;

        ObligationBuild obligations;
        if (!propagate_singletons(problem_, selected, excluded, obligations)) {
            return;
        }
        const std::size_t hidden = (selected & ~problem_.mandatory).count();
        if (hidden > incumbent_) {
            return;
        }
        if (obligations.domains.empty()) {
            record_solution(selected, hidden);
            return;
        }

        const std::size_t obligation_bound =
            disjoint_obligation_lower_bound(obligations.domains, problem_.vertex_count);
        const std::size_t connectivity_bound = connection_lower_bound(problem_, selected);
        const std::size_t lower_bound = hidden + std::max(obligation_bound, connectivity_bound);
        if (lower_bound > incumbent_) {
            return;
        }

        const auto selected_obligation =
            std::min_element(obligations.domains.begin(), obligations.domains.end(),
                             [this](const VertexSet& left, const VertexSet& right) {
                                 if (left.count() != right.count()) {
                                     return left.count() < right.count();
                                 }
                                 return set_lexicographic_less(left, right, problem_.vertex_count);
                             });
        VertexSet prefix_excluded = excluded;
        for (std::size_t vertex = 0; vertex < problem_.vertex_count; ++vertex) {
            if (!selected_obligation->test(vertex)) {
                continue;
            }
            VertexSet branch_selected = selected;
            branch_selected.set(vertex);
            visit(std::move(branch_selected), prefix_excluded);
            prefix_excluded.set(vertex);
            if (aborted_) {
                return;
            }
        }
    }

    const NormalizedProblem& problem_;
    const ObligationBnbOptions& options_;
    std::size_t incumbent_ = std::numeric_limits<std::size_t>::max();
    bool family_overflow_ = false;
    bool aborted_ = false;
    std::uint64_t search_nodes_ = 0;
    std::vector<std::vector<HypercubeVertex>> optimal_family_;
};

ExactStructuralResult invalid_result(ExactKernelReason reason, std::string message) {
    ExactStructuralResult result;
    result.objective_state = ExactObjectiveState::kAbstainNotIdentifiable;
    result.family_state = ExactFamilyState::kAbstainNotIdentifiable;
    result.reason = reason;
    result.message = std::move(message);
    return result;
}

}  // namespace

const char* to_string(ExactObjectiveState state) noexcept {
    switch (state) {
        case ExactObjectiveState::kObjectiveCertified:
            return "OBJECTIVE_CERTIFIED";
        case ExactObjectiveState::kAbstainKernelNotVerified:
            return "ABSTAIN_KERNEL_NOT_VERIFIED";
        case ExactObjectiveState::kAbstainNotIdentifiable:
            return "ABSTAIN_NOT_IDENTIFIABLE";
        case ExactObjectiveState::kAbstainResourceLimit:
            return "ABSTAIN_RESOURCE_LIMIT";
        case ExactObjectiveState::kAbstainDifferentialMismatch:
            return "ABSTAIN_DIFFERENTIAL_MISMATCH";
    }
    return "UNKNOWN";
}

const char* to_string(ExactFamilyState state) noexcept {
    switch (state) {
        case ExactFamilyState::kFamilyComplete:
            return "FAMILY_COMPLETE";
        case ExactFamilyState::kFamilyIncompleteCap:
            return "FAMILY_INCOMPLETE_CAP";
        case ExactFamilyState::kAbstainKernelNotVerified:
            return "ABSTAIN_KERNEL_NOT_VERIFIED";
        case ExactFamilyState::kAbstainNotIdentifiable:
            return "ABSTAIN_NOT_IDENTIFIABLE";
        case ExactFamilyState::kAbstainResourceLimit:
            return "ABSTAIN_RESOURCE_LIMIT";
        case ExactFamilyState::kAbstainDifferentialMismatch:
            return "ABSTAIN_DIFFERENTIAL_MISMATCH";
    }
    return "UNKNOWN";
}

const char* to_string(ExactKernelReason reason) noexcept {
    switch (reason) {
        case ExactKernelReason::kNone:
            return "NONE";
        case ExactKernelReason::kMalformedProblem:
            return "MALFORMED_PROBLEM";
        case ExactKernelReason::kUnsupportedBitCount:
            return "UNSUPPORTED_BIT_COUNT";
        case ExactKernelReason::kFamilySizeLimitReached:
            return "FAMILY_SIZE_LIMIT_REACHED";
        case ExactKernelReason::kSearchNodeLimitReached:
            return "SEARCH_NODE_LIMIT_REACHED";
        case ExactKernelReason::kStateSpaceLimitReached:
            return "STATE_SPACE_LIMIT_REACHED";
        case ExactKernelReason::kDifferentialMismatch:
            return "DIFFERENTIAL_MISMATCH";
        case ExactKernelReason::kKernelNotVerified:
            return "KERNEL_NOT_VERIFIED";
    }
    return "UNKNOWN";
}

ExactStructuralResult solve_obligation_bnb(const ExactTopologyProblem& problem, const ObligationBnbOptions& options) {
    NormalizedProblem normalized;
    std::string validation_message;
    if (!normalize_problem(problem, normalized, validation_message)) {
        if (problem.bit_count > kMaximumExactHypercubeBits) {
            ExactStructuralResult result;
            result.objective_state = ExactObjectiveState::kAbstainKernelNotVerified;
            result.family_state = ExactFamilyState::kAbstainKernelNotVerified;
            result.reason = ExactKernelReason::kUnsupportedBitCount;
            result.message = std::move(validation_message);
            return result;
        }
        return invalid_result(ExactKernelReason::kMalformedProblem, std::move(validation_message));
    }

    ExactSearch search(normalized, options);
    search.run();

    ExactStructuralResult result;
    result.search_nodes = search.search_nodes();
    if (search.aborted()) {
        result.objective_state = ExactObjectiveState::kAbstainResourceLimit;
        result.family_state = ExactFamilyState::kAbstainResourceLimit;
        result.reason = ExactKernelReason::kSearchNodeLimitReached;
        result.message = "search-node limit reached; objective and family withheld";
        return result;
    }
    if (!search.has_incumbent()) {
        return invalid_result(ExactKernelReason::kMalformedProblem,
                              "no rooted family satisfies the declared terminal groups");
    }

    result.objective_state = ExactObjectiveState::kObjectiveCertified;
    result.objective_h = static_cast<std::uint32_t>(search.incumbent());
    result.objective_search_exhausted = true;
    if (search.family_overflow()) {
        result.family_state = ExactFamilyState::kFamilyIncompleteCap;
        result.reason = ExactKernelReason::kFamilySizeLimitReached;
        result.family_enumeration_exhausted = false;
        result.message =
            "h* certified after exhaustive search; explicit minimum family "
            "withheld because its configured size limit was exceeded";
        return result;
    }

    std::vector<std::vector<HypercubeVertex>> family = search.take_family();
    result.minimum_family.reserve(family.size());
    for (std::vector<HypercubeVertex>& vertices : family) {
        ExactStructuralCandidate candidate;
        candidate.vertices = std::move(vertices);
        candidate.parent_mapping = summarize_exact_parent_mappings(normalized.bit_count, candidate.vertices);
        if (!candidate.parent_mapping.valid) {
            return invalid_result(ExactKernelReason::kDifferentialMismatch,
                                  "internal exact family failed legal-parent factorization");
        }
        result.minimum_family.push_back(std::move(candidate));
    }
    result.family_state = ExactFamilyState::kFamilyComplete;
    result.reason = ExactKernelReason::kNone;
    result.family_enumeration_exhausted = true;
    result.message =
        "exact dynamic-obligation B&B exhausted all branches with "
        "lower_bound <= h*";
    return result;
}

}  // namespace longlineage::solver
