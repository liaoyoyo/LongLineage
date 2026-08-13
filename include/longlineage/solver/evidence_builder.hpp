// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "longlineage/solver/small_q_oracle.hpp"

namespace longlineage::solver {

enum class EvidenceAdapterState {
    kReady,
    kNoInformativeRead,
    kNoStructuralAlternate,
    kActiveDimensionUnsupported,
    kMalformedEvidence,
};

struct ReadPatternObservation {
    // Original component-locus order. R/A are fixed calls. O is a measured
    // non-REF/non-ALT base and X is unavailable; both are marginalized.
    std::string pattern_raox;
    std::vector<std::optional<std::uint8_t>> base_qualities;
    std::uint64_t multiplicity = 1;
};

struct EvidenceBuilderOptions {
    std::uint64_t structural_minimum_multiplicity = 1;
    double minimum_error_rate = 1e-6;
    double maximum_error_rate = 0.25;
};

struct StructuralPatternEvidence {
    std::string pattern_rax;
    std::uint64_t multiplicity = 0;
    bool partial = false;
};

struct QualityPatternEvidence {
    // Pattern and qualities are compressed to active ALT loci. X qualities are
    // exactly -1. Fixed calls outside the active universe contribute the same
    // log factor to every candidate and are retained separately.
    std::string pattern_rax;
    std::vector<std::int16_t> base_qualities;
    std::uint64_t multiplicity = 0;
    double inactive_log_probability = 0.0;
};

struct BiallelicEmission {
    double error_rate = 0.0;
    double match_probability = 0.0;
    double flip_probability = 0.0;
};

struct TopologyEvidenceBundle {
    EvidenceAdapterState state = EvidenceAdapterState::kMalformedEvidence;
    std::size_t original_locus_count = 0;
    std::vector<std::size_t> active_loci;
    std::vector<StructuralPatternEvidence> structural_patterns;
    std::vector<QualityPatternEvidence> scoring_groups;
    TopologyProblem structural_problem;
    std::array<std::uint64_t, 4> raw_code_counts{};
    std::uint64_t input_observations = 0;
    std::uint64_t informative_observations = 0;
    std::uint64_t all_missing_observations = 0;
    double minimum_error_rate = 1e-6;
    double maximum_error_rate = 0.25;
    std::string input_evidence_sha256;
    std::string message;
};

BiallelicEmission conditional_biallelic_emission(std::uint8_t base_quality, double minimum_error_rate,
                                                 double maximum_error_rate);

// Canonicalize observation groups, map O->X, derive structural active ALT
// loci, compress structural/scoring evidence, and bind the order-invariant
// evidence stream by SHA-256.
TopologyEvidenceBundle build_topology_evidence(const std::vector<ReadPatternObservation>& observations,
                                               const EvidenceBuilderOptions& options = {});

}  // namespace longlineage::solver
