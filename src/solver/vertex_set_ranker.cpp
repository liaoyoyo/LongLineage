// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/solver/vertex_set_ranker.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"

namespace longlineage::solver {
namespace {

constexpr double kLineSearchDerivativeTolerance = 1e-14;
constexpr std::size_t kLineSearchBisections = 80;

bool is_sha256(const std::string& value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char symbol) {
               return (symbol >= '0' && symbol <= '9') || (symbol >= 'a' && symbol <= 'f');
           });
}

std::string digest_or_throw(const std::string& canonical) {
    auto digest = longlineage::sha256_hex(canonical);
    if (!digest.ok() || !digest.value.has_value()) {
        throw std::runtime_error("OpenSSL SHA-256 failed while binding topology ranking");
    }
    return *digest.value;
}

bool valid_options(const VertexSetRankerOptions& options) noexcept {
    return std::isfinite(options.tie_tolerance) && std::isfinite(options.global_gap_tolerance) &&
           options.tie_tolerance >= 0.0 && options.global_gap_tolerance > 0.0 && options.maximum_iterations > 0;
}

bool valid_candidate(std::size_t bit_count, const std::vector<HypercubeVertex>& vertices) noexcept {
    const std::size_t vertex_count = std::size_t{1} << bit_count;
    return !vertices.empty() && vertices.front() == 0 && std::is_sorted(vertices.begin(), vertices.end()) &&
           std::adjacent_find(vertices.begin(), vertices.end()) == vertices.end() &&
           std::all_of(vertices.begin(), vertices.end(), [vertex_count](HypercubeVertex vertex) {
               return static_cast<std::size_t>(vertex) < vertex_count;
           });
}

double emission_probability(const QualityPatternEvidence& group, HypercubeVertex vertex, double minimum_error_rate,
                            double maximum_error_rate) {
    double probability = 1.0;
    for (std::size_t bit = 0; bit < group.pattern_rax.size(); ++bit) {
        const char observed = group.pattern_rax[bit];
        if (observed == 'X') {
            if (group.base_qualities[bit] != -1) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            continue;
        }
        if ((observed != 'R' && observed != 'A') || group.base_qualities[bit] < 0 ||
            group.base_qualities[bit] > std::numeric_limits<std::uint8_t>::max()) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const auto quality = static_cast<std::uint8_t>(group.base_qualities[bit]);
        const BiallelicEmission emission =
            conditional_biallelic_emission(quality, minimum_error_rate, maximum_error_rate);
        const bool state_is_alt = (vertex & static_cast<HypercubeVertex>(HypercubeVertex{1} << bit)) != 0;
        const bool observation_is_alt = observed == 'A';
        probability *= observation_is_alt == state_is_alt ? emission.match_probability : emission.flip_probability;
    }
    return probability;
}

struct PreparedLikelihood {
    std::vector<std::uint64_t> counts;
    std::vector<double> inactive_log_probabilities;
    std::vector<std::vector<double>> component_probabilities;
    double total_count = 0.0;
};

bool prepare_likelihood(const TopologyEvidenceBundle& evidence, const std::vector<HypercubeVertex>& vertices,
                        PreparedLikelihood& prepared) {
    const std::size_t bit_count = evidence.active_loci.size();
    for (const QualityPatternEvidence& group : evidence.scoring_groups) {
        if (group.pattern_rax.size() != bit_count || group.base_qualities.size() != bit_count ||
            group.multiplicity == 0 || !std::isfinite(group.inactive_log_probability) ||
            group.inactive_log_probability > 0.0) {
            return false;
        }
        std::vector<double> probabilities;
        probabilities.reserve(vertices.size());
        for (HypercubeVertex vertex : vertices) {
            const double probability =
                emission_probability(group, vertex, evidence.minimum_error_rate, evidence.maximum_error_rate);
            if (!(probability > 0.0) || !std::isfinite(probability)) {
                return false;
            }
            probabilities.push_back(probability);
        }
        prepared.counts.push_back(group.multiplicity);
        prepared.inactive_log_probabilities.push_back(group.inactive_log_probability);
        prepared.component_probabilities.push_back(std::move(probabilities));
        prepared.total_count += static_cast<double>(group.multiplicity);
    }
    return !prepared.counts.empty() && std::isfinite(prepared.total_count) && prepared.total_count > 0.0;
}

