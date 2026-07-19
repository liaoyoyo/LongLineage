// SPDX-License-Identifier: GPL-3.0-only
#include <htslib/hts_endian.h>
#include <htslib/sam.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/common/digest.hpp"
#include "longlineage/common/types.hpp"
#include "longlineage/io/alignment.hpp"
#include "longlineage/io/hts_preflight.hpp"
#include "longlineage/io/mm_ml.hpp"
#include "longlineage/io/sidecar.hpp"
#include "longlineage/manifest/production_manifest.hpp"

namespace {

#define CHECK(condition)                                           \
    do {                                                           \
        if (!static_cast<bool>(condition)) {                       \
            throw std::runtime_error("CHECK failed: " #condition); \
        }                                                          \
    } while (false)

using BamPointer = std::unique_ptr<bam1_t, decltype(&bam_destroy1)>;
using HeaderPointer = std::unique_ptr<sam_hdr_t, decltype(&sam_hdr_destroy)>;

[[nodiscard]] BamPointer build_synthetic_alignment(char integer_type = 'c', bool include_cigar = true,
                                                   std::uint16_t flag = 0,
                                                   std::string_view stored_sequence = "ACCCGTCC") {
    BamPointer alignment(bam_init1(), &bam_destroy1);
    CHECK(alignment);
    const std::string sequence(stored_sequence);
    const std::uint32_t cigar = bam_cigar_gen(static_cast<std::uint32_t>(sequence.size()), BAM_CMATCH);
    const int result = bam_set1(alignment.get(), 5, "readS", flag, 0, 100, 60, 1, &cigar, -1, -1, 0, sequence.size(),
                                sequence.c_str(), nullptr, 128);
    CHECK(result >= 0);
    if (!include_cigar) {
        alignment->core.n_cigar = 0;
    }

    const std::string mm = "C+m?,0,1,0;";
    CHECK(bam_aux_append(alignment.get(), "MM", 'Z', static_cast<int>(mm.size() + 1),
                         reinterpret_cast<const std::uint8_t*>(mm.c_str())) == 0);

    const std::vector<std::uint8_t> ml_values = {0, 128, 255};
    std::vector<std::uint8_t> ml(5 + ml_values.size());
    ml[0] = 'C';
    u32_to_le(static_cast<std::uint32_t>(ml_values.size()), ml.data() + 1);
    std::copy(ml_values.begin(), ml_values.end(), ml.begin() + 5);
    CHECK(bam_aux_append(alignment.get(), "ML", 'B', static_cast<int>(ml.size()), ml.data()) == 0);

    std::uint8_t mn[4]{};
    u32_to_le(static_cast<std::uint32_t>(sequence.size()), mn);
    CHECK(bam_aux_append(alignment.get(), "MN", 'i', 4, mn) == 0);

    const std::uint8_t integer_value = 7;
    CHECK(bam_aux_append(alignment.get(), "ZZ", integer_type, 1, &integer_value) == 0);
    const char rg[] = "synthetic";
    CHECK(bam_aux_append(alignment.get(), "RG", 'Z', sizeof(rg), reinterpret_cast<const std::uint8_t*>(rg)) == 0);
    return alignment;
}

[[nodiscard]] std::string valid_manifest_json() {
    static constexpr const char* kRoles[] = {
        "raw_bam",
        "raw_bam_index",
        "pass_biallelic_ssnv_vcf",
        "pass_biallelic_ssnv_vcf_index",
        "latest_hp_ps_sidecar",
        "latest_hp_ps_sidecar_index",
        "reference_fasta",
        "reference_fai",
    };
    std::ostringstream json;
    json << R"({"schema_name":"longlineage.production_manifest","schema_version":"1.0.0",)"
         << R"("run_id":"synthetic-run","authority_profile":"SYNTHETIC",)"
         << R"("output_root":"/synthetic/.staging/synthetic-run","datasets":[)"
         << R"({"dataset_id":"synthetic","dataset_order":0,"files":[)";
    for (std::size_t index = 0; index < 8; ++index) {
        if (index != 0) {
            json << ',';
        }
        json << R"({"role":")" << kRoles[index] << R"(","path":"/synthetic/)" << index
             << R"(","size_bytes":1,"sha256":")" << std::string(64, '0') << R"("})";
    }
    json << R"(]}],"runtime":{"compute_workers":40,"writer_threads":4,"coordinator_slots":2,)"
         << R"("buffer_bytes":8589934592,"max_focal_sites_per_block":4096,)"
         << R"("max_estimated_alignments_per_block":250000,"halo_bp":5000},)"
         << R"("contract_bindings":{"science_parameters_sha256":")" << std::string(64, '1')
         << R"(","schema_catalog_sha256":")" << std::string(64, '2') << R"(","status_reason_registry_sha256":")"
         << std::string(64, '3') << R"(","type_registry_sha256":")" << std::string(64, '4')
         << R"(","transform_registry_sha256":")" << std::string(64, '5') << R"(","authority_manifest_sha256":")"
         << std::string(64, '6') << R"(","source_to_target_manifest_sha256":")" << std::string(64, '7')
         << R"(","production_input_authority_sha256":")" << std::string(64, '8') << R"(","schema_id_registry_sha256":")"
         << std::string(64, '9') << R"(","release_attestation_sha256":")" << std::string(64, 'a') << R"("}})";

    return json.str();
}

