// SPDX-License-Identifier: GPL-3.0-only

#include <htslib/bgzf.h>
#include <htslib/faidx.h>
#include <htslib/hts_endian.h>
#include <htslib/sam.h>
#include <htslib/tbx.h>
#include <htslib/vcf.h>
#include <jansson.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/artifact/dataset_closeout.hpp"
#include "longlineage/common/digest.hpp"
#include "longlineage/io/reference_reader.hpp"
#include "longlineage/io/variant_sites.hpp"
#include "longlineage/pipeline/dataset_plan.hpp"
#include "longlineage/pipeline/dataset_producer.hpp"

namespace {

using JsonPtr = std::unique_ptr<json_t, decltype(&json_decref)>;

constexpr std::uint64_t kContigLength = 3000U;
constexpr std::uint64_t kReadStart0 = 500U;
constexpr std::uint32_t kReadLength = 1000U;
constexpr std::uint64_t kFirstSitePosition1 = 1001U;
constexpr std::uint64_t kSecondSitePosition1 = 1401U;
constexpr std::string_view kRunId = "synthetic-producer-e2e";
constexpr std::string_view kDatasetId = "SYNTHETIC_E2E";

const std::array<std::string, 8> kInputRoles{
    "raw_bam",
    "raw_bam_index",
    "pass_biallelic_ssnv_vcf",
    "pass_biallelic_ssnv_vcf_index",
    "latest_hp_ps_sidecar",
    "latest_hp_ps_sidecar_index",
    "reference_fasta",
    "reference_fai",
};

const std::set<std::string> kScientificArtifacts{
    "site_reads",         "methyl_calls",       "bernoulli_upper", "m1_sites", "m1_assignments",
    "cooccurrence_pairs", "cooccurrence_sites", "topology_units",  "summary",
};

void check(bool condition, const std::string& detail) {
    if (!condition) {
        throw std::runtime_error(detail);
    }
}

std::string digest_file(const std::filesystem::path& path) {
    auto digest = longlineage::sha256_file(path);
    check(digest.ok() && digest.value.has_value(), "cannot SHA-256 " + path.string() + ": " + digest.detail);
    return std::move(*digest.value);
}

std::string digest_bytes(std::string_view bytes) {
    auto digest = longlineage::sha256_hex(bytes);
    check(digest.ok() && digest.value.has_value(), "cannot SHA-256 synthetic bytes: " + digest.detail);
    return std::move(*digest.value);
}

void write_binary(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "cannot open synthetic file " + path.string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(static_cast<bool>(output), "cannot close synthetic file " + path.string());
}

void write_bgzf(const std::filesystem::path& path, std::string_view bytes) {
    BGZF* output = bgzf_open(path.c_str(), "w");
    check(output != nullptr, "cannot open synthetic BGZF " + path.string());
    const ssize_t written = bgzf_write(output, bytes.data(), bytes.size());
    const int close_status = bgzf_close(output);
    check(written == static_cast<ssize_t>(bytes.size()) && close_status == 0,
          "cannot finish synthetic BGZF " + path.string());
}

struct SyntheticInputs {
    std::filesystem::path root;
    std::filesystem::path fasta;
    std::filesystem::path fai;
    std::filesystem::path vcf;
    std::filesystem::path vcf_index;
    std::filesystem::path bam;
    std::filesystem::path bam_index;
    std::filesystem::path sidecar;
    std::filesystem::path sidecar_index;

