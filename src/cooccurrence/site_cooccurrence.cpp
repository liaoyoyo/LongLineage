// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/cooccurrence/site_cooccurrence.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "longlineage/common/digest.hpp"
#include "longlineage/m1/science.hpp"

namespace longlineage::cooccurrence {
namespace {

[[nodiscard]] bool within_halo(const VariantSite& focal, const VariantSite& partner) noexcept {
    if (focal.contig != partner.contig || focal.site_order == partner.site_order) {
        return false;
    }
    const std::uint64_t lhs = focal.position.value();
    const std::uint64_t rhs = partner.position.value();
    const std::uint64_t distance = lhs > rhs ? lhs - rhs : rhs - lhs;
    return distance <= 5000;
}

[[nodiscard]] std::string conditional_stratum(const pipeline::JoinedReadEvidence& read) {
    std::string output(pipeline::hp_family_for_token(read.latest_tags.hp));
    output.append("|PS=");
    output.append(read.latest_tags.ps.has_value() ? std::to_string(*read.latest_tags.ps) : ".");
    output.append("|strand=");
    output.push_back(to_char(read.projection.strand));
    return output;
}

[[nodiscard]] double cramer_v_relaxed(const std::vector<std::string>& labels,
                                      const std::vector<AlleleCall>& categories) {
    const GroupAlleleAssociation summary = summarize_group_allele_association(labels, categories, 1, 1, 1);
    return summary.cramers_v.value_or(0.0);
}

[[nodiscard]] std::uint32_t hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint32_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint32_t>(value - 'a' + 10);
    }
    throw std::invalid_argument("SHA-256 digest contains a non-hex digit");
}

[[nodiscard]] std::uint32_t deterministic_pair_seed(const PairInference& pair) {
    std::array<std::string, 10> parts{
        pair.focal.dataset_id,
        pair.focal.contig.value(),
        std::to_string(pair.focal.position.value()),
        std::string(1, pair.focal.reference),
        std::string(1, pair.focal.alternate),
        pair.partner.contig.value(),
        std::to_string(pair.partner.position.value()),
        std::string(1, pair.partner.reference),
        std::string(1, pair.partner.alternate),
        "conditional_sensitivity",
    };
    std::string payload;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) {
            payload.push_back('\x1f');
        }
        payload.append(parts[index]);
    }
    auto digest = sha256_hex(payload);
    if (!digest.ok() || digest.value->size() != 64) {
        throw std::runtime_error("cannot hash conditional permutation seed");
    }
    // Python: int.from_bytes(digest[:8], "big") % 2**32. In hex this is
    // bytes 4..7, i.e. characters 8..15.
    std::uint32_t seed = 0;
    for (std::size_t index = 8; index < 16; ++index) {
        seed = static_cast<std::uint32_t>((seed << 4U) | hex_digit((*digest.value)[index]));
    }
    return seed;
}

