// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/solver/evidence_builder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "longlineage/common/digest.hpp"
#include "longlineage/solver/topology_types.hpp"

namespace longlineage::solver {
namespace {

struct CanonicalObservationKey {
    std::string raw_pattern;
    std::vector<std::int16_t> qualities;

    bool operator<(const CanonicalObservationKey& other) const {
        return std::tie(raw_pattern, qualities) < std::tie(other.raw_pattern, other.qualities);
    }
};

bool checked_add(std::uint64_t value, std::uint64_t increment, std::uint64_t& output) noexcept {
    if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
        return false;
    }
    output = value + increment;
    return true;
}

std::size_t code_index(char code) {
    switch (code) {
        case 'R':
            return 0;
        case 'A':
            return 1;
        case 'O':
            return 2;
        case 'X':
            return 3;
        default:
            throw std::invalid_argument("unsupported evidence call code");
    }
}

std::string normalized_rax(const std::string& raw_pattern) {
    std::string normalized = raw_pattern;
    std::replace(normalized.begin(), normalized.end(), 'O', 'X');
    return normalized;
}

bool is_informative(const std::string& pattern) {
    return pattern.find('R') != std::string::npos || pattern.find('A') != std::string::npos;
}

std::string digest_or_throw(const std::string& canonical) {
    auto digest = longlineage::sha256_hex(canonical);
    if (!digest.ok() || !digest.value.has_value()) {
        throw std::runtime_error("OpenSSL SHA-256 failed while binding topology evidence");
    }
    return *digest.value;
}

TopologyEvidenceBundle malformed(std::string message, double minimum_error_rate = 1e-6,
                                 double maximum_error_rate = 0.25) {
    TopologyEvidenceBundle result;
    result.state = EvidenceAdapterState::kMalformedEvidence;
    result.minimum_error_rate = minimum_error_rate;
    result.maximum_error_rate = maximum_error_rate;
    result.message = std::move(message);
    return result;
}

}  // namespace

BiallelicEmission conditional_biallelic_emission(std::uint8_t base_quality, double minimum_error_rate,
                                                 double maximum_error_rate) {
    if (!(minimum_error_rate > 0.0) || !(maximum_error_rate < 0.5) || minimum_error_rate > maximum_error_rate ||
        !std::isfinite(minimum_error_rate) || !std::isfinite(maximum_error_rate)) {
        throw std::invalid_argument("invalid conditional biallelic error-rate bounds");
    }
    const double phred_error = std::pow(10.0, -static_cast<double>(base_quality) / 10.0);
    const double error = std::clamp(phred_error, minimum_error_rate, maximum_error_rate);
    const double denominator = 1.0 - (2.0 * error / 3.0);
    return BiallelicEmission{
        error,
        (1.0 - error) / denominator,
        (error / 3.0) / denominator,
    };
}