    std::map<std::string, std::filesystem::path> role_paths() const {
        return {
            {"raw_bam", bam},
            {"raw_bam_index", bam_index},
            {"pass_biallelic_ssnv_vcf", vcf},
            {"pass_biallelic_ssnv_vcf_index", vcf_index},
            {"latest_hp_ps_sidecar", sidecar},
            {"latest_hp_ps_sidecar_index", sidecar_index},
            {"reference_fasta", fasta},
            {"reference_fai", fai},
        };
    }
};

void append_mm_ml_mn(bam1_t& record, std::uint8_t ml) {
    const std::string mm = "C+m?,0;";
    check(bam_aux_append(&record, "MM", 'Z', static_cast<int>(mm.size() + 1U),
                         reinterpret_cast<const std::uint8_t*>(mm.c_str())) == 0,
          "cannot append synthetic MM");
    std::array<std::uint8_t, 6> encoded_ml{};
    encoded_ml[0] = 'C';
    u32_to_le(1U, encoded_ml.data() + 1U);
    encoded_ml[5] = ml;
    check(bam_aux_append(&record, "ML", 'B', static_cast<int>(encoded_ml.size()), encoded_ml.data()) == 0,
          "cannot append synthetic ML");
    std::array<std::uint8_t, 4> encoded_mn{};
    u32_to_le(kReadLength, encoded_mn.data());
    check(bam_aux_append(&record, "MN", 'i', static_cast<int>(encoded_mn.size()), encoded_mn.data()) == 0,
          "cannot append synthetic MN");
}

void write_read(samFile& output, sam_hdr_t& header, const std::string& qname, bool alternate, std::uint8_t ml) {
    std::unique_ptr<bam1_t, decltype(&bam_destroy1)> record(bam_init1(), &bam_destroy1);
    check(record != nullptr, "cannot allocate synthetic BAM record");
    std::string sequence(kReadLength, 'A');
    // One synthetic reference-CpG methylation call, independent of both
    // synthetic sSNVs. These constants are fixture coordinates only.
    sequence[100] = 'C';
    if (alternate) {
        sequence[static_cast<std::size_t>(kFirstSitePosition1 - 1U - kReadStart0)] = 'T';
        sequence[static_cast<std::size_t>(kSecondSitePosition1 - 1U - kReadStart0)] = 'T';
    }
    const std::string qualities(kReadLength, 'I');
    const std::uint32_t cigar = bam_cigar_gen(kReadLength, BAM_CMATCH);
    check(bam_set1(record.get(), qname.size(), qname.c_str(), 0, 0, static_cast<hts_pos_t>(kReadStart0), 60, 1, &cigar,
                   -1, -1, 0, kReadLength, sequence.c_str(), qualities.c_str(), 64) >= 0,
          "cannot encode synthetic BAM record");
    append_mm_ml_mn(*record, ml);
    check(sam_write1(&output, &header, record.get()) >= 0, "cannot write synthetic BAM record");
}

SyntheticInputs build_inputs(const std::filesystem::path& root) {
    SyntheticInputs fixture;
    fixture.root = root;
    std::filesystem::create_directories(root);
    fixture.fasta = root / "synthetic.fa";
    fixture.fai = root / "synthetic.explicit.fai";
    fixture.vcf = root / "synthetic.vcf.gz";
    fixture.vcf_index = root / "synthetic.vcf.explicit.csi";
    fixture.bam = root / "synthetic.bam";
    fixture.bam_index = root / "synthetic.explicit.bai";
    fixture.sidecar = root / "synthetic.read_tags.tsv.gz";
    fixture.sidecar_index = root / "synthetic.read_tags.explicit.tbi";

    std::string reference(kContigLength, 'A');
    reference[600] = 'C';
    reference[601] = 'G';
    // Legal lowercase IUPAC symbols are deliberately inside the producer
    // fetch context but away from the only methylation call. Before the
    // IUPAC reference fix this makes the complete producer fail closed.
    reference[700] = 'm';
    reference[701] = 'r';
    write_binary(fixture.fasta, ">chr1\n" + reference + "\n");
    check(fai_build3(fixture.fasta.c_str(), fixture.fai.c_str(), nullptr) == 0, "cannot index synthetic FASTA");

    const std::string vcf =
        "##fileformat=VCFv4.2\n"
        "##contig=<ID=chr1,length=3000>\n"
        "##FILTER=<ID=PASS,Description=\"Synthetic PASS\">\n"
        "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n"
        "chr1\t1001\t.\tA\tT\t.\tPASS\t.\n"
        "chr1\t1401\t.\tA\tT\t.\tPASS\t.\n";
    write_bgzf(fixture.vcf, vcf);
    check(bcf_index_build3(fixture.vcf.c_str(), fixture.vcf_index.c_str(), 14, 1) == 0, "cannot index synthetic VCF");

    const std::string header_text =
        "@HD\tVN:1.6\tSO:coordinate\n"
        "@SQ\tSN:chr1\tLN:3000\n";
    std::unique_ptr<sam_hdr_t, decltype(&sam_hdr_destroy)> header(
        sam_hdr_parse(header_text.size(), header_text.c_str()), &sam_hdr_destroy);
    check(header != nullptr, "cannot construct synthetic BAM header");
    std::unique_ptr<samFile, decltype(&hts_close)> bam_output(sam_open(fixture.bam.c_str(), "wb"), &hts_close);
    check(bam_output != nullptr, "cannot open synthetic BAM");
    check(sam_hdr_write(bam_output.get(), header.get()) >= 0, "cannot write synthetic BAM header");
    write_read(*bam_output, *header, "synthetic-alt-read", true, 224);
    write_read(*bam_output, *header, "synthetic-ref-read", false, 32);
    bam_output.reset();
    check(sam_index_build3(fixture.bam.c_str(), fixture.bam_index.c_str(), 0, 1) == 0, "cannot index synthetic BAM");

    const std::string cigar_digest = longlineage::blake2b_64_hex("1000M");
    const std::string sidecar = "chr1\t500\t1500\tsynthetic-alt-read\t0\t60\t" + cigar_digest + "\t1-1\t7\n" +
                                "chr1\t500\t1500\tsynthetic-ref-read\t0\t60\t" + cigar_digest + "\t2-1\t7\n";
    write_bgzf(fixture.sidecar, sidecar);
    const tbx_conf_t sidecar_conf{TBX_GENERIC | TBX_UCSC, 1, 2, 3, '#', 0};
    check(tbx_index_build3(fixture.sidecar.c_str(), fixture.sidecar_index.c_str(), 0, 1, &sidecar_conf) == 0,
          "cannot index synthetic latest sidecar");
    return fixture;
}

struct CatalogBinding {
    std::uint64_t logical_rows{0};
    std::string semantic_sha256;