[[nodiscard]] ConditionalPermutationResult conditional_permutation(const ConditionalPermutationPayload& payload,
                                                                   std::uint64_t seed, std::uint64_t permutations) {
    if (payload.labels.size() != payload.categories.size() || payload.labels.size() != payload.strata.size() ||
        permutations == 0) {
        throw std::invalid_argument("conditional permutation payload/options are malformed");
    }
    ConditionalPermutationResult output;
    output.strata = std::set<std::string>(payload.strata.begin(), payload.strata.end()).size();
    std::set<std::string> groups(payload.labels.begin(), payload.labels.end());
    std::set<AlleleCall> categories(payload.categories.begin(), payload.categories.end());
    if (groups.size() < 2 || categories.size() < 2) {
        output.status = ConditionalStatus::kNotIdentifiableDegenerateTable;
        return output;
    }

    std::vector<std::string> stratum_order;
    std::map<std::string, std::size_t> stratum_index;
    std::vector<std::vector<std::size_t>> indices_by_stratum;
    for (std::size_t index = 0; index < payload.strata.size(); ++index) {
        const auto inserted = stratum_index.emplace(payload.strata[index], indices_by_stratum.size());
        if (inserted.second) {
            stratum_order.push_back(payload.strata[index]);
            indices_by_stratum.emplace_back();
        }
        indices_by_stratum[inserted.first->second].push_back(index);
    }
    if (stratum_order.size() != indices_by_stratum.size()) {
        throw std::logic_error("conditional stratum order drift");
    }
    std::vector<std::vector<std::size_t>> exchangeable;
    for (const auto& indices : indices_by_stratum) {
        std::set<std::string> labels;
        std::set<AlleleCall> states;
        for (const std::size_t index : indices) {
            labels.insert(payload.labels[index]);
            states.insert(payload.categories[index]);
        }
        if (indices.size() >= 2 && labels.size() >= 2) {
            exchangeable.push_back(indices);
            ++output.exchangeable_strata;
            if (states.size() >= 2) {
                ++output.informative_exchangeable_strata;
            }
        }
    }
    if (output.informative_exchangeable_strata == 0) {
        output.status = ConditionalStatus::kNotIdentifiableNonexchangeable;
        return output;
    }

    const double observed = cramer_v_relaxed(payload.labels, payload.categories);
    m1::NumpyPcg64 generator(seed);
    for (std::uint64_t permutation = 0; permutation < permutations; ++permutation) {
        std::vector<std::string> labels = payload.labels;
        for (const auto& indices : exchangeable) {
            const std::vector<std::size_t> order = generator.permutation(indices.size());
            std::vector<std::string> original;
            original.reserve(indices.size());
            for (const std::size_t index : indices) {
                original.push_back(labels[index]);
            }
            for (std::size_t offset = 0; offset < indices.size(); ++offset) {
                labels[indices[offset]] = original[order[offset]];
            }
        }
        if (cramer_v_relaxed(labels, payload.categories) >= observed - 1e-12) {
            ++output.exceedance;
        }
    }
    output.permutations = permutations;
    output.p_value = (static_cast<double>(output.exceedance) + 1.0) / (static_cast<double>(permutations) + 1.0);
    output.permutable = true;
    output.status = ConditionalStatus::kPermutable;
    return output;
}

[[nodiscard]] ParseResult<std::string> pair_digest(const PairInference& pair) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "longlineage.cooccurrence_pair\t1.0.0\n"
           << pair.focal.dataset_order << '\t' << pair.focal.site_order << '\t' << pair.partner.site_order << '\t'
           << pair.distance_bp << '\n'
           << "family=" << to_string(pair.family_status) << ";n=" << pair.endpoint_a.n_informative
           << ";exact=" << to_string(pair.exact.status) << ";p=";
    if (pair.exact.p_value.has_value()) {
        stream << std::setprecision(17) << *pair.exact.p_value;
    } else {
        stream << '.';
    }
    stream << ";bh=";
    if (pair.q_global_bh.has_value()) {
        stream << std::setprecision(17) << *pair.q_global_bh;
    } else {
        stream << '.';
    }
    stream << ";by=";
    if (pair.q_global_by.has_value()) {
        stream << std::setprecision(17) << *pair.q_global_by;
    } else {
        stream << '.';
    }
    stream << ";formal=" << pair.formal_pair_by_confirmed << '\n';
    for (const auto& row : pair.endpoint_a.table) {
        stream << row[0] << ',' << row[1] << ';';
    }
    stream << '\n';
    for (const std::uint64_t cell : pair.endpoint_b.counts.row_major_cells()) {
        stream << cell << ',';
    }
    stream << '\n';
    return sha256_hex(stream.str());
}

[[nodiscard]] ParseResult<std::string> site_digest(const pipeline::FocalSiteEvidence& focal,
                                                   const std::vector<PairInference>& pairs) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "longlineage.site_cooccurrence\t1.0.0\n"
           << focal.site.dataset_order << '\t' << focal.site.site_order << '\t' << pairs.size() << '\n';
    for (const auto& pair : pairs) {
        stream << pair.partner.site_order << '\t' << pair.semantic_sha256 << '\n';
    }
    return sha256_hex(stream.str());
}

}  // namespace

