// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/solver/topology_record.hpp"

#include <jansson.h>

#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"

namespace longlineage::solver {
namespace {

using boost::multiprecision::cpp_int;

bool is_sha256(const std::string& value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char symbol) {
               return (symbol >= '0' && symbol <= '9') || (symbol >= 'a' && symbol <= 'f');
           });
}

std::string digest_or_throw(const std::string& canonical) {
    auto digest = longlineage::sha256_hex(canonical);
    if (!digest.ok() || !digest.value.has_value()) {
        throw std::runtime_error("OpenSSL SHA-256 failed while binding topology record");
    }
    return *digest.value;
}

std::string decimal(const cpp_int& value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << value;
    return output.str();
}

bool parse_nonnegative_decimal(const std::string& text, cpp_int& value) noexcept {
    if (text.empty()) {
        return false;
    }
    value = 0;
    for (char symbol : text) {
        if (symbol < '0' || symbol > '9') {
            return false;
        }
        value *= 10;
        value += static_cast<unsigned>(symbol - '0');
    }
    return true;
}

std::size_t popcount(HypercubeVertex value) noexcept {
    std::size_t count = 0;
    while (value != 0) {
        count += static_cast<std::size_t>(value & HypercubeVertex{1});
        value = static_cast<HypercubeVertex>(value >> 1);
    }
    return count;
}

std::string binary_state(HypercubeVertex vertex, std::size_t bit_count) {
    std::string state(bit_count, '0');
    for (std::size_t bit = 0; bit < bit_count; ++bit) {
        if ((vertex & static_cast<HypercubeVertex>(HypercubeVertex{1} << bit)) != 0) {
            state[bit] = '1';
        }
    }
    return state;
}

std::string binary_pattern(const std::string& pattern_rax) {
    std::string pattern = pattern_rax;
    for (char& symbol : pattern) {
        if (symbol == 'R') {
            symbol = '0';
        } else if (symbol == 'A') {
            symbol = '1';
        } else if (symbol != 'X') {
            throw std::invalid_argument("structural input pattern contains a non-R/A/X code");
        }
    }
    return pattern;
}

void set_new(json_t* object, const char* key, json_t* value) {
    if (value == nullptr) {
        throw std::runtime_error("Jansson value allocation failed");
    }
    if (json_object_set_new(object, key, value) != 0) {
        throw std::runtime_error("Jansson object construction failed");
    }
}

void append_new(json_t* array, json_t* value) {
    if (value == nullptr) {
        throw std::runtime_error("Jansson value allocation failed");
    }
    if (json_array_append_new(array, value) != 0) {
        throw std::runtime_error("Jansson array construction failed");
    }
}

const char* route_name(ExactSolverRoute route) {
    switch (route) {
        case ExactSolverRoute::kSmallQOracleDifferentialBnb:
            return "SMALL_Q_ORACLE_DIFFERENTIAL_BNB";
        case ExactSolverRoute::kBitsetObligationBnb:
            return "BITSET_OBLIGATION_BNB";
        case ExactSolverRoute::kAbstain:
            return "ABSTAIN";
    }
    return "ABSTAIN";
}

const char* objective_state_name(ExactObjectiveState state) {
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
    return "ABSTAIN_NOT_IDENTIFIABLE";
}

const char* objective_reason_name(ExactObjectiveState state) {
    switch (state) {
        case ExactObjectiveState::kObjectiveCertified:
            return "NONE";
        case ExactObjectiveState::kAbstainKernelNotVerified:
            return "SOLVER_ROUTE_NOT_VERIFIED";
        case ExactObjectiveState::kAbstainNotIdentifiable:
            return "MODEL_NOT_IDENTIFIABLE";
        case ExactObjectiveState::kAbstainResourceLimit:
            return "SEARCH_RESOURCE_LIMIT_REACHED";
        case ExactObjectiveState::kAbstainDifferentialMismatch:
            return "DIFFERENTIAL_CHECK_FAILED";
    }
    return "MODEL_NOT_IDENTIFIABLE";
}

const char* family_state_name(ExactFamilyState state) {
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
    return "ABSTAIN_NOT_IDENTIFIABLE";
}

const char* family_reason_name(ExactFamilyState state) {
    switch (state) {
        case ExactFamilyState::kFamilyComplete:
            return "NONE";
        case ExactFamilyState::kFamilyIncompleteCap:
            return "MAX_FAMILY_SIZE_REACHED";
        case ExactFamilyState::kAbstainKernelNotVerified:
            return "SOLVER_ROUTE_NOT_VERIFIED";
        case ExactFamilyState::kAbstainNotIdentifiable:
            return "MODEL_NOT_IDENTIFIABLE";
        case ExactFamilyState::kAbstainResourceLimit:
            return "SEARCH_RESOURCE_LIMIT_REACHED";
        case ExactFamilyState::kAbstainDifferentialMismatch:
            return "DIFFERENTIAL_CHECK_FAILED";
    }
    return "MODEL_NOT_IDENTIFIABLE";
}

bool is_objective_abstain(ExactObjectiveState state) noexcept {
    return state != ExactObjectiveState::kObjectiveCertified;
}

const ExactStructuralCandidate* find_candidate(std::size_t bit_count, const ExactStructuralResult& structural,
                                               const std::string& digest) {
    for (const ExactStructuralCandidate& candidate : structural.minimum_family) {
        if (exact_vertex_set_sha256(bit_count, candidate.vertices) == digest) {
            return &candidate;
        }
    }
    return nullptr;
}

json_t* make_string_array(const std::vector<std::string>& values) {
    json_t* array = json_array();
    for (const std::string& value : values) {
        append_new(array, json_string(value.c_str()));
    }
    return array;
}

struct FamilySerialization {
    std::vector<const ExactStructuralCandidate*> candidates;
    std::vector<std::string> candidate_digests;
    std::string candidate_family_digest;
    std::string family_evidence_sha256;
    std::string total_tree_count = "0";
};

