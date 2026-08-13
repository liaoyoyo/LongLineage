// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "longlineage/solver/evidence_builder.hpp"
#include "longlineage/solver/topology_types.hpp"

namespace longlineage::solver {

enum class FamilyRankingGate {
    kUseStructuralState,
    kForceIncompleteDeadline,
};

enum class VertexSetRankingState {
    kRankingComplete,
    kNotRunFamilyIncompleteCap,
    kNotRunFamilyIncompleteDeadline,
    kNotRunObjectiveAbstain,
    kAbstainMalformedEvidence,
    kAbstainNumericalCertificate,
};

struct VertexSetRankerOptions {
    FamilyRankingGate family_gate = FamilyRankingGate::kUseStructuralState;
    double tie_tolerance = 1e-8;
    double global_gap_tolerance = 1e-8;
    std::size_t maximum_iterations = 50000;
};

struct CandidateVertexSetScore {
    std::string vertex_set_sha256;
    std::vector<HypercubeVertex> vertices;
    double log_likelihood = 0.0;
    std::vector<double> mixture_weights;
    double global_log_likelihood_gap_bound = 0.0;
    std::size_t iterations = 0;
    bool converged = false;
};

struct VertexSetRankingResult {
    VertexSetRankingState state = VertexSetRankingState::kAbstainMalformedEvidence;
    std::vector<CandidateVertexSetScore> candidate_scores;
    std::vector<std::string> best_vertex_set_tie_class;
    std::optional<double> best_log_likelihood;
    std::size_t evaluated_vertex_set_count = 0;
    std::string ranking_evidence_sha256;
    std::string certificate_sha256;
    std::string message;
};

std::string exact_vertex_set_sha256(std::size_t bit_count, const std::vector<HypercubeVertex>& vertices);

// Exhaustively evaluate each distinct vertex set exactly once. The fitted
// simplex mixture uses only BQ read-pattern emissions; parent choices and edge
// scores are intentionally absent from this interface.
VertexSetRankingResult rank_complete_vertex_sets(const TopologyEvidenceBundle& evidence,
                                                 const ExactStructuralResult& structural,
                                                 const VertexSetRankerOptions& options = {});

}  // namespace longlineage::solver