TopologyEvidenceBundle build_topology_evidence(const std::vector<ReadPatternObservation>& observations,
                                               const EvidenceBuilderOptions& options) {
    if (options.structural_minimum_multiplicity == 0) {
        return malformed("structural_minimum_multiplicity must be positive", options.minimum_error_rate,
                         options.maximum_error_rate);
    }
    try {
        static_cast<void>(conditional_biallelic_emission(0, options.minimum_error_rate, options.maximum_error_rate));
    } catch (const std::invalid_argument& error) {
        return malformed(error.what(), options.minimum_error_rate, options.maximum_error_rate);
    }

    TopologyEvidenceBundle result;
    result.minimum_error_rate = options.minimum_error_rate;
    result.maximum_error_rate = options.maximum_error_rate;
    if (observations.empty()) {
        result.state = EvidenceAdapterState::kNoInformativeRead;
        result.input_evidence_sha256 = digest_or_throw("observations=0\n");
        result.message = "no read-pattern observations";
        return result;
    }

    const std::size_t locus_count = observations.front().pattern_raox.size();
    if (locus_count == 0 || locus_count > 64) {
        return malformed("read-pattern locus count must be in [1,64]", options.minimum_error_rate,
                         options.maximum_error_rate);
    }
    result.original_locus_count = locus_count;

    std::map<CanonicalObservationKey, std::uint64_t> canonical_groups;
    std::map<std::string, std::uint64_t> structural_counts;
    for (const ReadPatternObservation& observation : observations) {
        if (observation.pattern_raox.size() != locus_count || observation.base_qualities.size() != locus_count ||
            observation.multiplicity == 0) {
            return malformed(
                "observation length differs from the component or "
                "multiplicity is zero",
                options.minimum_error_rate, options.maximum_error_rate);
        }
        CanonicalObservationKey key;
        key.raw_pattern = observation.pattern_raox;
        key.qualities.reserve(locus_count);
        for (std::size_t locus = 0; locus < locus_count; ++locus) {
            const char code = observation.pattern_raox[locus];
            if (code != 'R' && code != 'A' && code != 'O' && code != 'X') {
                return malformed("pattern contains a code outside R/A/O/X", options.minimum_error_rate,
                                 options.maximum_error_rate);
            }
            const auto quality = observation.base_qualities[locus];
            if (code == 'R' || code == 'A') {
                if (!quality.has_value() || *quality == std::numeric_limits<std::uint8_t>::max()) {
                    return malformed("fixed R/A call lacks a usable base quality", options.minimum_error_rate,
                                     options.maximum_error_rate);
                }
                key.qualities.push_back(static_cast<std::int16_t>(*quality));
            } else {
                if (quality.has_value()) {
                    return malformed("marginalized O/X call must not carry a base quality", options.minimum_error_rate,
                                     options.maximum_error_rate);
                }
                key.qualities.push_back(-1);
            }
            std::uint64_t updated = 0;
            if (!checked_add(result.raw_code_counts[code_index(code)], observation.multiplicity, updated)) {
                return malformed("raw evidence counter overflow", options.minimum_error_rate,
                                 options.maximum_error_rate);
            }
            result.raw_code_counts[code_index(code)] = updated;
        }

        std::uint64_t updated = 0;
        if (!checked_add(canonical_groups[key], observation.multiplicity, updated)) {
            return malformed("canonical observation multiplicity overflow", options.minimum_error_rate,
                             options.maximum_error_rate);
        }
        canonical_groups[key] = updated;

        const std::string normalized = normalized_rax(observation.pattern_raox);
        if (!checked_add(structural_counts[normalized], observation.multiplicity, updated)) {
            return malformed("structural pattern multiplicity overflow", options.minimum_error_rate,
                             options.maximum_error_rate);
        }
        structural_counts[normalized] = updated;
        if (!checked_add(result.input_observations, observation.multiplicity, updated)) {
            return malformed("input observation counter overflow", options.minimum_error_rate,
                             options.maximum_error_rate);
        }
        result.input_observations = updated;
    }

    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "schema=longlineage.topology_evidence.v1\n"
              << "k=" << locus_count << '\n'
              << "structural_minimum_multiplicity=" << options.structural_minimum_multiplicity << '\n'
              << "minimum_error_rate=" << std::hexfloat << options.minimum_error_rate << '\n'
              << "maximum_error_rate=" << options.maximum_error_rate << '\n';
    for (const auto& entry : canonical_groups) {
        canonical << entry.first.raw_pattern << '\t';
        for (std::size_t index = 0; index < entry.first.qualities.size(); ++index) {
            if (index != 0) {
                canonical << ',';
            }
            if (entry.first.qualities[index] < 0) {
                canonical << '.';
            } else {
                canonical << entry.first.qualities[index];
            }
        }
        canonical << '\t' << entry.second << '\n';
    }
    result.input_evidence_sha256 = digest_or_throw(canonical.str());

    for (const auto& entry : canonical_groups) {
        const std::string normalized = normalized_rax(entry.first.raw_pattern);
        std::uint64_t updated = 0;
        if (is_informative(normalized)) {
            if (!checked_add(result.informative_observations, entry.second, updated)) {
                return malformed("informative observation counter overflow", options.minimum_error_rate,
                                 options.maximum_error_rate);
            }
            result.informative_observations = updated;
        } else {
            if (!checked_add(result.all_missing_observations, entry.second, updated)) {
                return malformed("all-missing observation counter overflow", options.minimum_error_rate,
                                 options.maximum_error_rate);
            }
            result.all_missing_observations = updated;
        }
    }

    for (const auto& entry : structural_counts) {
        if (entry.second < options.structural_minimum_multiplicity || !is_informative(entry.first)) {
            continue;
        }
        for (std::size_t locus = 0; locus < locus_count; ++locus) {
            if (entry.first[locus] == 'A') {
                result.active_loci.push_back(locus);
            }
        }
    }
    std::sort(result.active_loci.begin(), result.active_loci.end());
    result.active_loci.erase(std::unique(result.active_loci.begin(), result.active_loci.end()),
                             result.active_loci.end());

    if (result.active_loci.empty()) {
        if (result.informative_observations == 0) {
            result.state = EvidenceAdapterState::kNoInformativeRead;
            result.message = "all observations are O/X and candidate-constant";
        } else {
            result.state = EvidenceAdapterState::kNoStructuralAlternate;
            result.message = "no retained structural pattern contains an ALT call";
        }
        return result;
    }

    std::map<std::string, std::uint64_t> compressed_structural;
    for (const auto& entry : structural_counts) {
        if (entry.second < options.structural_minimum_multiplicity || !is_informative(entry.first)) {
            continue;
        }
        std::string compressed;
        compressed.reserve(result.active_loci.size());
        for (std::size_t locus : result.active_loci) {
            compressed.push_back(entry.first[locus]);
        }
        std::uint64_t updated = 0;
        if (!checked_add(compressed_structural[compressed], entry.second, updated)) {
            return malformed("compressed structural multiplicity overflow", options.minimum_error_rate,
                             options.maximum_error_rate);
        }
        compressed_structural[compressed] = updated;
    }
    result.structural_problem.k = result.active_loci.size();
    for (const auto& entry : compressed_structural) {
        const bool partial = entry.first.find('X') != std::string::npos;
        result.structural_patterns.push_back(StructuralPatternEvidence{entry.first, entry.second, partial});
        if (partial) {
            result.structural_problem.partial_patterns.push_back(entry.first);
        } else {
            result.structural_problem.full_patterns.push_back(entry.first);
        }
    }

    std::vector<bool> active(locus_count, false);
    for (std::size_t locus : result.active_loci) {
        active[locus] = true;
    }
    for (const auto& entry : canonical_groups) {
        const std::string normalized = normalized_rax(entry.first.raw_pattern);
        if (!is_informative(normalized)) {
            continue;
        }

        QualityPatternEvidence group;
        group.pattern_rax.reserve(result.active_loci.size());
        group.base_qualities.reserve(result.active_loci.size());
        group.multiplicity = entry.second;
        for (std::size_t locus = 0; locus < locus_count; ++locus) {
            const char symbol = normalized[locus];
            if (active[locus]) {
                group.pattern_rax.push_back(symbol);
                group.base_qualities.push_back(entry.first.qualities[locus]);
                continue;
            }
            if (symbol == 'R' || symbol == 'A') {
                const auto quality = static_cast<std::uint8_t>(entry.first.qualities[locus]);
                const BiallelicEmission emission =
                    conditional_biallelic_emission(quality, options.minimum_error_rate, options.maximum_error_rate);
                const double probability = symbol == 'R' ? emission.match_probability : emission.flip_probability;
                group.inactive_log_probability += std::log(probability);
            }
        }
        result.scoring_groups.push_back(std::move(group));
    }

    if (result.informative_observations == 0 || result.scoring_groups.empty()) {
        result.state = EvidenceAdapterState::kNoInformativeRead;
        result.message = "all observations are O/X and candidate-constant";
        return result;
    }
    if (result.active_loci.size() > kMaximumExactHypercubeBits) {
        result.state = EvidenceAdapterState::kActiveDimensionUnsupported;
        result.message = "active ALT dimension exceeds the exact 12-bit boundary";
        return result;
    }

    result.state = EvidenceAdapterState::kReady;
    result.message = "R/A BQ evidence ready; O/X marginalized and active loci compressed";
    return result;
}

}  // namespace longlineage::solver