FamilySerialization prepare_complete_family(std::size_t bit_count, const ExactStructuralResult& structural,
                                            const std::string& objective_evidence_sha256,
                                            const std::string& input_evidence_sha256) {
    FamilySerialization family;
    if (structural.family_state != ExactFamilyState::kFamilyComplete || !structural.family_enumeration_exhausted ||
        structural.minimum_family.empty()) {
        throw std::invalid_argument("complete family serialization requires exhausted candidates");
    }
    family.candidates.reserve(structural.minimum_family.size());
    for (const ExactStructuralCandidate& candidate : structural.minimum_family) {
        if (!candidate.parent_mapping.valid) {
            throw std::invalid_argument("candidate lacks its fixed-node legal-parent summary");
        }
        family.candidates.push_back(&candidate);
    }
    std::sort(family.candidates.begin(), family.candidates.end(),
              [](const ExactStructuralCandidate* left, const ExactStructuralCandidate* right) {
                  return left->vertices < right->vertices;
              });
    for (std::size_t index = 0; index < family.candidates.size(); ++index) {
        if (index != 0 && family.candidates[index - 1]->vertices == family.candidates[index]->vertices) {
            throw std::invalid_argument("complete family contains duplicate vertex sets");
        }
        const std::string digest = exact_vertex_set_sha256(bit_count, family.candidates[index]->vertices);
        if (!is_sha256(digest)) {
            throw std::invalid_argument("complete family contains a malformed vertex set");
        }
        family.candidate_digests.push_back(digest);
    }

    std::ostringstream candidate_stream;
    candidate_stream.imbue(std::locale::classic());
    candidate_stream << "schema=longlineage.candidate_family.v1\n"
                     << "q=" << bit_count << '\n';
    cpp_int tree_total = 0;
    for (std::size_t index = 0; index < family.candidates.size(); ++index) {
        const ExactStructuralCandidate& candidate = *family.candidates[index];
        cpp_int candidate_trees;
        if (!parse_nonnegative_decimal(candidate.parent_mapping.tree_count, candidate_trees) || candidate_trees <= 0) {
            throw std::invalid_argument("candidate tree_count is not a positive decimal integer");
        }
        tree_total += candidate_trees;
        candidate_stream << family.candidate_digests[index] << '\t' << candidate.parent_mapping.legal_parent_count
                         << '\t' << candidate.parent_mapping.tree_count << '\n';
    }
    family.total_tree_count = decimal(tree_total);
    family.candidate_family_digest = digest_or_throw(candidate_stream.str());

    std::ostringstream family_evidence;
    family_evidence.imbue(std::locale::classic());
    family_evidence << "schema=longlineage.complete_family_evidence.v1\n"
                    << "input_evidence_sha256=" << input_evidence_sha256 << '\n'
                    << "objective_evidence_sha256=" << objective_evidence_sha256 << '\n'
                    << "candidate_family_digest=" << family.candidate_family_digest << '\n'
                    << "candidate_count=" << family.candidates.size() << '\n'
                    << "tree_count=" << family.total_tree_count << '\n'
                    << "family_enumeration_exhausted=1\n";
    family.family_evidence_sha256 = digest_or_throw(family_evidence.str());
    return family;
}

json_t* serialize_candidate(std::size_t bit_count, const ExactStructuralCandidate& candidate,
                            const std::string& digest) {
    json_t* object = json_object();
    json_t* vertices = json_array();
    std::vector<std::string> vertex_states;
    vertex_states.reserve(candidate.vertices.size());
    for (HypercubeVertex vertex : candidate.vertices) {
        vertex_states.push_back(binary_state(vertex, bit_count));
    }
    std::sort(vertex_states.begin(), vertex_states.end());
    for (const std::string& state : vertex_states) {
        append_new(vertices, json_string(state.c_str()));
    }
    set_new(object, "vertex_set", vertices);
    set_new(object, "vertex_set_sha256", json_string(digest.c_str()));

    json_t* choices_array = json_array();
    std::vector<std::pair<std::string, std::vector<std::string>>> state_choices;
    state_choices.reserve(candidate.parent_mapping.legal_parents.size());
    for (const ExactLegalParentChoices& choices : candidate.parent_mapping.legal_parents) {
        std::vector<std::string> parents;
        parents.reserve(choices.parents.size());
        for (HypercubeVertex parent : choices.parents) {
            parents.push_back(binary_state(parent, bit_count));
        }
        std::sort(parents.begin(), parents.end());
        state_choices.emplace_back(binary_state(choices.vertex, bit_count), std::move(parents));
    }
    std::sort(state_choices.begin(), state_choices.end());
    for (const auto& state_choice : state_choices) {
        json_t* choice = json_object();
        set_new(choice, "vertex", json_string(state_choice.first.c_str()));
        json_t* parents_array = json_array();
        for (const std::string& parent : state_choice.second) {
            append_new(parents_array, json_string(parent.c_str()));
        }
        set_new(choice, "parents", parents_array);
        append_new(choices_array, choice);
    }
    set_new(object, "legal_parent_choices", choices_array);
    set_new(object, "legal_parent_count", json_string(candidate.parent_mapping.legal_parent_count.c_str()));
    set_new(object, "tree_count", json_string(candidate.parent_mapping.tree_count.c_str()));
    return object;
}