bool likelihood_and_gradient(const PreparedLikelihood& prepared, const std::vector<double>& weights,
                             double& log_likelihood, std::vector<double>& gradient) {
    log_likelihood = 0.0;
    gradient.assign(weights.size(), 0.0);
    for (std::size_t group_index = 0; group_index < prepared.counts.size(); ++group_index) {
        const auto& probabilities = prepared.component_probabilities[group_index];
        double mixture_probability = 0.0;
        for (std::size_t component = 0; component < weights.size(); ++component) {
            mixture_probability += weights[component] * probabilities[component];
        }
        if (!(mixture_probability > 0.0) || !std::isfinite(mixture_probability)) {
            return false;
        }
        const double count = static_cast<double>(prepared.counts[group_index]);
        log_likelihood += count * (prepared.inactive_log_probabilities[group_index] + std::log(mixture_probability));
        for (std::size_t component = 0; component < weights.size(); ++component) {
            gradient[component] += count * probabilities[component] / mixture_probability;
        }
    }
    return std::isfinite(log_likelihood) &&
           std::all_of(gradient.begin(), gradient.end(), [](double value) { return std::isfinite(value); });
}

double pairwise_direction_derivative(const PreparedLikelihood& prepared, const std::vector<double>& weights,
                                     std::size_t target, std::size_t away, double step) {
    double derivative = 0.0;
    for (std::size_t group_index = 0; group_index < prepared.counts.size(); ++group_index) {
        const auto& probabilities = prepared.component_probabilities[group_index];
        double current = 0.0;
        for (std::size_t component = 0; component < weights.size(); ++component) {
            current += weights[component] * probabilities[component];
        }
        const double difference = probabilities[target] - probabilities[away];
        const double at_step = current + step * difference;
        if (!(at_step > 0.0) || !std::isfinite(at_step)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        derivative += static_cast<double>(prepared.counts[group_index]) * difference / at_step;
    }
    return derivative;
}

double exact_pairwise_step(const PreparedLikelihood& prepared, const std::vector<double>& weights, std::size_t target,
                           std::size_t away, double maximum_step) {
    const double derivative_at_maximum = pairwise_direction_derivative(prepared, weights, target, away, maximum_step);
    if (!std::isfinite(derivative_at_maximum)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (derivative_at_maximum >= -kLineSearchDerivativeTolerance) {
        return maximum_step;
    }
    double lower = 0.0;
    double upper = maximum_step;
    for (std::size_t iteration = 0; iteration < kLineSearchBisections; ++iteration) {
        const double middle = (lower + upper) / 2.0;
        const double derivative = pairwise_direction_derivative(prepared, weights, target, away, middle);
        if (!std::isfinite(derivative)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (derivative > 0.0) {
            lower = middle;
        } else {
            upper = middle;
        }
    }
    return (lower + upper) / 2.0;
}

CandidateVertexSetScore fit_candidate(const TopologyEvidenceBundle& evidence,
                                      const std::vector<HypercubeVertex>& vertices,
                                      const VertexSetRankerOptions& options) {
    CandidateVertexSetScore score;
    score.vertices = vertices;
    score.vertex_set_sha256 = exact_vertex_set_sha256(evidence.active_loci.size(), vertices);

    PreparedLikelihood prepared;
    if (!prepare_likelihood(evidence, vertices, prepared)) {
        return score;
    }
    std::vector<double> weights(vertices.size(), 1.0 / static_cast<double>(vertices.size()));
    std::vector<double> gradient;
    double log_likelihood = 0.0;
    for (std::size_t iteration = 0; iteration < options.maximum_iterations; ++iteration) {
        if (!likelihood_and_gradient(prepared, weights, log_likelihood, gradient)) {
            return score;
        }
        const auto best_gradient = std::max_element(gradient.begin(), gradient.end());
        const std::size_t target = static_cast<std::size_t>(std::distance(gradient.begin(), best_gradient));
        std::size_t away = target;
        double away_gradient = std::numeric_limits<double>::infinity();
        for (std::size_t component = 0; component < weights.size(); ++component) {
            if (weights[component] > 0.0 && gradient[component] < away_gradient) {
                away = component;
                away_gradient = gradient[component];
            }
        }
        // For this concave simplex likelihood, w dot gradient equals the
        // total molecule count. Therefore max(gradient)-N is a global
        // first-order upper bound on the remaining log-likelihood gain.
        double gap = *best_gradient - prepared.total_count;
        if (gap < 0.0 && gap > -64.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, prepared.total_count)) {
            gap = 0.0;
        }
        score.log_likelihood = log_likelihood;
        score.mixture_weights = weights;
        score.global_log_likelihood_gap_bound = gap;
        score.iterations = iteration + 1;
        if (gap <= options.global_gap_tolerance && gap >= 0.0) {
            score.converged = true;
            return score;
        }
        if (!(gap > 0.0) || !std::isfinite(gap)) {
            return score;
        }

        if (away == target || !(weights[away] > 0.0)) {
            return score;
        }
        // Exchange mass only between the best forward component and the
        // weakest positive support component. Exact one-dimensional line
        // search avoids the slow all-component shrinkage of a vertex step.
        const double maximum_step = weights[away];
        const double step = exact_pairwise_step(prepared, weights, target, away, maximum_step);
        if (!(step > 0.0) || !(step <= 1.0) || step > maximum_step || !std::isfinite(step)) {
            return score;
        }
        weights[target] += step;
        weights[away] -= step;
        if (weights[away] < 64.0 * std::numeric_limits<double>::epsilon()) {
            weights[target] += weights[away];
            weights[away] = 0.0;
        }
    }

    if (likelihood_and_gradient(prepared, weights, log_likelihood, gradient)) {
        const double maximum_gradient = *std::max_element(gradient.begin(), gradient.end());
        score.log_likelihood = log_likelihood;
        score.mixture_weights = std::move(weights);
        score.global_log_likelihood_gap_bound = std::max(0.0, maximum_gradient - prepared.total_count);
        score.iterations = options.maximum_iterations;
    }
    return score;
}

void append_double(std::ostringstream& output, double value) {
    output << std::scientific << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
}

std::string ranking_evidence_digest(const TopologyEvidenceBundle& evidence, const VertexSetRankerOptions& options,
                                    const std::vector<CandidateVertexSetScore>& scores) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "schema=longlineage.topology_ranking_evidence.v1\n"
              << "input_evidence_sha256=" << evidence.input_evidence_sha256 << '\n'
              << "q=" << evidence.active_loci.size() << '\n'
              << "tie_tolerance=";
    append_double(canonical, options.tie_tolerance);
    canonical << "\nglobal_gap_tolerance=";
    append_double(canonical, options.global_gap_tolerance);
    canonical << "\nmaximum_iterations=" << options.maximum_iterations << '\n';
    for (const CandidateVertexSetScore& score : scores) {
        canonical << "candidate=" << score.vertex_set_sha256 << '\t';
        append_double(canonical, score.log_likelihood);
        canonical << '\t';
        append_double(canonical, score.global_log_likelihood_gap_bound);
        canonical << '\t' << score.iterations << '\t' << (score.converged ? 1 : 0);
        for (double weight : score.mixture_weights) {
            canonical << '\t';
            append_double(canonical, weight);
        }
        canonical << '\n';
    }
    return digest_or_throw(canonical.str());
}

}  // namespace