std::string_view to_string(PairFamilyStatus status) noexcept {
    switch (status) {
        case PairFamilyStatus::kIneligibleM2Screen:
            return "INELIGIBLE_M2_SCREEN";
        case PairFamilyStatus::kEligibleM2EndpointANotTestable:
            return "ELIGIBLE_M2_ENDPOINT_A_NOT_TESTABLE";
        case PairFamilyStatus::kEligibleM2ExactNotIdentifiable:
            return "ELIGIBLE_M2_EXACT_NOT_IDENTIFIABLE";
        case PairFamilyStatus::kEligibleM2ExactFamily:
            return "ELIGIBLE_M2_EXACT_FAMILY";
    }
    return "INELIGIBLE_M2_SCREEN";
}

std::string_view to_string(ConditionalStatus status) noexcept {
    switch (status) {
        case ConditionalStatus::kNotRunNotExactByDiscovery:
            return "NOT_RUN_NOT_EXACT_BY_DISCOVERY";
        case ConditionalStatus::kNotIdentifiableDegenerateTable:
            return "NOT_IDENTIFIABLE_DEGENERATE_TABLE";
        case ConditionalStatus::kNotIdentifiableNonexchangeable:
            return "NOT_IDENTIFIABLE_NONEXCHANGEABLE";
        case ConditionalStatus::kPermutable:
            return "PERMUTABLE";
    }
    return "NOT_IDENTIFIABLE_DEGENERATE_TABLE";
}