json_t* serialize_edge_endpoint(std::size_t bit_count, const ParentEdgeEndpointResult& endpoint) {
    json_t* object = json_object();
    if (endpoint.state == ParentEdgeEndpointState::kNotRun) {
        set_new(object, "status", json_string("EDGE_NOT_RUN"));
        set_new(object, "endpoint_id", json_null());
        set_new(object, "evidence_sha256", json_null());
        set_new(object, "candidate_results", json_array());
        set_new(object, "reason", json_string("NOT_REQUESTED"));
        return object;
    }
    if (endpoint.state == ParentEdgeEndpointState::kAbstainEvidenceMissing) {
        if (!endpoint.candidate_results.empty()) {
            throw std::invalid_argument("abstaining edge endpoint contains candidate results");
        }
        set_new(object, "status", json_string("EDGE_ABSTAIN_EVIDENCE_MISSING"));
        set_new(object, "endpoint_id", json_string("ADDITIVE_PARENT_EDGE_SCORE_V1"));
        set_new(object, "evidence_sha256", json_null());
        set_new(object, "candidate_results", json_array());
        set_new(object, "reason", json_string("PARENT_SCORE_EVIDENCE_MISSING"));
        return object;
    }
    if (!is_sha256(endpoint.evidence_sha256) || endpoint.candidate_results.empty()) {
        throw std::invalid_argument("complete edge endpoint lacks evidence or results");
    }
    set_new(object, "status", json_string("EDGE_COMPLETE"));
    set_new(object, "endpoint_id", json_string("ADDITIVE_PARENT_EDGE_SCORE_V1"));
    set_new(object, "evidence_sha256", json_string(endpoint.evidence_sha256.c_str()));
    json_t* results = json_array();
    std::set<std::string> seen;
    std::vector<const ParentEdgeCandidateResult*> ordered_results;
    ordered_results.reserve(endpoint.candidate_results.size());
    for (const ParentEdgeCandidateResult& result : endpoint.candidate_results) {
        ordered_results.push_back(&result);
    }
    std::sort(ordered_results.begin(), ordered_results.end(),
              [](const ParentEdgeCandidateResult* left, const ParentEdgeCandidateResult* right) {
                  return left->candidate_vertex_set_sha256 < right->candidate_vertex_set_sha256;
              });
    for (const ParentEdgeCandidateResult* result_pointer : ordered_results) {
        const ParentEdgeCandidateResult& result = *result_pointer;
        if (!is_sha256(result.candidate_vertex_set_sha256) || !seen.insert(result.candidate_vertex_set_sha256).second ||
            !std::isfinite(result.best_additive_edge_score)) {
            throw std::invalid_argument("complete edge endpoint contains malformed results");
        }
        cpp_int tie_count;
        if (!parse_nonnegative_decimal(result.best_parent_tie_count, tie_count) || tie_count <= 0 ||
            ((tie_count == 1) != !result.published_parent_mapping.empty())) {
            throw std::invalid_argument("edge tie count and published mapping disagree");
        }
        json_t* item = json_object();
        set_new(item, "candidate_vertex_set_sha256", json_string(result.candidate_vertex_set_sha256.c_str()));
        set_new(item, "best_additive_edge_score", json_real(result.best_additive_edge_score));
        set_new(item, "best_parent_tie_count", json_string(result.best_parent_tie_count.c_str()));
        if (result.published_parent_mapping.empty()) {
            set_new(item, "published_parent_mapping", json_null());
        } else {
            json_t* mapping = json_array();
            std::vector<std::pair<std::string, std::string>> ordered_mapping;
            ordered_mapping.reserve(result.published_parent_mapping.size());
            for (const auto& parent_child : result.published_parent_mapping) {
                ordered_mapping.emplace_back(binary_state(parent_child.second, bit_count),
                                             binary_state(parent_child.first, bit_count));
            }
            std::sort(ordered_mapping.begin(), ordered_mapping.end());
            for (const auto& child_parent : ordered_mapping) {
                json_t* edge = json_object();
                set_new(edge, "child_state", json_string(child_parent.first.c_str()));
                set_new(edge, "parent_state", json_string(child_parent.second.c_str()));
                append_new(mapping, edge);
            }
            set_new(item, "published_parent_mapping", mapping);
        }
        append_new(results, item);
    }
    set_new(object, "candidate_results", results);
    set_new(object, "reason", json_string("NONE"));
    return object;
}

json_t* serialize_unary_projection(std::size_t bit_count, const MultiMutationEdgeProjection& projection) {
    json_t* object = json_object();
    set_new(object, "candidate_vertex_set_sha256", json_string(projection.candidate_vertex_set_sha256.c_str()));
    set_new(object, "kind", json_string("MULTI_MUTATION_EDGE_EQUIVALENCE"));
    set_new(object, "from_state", json_string(binary_state(projection.from_vertex, bit_count).c_str()));
    set_new(object, "to_state", json_string(binary_state(projection.to_vertex, bit_count).c_str()));
    json_t* mutation_bits = json_array();
    for (std::size_t bit : projection.mutation_active_bits) {
        if (bit > static_cast<std::size_t>(std::numeric_limits<json_int_t>::max())) {
            throw std::invalid_argument("unary mutation bit exceeds JSON integer range");
        }
        append_new(mutation_bits, json_integer(static_cast<json_int_t>(bit)));
    }
    set_new(object, "mutation_active_bits", mutation_bits);
    set_new(object, "order_state", json_string("UNRESOLVED_NO_READ_EVIDENCE"));
    set_new(object, "order_count", json_string(projection.order_count.c_str()));
    set_new(object, "eligibility_evidence_sha256", json_string(projection.eligibility_evidence_sha256.c_str()));
    set_new(object, "projection_only", json_true());
    return object;
}

}  // namespace