std::string exact_vertex_set_sha256(std::size_t bit_count, const std::vector<HypercubeVertex>& vertices) {
    if (bit_count > kMaximumExactHypercubeBits || !valid_candidate(bit_count, vertices)) {
        return {};
    }
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "schema=longlineage.exact_vertex_set.v1\n"
              << "q=" << bit_count << "\nvertices=";
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (index != 0) {
            canonical << ',';
        }
        canonical << vertices[index];
    }
    canonical << '\n';
    return digest_or_throw(canonical.str());
}

VertexSetRankingResult rank_complete_vertex_sets(const TopologyEvidenceBundle& evidence,
                                                 const ExactStructuralResult& structural,
                                                 const VertexSetRankerOptions& options) {
    VertexSetRankingResult result;
    if (evidence.state != EvidenceAdapterState::kReady || evidence.active_loci.empty() ||
        evidence.active_loci.size() > kMaximumExactHypercubeBits || !is_sha256(evidence.input_evidence_sha256) ||
        !valid_options(options)) {
        result.message = "ranking requires ready evidence and valid finite options";
        return result;
    }
    if (structural.objective_state != ExactObjectiveState::kObjectiveCertified) {
        result.state = VertexSetRankingState::kNotRunObjectiveAbstain;
        result.message = "objective is not certified; abundance ranking was not run";
        return result;
    }
    if (options.family_gate == FamilyRankingGate::kForceIncompleteDeadline) {
        result.state = VertexSetRankingState::kNotRunFamilyIncompleteDeadline;
        result.message = "family deadline gate forced incomplete; no candidate was scored";
        return result;
    }
    if (structural.family_state == ExactFamilyState::kFamilyIncompleteCap) {
        result.state = VertexSetRankingState::kNotRunFamilyIncompleteCap;
        result.message = "structural family is incomplete at cap; no candidate was scored";
        return result;
    }
    if (structural.family_state != ExactFamilyState::kFamilyComplete || !structural.family_enumeration_exhausted ||
        structural.minimum_family.empty()) {
        result.state = VertexSetRankingState::kNotRunObjectiveAbstain;
        result.message = "structural family is not an exhausted complete family";
        return result;
    }

    std::vector<std::vector<HypercubeVertex>> family;
    family.reserve(structural.minimum_family.size());
    for (const ExactStructuralCandidate& candidate : structural.minimum_family) {
        if (!valid_candidate(evidence.active_loci.size(), candidate.vertices)) {
            result.message = "complete family contains a malformed vertex set";
            return result;
        }
        family.push_back(candidate.vertices);
    }
    std::sort(family.begin(), family.end());
    if (std::adjacent_find(family.begin(), family.end()) != family.end()) {
        result.message = "complete family contains a duplicate vertex set";
        return result;
    }

    result.candidate_scores.reserve(family.size());
    for (const auto& vertices : family) {
        result.candidate_scores.push_back(fit_candidate(evidence, vertices, options));
    }
    result.evaluated_vertex_set_count = result.candidate_scores.size();
    result.ranking_evidence_sha256 = ranking_evidence_digest(evidence, options, result.candidate_scores);

    const bool all_converged = std::all_of(
        result.candidate_scores.begin(), result.candidate_scores.end(), [](const CandidateVertexSetScore& score) {
            return score.converged && !score.vertex_set_sha256.empty() && std::isfinite(score.log_likelihood) &&
                   std::isfinite(score.global_log_likelihood_gap_bound);
        });
    if (!all_converged) {
        result.state = VertexSetRankingState::kAbstainNumericalCertificate;
        result.best_vertex_set_tie_class.clear();
        result.best_log_likelihood.reset();
        result.certificate_sha256.clear();
        result.message = "at least one exhaustive candidate lacks a global gap certificate";
        return result;
    }

    const double best = std::max_element(result.candidate_scores.begin(), result.candidate_scores.end(),
                                         [](const CandidateVertexSetScore& left, const CandidateVertexSetScore& right) {
                                             return left.log_likelihood < right.log_likelihood;
                                         })
                            ->log_likelihood;
    result.best_log_likelihood = best;
    for (const CandidateVertexSetScore& score : result.candidate_scores) {
        const double certified_upper = score.log_likelihood + score.global_log_likelihood_gap_bound;
        if (best - certified_upper <= options.tie_tolerance) {
            result.best_vertex_set_tie_class.push_back(score.vertex_set_sha256);
        }
    }
    std::sort(result.best_vertex_set_tie_class.begin(), result.best_vertex_set_tie_class.end());

    std::ostringstream certificate;
    certificate.imbue(std::locale::classic());
    certificate << "schema=longlineage.topology_ranking_certificate.v1\n"
                << "ranking_evidence_sha256=" << result.ranking_evidence_sha256 << '\n'
                << "mode=EXHAUSTIVE\n"
                << "evaluated_vertex_set_count=" << result.evaluated_vertex_set_count << '\n'
                << "best_score=";
    append_double(certificate, best);
    certificate << '\n';
    for (const std::string& digest : result.best_vertex_set_tie_class) {
        certificate << "best=" << digest << '\n';
    }
    result.certificate_sha256 = digest_or_throw(certificate.str());
    result.state = VertexSetRankingState::kRankingComplete;
    result.message =
        "every complete-family vertex set scored once with a global "
        "simplex-likelihood gap certificate";
    return result;
}

}  // namespace longlineage::solver