    friend bool operator==(const CatalogBinding& left, const CatalogBinding& right) {
        return left.logical_rows == right.logical_rows && left.semantic_sha256 == right.semantic_sha256;
    }
};

std::map<std::string, CatalogBinding> read_catalog(const std::filesystem::path& path) {
    BGZF* input = bgzf_open(path.c_str(), "r");
    check(input != nullptr, "cannot open generated artifact catalog");
    std::map<std::string, CatalogBinding> output;
    kstring_t line{0, 0, nullptr};
    while (true) {
        const int status = bgzf_getline(input, '\n', &line);
        if (status == -1) {
            break;
        }
        check(status >= 0, "cannot read generated artifact catalog");
        json_error_t error{};
        JsonPtr record(json_loadb(line.s, line.l, JSON_REJECT_DUPLICATES, &error), json_decref);
        check(record != nullptr, "cannot parse generated artifact catalog");
        const json_t* artifact = json_object_get(record.get(), "artifact");
        const json_t* id = json_object_get(artifact, "artifact_id");
        const json_t* rows = json_object_get(artifact, "logical_rows");
        const json_t* semantic = json_object_get(artifact, "semantic_sha256");
        check(json_is_string(id) && json_is_integer(rows) && json_integer_value(rows) >= 0 && json_is_string(semantic),
              "generated artifact catalog row is malformed");
        check(output
                  .emplace(json_string_value(id), CatalogBinding{static_cast<std::uint64_t>(json_integer_value(rows)),
                                                                 json_string_value(semantic)})
                  .second,
              "generated artifact catalog has duplicate ID");
    }
    std::free(line.s);
    check(bgzf_close(input) == 0, "cannot close generated artifact catalog");
    return output;
}

std::string counters_identity(const longlineage::pipeline::DatasetProductionCounters& value) {
    std::ostringstream output;
    output << value.site_keys << '\t' << value.site_read_rows << '\t' << value.methyl_call_rows << '\t'
           << value.m1_evaluable << '\t' << value.m1_insufficient_alt_reads << '\t' << value.m1_incomplete_distance
           << '\t' << value.m1_stable_assignments << '\t' << value.latest_tag_exact_joins << '\t' << value.raw_expected
           << '\t' << value.raw_matched << '\t' << value.raw_rg_only_duplicate_occurrences << '\t' << value.m2_eligible
           << '\t' << value.m2_evaluable_ineligible << '\t' << value.m2_axis_indeterminate << '\t'
           << value.m2_group_count_gt10 << '\t' << value.cooccurrence_pairs << '\t' << value.exact_testable_pairs
           << '\t' << value.global_bh_discoveries << '\t' << value.global_by_discoveries << '\t'
           << value.formal_pair_by_confirmed << '\t' << value.topology_primary_hp_units << '\t'
           << value.topology_regions << '\t' << value.topology_fully_complete_regions << '\t'
           << value.topology_incomplete_regions << '\t' << value.topology_incomplete_units_with_winner;
    return output.str();
}

std::vector<longlineage::artifact::ArtifactInputBinding> manifest_inputs(const SyntheticInputs& fixture,
                                                                         const std::string& manifest_sha256) {
    std::vector<longlineage::artifact::ArtifactInputBinding> output;
    const auto paths = fixture.role_paths();
    for (const auto& role : kInputRoles) {
        output.push_back(
            {"MANIFEST_INPUT", std::string(kDatasetId) + ":" + role, "PHYSICAL_SHA256", digest_file(paths.at(role))});
    }
    output.push_back({"MANIFEST_INPUT", "run_manifest", "PHYSICAL_SHA256", manifest_sha256});
    return output;
}

std::vector<longlineage::artifact::InputMountIdentity> mount_identities(const SyntheticInputs& fixture) {
    std::vector<longlineage::artifact::InputMountIdentity> output;
    const auto paths = fixture.role_paths();
    for (const auto& role : kInputRoles) {
        output.push_back({std::string(kDatasetId), role, std::filesystem::canonical(paths.at(role)),
                          "synthetic-fixture", "syntheticfs", true, digest_bytes("synthetic-readonly-mount")});
    }
    return output;
}

struct FrozenInputContract {
    std::filesystem::path manifest;
    std::string manifest_sha256;
    std::string snapshot_sha256;
    std::string input_lock_sha256;
};

FrozenInputContract freeze_input_contract(const SyntheticInputs& fixture, const std::filesystem::path& run_root,
                                          const std::filesystem::path& repo_root, std::size_t workers) {
    JsonPtr manifest(json_object(), json_decref);
    json_object_set_new(manifest.get(), "schema_name", json_string("longlineage.production_manifest"));
    json_object_set_new(manifest.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(manifest.get(), "run_id", json_string(std::string(kRunId).c_str()));
    json_object_set_new(manifest.get(), "authority_profile", json_string("SYNTHETIC"));
    json_object_set_new(manifest.get(), "output_root", json_string(run_root.string().c_str()));
    JsonPtr datasets(json_array(), json_decref);
    JsonPtr dataset(json_object(), json_decref);
    json_object_set_new(dataset.get(), "dataset_id", json_string(std::string(kDatasetId).c_str()));
    json_object_set_new(dataset.get(), "dataset_order", json_integer(0));
    JsonPtr files(json_array(), json_decref);
    const auto locked_paths = fixture.role_paths();
    for (const auto& role : kInputRoles) {
        const std::filesystem::path canonical = std::filesystem::canonical(locked_paths.at(role));
        JsonPtr file(json_object(), json_decref);
        json_object_set_new(file.get(), "role", json_string(role.c_str()));
        json_object_set_new(file.get(), "path", json_string(canonical.string().c_str()));
        json_object_set_new(file.get(), "size_bytes",
                            json_integer(static_cast<json_int_t>(std::filesystem::file_size(canonical))));
        const std::string digest = digest_file(canonical);
        json_object_set_new(file.get(), "sha256", json_string(digest.c_str()));
        json_array_append_new(files.get(), file.release());
    }
    json_object_set_new(dataset.get(), "files", files.release());
    json_array_append_new(datasets.get(), dataset.release());
    json_object_set_new(manifest.get(), "datasets", datasets.release());
    JsonPtr runtime(json_object(), json_decref);
    json_object_set_new(runtime.get(), "compute_workers", json_integer(static_cast<json_int_t>(workers)));
    json_object_set_new(runtime.get(), "writer_threads", json_integer(4));
    json_object_set_new(runtime.get(), "coordinator_slots", json_integer(2));
    json_object_set_new(runtime.get(), "buffer_bytes", json_integer(64U * 1024U * 1024U));
    json_object_set_new(runtime.get(), "max_focal_sites_per_block", json_integer(1));
    json_object_set_new(runtime.get(), "max_estimated_alignments_per_block", json_integer(250000));
    json_object_set_new(runtime.get(), "halo_bp", json_integer(5000));
    json_object_set_new(manifest.get(), "runtime", runtime.release());
    JsonPtr bindings(json_object(), json_decref);
    const std::vector<std::pair<const char*, const char*>> contracts = {
        {"science_parameters_sha256", "contracts/v1/science_parameters.json"},
        {"schema_catalog_sha256", "schema/catalog.json"},
        {"status_reason_registry_sha256", "contracts/v1/status_reason_codes.tsv"},
        {"type_registry_sha256", "contracts/v1/type_registry.tsv"},
        {"transform_registry_sha256", "contracts/v1/transform_registry.tsv"},
        {"authority_manifest_sha256", "oracle/authority_manifest.json"},
        {"source_to_target_manifest_sha256", "provenance/source_to_target_manifest.json"},
        {"production_input_authority_sha256", "oracle/production_input_authority.json"},
        {"schema_id_registry_sha256", "schema/id_registry.json"},
        {"release_attestation_sha256", "state/release_attestation.json"},
    };
    for (const auto& [field, relative] : contracts) {
        const std::string digest = digest_file(repo_root / relative);
        json_object_set_new(bindings.get(), field, json_string(digest.c_str()));
    }
    json_object_set_new(manifest.get(), "contract_bindings", bindings.release());
    const std::filesystem::path manifest_path = run_root.parent_path().parent_path() / "production_manifest.json";
    std::filesystem::create_directories(manifest_path.parent_path());
    char* encoded = json_dumps(manifest.get(), JSON_INDENT(2) | JSON_ENSURE_ASCII | JSON_PRESERVE_ORDER);
    check(encoded != nullptr, "cannot encode synthetic production manifest");
    std::string manifest_bytes(encoded);
    std::free(encoded);
    manifest_bytes.push_back('\n');
    write_binary(manifest_path, manifest_bytes);

    std::ostringstream snapshot;
    std::ostringstream locks;
    snapshot << "longlineage.input_snapshot\t1.1.0\n";
    locks << "longlineage.input_lock\t1.0.0\n";
    for (const auto& role : kInputRoles) {
        const std::filesystem::path canonical = std::filesystem::canonical(locked_paths.at(role));
        struct stat status {};
        check(::stat(canonical.c_str(), &status) == 0, "cannot stat synthetic locked input");
        const auto size = static_cast<std::uint64_t>(status.st_size);
        const std::string digest = digest_file(canonical);
        snapshot << "0\t" << kDatasetId << '\t' << role << '\t' << canonical.string() << '\t'
                 << static_cast<std::uint64_t>(status.st_dev) << '\t' << static_cast<std::uint64_t>(status.st_ino)
                 << '\t' << size << '\t' << static_cast<std::int64_t>(status.st_mtim.tv_sec) << '\t'
                 << static_cast<std::int64_t>(status.st_mtim.tv_nsec) << '\t'
                 << static_cast<std::int64_t>(status.st_ctim.tv_sec) << '\t'
                 << static_cast<std::int64_t>(status.st_ctim.tv_nsec) << '\t' << digest << '\n';
        locks << "0\t" << kDatasetId << '\t' << role << '\t' << canonical.string() << '\t' << size << '\t' << digest
              << '\n';
    }
    return {std::filesystem::canonical(manifest_path), digest_file(manifest_path), digest_bytes(snapshot.str()),
            digest_bytes(locks.str())};
}

struct ProducedRun {
    std::filesystem::path root;
    std::filesystem::path manifest;
    longlineage::pipeline::DatasetProductionReceipt production;
    longlineage::artifact::DatasetCloseoutReceipt closeout;
    std::map<std::string, CatalogBinding> catalog;
};

ProducedRun produce(const SyntheticInputs& fixture, const longlineage::VariantSiteSet& variants,
                    const longlineage::pipeline::DatasetExecutionPlan& plan, const std::filesystem::path& repo_root,
                    const std::filesystem::path& executable, std::size_t workers,
                    const std::filesystem::path& run_root) {
    const longlineage::pipeline::DatasetReadPaths paths{fixture.bam,           fixture.bam_index, fixture.sidecar,
                                                        fixture.sidecar_index, fixture.fasta,     fixture.fai};
    const FrozenInputContract frozen = freeze_input_contract(fixture, run_root, repo_root, workers);
    longlineage::pipeline::DatasetProductionOptions options;
    options.workers = workers;
    options.task_queue_capacity_bytes = 16U * 1024U * 1024U;
    options.reorder_capacity_bytes = 64U * 1024U * 1024U;
    options.maximum_block_payload_bytes = 16U * 1024U * 1024U;
    options.bgzf_compression_threads = 4;
    options.task_type = "B";
    options.completeness = "FULL";
    options.site_population = "SYNTHETIC_AUTOSOMAL_SSNV_FIXTURE";
    auto production = longlineage::pipeline::produce_dataset_scientific_artifacts(paths, variants, plan, run_root,
                                                                                  std::string(kRunId), options);
    check(production.ok() && production.value.has_value(), "dataset producer failed: " + production.detail);

    longlineage::artifact::DatasetCloseoutOptions closeout_options;
    closeout_options.repo_root = repo_root;
    closeout_options.staging_root = run_root;
    closeout_options.run_id = std::string(kRunId);
    closeout_options.executable = {"0.1.0", std::string(40U, '1'), digest_file(executable), "synthetic-c++17", "1.18"};
    closeout_options.producer_hostname = "synthetic-host";
    closeout_options.producer_kernel_release = "synthetic-kernel";
    closeout_options.input_mount_identity = mount_identities(fixture);
    closeout_options.manifest_inputs = manifest_inputs(fixture, frozen.manifest_sha256);
    closeout_options.manifest_sha256 = frozen.manifest_sha256;
    closeout_options.input_snapshot_before_sha256 = frozen.snapshot_sha256;
    closeout_options.input_snapshot_after_sha256 = frozen.snapshot_sha256;
    closeout_options.input_lock_sha256 = frozen.input_lock_sha256;
    closeout_options.phase_ledger_sha256 = digest_file(repo_root / "state/phase_ledger.json");
    closeout_options.performance.wall_seconds = production.value->timing.total_seconds;
    closeout_options.performance.peak_threads = production.value->peak_process_threads;
    closeout_options.performance.queue_wait_seconds = production.value->timing.queue_wait_seconds;
    closeout_options.performance.reorder_wait_seconds = production.value->timing.reorder_wait_seconds;
    closeout_options.performance.task_latency_p50_seconds = production.value->timing.task_latency_p50_seconds;
    closeout_options.performance.task_latency_p95_seconds = production.value->timing.task_latency_p95_seconds;
    closeout_options.performance.task_latency_p99_seconds = production.value->timing.task_latency_p99_seconds;
    closeout_options.performance.task_latency_max_seconds = production.value->timing.task_latency_max_seconds;
    for (const auto& artifact : production.value->artifacts) {
        closeout_options.performance.logical_records += artifact.logical_rows;
        closeout_options.performance.logical_bytes += artifact.logical_bytes;
        ++closeout_options.performance.final_file_count;
        if (!artifact.index.path.empty()) {
            ++closeout_options.performance.final_file_count;
        }
    }
    closeout_options.performance.final_file_count += 4U;
    closeout_options.performance.cache_condition = "UNKNOWN";
    closeout_options.finished_at = "2026-07-20T00:00:00Z";

    auto closeout =
        longlineage::artifact::write_dataset_producer_closeout(production.value->artifacts, closeout_options);
    check(closeout.ok() && closeout.value.has_value(), "dataset closeout failed: " + closeout.detail);
    return {run_root, frozen.manifest, std::move(*production.value), std::move(*closeout.value),
            read_catalog(run_root / "artifact_catalog.jsonl.bgz")};
}

void verify_run(const ProducedRun& run, std::size_t expected_sites) {
    check(run.production.complete_plan, "synthetic producer did not cover its complete plan");
    check(run.production.artifacts.size() == kScientificArtifacts.size(),
          "producer did not emit exactly nine scientific artifacts");
    check(run.production.counters.site_keys == expected_sites, "producer site census differs from frozen input");
    check(run.production.counters.site_read_rows == expected_sites * 2U,
          "producer site/read census differs from synthetic BAM");
    check(run.production.counters.methyl_call_rows == expected_sites,
          "IUPAC context changed exact-CG methylation row conservation");
    check(run.production.counters.raw_expected == expected_sites * 2U &&
              run.production.counters.raw_matched == expected_sites * 2U &&
              run.production.counters.latest_tag_exact_joins == expected_sites * 2U,
          "producer latest-sidecar join conservation differs");
    check(run.production.counters.m1_insufficient_alt_reads == expected_sites,
          "synthetic insufficient-ALT abstention census differs");
    check(run.production.counters.m2_eligible == 0U && run.production.counters.topology_regions == 0U,
          "synthetic abstention unexpectedly entered M2/topology");
    check(run.production.counters.m2_eligible + run.production.counters.m2_evaluable_ineligible +
                  run.production.counters.m2_axis_indeterminate + run.production.counters.m2_group_count_gt10 ==
              run.production.counters.m1_stable_assignments,
          "producer M2 status partition does not conserve stable M1 sites");

    std::set<std::string> observed;
    for (const auto& artifact : run.production.artifacts) {
        observed.insert(artifact.artifact_id);
    }
    check(observed == kScientificArtifacts, "scientific artifact membership differs from canonical set");
    check(run.catalog.size() == kScientificArtifacts.size(), "artifact catalog does not have nine scientific rows");
    for (const auto& id : kScientificArtifacts) {
        check(run.catalog.count(id) == 1U, "artifact catalog is missing " + id);
    }
    check(run.closeout.artifacts.size() == 12U,
          "producer closeout did not bind nine science plus three closeout artifacts");
    check(std::filesystem::is_regular_file(run.root / "receipts/producer_receipt.json") &&
              std::filesystem::is_regular_file(run.root / "checksums.sha256") &&
              std::filesystem::is_regular_file(run.root / "semantic_digests.tsv"),
          "producer closeout files are incomplete");
    check(!std::filesystem::exists(run.root / "validation_receipt.json") &&
              !std::filesystem::exists(run.root / "run_receipt.json"),
          "producer forged validator/run authority");
}

void compare_semantics(const ProducedRun& one, const ProducedRun& two) {
    check(counters_identity(one.production.counters) == counters_identity(two.production.counters),
          "one/two-worker conservation counters differ");
    check(one.production.semantic_sha256 == two.production.semantic_sha256,
          "one/two-worker production semantic SHA-256 differs");
    check(one.catalog == two.catalog, "one/two-worker catalog logical rows or semantic SHA-256 differ");

    std::map<std::string, CatalogBinding> one_receipts;
    std::map<std::string, CatalogBinding> two_receipts;
    for (const auto& artifact : one.production.artifacts) {
        one_receipts.emplace(artifact.artifact_id, CatalogBinding{artifact.logical_rows, artifact.semantic_sha256});
    }
    for (const auto& artifact : two.production.artifacts) {
        two_receipts.emplace(artifact.artifact_id, CatalogBinding{artifact.logical_rows, artifact.semantic_sha256});
    }
    check(one_receipts == two_receipts && one_receipts == one.catalog,
          "producer receipts and catalog semantic bindings differ");
}

void run_independent_validator(const std::filesystem::path& validator, const std::filesystem::path& repo_root,
                               const ProducedRun& run) {
    const pid_t child = ::fork();
    check(child >= 0, "cannot fork independent validator");
    if (child == 0) {
        ::execl(validator.c_str(), validator.c_str(), "--run-root", run.root.c_str(), "--manifest",
                run.manifest.c_str(), "--repo", repo_root.c_str(), "--check-only", static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child, "cannot wait for independent validator");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "independent validator rejected producer closeout");
    check(!std::filesystem::exists(run.root / "validation_receipt.json") &&
              !std::filesystem::exists(run.root / "run_receipt.json"),
          "check-only validator crossed its receipt boundary");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "usage: test_dataset_producer VALIDATOR_BIN REPO_ROOT");
        const std::filesystem::path validator = std::filesystem::canonical(argv[1]);
        const std::filesystem::path repo_root = std::filesystem::canonical(argv[2]);
        const std::filesystem::path executable = std::filesystem::canonical("/proc/self/exe");
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path fixture_root =
            std::filesystem::path("/tmp") /
            ("longlineage_dataset_producer_" + std::to_string(static_cast<long long>(::getpid())) + "_" +
             std::to_string(nonce));
        const SyntheticInputs fixture = build_inputs(fixture_root / "inputs");

        auto reference = longlineage::IndexedReferenceReader::open(fixture.fasta, fixture.fai);
        check(reference.ok() && reference.value.has_value(),
              "cannot open synthetic indexed reference: " + reference.detail);
        auto variants = longlineage::load_variant_sites(fixture.vcf, fixture.vcf_index, **reference.value, 0,
                                                        std::string(kDatasetId));
        check(variants.ok() && variants.value.has_value(), "cannot load synthetic variant sites: " + variants.detail);
        check(variants.value->sites.size() == 2U && variants.value->census.selected_scope == 2U,
              "synthetic VCF site census differs");

        longlineage::pipeline::DatasetPlanOptions plan_options;
        plan_options.max_focal_sites_per_block = 1U;
        auto plan = longlineage::pipeline::plan_dataset_execution(*variants.value, plan_options);
        check(plan.ok() && plan.value.has_value(), "cannot plan synthetic dataset: " + plan.detail);
        check(plan.value->blocks.size() == 2U && plan.value->focal_site_count == 2U &&
                  plan.value->marker_occurrences == 4U,
              "synthetic two-block/halo plan differs");

        ProducedRun one = produce(fixture, *variants.value, *plan.value, repo_root, executable, 1U,
                                  fixture_root / "worker-1" / ".staging" / kRunId);
        ProducedRun two = produce(fixture, *variants.value, *plan.value, repo_root, executable, 2U,
                                  fixture_root / "worker-2" / ".staging" / kRunId);
        ProducedRun forty = produce(fixture, *variants.value, *plan.value, repo_root, executable, 40U,
                                    fixture_root / "worker-40" / ".staging" / kRunId);
        verify_run(one, variants.value->sites.size());
        verify_run(two, variants.value->sites.size());
        verify_run(forty, variants.value->sites.size());
        check(forty.production.peak_process_threads == 46U,
              "40 compute workers plus four BGZF workers and two coordinator "
              "threads must occupy exactly 46 process threads");
        compare_semantics(one, two);
        compare_semantics(one, forty);
        run_independent_validator(validator, repo_root, one);
        run_independent_validator(validator, repo_root, two);
        run_independent_validator(validator, repo_root, forty);

        std::cout << "dataset_producer_e2e: PASS"
                  << " sites=" << one.production.counters.site_keys
                  << " site_reads=" << one.production.counters.site_read_rows
                  << " science_artifacts=" << one.production.artifacts.size()
                  << " closeout_artifacts=" << one.closeout.artifacts.size()
                  << " semantic_sha256=" << one.production.semantic_sha256
                  << " peak_threads_w40=" << forty.production.peak_process_threads << " validator_replays=3\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dataset_producer_e2e: FAIL: " << error.what() << '\n';
        return 1;
    }
}