ParentEdgeEndpointResult evaluate_ranked_parent_edges(std::size_t bit_count, const ExactStructuralResult& structural,
                                                      const VertexSetRankingResult& ranking,
                                                      const ParentEdgeEndpointRequest& request) {
    ParentEdgeEndpointResult result;
    if (!request.requested || ranking.state != VertexSetRankingState::kRankingComplete) {
        result.message = "edge endpoint was not requested for a complete abundance rank";
        return result;
    }
    const auto abstain = [&result](const std::string& message) {
        result.state = ParentEdgeEndpointState::kAbstainEvidenceMissing;
        result.evidence_sha256.clear();
        result.candidate_results.clear();
        result.message = message;
    };
    if (bit_count > kMaximumExactHypercubeBits || !is_sha256(request.evidence_sha256) ||
        !std::isfinite(request.tie_tolerance) || request.tie_tolerance < 0.0 ||
        ranking.best_vertex_set_tie_class.empty()) {
        abstain("edge request metadata is malformed or incomplete");
        return result;
    }

    std::map<std::pair<HypercubeVertex, HypercubeVertex>, double> local_scores;
    const std::size_t vertex_count = std::size_t{1} << bit_count;
    for (const ParentEdgeLocalScore& score : request.local_scores) {
        const auto key = std::make_pair(score.parent, score.child);
        if (static_cast<std::size_t>(score.parent) >= vertex_count ||
            static_cast<std::size_t>(score.child) >= vertex_count || !std::isfinite(score.additive_score) ||
            !local_scores.emplace(key, score.additive_score).second) {
            abstain("edge score evidence is malformed, duplicated or out of range");
            return result;
        }
    }

    std::vector<std::string> ranked = ranking.best_vertex_set_tie_class;
    std::sort(ranked.begin(), ranked.end());
    if (std::adjacent_find(ranked.begin(), ranked.end()) != ranked.end()) {
        abstain("ranking tie class contains a duplicate candidate");
        return result;
    }
    for (const std::string& digest : ranked) {
        const ExactStructuralCandidate* candidate = find_candidate(bit_count, structural, digest);
        if (candidate == nullptr || !candidate->parent_mapping.valid) {
            abstain("ranked candidate cannot resolve to fixed-node parent choices");
            return result;
        }
        ParentEdgeCandidateResult candidate_result;
        candidate_result.candidate_vertex_set_sha256 = digest;
        cpp_int tie_count = 1;
        bool unique_mapping = true;
        for (const ExactLegalParentChoices& choices : candidate->parent_mapping.legal_parents) {
            double best_score = -std::numeric_limits<double>::infinity();
            for (HypercubeVertex parent : choices.parents) {
                const auto found = local_scores.find(std::make_pair(parent, choices.vertex));
                if (found == local_scores.end()) {
                    abstain("at least one legal fixed-node parent edge lacks a score");
                    return result;
                }
                best_score = std::max(best_score, found->second);
            }
            std::vector<HypercubeVertex> best_parents;
            for (HypercubeVertex parent : choices.parents) {
                const double score = local_scores.at(std::make_pair(parent, choices.vertex));
                if (best_score - score <= request.tie_tolerance) {
                    best_parents.push_back(parent);
                }
            }
            if (best_parents.empty() || !std::isfinite(best_score)) {
                abstain("fixed-node parent score comparison did not produce a winner");
                return result;
            }
            candidate_result.best_additive_edge_score += best_score;
            tie_count *= best_parents.size();
            if (best_parents.size() == 1) {
                candidate_result.published_parent_mapping.emplace_back(best_parents.front(), choices.vertex);
            } else {
                unique_mapping = false;
            }
        }
        if (!unique_mapping) {
            candidate_result.published_parent_mapping.clear();
        }
        candidate_result.best_parent_tie_count = decimal(tie_count);
        result.candidate_results.push_back(std::move(candidate_result));
    }
    result.state = ParentEdgeEndpointState::kComplete;
    result.evidence_sha256 = request.evidence_sha256;
    result.message = "parent edges evaluated only after abundance-ranked node sets were fixed";
    return result;
}

MultiMutationEdgeProjection make_unresolved_multi_mutation_projection(const std::string& candidate_vertex_set_sha256,
                                                                      HypercubeVertex from_vertex,
                                                                      HypercubeVertex to_vertex,
                                                                      const std::vector<std::size_t>& active_loci,
                                                                      const std::string& eligibility_evidence_sha256) {
    if (!is_sha256(candidate_vertex_set_sha256) || !is_sha256(eligibility_evidence_sha256) || active_loci.size() < 2 ||
        active_loci.size() > kMaximumExactHypercubeBits || !std::is_sorted(active_loci.begin(), active_loci.end()) ||
        std::adjacent_find(active_loci.begin(), active_loci.end()) != active_loci.end()) {
        throw std::invalid_argument("unary projection identity or active-locus map is malformed");
    }
    const std::size_t vertex_count = std::size_t{1} << active_loci.size();
    if (static_cast<std::size_t>(from_vertex) >= vertex_count || static_cast<std::size_t>(to_vertex) >= vertex_count ||
        (from_vertex & to_vertex) != from_vertex) {
        throw std::invalid_argument("unary projection endpoints violate the forward hypercube order");
    }
    const HypercubeVertex difference = static_cast<HypercubeVertex>(from_vertex ^ to_vertex);
    const std::size_t mutation_count = popcount(difference);
    if (mutation_count < 2) {
        throw std::invalid_argument("unary projection requires at least two unresolved mutations");
    }

    MultiMutationEdgeProjection projection;
    projection.candidate_vertex_set_sha256 = candidate_vertex_set_sha256;
    projection.from_vertex = from_vertex;
    projection.to_vertex = to_vertex;
    for (std::size_t bit = 0; bit < active_loci.size(); ++bit) {
        if ((difference & static_cast<HypercubeVertex>(HypercubeVertex{1} << bit)) != 0) {
            projection.mutation_active_bits.push_back(active_loci[bit]);
        }
    }
    cpp_int order_count = 1;
    for (std::size_t factor = 2; factor <= mutation_count; ++factor) {
        order_count *= factor;
    }
    projection.order_count = decimal(order_count);
    projection.eligibility_evidence_sha256 = eligibility_evidence_sha256;
    return projection;
}

