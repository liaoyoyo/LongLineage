// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "longlineage/artifact/dataset_artifacts.hpp"
#include "longlineage/common/digest.hpp"
#include "longlineage/io/reference_reader.hpp"
#include "longlineage/pipeline/block_science.hpp"
#include "longlineage/pipeline/dataset_plan.hpp"
#include "longlineage/pipeline/dataset_producer.hpp"
#include "longlineage/pipeline/dataset_runner.hpp"
#include "longlineage/pipeline/dataset_science_runner.hpp"
#include "longlineage/pipeline/site_matrix.hpp"
#include "longlineage/pipeline/site_science.hpp"

namespace {

using longlineage::AlignmentBlock;
using longlineage::AlleleCall;
using longlineage::BlockReadBatch;
using longlineage::ContigId;
using longlineage::DecodedBlockRead;
using longlineage::FocalSiteCost;
using longlineage::FullAlignmentIdentity;
using longlineage::Interval0;
using longlineage::LatestTags;
using longlineage::MethylationCall;
using longlineage::MmMlMnTags;
using longlineage::MmSkipSemantics;
using longlineage::Position1;
using longlineage::ReadProjectionIdentity;
using longlineage::SidecarFullIdentity;
using longlineage::SidecarLookup;
using longlineage::SidecarRecord;
using longlineage::Strand;
using longlineage::VariantSite;
using longlineage::VariantSiteSet;
using longlineage::pipeline::BlockScienceOptions;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_ml_interval_lookup_matches_frozen_sci17() {
    for (std::uint16_t raw = 0; raw <= 255U; ++raw) {
        const auto encoded = longlineage::artifact::canonical_ml_probability_interval(static_cast<std::uint8_t>(raw));
        const double lower = static_cast<double>(raw) / 256.0;
        const double upper = raw == 255U ? 1.0 : static_cast<double>(raw + 1U) / 256.0;
        check(encoded.first == longlineage::artifact::canonical_float64(lower) &&
                  encoded.second == longlineage::artifact::canonical_float64(upper),
              "ML interval lookup differs from frozen SCI17 at raw=" + std::to_string(raw));
    }
}

ContigId contig(std::string value) {
    auto parsed = ContigId::from_string(std::move(value));
    check(parsed.ok(), parsed.detail);
    return std::move(*parsed.value);
}

Position1 position(std::uint64_t value) {
    auto parsed = Position1::from_value(value);
    check(parsed.ok(), parsed.detail);
    return *parsed.value;
}

Interval0 interval(std::uint64_t begin, std::uint64_t end) {
    auto parsed = Interval0::from_bounds(begin, end);
    check(parsed.ok(), parsed.detail);
    return *parsed.value;
}

struct Fixture {
    AlignmentBlock block;
    std::vector<VariantSite> markers;
    BlockReadBatch batch;
    SidecarLookup sidecar;
    Interval0 reference_interval;
    std::string reference;
};

Fixture fixture(bool duplicate = true, bool reference_cpg = true) {
    const ContigId chromosome = contig("chrSynthetic");
    const Position1 focal_position = position(16);
    const Position1 partner_position = position(20);
    const FocalSiteCost focal{0, "synthetic", 100, chromosome, focal_position, 2, 100};
    AlignmentBlock block{7,   0,   "synthetic",      chromosome,       100, 5,
                         100, 100, interval(15, 16), interval(10, 30), 2,   {focal}};
    std::vector<VariantSite> markers{
        VariantSite{0, "synthetic", 0, 100, chromosome, 100, focal_position, 'C', 'T'},
        VariantSite{0, "synthetic", 1, 101, chromosome, 100, partner_position, 'A', 'G'},
    };

    const ReadProjectionIdentity projection{"read-alpha", chromosome, interval(12, 24), 60, Strand::kForward};
    const FullAlignmentIdentity identity{projection, "read-alpha",         0,
                                         "12M",      std::string(64, 'a'), "MM:Z:C+m?;|ML:B:C,220"};
    std::string sequence = "AAATACTCAAAA";
    // focal zero-based 15 -> query 3 = T (ALT); partner zero-based 19 ->
    // query 7 = C (OTHER). q5 projects to zero-based reference 17.
    std::vector<std::uint8_t> qualities(12, 30);
    MmMlMnTags mm_ml{
        "C+m?,0;",
        {220},
        12,
        {MethylationCall{5, 0, 0, 220, 220.0 / 256.0, 221.0 / 256.0, MmSkipSemantics::kUnknown}},
    };
    DecodedBlockRead read{identity, sequence, qualities, mm_ml};
    BlockReadBatch batch;
    batch.block_sequence = block.sequence;
    batch.reads.push_back(read);
    if (duplicate) {
        batch.reads.push_back(read);
    }
    batch.counters.retained_records = batch.reads.size();

    SidecarLookup sidecar;
    const SidecarFullIdentity sidecar_identity{"read-alpha", chromosome, interval(12, 24), 0,
                                               longlineage::blake2b_64_hex("12M")};
    const SidecarRecord sidecar_record{projection, sidecar_identity, LatestTags{"1-1", 15}};
    check(sidecar.add(sidecar_record).ok(), "cannot add sidecar fixture row");
    if (duplicate) {
        check(sidecar.add(sidecar_record).ok(), "cannot add duplicate sidecar fixture row");
    }

    std::string reference(21, 'A');
    if (reference_cpg) {
        reference[7] = 'C';
        reference[8] = 'G';
    }
    return Fixture{std::move(block),   std::move(markers), std::move(batch),
                   std::move(sidecar), interval(10, 31),   std::move(reference)};
}

void test_positive_one_pass_and_duplicate_conservation() {
    Fixture input = fixture();
    auto evidence = longlineage::pipeline::build_block_science_evidence(
        input.block, input.markers, input.batch, input.sidecar, input.reference_interval, input.reference);
    check(evidence.ok(), evidence.detail);
    check(evidence.value->reads.size() == 1, "RG-only duplicate was not collapsed");
    check(evidence.value->focal_sites.size() == 1, "focal site count drift");
    check(evidence.value->focal_sites.front().covering_read_indices == std::vector<std::size_t>{0},
          "covering-read index drift");
    const auto& read = evidence.value->reads.front();
    check(read.raw_alignment_occurrences == 2 && read.sidecar_identity_occurrences == 2,
          "raw/sidecar occurrence conservation drift");
    check(read.latest_tags.hp == "1-1" && read.latest_tags.ps == 15, "latest HP/PS join drift");
    check(read.allele_calls.size() == 2, "marker projection cardinality drift");
    check(read.allele_calls[0].call == AlleleCall::kAlternate && read.allele_calls[1].call == AlleleCall::kOther,
          "R/A/O/X projection drift");
    check(read.methylation_calls.size() == 1 && read.methylation_calls[0].candidate_cpg_position == position(18),
          "reference CpG admission drift");
    check(evidence.value->counters.rg_only_duplicate_occurrences == 1 &&
              evidence.value->counters.exact_sidecar_joins == 1 &&
              evidence.value->counters.projected_marker_calls == 2 &&
              evidence.value->counters.admitted_reference_cpg_calls == 1,
          "block conservation counters drift");
    check(evidence.value->logical_retained_bytes() > sizeof(*evidence.value),
          "logical retained-byte accounting is empty");
    auto digest = longlineage::pipeline::block_science_semantic_sha256(*evidence.value);
    check(digest.ok() && digest.value->size() == 64, "block semantic digest was not produced");
    auto replay = longlineage::pipeline::block_science_semantic_sha256(*evidence.value);
    check(replay.ok() && replay.value == digest.value, "block semantic digest is not deterministic");
}

void test_non_cpg_is_rejected_without_reinterpreting_mm() {
    Fixture input = fixture(false, false);
    auto evidence = longlineage::pipeline::build_block_science_evidence(
        input.block, input.markers, input.batch, input.sidecar, input.reference_interval, input.reference);
    check(evidence.ok(), evidence.detail);
    check(evidence.value->reads.front().methylation_calls.empty(), "non-CpG C+m? call was admitted");
    check(evidence.value->counters.rejected_non_cpg_calls == 1, "non-CpG rejection counter drift");
}

void test_iupac_ambiguity_never_fabricates_reference_cpg() {
    static constexpr std::string_view kAmbiguousBases = "RYSWKMBDHVN";
    for (const char ambiguity : kAmbiguousBases) {
        Fixture ambiguous_c = fixture(false, false);
        ambiguous_c.reference[7] = ambiguity;
        ambiguous_c.reference[8] = 'G';
        auto left = longlineage::pipeline::build_block_science_evidence(
            ambiguous_c.block, ambiguous_c.markers, ambiguous_c.batch, ambiguous_c.sidecar,
            ambiguous_c.reference_interval, ambiguous_c.reference);
        check(left.ok(), left.detail);
        check(left.value->reads.front().methylation_calls.empty(),
              "IUPAC possible-C was admitted as an exact reference CpG");
        check(
            left.value->counters.admitted_reference_cpg_calls == 0 && left.value->counters.rejected_non_cpg_calls == 1,
            "IUPAC possible-C rejection counters drift");

        Fixture ambiguous_g = fixture(false, false);
        ambiguous_g.reference[7] = 'C';
        ambiguous_g.reference[8] = ambiguity;
        auto right = longlineage::pipeline::build_block_science_evidence(
            ambiguous_g.block, ambiguous_g.markers, ambiguous_g.batch, ambiguous_g.sidecar,
            ambiguous_g.reference_interval, ambiguous_g.reference);
        check(right.ok(), right.detail);
        check(right.value->reads.front().methylation_calls.empty(),
              "IUPAC possible-G was admitted as an exact reference CpG");
        check(right.value->counters.admitted_reference_cpg_calls == 0 &&
                  right.value->counters.rejected_non_cpg_calls == 1,
              "IUPAC possible-G rejection counters drift");
    }
}

void test_non_equivalent_duplicate_fails_closed() {
    Fixture input = fixture();
    input.batch.reads[1].identity.sam_core_sha256 = std::string(64, 'b');
    auto evidence = longlineage::pipeline::build_block_science_evidence(
        input.block, input.markers, input.batch, input.sidecar, input.reference_interval, input.reference);
    check(!evidence.ok(), "non-equivalent duplicate did not fail closed");
}

void test_sidecar_occurrence_mismatch_fails_closed() {
    Fixture input = fixture(true, true);
    // Retain two raw occurrences but rebuild a one-occurrence sidecar.
    input.sidecar = SidecarLookup{};
    const auto& identity = input.batch.reads.front().identity;
    const SidecarRecord row{identity.projection, longlineage::sidecar_identity_from_alignment(identity),
                            LatestTags{"1-1", 15}};
    check(input.sidecar.add(row).ok(), "cannot add mismatch sidecar row");
    auto evidence = longlineage::pipeline::build_block_science_evidence(
        input.block, input.markers, input.batch, input.sidecar, input.reference_interval, input.reference);
    check(!evidence.ok(), "raw/sidecar occurrence mismatch did not fail closed");

    auto diagnostic = longlineage::pipeline::build_block_science_evidence(
        input.block, input.markers, input.batch, input.sidecar, input.reference_interval, input.reference,
        BlockScienceOptions{20, false});
    check(diagnostic.ok(), "explicit diagnostic mode could not retain the mismatch evidence");
}

void test_site_matrix_preserves_ra_column_universe_and_point_modes() {
    Fixture input = fixture(false, true);
    auto evidence = longlineage::pipeline::build_block_science_evidence(
        input.block, input.markers, input.batch, input.sidecar, input.reference_interval, input.reference);
    check(evidence.ok(), evidence.detail);
    const auto& focal = evidence.value->focal_sites.front();
    auto matrix = longlineage::pipeline::build_site_methylation_matrix(*evidence.value, focal);
    check(matrix.ok(), matrix.detail);
    check(matrix.value->read_indices.size() == 1 && matrix.value->alt_row_indices == std::vector<std::size_t>{0},
          "M1 R/A and ALT row universe drift");
    check(matrix.value->cpg_positions == std::vector<Position1>{position(18)} &&
              matrix.value->called_cpg_counts == std::vector<std::size_t>{1},
          "M1 CpG column/call count drift");
    const double raw =
        longlineage::pipeline::m1_point_from_ml(1, longlineage::pipeline::M1PointMode::kFloat32RawDiv255);
    const double legacy =
        longlineage::pipeline::m1_point_from_ml(1, longlineage::pipeline::M1PointMode::kLegacyCsvRound4);
    check(raw != legacy && legacy == 0.0039, "raw binary32 and legacy four-decimal point modes were conflated");

    // O/X evidence remains available for co-occurrence but cannot expand the
    // historical M1 methylation matrix.
    evidence.value->reads.front().allele_calls.front().call = AlleleCall::kOther;
    auto no_ra = longlineage::pipeline::build_site_methylation_matrix(*evidence.value, focal);
    check(no_ra.ok() && no_ra.empty() && no_ra.value->cpg_positions.empty(),
          "O call entered the M1 R/A matrix universe");
}

void test_site_science_explicit_representation_and_fail_closed_m2() {
    Fixture input = fixture(false, true);
    auto evidence = longlineage::pipeline::build_block_science_evidence(
        input.block, input.markers, input.batch, input.sidecar, input.reference_interval, input.reference);
    check(evidence.ok(), evidence.detail);
    const auto& focal = evidence.value->focal_sites.front();

    longlineage::pipeline::SiteScienceOptions historical;
    historical.m1_representation = longlineage::pipeline::M1Representation::kHistoricalObservedRound6NullRound4;
    auto historical_result = longlineage::pipeline::run_site_science(*evidence.value, focal, historical);
    check(historical_result.ok(), historical_result.detail);
    check(historical_result.value->m1.status == longlineage::m1::M1ReadsetStatus::kInsufficientAltReads &&
              historical_result.value->assignments.empty(),
          "insufficient ALT site did not preserve M1 abstention");
    check(historical_result.value->m2.decision.status == longlineage::m2::M2Status::kNotRun &&
              historical_result.value->m2.decision.reason == longlineage::m2::M2Reason::kM1NotFlagged,
          "M2 ran without a stable M1 partition");
    check(historical_result.value->semantic_sha256.size() == 64, "historical site-science digest is absent");

    longlineage::pipeline::SiteScienceOptions raw;
    raw.m1_representation = longlineage::pipeline::M1Representation::kRawBinary32Point;
    auto raw_result = longlineage::pipeline::run_site_science(*evidence.value, focal, raw);
    check(raw_result.ok(), raw_result.detail);
    check(raw_result.value->semantic_sha256 != historical_result.value->semantic_sha256,
          "raw and historical M1 representations were not identity-separated");

    check(longlineage::pipeline::hp_family_for_token("1-2") == "HP1-side" &&
              longlineage::pipeline::hp_family_for_token("2-1") == "HP2-side" &&
              longlineage::pipeline::hp_family_for_token("3") == "HP3-ambiguous" &&
              longlineage::pipeline::hp_family_for_token("4") == "HP4-both" &&
              longlineage::pipeline::hp_family_for_token(".") == "untagged" &&
              longlineage::pipeline::hp_family_for_token("unexpected") == "untagged",
          "frozen HP-family mapping drift");
}

void test_dataset_plan_span_and_partner_halo() {
    const ContigId chromosome = contig("chrSynthetic");
    VariantSiteSet variants;
    variants.sites = {
        VariantSite{0, "synthetic", 0, 0, chromosome, 1000000, position(100), 'A', 'C'},
        VariantSite{0, "synthetic", 1, 1, chromosome, 1000000, position(140), 'C', 'T'},
        VariantSite{0, "synthetic", 2, 2, chromosome, 1000000, position(190), 'G', 'A'},
        VariantSite{0, "synthetic", 3, 3, chromosome, 1000000, position(260), 'T', 'G'},
    };
    variants.census.selected_scope = variants.sites.size();
    const longlineage::pipeline::DatasetPlanOptions options{2, 250000, 100, 5000, 1};
    auto plan = longlineage::pipeline::plan_dataset_execution(variants, options);
    check(plan.ok(), plan.detail);
    check(plan.value->blocks.size() == 2 && plan.value->focal_site_count == 4,
          "dataset site/span planning split drift");
    check(plan.value->blocks[0].alignment.focal_sites.size() == 2 &&
              plan.value->blocks[1].alignment.focal_sites.size() == 2,
          "dataset block focal cardinality drift");
    check(plan.value->blocks[0].marker_begin == 0 && plan.value->blocks[0].marker_end == 4 &&
              plan.value->blocks[1].marker_begin == 0 && plan.value->blocks[1].marker_end == 4,
          "partner halo did not retain all nearby markers");
    check(plan.value->maximum_observed_focal_span_bp == 70, "maximum focal span receipt drift");
}

void test_dataset_receipt_digest_excludes_runtime_fields() {
    longlineage::pipeline::BlockExtractionReceipt first;
    first.block_sequence = 4;
    first.contig = "chrSynthetic";
    first.focal_sites = 2;
    first.markers = 3;
    first.read_counters.iterator_records = 11;
    first.read_counters.retained_records = 7;
    first.science_counters.unique_read_projections = 6;
    first.science_counters.rg_only_duplicate_occurrences = 1;
    first.science_counters.exact_sidecar_joins = 6;
    first.science_counters.projected_marker_calls = 18;
    first.science_counters.admitted_reference_cpg_calls = 12;
    first.matrix_rows = 8;
    first.alt_rows = 5;
    first.matrix_cells = 48;
    first.block_semantic_sha256 = std::string(64, 'a');
    first.bam_seconds = 1.0;

    auto second = first;
    second.block_sequence = 5;
    second.block_semantic_sha256 = std::string(64, 'b');
    second.bam_seconds = 99.0;
    std::vector<longlineage::pipeline::BlockExtractionReceipt> receipts{first, second};
    auto digest = longlineage::pipeline::dataset_extraction_semantic_sha256(receipts, 4);
    check(digest.ok() && digest.value->size() == 64, "dataset extraction digest was not produced");
    receipts[0].bam_seconds = 1000.0;
    auto timing_replay = longlineage::pipeline::dataset_extraction_semantic_sha256(receipts, 4);
    check(timing_replay.ok() && timing_replay.value == digest.value, "dataset digest depends on runtime timing");
    receipts[1].matrix_cells += 1;
    auto semantic_change = longlineage::pipeline::dataset_extraction_semantic_sha256(receipts, 4);
    check(semantic_change.ok() && semantic_change.value != digest.value,
          "dataset digest ignored a scientific conservation change");
    receipts[1].block_sequence = 7;
    auto gap = longlineage::pipeline::dataset_extraction_semantic_sha256(receipts, 4);
    check(!gap.ok(), "dataset receipt sequence gap did not fail closed");
}

int run_real_census(char** argv, std::size_t max_focal_sites_per_block = 256U) {
    auto reference = longlineage::IndexedReferenceReader::open(argv[3], argv[4]);
    check(reference.ok(), reference.detail);
    auto variants = longlineage::load_variant_sites(argv[1], argv[2], **reference.value, 0, "HCC1395");
    check(variants.ok(), variants.detail);
    const longlineage::pipeline::DatasetPlanOptions options{max_focal_sites_per_block, 250000, 250000, 5000, 1};
    auto plan = longlineage::pipeline::plan_dataset_execution(*variants.value, options);
    check(plan.ok(), plan.detail);
    std::size_t maximum_focal_sites = 0;
    std::size_t maximum_marker_count = 0;
    std::size_t maximum_focal_block = 0;
    for (std::size_t index = 0; index < plan.value->blocks.size(); ++index) {
        const auto& block = plan.value->blocks[index];
        if (block.alignment.focal_sites.size() > maximum_focal_sites) {
            maximum_focal_sites = block.alignment.focal_sites.size();
            maximum_focal_block = index;
        }
        maximum_marker_count = std::max(maximum_marker_count, block.marker_end - block.marker_begin);
    }
    std::cout << "{\"mode\":\"REAL_CENSUS_READ_ONLY\","
              << "\"max_focal_sites_per_block\":" << max_focal_sites_per_block << ','
              << "\"vcf_records\":" << variants.value->census.vcf_records << ','
              << "\"pass_biallelic_ssnv\":" << variants.value->census.pass_biallelic_ssnv << ','
              << "\"autosomal_sites\":" << variants.value->census.selected_scope << ','
              << "\"outside_scope\":" << variants.value->census.excluded_outside_scope << ','
              << "\"blocks\":" << plan.value->blocks.size() << ','
              << "\"marker_occurrences\":" << plan.value->marker_occurrences << ','
              << "\"max_block_focal_sites\":" << maximum_focal_sites << ','
              << "\"max_block_marker_count\":" << maximum_marker_count << ','
              << "\"max_focal_block_index\":" << maximum_focal_block << ','
              << "\"max_focal_span_bp\":" << plan.value->maximum_observed_focal_span_bp << "}\n";
    return 0;
}

double elapsed_seconds(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

int run_real_block(char** argv, std::size_t max_focal_sites_per_block = 256U) {
    const auto total_begin = std::chrono::steady_clock::now();
    const auto input_begin = std::chrono::steady_clock::now();
    auto reference = longlineage::IndexedReferenceReader::open(argv[7], argv[8]);
    check(reference.ok(), reference.detail);
    auto variants = longlineage::load_variant_sites(argv[5], argv[6], **reference.value, 0, "HCC1395");
    check(variants.ok(), variants.detail);
    const longlineage::pipeline::DatasetPlanOptions options{max_focal_sites_per_block, 250000, 250000, 5000, 1};
    auto plan = longlineage::pipeline::plan_dataset_execution(*variants.value, options);
    check(plan.ok(), plan.detail);
    const std::size_t requested = static_cast<std::size_t>(std::stoull(argv[9]));
    check(requested < plan.value->blocks.size(), "requested real block index is outside the plan");
    const double input_seconds = elapsed_seconds(input_begin);

    const auto& planned = plan.value->blocks[requested];
    auto bam = longlineage::IndexedBamBlockReader::open(argv[1], argv[2]);
    check(bam.ok(), bam.detail);
    const auto bam_begin = std::chrono::steady_clock::now();
    auto raw = (*bam.value)->read_block(planned.alignment);
    check(raw.ok(), raw.detail);
    const double bam_seconds = elapsed_seconds(bam_begin);

    auto sidecar = longlineage::IndexedSidecarReader::open(argv[3], argv[4]);
    check(sidecar.ok(), sidecar.detail);
    const auto sidecar_begin = std::chrono::steady_clock::now();
    auto lookup = (*sidecar.value)->fetch(planned.alignment.contig, planned.alignment.fetch_interval);
    check(lookup.ok(), lookup.detail);
    const double sidecar_seconds = elapsed_seconds(sidecar_begin);

    const std::uint64_t reference_end = planned.alignment.fetch_interval.end() < planned.alignment.contig_length
                                            ? planned.alignment.fetch_interval.end() + 1
                                            : planned.alignment.fetch_interval.end();
    const Interval0 reference_context = interval(planned.alignment.fetch_interval.begin(), reference_end);
    const auto reference_begin = std::chrono::steady_clock::now();
    auto sequence = (*reference.value)->fetch(planned.alignment.contig, reference_context);
    check(sequence.ok(), sequence.detail);
    const double reference_seconds = elapsed_seconds(reference_begin);

    std::vector<VariantSite> markers(variants.value->sites.begin() + static_cast<std::ptrdiff_t>(planned.marker_begin),
                                     variants.value->sites.begin() + static_cast<std::ptrdiff_t>(planned.marker_end));
    const auto science_begin = std::chrono::steady_clock::now();
    auto evidence = longlineage::pipeline::build_block_science_evidence(
        planned.alignment, markers, *raw.value, *lookup.value, reference_context, *sequence.value);
    check(evidence.ok(), evidence.detail);
    std::uint64_t matrix_rows = 0;
    std::uint64_t alt_rows = 0;
    std::uint64_t matrix_cells = 0;
    for (const auto& focal : evidence.value->focal_sites) {
        auto matrix = longlineage::pipeline::build_site_methylation_matrix(*evidence.value, focal);
        check(matrix.ok(), matrix.detail);
        matrix_rows += matrix.value->read_indices.size();
        alt_rows += matrix.value->alt_row_indices.size();
        matrix_cells += matrix.value->read_indices.size() * matrix.value->cpg_positions.size();
    }
    const double science_seconds = elapsed_seconds(science_begin);
    std::cout << "{\"mode\":\"REAL_BLOCK_PARTIAL\","
              << "\"production_claim_allowed\":false,"
              << "\"max_focal_sites_per_block\":" << max_focal_sites_per_block << ',' << "\"block_index\":" << requested
              << ',' << "\"contig\":\"" << planned.alignment.contig.value() << "\","
              << "\"focal_sites\":" << planned.alignment.focal_sites.size() << ',' << "\"markers\":" << markers.size()
              << ',' << "\"iterator_records\":" << raw.value->counters.iterator_records << ','
              << "\"retained_records\":" << raw.value->counters.retained_records << ','
              << "\"raw_logical_bytes\":" << raw.value->logical_retained_bytes() << ','
              << "\"unique_reads\":" << evidence.value->reads.size() << ',' << "\"matrix_rows\":" << matrix_rows << ','
              << "\"alt_rows\":" << alt_rows << ',' << "\"matrix_cells\":" << matrix_cells << ','
              << "\"admitted_cpg_calls\":" << evidence.value->counters.admitted_reference_cpg_calls << ','
              << "\"input_plan_seconds\":" << input_seconds << ',' << "\"bam_seconds\":" << bam_seconds << ','
              << "\"sidecar_seconds\":" << sidecar_seconds << ',' << "\"reference_seconds\":" << reference_seconds
              << ',' << "\"science_projection_seconds\":" << science_seconds << ','
              << "\"total_seconds\":" << elapsed_seconds(total_begin) << "}\n";
    return 0;
}

int run_real_producer_probe(char** argv) {
    check(std::string(argv[12]) == "PRODUCE", "real producer probe mode token must be PRODUCE");
    const auto total_begin = std::chrono::steady_clock::now();
    const std::size_t workers = static_cast<std::size_t>(std::stoull(argv[9]));
    const std::size_t first = static_cast<std::size_t>(std::stoull(argv[10]));
    const std::size_t count = static_cast<std::size_t>(std::stoull(argv[11]));
    const std::filesystem::path output_root = argv[13];
    const std::size_t max_focal_sites_per_block = static_cast<std::size_t>(std::stoull(argv[14]));
    check(output_root.is_absolute(), "real producer probe output root must be absolute");

    const auto input_begin = std::chrono::steady_clock::now();
    auto reference = longlineage::IndexedReferenceReader::open(argv[7], argv[8]);
    check(reference.ok(), reference.detail);
    auto variants = longlineage::load_variant_sites(argv[5], argv[6], **reference.value, 0, "HCC1395");
    check(variants.ok(), variants.detail);
    const longlineage::pipeline::DatasetPlanOptions plan_options{max_focal_sites_per_block, 250000, 250000, 5000, 1};
    auto plan = longlineage::pipeline::plan_dataset_execution(*variants.value, plan_options);
    check(plan.ok(), plan.detail);
    const double input_seconds = elapsed_seconds(input_begin);

    const longlineage::pipeline::DatasetReadPaths paths{argv[1], argv[2], argv[3], argv[4], argv[7], argv[8]};
    longlineage::pipeline::DatasetProductionOptions options;
    options.workers = workers;
    options.first_block = first;
    options.block_count = count;
    options.task_queue_capacity_bytes = 16U * 1024U * 1024U;
    options.reorder_capacity_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    options.maximum_block_payload_bytes = 512ULL * 1024ULL * 1024ULL;
    options.bgzf_compression_threads = 4;
    options.task_type = "A";
    options.completeness = "PARTIAL";
    options.site_population = "HCC1395_READINESS_PARTIAL_AUTOSOMES_CHR1_TO_CHR22";
    auto receipt = longlineage::pipeline::produce_dataset_scientific_artifacts(
        paths, *variants.value, *plan.value, output_root, "hcc1395-readiness-probe", options);
    check(receipt.ok(), receipt.detail);

    std::uint64_t logical_rows = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t physical_bytes = 0;
    for (const auto& artifact : receipt.value->artifacts) {
        logical_rows += artifact.logical_rows;
        logical_bytes += artifact.logical_bytes;
        physical_bytes += artifact.physical_bytes;
        physical_bytes += artifact.index.physical_bytes;
    }
    const auto& counters = receipt.value->counters;
    const auto& timing = receipt.value->timing;
    std::cout << "{\"mode\":\"REAL_PRODUCER_READINESS_PARTIAL\","
              << "\"production_claim_allowed\":false,"
              << "\"output_root\":" << longlineage::artifact::canonical_json_quote(output_root.string()) << ','
              << "\"workers\":" << receipt.value->workers << ','
              << "\"max_focal_sites_per_block\":" << max_focal_sites_per_block << ','
              << "\"first_block\":" << receipt.value->first_block << ','
              << "\"executed_blocks\":" << receipt.value->block_count << ','
              << "\"planned_blocks\":" << plan.value->blocks.size() << ',' << "\"site_keys\":" << counters.site_keys
              << ',' << "\"site_read_rows\":" << counters.site_read_rows << ','
              << "\"methyl_call_rows\":" << counters.methyl_call_rows << ','
              << "\"m1_evaluable\":" << counters.m1_evaluable << ','
              << "\"m1_insufficient_alt_reads\":" << counters.m1_insufficient_alt_reads << ','
              << "\"m1_incomplete_distance\":" << counters.m1_incomplete_distance << ','
              << "\"m1_stable_assignments\":" << counters.m1_stable_assignments << ','
              << "\"latest_tag_exact_joins\":" << counters.latest_tag_exact_joins << ','
              << "\"raw_expected\":" << counters.raw_expected << ',' << "\"raw_matched\":" << counters.raw_matched
              << ',' << "\"raw_rg_only_duplicate_occurrences\":" << counters.raw_rg_only_duplicate_occurrences << ','
              << "\"m2_eligible\":" << counters.m2_eligible << ','
              << "\"m2_evaluable_ineligible\":" << counters.m2_evaluable_ineligible << ','
              << "\"m2_axis_indeterminate\":" << counters.m2_axis_indeterminate << ','
              << "\"cooccurrence_pairs\":" << counters.cooccurrence_pairs << ','
              << "\"exact_testable_pairs\":" << counters.exact_testable_pairs << ','
              << "\"global_bh_discoveries\":" << counters.global_bh_discoveries << ','
              << "\"global_by_discoveries\":" << counters.global_by_discoveries << ','
              << "\"topology_regions\":" << counters.topology_regions << ',' << "\"logical_rows\":" << logical_rows
              << ',' << "\"logical_bytes\":" << logical_bytes << ',' << "\"physical_bytes\":" << physical_bytes << ','
              << "\"peak_reorder_bytes\":" << timing.peak_reorder_bytes << ','
              << "\"input_plan_seconds\":" << input_seconds << ','
              << "\"handle_open_seconds\":" << timing.handle_open_seconds << ','
              << "\"block_execution_seconds\":" << timing.block_execution_seconds << ','
              << "\"artifact_stream_seconds\":" << timing.artifact_stream_seconds << ','
              << "\"global_pair_seconds\":" << timing.global_pair_seconds << ','
              << "\"topology_second_pass_seconds\":" << timing.topology_second_pass_seconds << ','
              << "\"finalization_seconds\":" << timing.finalization_seconds << ','
              << "\"producer_total_seconds\":" << timing.total_seconds << ','
              << "\"semantic_sha256\":" << longlineage::artifact::canonical_json_quote(receipt.value->semantic_sha256)
              << ',' << "\"total_seconds\":" << elapsed_seconds(total_begin) << "}\n";
    return 0;
}

int run_real_dataset(char** argv) {
    const auto total_begin = std::chrono::steady_clock::now();
    const auto input_begin = std::chrono::steady_clock::now();
    auto reference = longlineage::IndexedReferenceReader::open(argv[7], argv[8]);
    check(reference.ok(), reference.detail);
    auto variants = longlineage::load_variant_sites(argv[5], argv[6], **reference.value, 0, "HCC1395");
    check(variants.ok(), variants.detail);
    const longlineage::pipeline::DatasetPlanOptions plan_options{256, 250000, 250000, 5000, 1};
    auto plan = longlineage::pipeline::plan_dataset_execution(*variants.value, plan_options);
    check(plan.ok(), plan.detail);
    const double input_seconds = elapsed_seconds(input_begin);

    const std::size_t workers = static_cast<std::size_t>(std::stoull(argv[9]));
    const std::size_t first = static_cast<std::size_t>(std::stoull(argv[10]));
    const std::size_t count = static_cast<std::size_t>(std::stoull(argv[11]));
    const longlineage::pipeline::DatasetReadPaths paths{argv[1], argv[2], argv[3], argv[4], argv[7], argv[8]};
    const longlineage::pipeline::DatasetExtractionOptions run_options{workers, first, count, 16U * 1024U * 1024U};
    auto receipt = longlineage::pipeline::run_dataset_extraction(paths, *variants.value, *plan.value, run_options);
    check(receipt.ok(), receipt.detail);
    double bam_worker_seconds = 0.0;
    double sidecar_worker_seconds = 0.0;
    double reference_worker_seconds = 0.0;
    double projection_worker_seconds = 0.0;
    double matrix_worker_seconds = 0.0;
    for (const auto& block : receipt.value->blocks) {
        bam_worker_seconds += block.bam_seconds;
        sidecar_worker_seconds += block.sidecar_seconds;
        reference_worker_seconds += block.reference_seconds;
        projection_worker_seconds += block.projection_seconds;
        matrix_worker_seconds += block.matrix_seconds;
    }
    const bool complete = first == 0 && receipt.value->block_count == plan.value->blocks.size();
    std::cout << "{\"mode\":\"REAL_DATASET_EXTRACTION_" << (complete ? "COMPLETE" : "PARTIAL") << "\","
              << "\"production_claim_allowed\":false,"
              << "\"workers\":" << receipt.value->workers << ',' << "\"first_block\":" << receipt.value->first_block
              << ',' << "\"executed_blocks\":" << receipt.value->block_count << ','
              << "\"planned_blocks\":" << plan.value->blocks.size() << ','
              << "\"focal_sites\":" << receipt.value->focal_sites << ','
              << "\"marker_occurrences\":" << receipt.value->markers << ','
              << "\"iterator_records\":" << receipt.value->read_counters.iterator_records << ','
              << "\"retained_records\":" << receipt.value->read_counters.retained_records << ','
              << "\"unique_read_projections\":" << receipt.value->science_counters.unique_read_projections << ','
              << "\"rg_only_duplicate_occurrences\":" << receipt.value->science_counters.rg_only_duplicate_occurrences
              << ',' << "\"exact_sidecar_joins\":" << receipt.value->science_counters.exact_sidecar_joins << ','
              << "\"projected_marker_calls\":" << receipt.value->science_counters.projected_marker_calls << ','
              << "\"admitted_reference_cpg_calls\":" << receipt.value->science_counters.admitted_reference_cpg_calls
              << ',' << "\"matrix_rows\":" << receipt.value->matrix_rows << ','
              << "\"alt_rows\":" << receipt.value->alt_rows << ',' << "\"matrix_cells\":" << receipt.value->matrix_cells
              << ',' << "\"maximum_raw_logical_bytes\":" << receipt.value->maximum_raw_logical_bytes << ','
              << "\"maximum_evidence_logical_bytes\":" << receipt.value->maximum_evidence_logical_bytes << ','
              << "\"peak_task_queue_bytes\":" << receipt.value->peak_task_queue_bytes << ',' << "\"semantic_sha256\":\""
              << receipt.value->semantic_sha256 << "\","
              << "\"input_plan_seconds\":" << input_seconds << ','
              << "\"handle_open_seconds\":" << receipt.value->handle_open_seconds << ','
              << "\"execution_seconds\":" << receipt.value->execution_seconds << ','
              << "\"bam_worker_seconds\":" << bam_worker_seconds << ','
              << "\"sidecar_worker_seconds\":" << sidecar_worker_seconds << ','
              << "\"reference_worker_seconds\":" << reference_worker_seconds << ','
              << "\"projection_worker_seconds\":" << projection_worker_seconds << ','
              << "\"matrix_worker_seconds\":" << matrix_worker_seconds << ','
              << "\"total_seconds\":" << elapsed_seconds(total_begin) << "}\n";
    return 0;
}

int run_real_science_census(char** argv) {
    check(std::string(argv[12]) == "SCIENCE", "real science census mode token must be SCIENCE");
    const auto total_begin = std::chrono::steady_clock::now();
    const auto input_begin = std::chrono::steady_clock::now();
    auto reference = longlineage::IndexedReferenceReader::open(argv[7], argv[8]);
    check(reference.ok(), reference.detail);
    auto variants = longlineage::load_variant_sites(argv[5], argv[6], **reference.value, 0, "HCC1395");
    check(variants.ok(), variants.detail);
    const longlineage::pipeline::DatasetPlanOptions plan_options{256, 250000, 250000, 5000, 1};
    auto plan = longlineage::pipeline::plan_dataset_execution(*variants.value, plan_options);
    check(plan.ok(), plan.detail);
    const double input_seconds = elapsed_seconds(input_begin);

    const std::size_t workers = static_cast<std::size_t>(std::stoull(argv[9]));
    const std::size_t first = static_cast<std::size_t>(std::stoull(argv[10]));
    const std::size_t count = static_cast<std::size_t>(std::stoull(argv[11]));
    const longlineage::pipeline::DatasetReadPaths paths{argv[1], argv[2], argv[3], argv[4], argv[7], argv[8]};
    longlineage::pipeline::DatasetScienceCensusOptions run_options;
    run_options.workers = workers;
    run_options.first_block = first;
    run_options.block_count = count;
    run_options.representation = longlineage::pipeline::M1Representation::kHistoricalObservedRound6NullRound4;
    auto receipt = longlineage::pipeline::run_dataset_science_census(paths, *variants.value, *plan.value, run_options);
    check(receipt.ok(), receipt.detail);
    double worker_input_seconds = 0.0;
    double m1_m2_worker_seconds = 0.0;
    double cooccurrence_worker_seconds = 0.0;
    for (const auto& block : receipt.value->blocks) {
        worker_input_seconds += block.input_seconds;
        m1_m2_worker_seconds += block.m1_m2_seconds;
        cooccurrence_worker_seconds += block.cooccurrence_seconds;
    }
    const bool complete = first == 0 && receipt.value->block_count == plan.value->blocks.size();
    std::cout << "{\"mode\":\"REAL_DATASET_SCIENCE_CENSUS_" << (complete ? "COMPLETE" : "PARTIAL") << "\","
              << "\"publication_claim_allowed\":false,"
              << "\"representation\":\"HISTORICAL_OBSERVED_ROUND6_NULL_ROUND4\","
              << "\"workers\":" << receipt.value->workers << ',' << "\"first_block\":" << receipt.value->first_block
              << ',' << "\"executed_blocks\":" << receipt.value->block_count << ','
              << "\"planned_blocks\":" << plan.value->blocks.size() << ','
              << "\"focal_sites\":" << receipt.value->focal_sites << ','
              << "\"m1_insufficient_alt_reads\":" << receipt.value->m1_insufficient_alt_reads << ','
              << "\"m1_incomplete_distance\":" << receipt.value->m1_incomplete_distance << ','
              << "\"m1_evaluable\":" << receipt.value->m1_evaluable << ','
              << "\"m1_stable\":" << receipt.value->m1_stable << ',' << "\"m2_eligible\":" << receipt.value->m2_eligible
              << ',' << "\"m2_evaluable_ineligible\":" << receipt.value->m2_evaluable_ineligible << ','
              << "\"m2_axis_indeterminate\":" << receipt.value->m2_axis_indeterminate << ','
              << "\"m2_group_count_gt10\":" << receipt.value->m2_group_count_gt10 << ','
              << "\"partner_pairs\":" << receipt.value->partner_pairs << ','
              << "\"endpoint_a_testable_pairs\":" << receipt.value->endpoint_a_testable_pairs << ','
              << "\"exact_identifiable_pairs\":" << receipt.value->exact_identifiable_pairs << ','
              << "\"exact_family_pairs\":" << receipt.value->exact_family_pairs << ',' << "\"semantic_sha256\":\""
              << receipt.value->semantic_sha256 << "\","
              << "\"input_plan_seconds\":" << input_seconds << ','
              << "\"handle_open_seconds\":" << receipt.value->handle_open_seconds << ','
              << "\"execution_seconds\":" << receipt.value->execution_seconds << ','
              << "\"worker_input_seconds\":" << worker_input_seconds << ','
              << "\"m1_m2_worker_seconds\":" << m1_m2_worker_seconds << ','
              << "\"cooccurrence_worker_seconds\":" << cooccurrence_worker_seconds << ','
              << "\"total_seconds\":" << elapsed_seconds(total_begin) << "}\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 5) {
            return run_real_census(argv);
        }
        if (argc == 6) {
            return run_real_census(argv, static_cast<std::size_t>(std::stoull(argv[5])));
        }
        if (argc == 10) {
            return run_real_block(argv);
        }
        if (argc == 11) {
            return run_real_block(argv, static_cast<std::size_t>(std::stoull(argv[10])));
        }
        if (argc == 12) {
            return run_real_dataset(argv);
        }
        if (argc == 13) {
            return run_real_science_census(argv);
        }
        if (argc == 15) {
            return run_real_producer_probe(argv);
        }
        check(argc == 1,
              "usage: test_dataset_engine [VCF VCF_INDEX FASTA FAI] "
              "optionally followed by [MAX_FOCAL_SITES_PER_BLOCK], or "
              "[BAM BAI SIDECAR TBI VCF CSI FASTA FAI BLOCK_INDEX] or "
              "the same followed by [MAX_FOCAL_SITES_PER_BLOCK] or "
              "[BAM BAI SIDECAR TBI VCF CSI FASTA FAI WORKERS FIRST_BLOCK "
              "BLOCK_COUNT] or the same followed by [SCIENCE] or by "
              "[PRODUCE OUTPUT_ROOT MAX_FOCAL_SITES_PER_BLOCK]");
        test_positive_one_pass_and_duplicate_conservation();
        test_non_cpg_is_rejected_without_reinterpreting_mm();
        test_iupac_ambiguity_never_fabricates_reference_cpg();
        test_non_equivalent_duplicate_fails_closed();
        test_sidecar_occurrence_mismatch_fails_closed();
        test_site_matrix_preserves_ra_column_universe_and_point_modes();
        test_site_science_explicit_representation_and_fail_closed_m2();
        test_dataset_plan_span_and_partner_halo();
        test_dataset_receipt_digest_excludes_runtime_fields();
        test_ml_interval_lookup_matches_frozen_sci17();
        std::cout << "PASS test_dataset_engine: one-pass projection, exact join, "
                     "duplicate/CpG fail-closed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL test_dataset_engine: " << error.what() << '\n';
        return 1;
    }
}