ParseResult<SiteCooccurrenceResult> build_site_cooccurrence(const pipeline::BlockScienceEvidence& block,
                                                            const pipeline::FocalSiteEvidence& focal,
                                                            const pipeline::SiteScienceResult& science) {
    try {
        SiteCooccurrenceResult output;
        if (!science.m1.analysis.has_value() || !science.m1.analysis->stable_null_multigroup) {
            auto digest = site_digest(focal, output.pairs);
            if (!digest.ok()) {
                return ParseResult<SiteCooccurrenceResult>::failure(digest.reason, std::move(digest.detail));
            }
            output.semantic_sha256 = std::move(*digest.value);
            return ParseResult<SiteCooccurrenceResult>::success_empty(std::move(output));
        }
        if (science.matrix.site_order != focal.site.site_order) {
            return ParseResult<SiteCooccurrenceResult>::failure(
                ParseReason::kMalformedValue, "site science matrix is bound to a different focal site");
        }

        std::vector<const pipeline::M1ReadAssignment*> core;
        for (const auto& assignment : science.assignments) {
            if (assignment.core) {
                core.push_back(&assignment);
            }
        }
        if (core.empty()) {
            return ParseResult<SiteCooccurrenceResult>::failure(ParseReason::kMalformedValue,
                                                                "stable M1 site has no core assignments");
        }

        for (const VariantSite& partner : block.markers) {
            if (!within_halo(focal.site, partner)) {
                continue;
            }
            ++output.partner_universe_size;
            PairInference pair{focal.site, partner};
            pair.distance_bp = static_cast<std::int64_t>(partner.position.value()) -
                               static_cast<std::int64_t>(focal.site.position.value());
            pair.group_count = science.m1.analysis->coarse_groups;

            std::vector<std::string> labels;
            std::vector<AlleleCall> partner_calls;
            std::vector<bool> noncallable;
            labels.reserve(core.size());
            partner_calls.reserve(core.size());
            noncallable.reserve(core.size());
            for (const auto* assignment : core) {
                const auto& read = block.reads.at(assignment->block_read_index);
                const ProjectedAlleleCall* call = pipeline::find_allele_call(read, partner.site_order);
                if (call == nullptr) {
                    return ParseResult<SiteCooccurrenceResult>::failure(ParseReason::kMalformedValue,
                                                                        "core read lacks a projected partner call");
                }
                labels.push_back(assignment->coarse_label);
                partner_calls.push_back(call->call);
                const bool missing = call->call != AlleleCall::kReference && call->call != AlleleCall::kAlternate;
                noncallable.push_back(missing);
                pair.noncallable_core_reads += missing;
                // Conditional permutations belong only to the governed M2
                // exact family. Retaining per-read strings for every stable
                // focal/partner pair would make whole-genome memory scale as
                // O(pairs * core_reads) even though ineligible pairs can never
                // enter that family.
                if (!missing && science.m2.decision.eligible) {
                    pair.conditional_payload.labels.push_back(assignment->coarse_label);
                    pair.conditional_payload.categories.push_back(call->call);
                    pair.conditional_payload.strata.push_back(conditional_stratum(read));
                }
            }
            pair.endpoint_a = summarize_group_allele_association(labels, partner_calls);
            if (pair.endpoint_a.testable) {
                pair.exact = fisher_freeman_halton_kx2(pair.endpoint_a.table);
            }
            pair.callability = summarize_binary_category_association(labels, noncallable);

            // Endpoint B is the complete four-state focal/partner overlap,
            // not the M1 R/A matrix subset. O and X at either endpoint remain
            // distinct cells and are required for the callability guardrail.
            for (const std::size_t read_index : focal.covering_read_indices) {
                const auto& read = block.reads.at(read_index);
                if (!read.projection.reference_interval.contains(partner.position)) {
                    continue;
                }
                const ProjectedAlleleCall* focal_call = pipeline::find_allele_call(read, focal.site.site_order);
                const ProjectedAlleleCall* partner_call = pipeline::find_allele_call(read, partner.site_order);
                if (focal_call == nullptr || partner_call == nullptr) {
                    return ParseResult<SiteCooccurrenceResult>::failure(
                        ParseReason::kMalformedValue, "focal covering read lacks a conserved focal/partner call");
                }
                pair.endpoint_b.counts.add(focal_call->call, partner_call->call);
                ++pair.pair_read_count;
            }
            if (!pair.endpoint_b.counts.conservation_holds(pair.pair_read_count)) {
                return ParseResult<SiteCooccurrenceResult>::failure(ParseReason::kMalformedValue,
                                                                    "pair 16-cell conservation failed");
            }
            pair.endpoint_b = summarize_four_state_relations(pair.endpoint_b.counts);

            if (!science.m2.decision.eligible) {
                pair.family_status = PairFamilyStatus::kIneligibleM2Screen;
            } else if (!pair.endpoint_a.testable) {
                pair.family_status = PairFamilyStatus::kEligibleM2EndpointANotTestable;
            } else if (!pair.exact.identifiable) {
                pair.family_status = PairFamilyStatus::kEligibleM2ExactNotIdentifiable;
            } else {
                pair.family_status = PairFamilyStatus::kEligibleM2ExactFamily;
            }
            auto digest = pair_digest(pair);
            if (!digest.ok()) {
                return ParseResult<SiteCooccurrenceResult>::failure(digest.reason, std::move(digest.detail));
            }
            pair.semantic_sha256 = std::move(*digest.value);
            output.pairs.push_back(std::move(pair));
        }
        auto digest = site_digest(focal, output.pairs);
        if (!digest.ok()) {
            return ParseResult<SiteCooccurrenceResult>::failure(digest.reason, std::move(digest.detail));
        }
        output.semantic_sha256 = std::move(*digest.value);
        return output.pairs.empty() ? ParseResult<SiteCooccurrenceResult>::success_empty(std::move(output))
                                    : ParseResult<SiteCooccurrenceResult>::success(std::move(output));
    } catch (const std::exception& error) {
        return ParseResult<SiteCooccurrenceResult>::failure(ParseReason::kMalformedValue,
                                                            std::string("site co-occurrence failed: ") + error.what());
    }
}