TopologyRecordResult serialize_topology_unit_v2(const TopologyRecordIdentity& identity,
                                                const TopologyEvidenceBundle& evidence,
                                                const TopologyRouterResult& topology,
                                                const VertexSetRankingResult& ranking,
                                                const ParentEdgeEndpointResult& edge_endpoint,
                                                const std::vector<MultiMutationEdgeProjection>& unary_projections) {
    TopologyRecordResult result;
    json_t* root = nullptr;
    try {
        if (identity.dataset_id.empty() || identity.unit_id.empty() ||
            identity.dataset_order > static_cast<std::uint64_t>(std::numeric_limits<json_int_t>::max()) ||
            identity.unit_order > static_cast<std::uint64_t>(std::numeric_limits<json_int_t>::max()) ||
            !is_sha256(evidence.input_evidence_sha256) || evidence.active_loci.empty() ||
            evidence.structural_patterns.empty() ||
            !std::is_sorted(evidence.active_loci.begin(), evidence.active_loci.end()) ||
            std::adjacent_find(evidence.active_loci.begin(), evidence.active_loci.end()) !=
                evidence.active_loci.end()) {
            throw std::invalid_argument("record identity or topology evidence is malformed");
        }
        const std::size_t bit_count = evidence.active_loci.size();
        if (bit_count > kMaximumExactHypercubeBits &&
            topology.structural.objective_state == ExactObjectiveState::kObjectiveCertified) {
            throw std::invalid_argument("objective certified outside the exact bit boundary");
        }
        if ((is_objective_abstain(topology.structural.objective_state) &&
             topology.route != ExactSolverRoute::kAbstain) ||
            (!is_objective_abstain(topology.structural.objective_state) &&
             topology.route == ExactSolverRoute::kAbstain)) {
            throw std::invalid_argument("solver route and objective authority disagree");
        }
        if (is_objective_abstain(topology.structural.objective_state) &&
            std::string(objective_state_name(topology.structural.objective_state)) !=
                family_state_name(topology.structural.family_state)) {
            throw std::invalid_argument("objective and family abstention states disagree");
        }
        if (topology.structural.objective_state == ExactObjectiveState::kObjectiveCertified &&
            (!topology.structural.objective_h.has_value() || !topology.structural.objective_search_exhausted)) {
            throw std::invalid_argument("certified objective lacks an exhausted exact proof");
        }
        if (topology.structural.objective_state == ExactObjectiveState::kObjectiveCertified &&
            evidence.state != EvidenceAdapterState::kReady) {
            throw std::invalid_argument("certified objective requires ready topology evidence");
        }
        if (topology.structural.objective_state == ExactObjectiveState::kObjectiveCertified) {
            std::vector<std::size_t> expected_router_loci(bit_count);
            for (std::size_t index = 0; index < bit_count; ++index) {
                expected_router_loci[index] = index;
            }
            if (topology.active_loci != expected_router_loci) {
                throw std::invalid_argument("router active-bit compression disagrees with evidence adapter");
            }
        }

        const bool deadline_incomplete = ranking.state == VertexSetRankingState::kNotRunFamilyIncompleteDeadline;
        const bool family_complete =
            topology.structural.family_state == ExactFamilyState::kFamilyComplete && !deadline_incomplete;
        const bool family_cap = topology.structural.family_state == ExactFamilyState::kFamilyIncompleteCap &&
                                ranking.state == VertexSetRankingState::kNotRunFamilyIncompleteCap;
        if (deadline_incomplete && topology.structural.objective_state != ExactObjectiveState::kObjectiveCertified) {
            throw std::invalid_argument("family deadline cannot override an abstaining objective");
        }
        if (family_complete && ranking.state != VertexSetRankingState::kRankingComplete &&
            ranking.state != VertexSetRankingState::kAbstainNumericalCertificate) {
            throw std::invalid_argument("complete family has an incompatible ranking state");
        }
        if (is_objective_abstain(topology.structural.objective_state) &&
            ranking.state != VertexSetRankingState::kNotRunObjectiveAbstain) {
            throw std::invalid_argument("objective abstention must suppress abundance ranking");
        }
        if (topology.structural.objective_state == ExactObjectiveState::kObjectiveCertified && !family_complete &&
            !family_cap && !deadline_incomplete) {
            throw std::invalid_argument("certified objective has an unsupported family state");
        }

        std::string objective_evidence_sha256;
        if (!is_objective_abstain(topology.structural.objective_state)) {
            std::ostringstream objective_evidence;
            objective_evidence.imbue(std::locale::classic());
            objective_evidence << "schema=longlineage.objective_evidence.v1\n"
                               << "input_evidence_sha256=" << evidence.input_evidence_sha256 << '\n'
                               << "solver_route=" << route_name(topology.route) << '\n'
                               << "objective_h=" << *topology.structural.objective_h << '\n'
                               << "search_nodes=" << topology.structural.search_nodes << '\n'
                               << "objective_search_exhausted=1\n";
            objective_evidence_sha256 = digest_or_throw(objective_evidence.str());
        }

        FamilySerialization family;
        if (family_complete) {
            family = prepare_complete_family(bit_count, topology.structural, objective_evidence_sha256,
                                             evidence.input_evidence_sha256);
        }

        root = json_object();
        set_new(root, "schema_name", json_string("longlineage.topology_unit"));
        set_new(root, "schema_version", json_string("2.0.0"));
        set_new(root, "dataset_order", json_integer(static_cast<json_int_t>(identity.dataset_order)));
        set_new(root, "dataset_id", json_string(identity.dataset_id.c_str()));
        set_new(root, "unit_order", json_integer(static_cast<json_int_t>(identity.unit_order)));
        set_new(root, "unit_id", json_string(identity.unit_id.c_str()));

        json_t* active_bits = json_array();
        for (std::size_t bit : evidence.active_loci) {
            if (bit > static_cast<std::size_t>(std::numeric_limits<json_int_t>::max())) {
                throw std::invalid_argument("active locus exceeds JSON integer range");
            }
            append_new(active_bits, json_integer(static_cast<json_int_t>(bit)));
        }
        set_new(root, "active_bits", active_bits);

        json_t* input_patterns = json_array();
        std::vector<std::string> observed_states;
        std::vector<const StructuralPatternEvidence*> ordered_patterns;
        ordered_patterns.reserve(evidence.structural_patterns.size());
        for (const StructuralPatternEvidence& pattern : evidence.structural_patterns) {
            ordered_patterns.push_back(&pattern);
        }
        std::sort(ordered_patterns.begin(), ordered_patterns.end(),
                  [](const StructuralPatternEvidence* left, const StructuralPatternEvidence* right) {
                      return std::tie(left->partial, left->pattern_rax) < std::tie(right->partial, right->pattern_rax);
                  });
        for (std::size_t pattern_index = 0; pattern_index < ordered_patterns.size(); ++pattern_index) {
            const StructuralPatternEvidence& pattern = *ordered_patterns[pattern_index];
            if (pattern.pattern_rax.size() != bit_count || pattern.multiplicity == 0 ||
                pattern.multiplicity > static_cast<std::uint64_t>(std::numeric_limits<json_int_t>::max())) {
                throw std::invalid_argument("structural pattern cannot be serialized");
            }
            if (pattern_index != 0 && ordered_patterns[pattern_index - 1]->partial == pattern.partial &&
                ordered_patterns[pattern_index - 1]->pattern_rax == pattern.pattern_rax) {
                throw std::invalid_argument("duplicate structural pattern was not consolidated");
            }
            const std::string binary = binary_pattern(pattern.pattern_rax);
            const bool partial = binary.find('X') != std::string::npos;
            if (partial != pattern.partial) {
                throw std::invalid_argument("structural pattern partial flag disagrees with X");
            }
            json_t* item = json_object();
            set_new(item, "kind", json_string(partial ? "PARTIAL_SUBCUBE" : "FULL_STATE"));
            set_new(item, "pattern", json_string(binary.c_str()));
            set_new(item, "multiplicity", json_integer(static_cast<json_int_t>(pattern.multiplicity)));
            append_new(input_patterns, item);
            if (!partial) {
                observed_states.push_back(binary);
            }
        }
        std::sort(observed_states.begin(), observed_states.end());
        observed_states.erase(std::unique(observed_states.begin(), observed_states.end()), observed_states.end());
        set_new(root, "input_patterns", input_patterns);
        set_new(root, "input_evidence_sha256", json_string(evidence.input_evidence_sha256.c_str()));
        set_new(root, "observed_states", make_string_array(observed_states));

        set_new(root, "solver_route", json_string(route_name(topology.route)));
        set_new(root, "objective_state", json_string(objective_state_name(topology.structural.objective_state)));
        if (topology.structural.objective_h.has_value() && !is_objective_abstain(topology.structural.objective_state)) {
            set_new(root, "objective_h", json_integer(static_cast<json_int_t>(*topology.structural.objective_h)));
            json_t* bounds = json_object();
            set_new(bounds, "lower_bound", json_integer(static_cast<json_int_t>(*topology.structural.objective_h)));
            set_new(bounds, "upper_bound", json_integer(static_cast<json_int_t>(*topology.structural.objective_h)));
            set_new(bounds, "gap", json_integer(0));
            set_new(root, "objective_bounds", bounds);
            set_new(root, "objective_evidence_sha256", json_string(objective_evidence_sha256.c_str()));
        } else {
            set_new(root, "objective_h", json_null());
            set_new(root, "objective_bounds", json_null());
            set_new(root, "objective_evidence_sha256", json_null());
        }
        set_new(root, "objective_reason", json_string(objective_reason_name(topology.structural.objective_state)));

        if (deadline_incomplete) {
            set_new(root, "family_state", json_string("FAMILY_INCOMPLETE_DEADLINE"));
            set_new(root, "family_reason", json_string("DEADLINE_REACHED"));
        } else {
            set_new(root, "family_state", json_string(family_state_name(topology.structural.family_state)));
            set_new(root, "family_reason", json_string(family_reason_name(topology.structural.family_state)));
        }
        if (family_complete) {
            set_new(root, "family_representation", json_string("EXPLICIT_MINIMUM_VERTEX_SETS_V1"));
            set_new(root, "family_evidence_sha256", json_string(family.family_evidence_sha256.c_str()));
            set_new(root, "candidate_family_digest", json_string(family.candidate_family_digest.c_str()));
            set_new(root, "candidate_count", json_integer(static_cast<json_int_t>(family.candidates.size())));
            set_new(root, "tree_count", json_string(family.total_tree_count.c_str()));
            json_t* candidates = json_array();
            for (std::size_t index = 0; index < family.candidates.size(); ++index) {
                append_new(candidates,
                           serialize_candidate(bit_count, *family.candidates[index], family.candidate_digests[index]));
            }
            set_new(root, "candidates", candidates);
        } else {
            set_new(root, "family_representation", json_null());
            set_new(root, "family_evidence_sha256", json_null());
            set_new(root, "candidate_family_digest", json_null());
            set_new(root, "candidate_count", json_integer(0));
            set_new(root, "tree_count", json_string("0"));
            set_new(root, "candidates", json_array());
        }

        if (ranking.state == VertexSetRankingState::kRankingComplete) {
            if (!is_sha256(ranking.ranking_evidence_sha256) || !is_sha256(ranking.certificate_sha256) ||
                !ranking.best_log_likelihood.has_value() || !std::isfinite(*ranking.best_log_likelihood) ||
                ranking.evaluated_vertex_set_count != family.candidates.size() ||
                ranking.candidate_scores.size() != family.candidates.size() ||
                ranking.best_vertex_set_tie_class.empty() ||
                !std::is_sorted(ranking.best_vertex_set_tie_class.begin(), ranking.best_vertex_set_tie_class.end())) {
                throw std::invalid_argument("complete ranking lacks exhaustive evidence");
            }
            const std::set<std::string> family_digest_set(family.candidate_digests.begin(),
                                                          family.candidate_digests.end());
            std::set<std::string> score_digest_set;
            for (const CandidateVertexSetScore& score : ranking.candidate_scores) {
                if (!score.converged || !family_digest_set.count(score.vertex_set_sha256) ||
                    !score_digest_set.insert(score.vertex_set_sha256).second) {
                    throw std::invalid_argument("ranking did not score every vertex set exactly once");
                }
            }
            if (score_digest_set != family_digest_set) {
                throw std::invalid_argument("ranking score set does not equal the complete structural family");
            }
            std::set<std::string> best_digest_set;
            for (const std::string& digest : ranking.best_vertex_set_tie_class) {
                if (!family_digest_set.count(digest) || !best_digest_set.insert(digest).second) {
                    throw std::invalid_argument("published ranking tie class is duplicated or not candidate-bound");
                }
            }
            set_new(root, "ranking_state", json_string("RANKING_COMPLETE"));
            set_new(root, "ranking_endpoint_id", json_string("BQ_AWARE_READ_PATTERN_MIXTURE_V1"));
            set_new(root, "ranking_mode", json_string("EXHAUSTIVE"));
            set_new(root, "ranking_evidence_sha256", json_string(ranking.ranking_evidence_sha256.c_str()));
            json_t* certificate = json_object();
            set_new(certificate, "mode", json_string("EXHAUSTIVE"));
            set_new(certificate, "evaluated_vertex_set_count",
                    json_integer(static_cast<json_int_t>(ranking.evaluated_vertex_set_count)));
            set_new(certificate, "excluded_by_interval_count", json_integer(0));
            set_new(certificate, "certificate_sha256", json_string(ranking.certificate_sha256.c_str()));
            set_new(root, "ranking_certificate", certificate);
            json_t* published = json_object();
            set_new(published, "endpoint_id", json_string("BQ_AWARE_READ_PATTERN_MIXTURE_V1"));
            set_new(published, "best_score", json_real(*ranking.best_log_likelihood));
            set_new(published, "best_vertex_set_tie_class", make_string_array(ranking.best_vertex_set_tie_class));
            set_new(root, "published_rank", published);
            set_new(root, "ranking_reason", json_string("NONE"));
        } else if (ranking.state == VertexSetRankingState::kNotRunFamilyIncompleteCap ||
                   ranking.state == VertexSetRankingState::kNotRunFamilyIncompleteDeadline) {
            if (!ranking.candidate_scores.empty() || ranking.best_log_likelihood.has_value()) {
                throw std::invalid_argument("incomplete family contains ranking output");
            }
            set_new(root, "ranking_state", json_string("RANKING_NOT_RUN_FAMILY_INCOMPLETE"));
            set_new(root, "ranking_endpoint_id", json_null());
            set_new(root, "ranking_mode", json_null());
            set_new(root, "ranking_evidence_sha256", json_null());
            set_new(root, "ranking_certificate", json_null());
            set_new(root, "published_rank", json_null());
            set_new(root, "ranking_reason", json_string("FAMILY_NOT_COMPLETE"));
        } else if (ranking.state == VertexSetRankingState::kNotRunObjectiveAbstain) {
            set_new(root, "ranking_state", json_string("RANKING_NOT_RUN_OBJECTIVE_ABSTAIN"));
            set_new(root, "ranking_endpoint_id", json_null());
            set_new(root, "ranking_mode", json_null());
            set_new(root, "ranking_evidence_sha256", json_null());
            set_new(root, "ranking_certificate", json_null());
            set_new(root, "published_rank", json_null());
            set_new(root, "ranking_reason", json_string("OBJECTIVE_NOT_CERTIFIED"));
        } else if (ranking.state == VertexSetRankingState::kAbstainNumericalCertificate) {
            if (!is_sha256(ranking.ranking_evidence_sha256)) {
                throw std::invalid_argument("numerical abstention lacks ranking evidence");
            }
            set_new(root, "ranking_state", json_string("RANKING_ABSTAIN_NUMERICAL_CERTIFICATE"));
            set_new(root, "ranking_endpoint_id", json_string("BQ_AWARE_READ_PATTERN_MIXTURE_V1"));
            set_new(root, "ranking_mode", json_null());
            set_new(root, "ranking_evidence_sha256", json_string(ranking.ranking_evidence_sha256.c_str()));
            set_new(root, "ranking_certificate", json_null());
            set_new(root, "published_rank", json_null());
            set_new(root, "ranking_reason", json_string("NUMERICAL_CERTIFICATE_UNAVAILABLE"));
        } else {
            throw std::invalid_argument("ranking state cannot be represented by topology_unit v2");
        }

        if (edge_endpoint.state == ParentEdgeEndpointState::kComplete &&
            ranking.state != VertexSetRankingState::kRankingComplete) {
            throw std::invalid_argument("edge endpoint cannot complete without a published rank");
        }
        if (edge_endpoint.state == ParentEdgeEndpointState::kComplete) {
            const std::set<std::string> top(ranking.best_vertex_set_tie_class.begin(),
                                            ranking.best_vertex_set_tie_class.end());
            for (const ParentEdgeCandidateResult& edge : edge_endpoint.candidate_results) {
                if (!top.count(edge.candidate_vertex_set_sha256)) {
                    throw std::invalid_argument("edge result is not bound to a top-ranked node set");
                }
                if (!edge.published_parent_mapping.empty()) {
                    const ExactStructuralCandidate* candidate =
                        find_candidate(bit_count, topology.structural, edge.candidate_vertex_set_sha256);
                    if (candidate == nullptr ||
                        edge.published_parent_mapping.size() != candidate->parent_mapping.legal_parents.size()) {
                        throw std::invalid_argument("published edge mapping is not complete for its fixed node set");
                    }
                    std::set<HypercubeVertex> mapped_children;
                    for (const auto& parent_child : edge.published_parent_mapping) {
                        if (!mapped_children.insert(parent_child.second).second) {
                            throw std::invalid_argument("published edge mapping repeats a child");
                        }
                        const auto choices = std::find_if(candidate->parent_mapping.legal_parents.begin(),
                                                          candidate->parent_mapping.legal_parents.end(),
                                                          [&parent_child](const ExactLegalParentChoices& item) {
                                                              return item.vertex == parent_child.second;
                                                          });
                        if (choices == candidate->parent_mapping.legal_parents.end() ||
                            !std::binary_search(choices->parents.begin(), choices->parents.end(), parent_child.first)) {
                            throw std::invalid_argument("published edge mapping contains a non-legal parent");
                        }
                    }
                }
            }
            if (edge_endpoint.candidate_results.size() != top.size()) {
                throw std::invalid_argument("edge endpoint omitted a top-ranked node set");
            }
        }
        set_new(root, "edge_endpoint", serialize_edge_endpoint(bit_count, edge_endpoint));

        std::ostringstream m0_evidence;
        m0_evidence.imbue(std::locale::classic());
        m0_evidence << "schema=longlineage.evo_m0_evidence.v1\n"
                    << "input_evidence_sha256=" << evidence.input_evidence_sha256 << '\n'
                    << "solver_route=" << route_name(topology.route) << '\n'
                    << "objective_evidence_sha256=" << objective_evidence_sha256 << '\n'
                    << "family_evidence_sha256=" << family.family_evidence_sha256 << '\n';
        const std::string m0_sha256 = digest_or_throw(m0_evidence.str());
        json_t* evolution = json_object();
        json_t* m0 = json_object();
        set_new(m0, "role", json_string("PRIMARY"));
        set_new(m0, "model", json_string("RECURRENCE_ALLOWED"));
        set_new(m0, "evidence_sha256", json_string(m0_sha256.c_str()));
        set_new(evolution, "evo_m0", m0);
        json_t* m1 = json_object();
        set_new(m1, "role", json_string("SENSITIVITY_ONLY"));
        set_new(m1, "model", json_string("STRICT_INFINITE_SITES"));
        set_new(m1, "cn_loh_gate", json_string("NOT_EVALUATED"));
        set_new(m1, "evidence_sha256", json_null());
        set_new(m1, "affects_primary", json_false());
        set_new(evolution, "evo_m1", m1);
        json_t* m2 = json_object();
        set_new(m2, "role", json_string("UNRESOLVED"));
        set_new(m2, "model", json_string("LOSS_SUPPORTED_DOLLO"));
        set_new(m2, "state", json_string("UNRESOLVED"));
        set_new(m2, "evidence_sha256", json_null());
        set_new(m2, "affects_primary", json_false());
        set_new(evolution, "evo_m2", m2);
        set_new(root, "evolution_models", evolution);

        json_t* unary = json_array();
        std::set<std::tuple<std::string, HypercubeVertex, HypercubeVertex>> unary_seen;
        const std::set<std::string> family_digest_set(family.candidate_digests.begin(), family.candidate_digests.end());
        std::vector<const MultiMutationEdgeProjection*> ordered_unary;
        ordered_unary.reserve(unary_projections.size());
        for (const MultiMutationEdgeProjection& projection : unary_projections) {
            ordered_unary.push_back(&projection);
        }
        std::sort(ordered_unary.begin(), ordered_unary.end(),
                  [](const MultiMutationEdgeProjection* left, const MultiMutationEdgeProjection* right) {
                      return std::tie(left->candidate_vertex_set_sha256, left->from_vertex, left->to_vertex,
                                      left->eligibility_evidence_sha256) <
                             std::tie(right->candidate_vertex_set_sha256, right->from_vertex, right->to_vertex,
                                      right->eligibility_evidence_sha256);
                  });
        for (const MultiMutationEdgeProjection* projection_pointer : ordered_unary) {
            const MultiMutationEdgeProjection& projection = *projection_pointer;
            const auto key =
                std::make_tuple(projection.candidate_vertex_set_sha256, projection.from_vertex, projection.to_vertex);
            const ExactStructuralCandidate* candidate =
                find_candidate(bit_count, topology.structural, projection.candidate_vertex_set_sha256);
            if (!family_complete || !family_digest_set.count(projection.candidate_vertex_set_sha256) ||
                !unary_seen.insert(key).second || candidate == nullptr ||
                !std::binary_search(candidate->vertices.begin(), candidate->vertices.end(), projection.from_vertex) ||
                !std::binary_search(candidate->vertices.begin(), candidate->vertices.end(), projection.to_vertex) ||
                projection.mutation_active_bits.size() < 2 ||
                !std::is_sorted(projection.mutation_active_bits.begin(), projection.mutation_active_bits.end()) ||
                std::adjacent_find(projection.mutation_active_bits.begin(), projection.mutation_active_bits.end()) !=
                    projection.mutation_active_bits.end() ||
                !is_sha256(projection.eligibility_evidence_sha256)) {
                throw std::invalid_argument("unary projection is not a valid candidate-bound equivalence");
            }
            const MultiMutationEdgeProjection expected_projection = make_unresolved_multi_mutation_projection(
                projection.candidate_vertex_set_sha256, projection.from_vertex, projection.to_vertex,
                evidence.active_loci, projection.eligibility_evidence_sha256);
            if (projection.mutation_active_bits != expected_projection.mutation_active_bits ||
                projection.order_count != expected_projection.order_count) {
                throw std::invalid_argument("unary projection mutation set or order count is not exact");
            }
            cpp_int order_count;
            if (!parse_nonnegative_decimal(projection.order_count, order_count) || order_count < 2) {
                throw std::invalid_argument("unary projection order count is invalid");
            }
            append_new(unary, serialize_unary_projection(bit_count, projection));
        }
        set_new(root, "multi_mutation_edge_equivalence", unary);

        char* dumped = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
        if (dumped == nullptr) {
            throw std::runtime_error("Jansson canonical JSON serialization failed");
        }
        result.canonical_json = dumped;
        std::free(dumped);
        result.canonical_json.push_back('\n');
        result.record_sha256 = digest_or_throw(result.canonical_json);
        result.valid = true;
        result.message = "topology_unit v2 serialized with fail-closed ranking and edge separation";
        json_decref(root);
        return result;
    } catch (const std::exception& error) {
        if (root != nullptr) {
            json_decref(root);
        }
        result.valid = false;
        result.canonical_json.clear();
        result.record_sha256.clear();
        result.message = error.what();
        return result;
    }
}

}  // namespace longlineage::solver
