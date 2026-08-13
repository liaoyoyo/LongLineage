// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "longlineage/solver/topology_router.hpp"
#include "longlineage/solver/vertex_set_ranker.hpp"

namespace longlineage::solver {

enum class ParentEdgeEndpointState {
    kComplete,
    kNotRun,
    kAbstainEvidenceMissing,
};

struct ParentEdgeLocalScore {
    HypercubeVertex parent = 0;
    HypercubeVertex child = 0;
    double additive_score = 0.0;
};

struct ParentEdgeEndpointRequest {
    bool requested = false;
    std::string evidence_sha256;
    std::vector<ParentEdgeLocalScore> local_scores;
    double tie_tolerance = 1e-12;
};

struct ParentEdgeCandidateResult {
    std::string candidate_vertex_set_sha256;
    double best_additive_edge_score = 0.0;
    std::string best_parent_tie_count = "0";
    std::vector<std::pair<HypercubeVertex, HypercubeVertex>> published_parent_mapping;
};

struct ParentEdgeEndpointResult {
    ParentEdgeEndpointState state = ParentEdgeEndpointState::kNotRun;
    std::string evidence_sha256;
    std::vector<ParentEdgeCandidateResult> candidate_results;
    std::string message;
};

struct MultiMutationEdgeProjection {
    std::string candidate_vertex_set_sha256;
    HypercubeVertex from_vertex = 0;
    HypercubeVertex to_vertex = 0;
    std::vector<std::size_t> mutation_active_bits;
    std::string order_count = "0";
    std::string eligibility_evidence_sha256;
};

struct TopologyRecordIdentity {
    std::uint64_t dataset_order = 0;
    std::string dataset_id;
    std::uint64_t unit_order = 0;
    std::string unit_id;
};

struct TopologyRecordResult {
    bool valid = false;
    std::string canonical_json;
    std::string record_sha256;
    std::string message;
};

// Evaluate local parent-edge evidence only for the already ranked best
// vertex-set tie class. A parent mapping is published only when every child has
// exactly one best local parent; edge ties never alter abundance ranking.
ParentEdgeEndpointResult evaluate_ranked_parent_edges(std::size_t bit_count, const ExactStructuralResult& structural,
                                                      const VertexSetRankingResult& ranking,
                                                      const ParentEdgeEndpointRequest& request);

MultiMutationEdgeProjection make_unresolved_multi_mutation_projection(const std::string& candidate_vertex_set_sha256,
                                                                      HypercubeVertex from_vertex,
                                                                      HypercubeVertex to_vertex,
                                                                      const std::vector<std::size_t>& active_loci,
                                                                      const std::string& eligibility_evidence_sha256);

// Serialize one schema-2.0.0 topology record. This adapter is fail-closed and
// emits no rank for objective abstention, family cap or family deadline.
TopologyRecordResult serialize_topology_unit_v2(const TopologyRecordIdentity& identity,
                                                const TopologyEvidenceBundle& evidence,
                                                const TopologyRouterResult& topology,
                                                const VertexSetRankingResult& ranking,
                                                const ParentEdgeEndpointResult& edge_endpoint,
                                                const std::vector<MultiMutationEdgeProjection>& unary_projections = {});

}  // namespace longlineage::solver