ParseResult<bool> finalize_global_pair_families(std::vector<PairInference>& pairs) {
    try {
        std::vector<std::optional<double>> endpoint_p;
        std::vector<std::optional<double>> callability_p;
        endpoint_p.reserve(pairs.size());
        callability_p.reserve(pairs.size());
        std::size_t family_size = 0;
        for (const auto& pair : pairs) {
            const bool in_family = pair.family_status == PairFamilyStatus::kEligibleM2ExactFamily &&
                                   pair.exact.identifiable && pair.exact.p_value.has_value();
            endpoint_p.push_back(in_family ? pair.exact.p_value : std::nullopt);
            family_size += in_family;
            callability_p.push_back(pair.callability.summary.testable ? pair.callability.p_analytic : std::nullopt);
        }
        const auto endpoint_bh = benjamini_hochberg(endpoint_p);
        const auto endpoint_by = benjamini_yekutieli(endpoint_p);
        const auto callability_bh = benjamini_hochberg(callability_p);
        const auto callability_by = benjamini_yekutieli(callability_p);
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            PairInference& pair = pairs[index];
            pair.fdr_family_size = family_size;
            pair.q_global_bh = endpoint_bh[index];
            pair.q_global_by = endpoint_by[index];
            pair.callability_q_global_bh = callability_bh[index];
            pair.callability_q_global_by = callability_by[index];
            pair.effect_gate_pass = pair.endpoint_a.testable && pair.endpoint_a.cramers_v.has_value() &&
                                    pair.endpoint_a.delta_alt_fraction.has_value() &&
                                    *pair.endpoint_a.cramers_v >= 0.30 && *pair.endpoint_a.delta_alt_fraction >= 0.50;
            pair.exact_bh_discovery = pair.family_status == PairFamilyStatus::kEligibleM2ExactFamily &&
                                      pair.q_global_bh.has_value() && *pair.q_global_bh <= 0.05 &&
                                      pair.effect_gate_pass;
            pair.exact_by_discovery = pair.family_status == PairFamilyStatus::kEligibleM2ExactFamily &&
                                      pair.q_global_by.has_value() && *pair.q_global_by <= 0.05 &&
                                      pair.effect_gate_pass;
            pair.callability_status = evaluate_callability(CallabilityInput{
                pair.noncallable_core_reads,
                pair.callability.summary.testable,
                pair.callability_q_global_by,
                pair.callability.summary.cramers_v,
            });
            auto digest = pair_digest(pair);
            if (!digest.ok()) {
                return ParseResult<bool>::failure(digest.reason, std::move(digest.detail));
            }
            pair.semantic_sha256 = std::move(*digest.value);
        }
        return ParseResult<bool>::success(true);
    } catch (const std::exception& error) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          std::string("global pair-family finalization failed: ") + error.what());
    }
}

ParseResult<bool> run_conditional_pair_sensitivity(std::vector<PairInference>& pairs, std::uint64_t permutations) {
    try {
        if (permutations != 999) {
            return ParseResult<bool>::failure(ParseReason::kUnsupportedValue,
                                              "v1 conditional sensitivity is frozen at 999 permutations");
        }
        for (PairInference& pair : pairs) {
            if (pair.exact_by_discovery) {
                pair.conditional =
                    conditional_permutation(pair.conditional_payload, deterministic_pair_seed(pair), permutations);
            }
            const bool callability_pass =
                pair.callability_status == CallabilityStatus::kPassAllCoreReadsCallable ||
                pair.callability_status == CallabilityStatus::kPassNoStrongDifferentialCallability;
            pair.formal_pair_by_confirmed = pair.exact_by_discovery && pair.effect_gate_pass &&
                                            pair.conditional.permutable && pair.conditional.p_value.has_value() &&
                                            *pair.conditional.p_value <= 0.05 && callability_pass;
            auto digest = pair_digest(pair);
            if (!digest.ok()) {
                return ParseResult<bool>::failure(digest.reason, std::move(digest.detail));
            }
            pair.semantic_sha256 = std::move(*digest.value);
        }
        return ParseResult<bool>::success(true);
    } catch (const std::exception& error) {
        return ParseResult<bool>::failure(ParseReason::kMalformedValue,
                                          std::string("conditional pair sensitivity failed: ") + error.what());
    }
}

}  // namespace longlineage::cooccurrence