void test_types_and_digests() {
    const auto invalid_position = longlineage::Position1::from_value(0);
    CHECK(!invalid_position.ok());
    const auto position = longlineage::Position1::from_value(101);
    CHECK(position.ok() && position.value->zero_based() == 100);
    const auto interval = longlineage::Interval0::from_bounds(100, 101);
    CHECK(interval.ok() && interval.value->contains(*position.value));
    CHECK(!longlineage::Interval0::from_bounds(100, 100).ok());
    CHECK(longlineage::classify_allele('G', 'A', 'C', true) == longlineage::AlleleCall::kOther);
    CHECK(longlineage::classify_allele('A', 'A', 'C', false) == longlineage::AlleleCall::kUnobservable);

    const auto sha = longlineage::sha256_hex("abc");
    CHECK(sha.ok());
    CHECK(*sha.value == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(longlineage::blake2b_64_hex("abc") == "d8bb14d833d59559");
    CHECK(longlineage::blake2b_64_hex("") == "e4a6a0577479b2b4");
    CHECK(longlineage::blake2b_64_hex("10M") == "edd6621a6ba0c69d");
    CHECK(longlineage::blake2b_64_hex("8M") == "2f5216f5f049f7c7");
}

void test_manifest() {
    const auto manifest = longlineage::parse_production_manifest_json(valid_manifest_json());
    CHECK(manifest.ok());
    CHECK(manifest.value->datasets.size() == 1);
    CHECK(manifest.value->datasets[0].files.size() == 8);
    CHECK(manifest.value->authority_profile == longlineage::AuthorityProfile::kSynthetic);
    CHECK(manifest.value->contract_bindings.schema_catalog_sha256 == std::string(64, '2'));

    const auto duplicate = longlineage::parse_production_manifest_json(R"({"schema_name":"a","schema_name":"b"})");
    CHECK(!duplicate.ok());
    CHECK(duplicate.reason == longlineage::ParseReason::kMalformedValue);

    const auto nested_leak =
        longlineage::parse_production_manifest_json(R"({"metadata":{"nested":{"truth_vcf":"/x"}}})");
    CHECK(!nested_leak.ok());
    CHECK(nested_leak.reason == longlineage::ParseReason::kUnsupportedValue);

    std::string bad_binding = valid_manifest_json();
    const std::string valid_binding = std::string(64, '7');
    bad_binding.replace(bad_binding.find(valid_binding), valid_binding.size(), "not-a-sha");
    const auto malformed_binding = longlineage::parse_production_manifest_json(bad_binding);
    CHECK(!malformed_binding.ok());
    CHECK(malformed_binding.reason == longlineage::ParseReason::kMalformedValue);
    std::string missing_schema_registry = valid_manifest_json();
    missing_schema_registry.replace(missing_schema_registry.find("schema_id_registry_sha256"),
                                    std::string("schema_id_registry_sha256").size(), "schema_id_registry_sha25X");
    CHECK(!longlineage::parse_production_manifest_json(missing_schema_registry).ok());
    std::string missing_release_attestation = valid_manifest_json();
    missing_release_attestation.replace(missing_release_attestation.find("release_attestation_sha256"),
                                        std::string("release_attestation_sha256").size(), "release_attestation_sha25X");
    CHECK(!longlineage::parse_production_manifest_json(missing_release_attestation).ok());
    std::string mismatched_root = valid_manifest_json();
    mismatched_root.replace(mismatched_root.find("/synthetic/.staging/synthetic-run"),
                            std::string("/synthetic/.staging/synthetic-run").size(), "/synthetic/.staging/other-run");
    CHECK(!longlineage::parse_production_manifest_json(mismatched_root).ok());
    std::string relative_root = valid_manifest_json();
    relative_root.replace(relative_root.find("/synthetic/.staging/synthetic-run"),
                          std::string("/synthetic/.staging/synthetic-run").size(), ".staging/synthetic-run");
    CHECK(!longlineage::parse_production_manifest_json(relative_root).ok());
    std::string traversal_root = valid_manifest_json();
    traversal_root.replace(traversal_root.find("/synthetic/.staging/synthetic-run"),
                           std::string("/synthetic/.staging/synthetic-run").size(),
                           "/synthetic/../synthetic/.staging/synthetic-run");
    CHECK(!longlineage::parse_production_manifest_json(traversal_root).ok());
    std::string relative_input = valid_manifest_json();
    relative_input.replace(relative_input.find("/synthetic/0"), std::string("/synthetic/0").size(), "synthetic/0");
    CHECK(!longlineage::parse_production_manifest_json(relative_input).ok());
    std::string invalid_profile = valid_manifest_json();
    invalid_profile.replace(invalid_profile.find("\"SYNTHETIC\""), std::string("\"SYNTHETIC\"").size(), "\"UNKNOWN\"");
    CHECK(!longlineage::parse_production_manifest_json(invalid_profile).ok());
}

void test_mm_ml_and_typed_aux() {
    auto alignment = build_synthetic_alignment();
    const auto tags = longlineage::parse_mm_ml_mn(*alignment);
    CHECK(tags.ok());
    CHECK(tags.value->calls.size() == 3);
    CHECK(tags.value->calls[0].query_pos0 == 1);
    CHECK(tags.value->calls[1].query_pos0 == 3);
    CHECK(tags.value->calls[2].query_pos0 == 6);
    CHECK(tags.value->calls[1].mm_group_index == 0);
    CHECK(tags.value->calls[1].ml_index == 1);
    CHECK(tags.value->calls[1].ml_raw == 128);
    CHECK(std::abs(tags.value->calls[1].probability_lower - 0.5) < 1e-15);
    CHECK(std::abs(tags.value->calls[1].probability_upper - (129.0 / 256.0)) < 1e-15);

    const auto empty = longlineage::parse_frozen_cm_unknown("AC", "C+m?;", {}, 2);
    CHECK(empty.ok() && empty.empty() && empty.value->calls.empty());
    const auto mismatch = longlineage::parse_frozen_cm_unknown("AC", "C+m?,0;", {}, 2);
    CHECK(!mismatch.ok());
    const auto multi_group =
        longlineage::parse_frozen_cm_unknown("ACCCGTCC", "C+h?,0;C+m?,0,1,0;", {17, 0, 128, 255}, 8);
    CHECK(multi_group.ok());
    CHECK(multi_group.value->calls.size() == 3);
    CHECK(multi_group.value->calls[0].mm_group_index == 1);
    CHECK(multi_group.value->calls[0].ml_index == 1);
    CHECK(multi_group.value->calls[0].ml_raw == 0);
    CHECK(multi_group.value->calls[2].ml_index == 3);
    CHECK(!longlineage::parse_frozen_cm_unknown("ACCCGTCC", "C+a?,0;C+m?,0,1,0;", {17, 0, 128, 255}, 8).ok());

    auto reverse_alignment = build_synthetic_alignment('c', true, BAM_FREVERSE, "GGACGGGT");
    const auto reverse_tags = longlineage::parse_mm_ml_mn(*reverse_alignment);
    CHECK(reverse_tags.ok());
    CHECK(reverse_tags.value->calls.size() == 3);
    CHECK(reverse_tags.value->calls[0].query_pos0 == 1);
    CHECK(reverse_tags.value->calls[1].query_pos0 == 3);
    CHECK(reverse_tags.value->calls[2].query_pos0 == 6);

    const auto canonical_c = longlineage::canonicalize_typed_aux(*alignment);
    CHECK(canonical_c.ok());
    CHECK(canonical_c.value->find("RG") == std::string::npos);
    auto unsigned_alignment = build_synthetic_alignment('C');
    const auto canonical_upper = longlineage::canonicalize_typed_aux(*unsigned_alignment);
    CHECK(canonical_upper.ok());
    CHECK(*canonical_c.value != *canonical_upper.value);

    auto signed_array_alignment = build_synthetic_alignment();
    std::vector<std::uint8_t> signed_array(6);
    signed_array[0] = 'c';
    u32_to_le(1, signed_array.data() + 1);
    signed_array[5] = 7;
    CHECK(bam_aux_append(signed_array_alignment.get(), "BA", 'B', static_cast<int>(signed_array.size()),
                         signed_array.data()) == 0);
    auto unsigned_array_alignment = build_synthetic_alignment();
    auto unsigned_array = signed_array;
    unsigned_array[0] = 'C';
    CHECK(bam_aux_append(unsigned_array_alignment.get(), "BA", 'B', static_cast<int>(unsigned_array.size()),
                         unsigned_array.data()) == 0);
    const auto canonical_signed_array = longlineage::canonicalize_typed_aux(*signed_array_alignment);
    const auto canonical_unsigned_array = longlineage::canonicalize_typed_aux(*unsigned_array_alignment);
    CHECK(canonical_signed_array.ok() && canonical_unsigned_array.ok());
    CHECK(*canonical_signed_array.value != *canonical_unsigned_array.value);

    const std::uint8_t duplicate_value = 9;
    CHECK(bam_aux_append(alignment.get(), "ZZ", 'c', 1, &duplicate_value) == 0);
    const auto duplicate_aux = longlineage::canonicalize_typed_aux(*alignment);
    CHECK(duplicate_aux.ok());
    const auto first_occurrence = duplicate_aux.value->find("ZZ:");
    CHECK(first_occurrence != std::string::npos);
    CHECK(duplicate_aux.value->find("ZZ:", first_occurrence + 1) != std::string::npos);
}

void test_sidecar_exact_join() {
    const std::string line = "synchr1\t100\t110\treadS\t0\t60\tedd6621a6ba0c69d\t1\t42";
    const auto row = longlineage::parse_sidecar_row(line);
    CHECK(row.ok());
    CHECK(row.value->projection.reference_interval.begin() == 100);
    CHECK(row.value->latest_tags.hp == "1");
    CHECK(row.value->latest_tags.ps == 42);

    static constexpr const char* kHpVocabulary[] = {
        ".", "1", "2", "3", "4", "1-1", "2-1", "1-2", "2-2",
    };
    for (const char* hp : kHpVocabulary) {
        const std::string vocabulary_row =
            std::string("synchr1\t100\t110\treadS\t0\t60\tedd6621a6ba0c69d\t") + hp + "\t.";
        const auto parsed = longlineage::parse_sidecar_row(vocabulary_row);
        CHECK(parsed.ok());
        CHECK(parsed.value->latest_tags.hp == (std::string(hp) == "." ? "0" : hp));
    }
    CHECK(!longlineage::parse_sidecar_row("synchr1\t100\t110\treadS\t0\t60\tedd6621a6ba0c69d\t\t.").ok());
    CHECK(!longlineage::parse_sidecar_row("synchr1\t100\t110\treadS\t0\t60\tedd6621a6ba0c69d\t5\t.").ok());
    CHECK(!longlineage::parse_sidecar_row("synchr1\t100\t110\treadS\t0\t60\tedd6621a6ba0c69d\t0\t.").ok());
    CHECK(!longlineage::parse_sidecar_row("synchr1\t100\t100\treadS\t0\t60\tedd6621a6ba0c69d\t1\t.").ok());
    CHECK(!longlineage::parse_sidecar_row("synchr1\t100\t110\treadS\t0\t60\tedd6621a6ba0c69d\t1\t-1").ok());
    const auto maximum_ps = longlineage::parse_sidecar_row(
        "synchr1\t100\t110\treadS\t0\t60\tedd6621a6ba0c69d\t1\t"
        "18446744073709551615");
    CHECK(maximum_ps.ok());
    CHECK(maximum_ps.value->latest_tags.ps == std::numeric_limits<std::uint64_t>::max());
    CHECK(!longlineage::parse_sidecar_row("synchr1\t100\t110\treadS\t0\t60\tedd6621a6ba0c69d\t1\t"
                                          "18446744073709551616")
               .ok());

    longlineage::SidecarLookup lookup;
    CHECK(lookup.add(*row.value).ok());
    CHECK(lookup.add(*row.value).ok());
    const auto joined = lookup.join(row.value->projection, row.value->full_identity);
    CHECK(joined.status == longlineage::JoinStatus::kExactMatch);
    CHECK(joined.exact_identity_occurrences == 2);
    CHECK(joined.tags->hp == "1");

    auto second_identity = *row.value;
    second_identity.full_identity.flag = BAM_FPROPER_PAIR;
    const auto identity_mismatch = lookup.join(row.value->projection, second_identity.full_identity);
    CHECK(identity_mismatch.status == longlineage::JoinStatus::kError);
    CHECK(identity_mismatch.reason == longlineage::JoinReason::kFullIdentityMismatch);
    CHECK(lookup.add(second_identity).ok());
    const auto ambiguous = lookup.join(row.value->projection, row.value->full_identity);
    CHECK(ambiguous.status == longlineage::JoinStatus::kError);
    CHECK(ambiguous.reason == longlineage::JoinReason::kMultipleFullIdentities);

    auto conflict = *row.value;
    conflict.latest_tags.hp = "2";
    const auto conflict_result = lookup.add(conflict);
    CHECK(!conflict_result.ok());
}

void test_alignment_identity_and_version() {
    auto alignment = build_synthetic_alignment();
    const std::string header_text = "@HD\tVN:1.6\tSO:coordinate\n@SQ\tSN:synchr1\tLN:1000\n";
    HeaderPointer header(sam_hdr_parse(header_text.size(), header_text.c_str()), &sam_hdr_destroy);
    CHECK(header);
    const auto identity = longlineage::build_full_alignment_identity(*alignment, *header);
    CHECK(identity.ok());
    CHECK(identity.value->projection.contig.value() == "synchr1");
    CHECK(identity.value->projection.reference_interval == *longlineage::Interval0::from_bounds(100, 108).value);
    CHECK(identity.value->cigar == "8M");
    const auto sidecar_identity = longlineage::sidecar_identity_from_alignment(*identity.value);
    CHECK(sidecar_identity.cigar_blake2b64 == longlineage::blake2b_64_hex("8M"));

    auto no_cigar = build_synthetic_alignment('c', false);
    CHECK(!longlineage::build_full_alignment_identity(*no_cigar, *header).ok());

    const auto htslib = longlineage::require_htslib_version("1.18");
    CHECK(htslib.ok());
    const auto missing_reference_index = longlineage::preflight_reference_fasta(
        "/tmp/longlineage-never-created-synchr1.fa", "/tmp/longlineage-never-created-synchr1.fa.fai");
    CHECK(!missing_reference_index.ok());
    CHECK(missing_reference_index.reason == longlineage::ParseReason::kIndexError);
}

}  // namespace

int main() {
    test_types_and_digests();
    test_manifest();
    test_mm_ml_and_typed_aux();
    test_sidecar_exact_join();
    test_alignment_identity_and_version();
    std::cout << "typed_io: PASS\n";
    return 0;
}
