// SPDX-License-Identifier: GPL-3.0-only

#include <jansson.h>
#include <openssl/evp.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/validation/regional_compat_validator.hpp"

namespace {

constexpr std::array<std::string_view, 8> kInputRoles = {
    "raw_bam",
    "raw_bam_index",
    "pass_biallelic_ssnv_vcf",
    "pass_biallelic_ssnv_vcf_index",
    "latest_hp_ps_sidecar",
    "latest_hp_ps_sidecar_index",
    "reference_fasta",
    "reference_fai",
};

constexpr std::array<std::string_view, 7> kDatasetOrder = {
    "HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954",
};

std::string g_v2_authority_sha256;
std::string g_hcc_authority_sha256;
std::filesystem::path g_v2_repository;

void seal_producer(const std::filesystem::path& root, std::string_view schema_version);

constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kContractBindings = {{
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
}};

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};

using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Sha256 final {
   public:
    Sha256() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        check(context_ != nullptr && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1,
              "cannot initialize test SHA-256");
    }

    void update(std::string_view bytes) {
        check(bytes.empty() || EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) == 1,
              "cannot update test SHA-256");
    }

    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> raw{};
        unsigned int size = 0;
        check(EVP_DigestFinal_ex(context_.get(), raw.data(), &size) == 1 && size == 32U,
              "cannot finalize test SHA-256");
        constexpr char kHex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64U);
        for (unsigned int index = 0; index < size; ++index) {
            const unsigned char byte = raw[index];
            result.push_back(kHex[(byte >> 4U) & 0x0fU]);
            result.push_back(kHex[byte & 0x0fU]);
        }
        return result;
    }

   private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

std::string sha256_bytes(std::string_view bytes) {
    Sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "cannot hash test file: " + path.string());
    Sha256 digest;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            digest.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    check(input.eof(), "test hash read failed");
    return digest.finish();
}

void write_text(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "cannot create test file: " + path.string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(output), "cannot write test file: " + path.string());
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "cannot read test file: " + path.string());
    std::string result((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    check(!input.bad(), "test text read failed: " + path.string());
    return result;
}

std::string dump_json(const json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    check(encoded != nullptr, "cannot encode test JSON");
    std::string result(encoded);
    std::free(encoded);
    result.push_back('\n');
    return result;
}

void write_json(const std::filesystem::path& path, const json_t* value) { write_text(path, dump_json(value)); }

JsonPtr load_json(const std::filesystem::path& path) {
    json_error_t error{};
    JsonPtr value(json_load_file(path.c_str(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY | JSON_ALLOW_NUL, &error));
    check(value != nullptr, "cannot load test JSON: " + path.string());
    return value;
}

std::filesystem::path create_v2_repository(const std::filesystem::path& base) {
    const std::filesystem::path repository = base / "v2-repository";
    const std::filesystem::path oracle = repository / "oracle";
    std::filesystem::create_directories(oracle);
    for (const auto& binding : kContractBindings) {
        const std::filesystem::path path = repository / std::filesystem::path(binding.second);
        write_text(path, std::string("synthetic contract: ") + std::string(binding.first) + "\n");
    }
    write_text(oracle / "production_input_authority.json", "{}\n");
    const std::string base_sha = sha256_file(oracle / "production_input_authority.json");

    JsonPtr hcc_authority(json_object());
    json_object_set_new(hcc_authority.get(), "schema_name", json_string("longlineage.dataset_gate_input_authority"));
    json_object_set_new(hcc_authority.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(hcc_authority.get(), "authority_id", json_string("synthetic-hcc-authority"));
    json_object_set_new(hcc_authority.get(), "authority_profile", json_string("HCC1395_DATASET_GATE"));
    json_object_set_new(hcc_authority.get(), "dataset_id", json_string("HCC1395"));
    json_object_set_new(hcc_authority.get(), "dataset_order", json_integer(0));
    json_object_set_new(hcc_authority.get(), "truth_fields", json_integer(0));
    json_object_set_new(hcc_authority.get(), "latest_tag_join", json_string("EXACT_PROJECTION_NO_FALLBACK"));
    json_object_set_new(hcc_authority.get(), "private_source_paths_stored", json_false());
    json_object_set_new(hcc_authority.get(), "tagged_bam_persisted", json_false());
    json_object_set_new(hcc_authority.get(), "full_content_freeze", json_true());
    json_object_set_new(hcc_authority.get(), "allowed_terminal_state", json_string("VALIDATED_FROZEN"));
    json_object_set_new(hcc_authority.get(), "variant_scope", json_string("PASS_BIALLELIC_SSNV"));
    json_object_set_new(hcc_authority.get(), "claim", json_string("DESCRIPTIVE_REGIONAL"));
    JsonPtr hcc_files(json_array());
    for (std::size_t file_index = 0; file_index < kInputRoles.size(); ++file_index) {
        JsonPtr row(json_object());
        const std::string role(kInputRoles[file_index]);
        const std::string digest(64U, static_cast<char>('0' + file_index));
        json_object_set_new(row.get(), "role", json_string(role.c_str()));
        json_object_set_new(row.get(), "path_token", json_string(("HCC1395_" + role).c_str()));
        json_object_set_new(row.get(), "size_bytes", json_integer(static_cast<json_int_t>(100U + file_index)));
        json_object_set_new(row.get(), "sha256", json_string(digest.c_str()));
        json_array_append_new(hcc_files.get(), row.release());
    }
    json_object_set_new(hcc_authority.get(), "files", hcc_files.release());
    write_json(oracle / "hcc1395_dataset_gate_input_authority.json", hcc_authority.get());
    g_hcc_authority_sha256 = sha256_file(oracle / "hcc1395_dataset_gate_input_authority.json");

    JsonPtr authority(json_object());
    json_object_set_new(authority.get(), "schema_name",
                        json_string("longlineage.regional_compat_all_datasets_input_authority"));
    json_object_set_new(authority.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(authority.get(), "profile_id", json_string("PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET"));
    json_object_set_new(authority.get(), "base_production_input_authority_sha256", json_string(base_sha.c_str()));
    json_object_set_new(authority.get(), "hcc1395_dataset_gate_input_authority_sha256",
                        json_string(g_hcc_authority_sha256.c_str()));
    JsonPtr constraints(json_object());
    json_object_set_new(constraints.get(), "latest_tag_join", json_string("EXACT_PROJECTION_NO_FALLBACK"));
    json_object_set_new(constraints.get(), "truth_fields", json_integer(0));
    json_object_set_new(constraints.get(), "persisted_tagged_bam_allowed", json_false());
    json_object_set_new(constraints.get(), "all_physical_sha256_verified", json_true());
    json_object_set_new(constraints.get(), "private_source_paths_stored", json_false());
    json_object_set_new(authority.get(), "constraints", constraints.release());
    JsonPtr datasets(json_array());
    for (std::size_t dataset_index = 0; dataset_index < kDatasetOrder.size(); ++dataset_index) {
        JsonPtr dataset(json_object());
        json_object_set_new(dataset.get(), "dataset_id",
                            json_string(std::string(kDatasetOrder[dataset_index]).c_str()));
        json_object_set_new(dataset.get(), "dataset_order", json_integer(static_cast<json_int_t>(dataset_index)));
        JsonPtr files(json_array());
        for (std::size_t file_index = 0; file_index < kInputRoles.size(); ++file_index) {
            JsonPtr row(json_object());
            const std::string role(kInputRoles[file_index]);
            const std::string digest(64U, static_cast<char>('0' + file_index));
            json_object_set_new(row.get(), "role", json_string(role.c_str()));
            json_object_set_new(row.get(), "size_bytes", json_integer(static_cast<json_int_t>(100U + file_index)));
            json_object_set_new(row.get(), "sha256", json_string(digest.c_str()));
            json_array_append_new(files.get(), row.release());
        }
        json_object_set_new(dataset.get(), "files", files.release());
        json_array_append_new(datasets.get(), dataset.release());
    }
    json_object_set_new(authority.get(), "datasets", datasets.release());
    write_json(oracle / "regional_compat_all_datasets_input_authority.json", authority.get());
    g_v2_authority_sha256 = sha256_file(oracle / "regional_compat_all_datasets_input_authority.json");
    g_v2_repository = repository;
    return repository;
}

std::filesystem::path write_v2_source_manifest(const std::filesystem::path& bundle_root) {
    check(!g_v2_repository.empty(), "v2 repository must exist before writing a source manifest");
    const std::filesystem::path manifest_path =
        bundle_root.parent_path() / "source-manifests" / (bundle_root.filename().string() + ".json");
    JsonPtr manifest(json_object());
    json_object_set_new(manifest.get(), "schema_name", json_string("longlineage.production_manifest"));
    json_object_set_new(manifest.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(manifest.get(), "authority_profile", json_string("PRODUCTION_7_DATASET"));
    json_object_set_new(manifest.get(), "run_id", json_string("synthetic-production-seven"));
    json_object_set_new(manifest.get(), "output_root", json_string("/synthetic/.staging/synthetic-production-seven"));

    JsonPtr datasets(json_array());
    for (std::size_t dataset_index = 0; dataset_index < kDatasetOrder.size(); ++dataset_index) {
        JsonPtr dataset(json_object());
        const std::string dataset_id(kDatasetOrder[dataset_index]);
        json_object_set_new(dataset.get(), "dataset_id", json_string(dataset_id.c_str()));
        json_object_set_new(dataset.get(), "dataset_order", json_integer(static_cast<json_int_t>(dataset_index)));
        JsonPtr files(json_array());
        for (std::size_t file_index = 0; file_index < kInputRoles.size(); ++file_index) {
            JsonPtr row(json_object());
            const std::string role(kInputRoles[file_index]);
            const std::string path = "/synthetic/" + dataset_id + "/" + role;
            const std::string digest(64U, static_cast<char>('0' + file_index));
            json_object_set_new(row.get(), "role", json_string(role.c_str()));
            json_object_set_new(row.get(), "path", json_string(path.c_str()));
            json_object_set_new(row.get(), "size_bytes", json_integer(static_cast<json_int_t>(100U + file_index)));
            json_object_set_new(row.get(), "sha256", json_string(digest.c_str()));
            json_array_append_new(files.get(), row.release());
        }
        json_object_set_new(dataset.get(), "files", files.release());
        json_array_append_new(datasets.get(), dataset.release());
    }
    json_object_set_new(manifest.get(), "datasets", datasets.release());

    JsonPtr runtime(json_object());
    json_object_set_new(runtime.get(), "compute_workers", json_integer(2));
    json_object_set_new(runtime.get(), "writer_threads", json_integer(1));
    json_object_set_new(runtime.get(), "coordinator_slots", json_integer(2));
    json_object_set_new(runtime.get(), "buffer_bytes", json_integer(1048576));
    json_object_set_new(runtime.get(), "max_focal_sites_per_block", json_integer(4096));
    json_object_set_new(runtime.get(), "max_estimated_alignments_per_block", json_integer(250000));
    json_object_set_new(runtime.get(), "halo_bp", json_integer(5000));
    json_object_set_new(manifest.get(), "runtime", runtime.release());

    JsonPtr bindings(json_object());
    for (const auto& binding : kContractBindings) {
        const std::string digest = sha256_file(g_v2_repository / std::filesystem::path(binding.second));
        json_object_set_new(bindings.get(), std::string(binding.first).c_str(), json_string(digest.c_str()));
    }
    json_object_set_new(manifest.get(), "contract_bindings", bindings.release());
    write_json(manifest_path, manifest.get());
    return manifest_path;
}

void bind_v2_summary_to_manifest(const std::filesystem::path& bundle_root, const std::filesystem::path& manifest_path) {
    JsonPtr summary = load_json(bundle_root / "summary.json");
    json_object_set_new(summary.get(), "source_manifest_path", json_string(manifest_path.c_str()));
    const std::string digest = sha256_file(manifest_path);
    json_object_set_new(summary.get(), "source_manifest_sha256", json_string(digest.c_str()));
    write_json(bundle_root / "summary.json", summary.get());
}

std::filesystem::path source_manifest_path(const std::filesystem::path& bundle_root) {
    const JsonPtr summary = load_json(bundle_root / "summary.json");
    return std::filesystem::path(json_string_value(json_object_get(summary.get(), "source_manifest_path")));
}

void mutate_v2_source_manifest(const std::filesystem::path& bundle_root, const std::function<void(json_t*)>& mutate,
                               bool update_summary_digest = true) {
    const std::filesystem::path manifest_path = source_manifest_path(bundle_root);
    JsonPtr manifest = load_json(manifest_path);
    mutate(manifest.get());
    write_json(manifest_path, manifest.get());
    if (update_summary_digest) {
        bind_v2_summary_to_manifest(bundle_root, manifest_path);
    }
    seal_producer(bundle_root, "2.0.0");
}

std::size_t data_rows(const std::filesystem::path& path) {
    std::ifstream input(path);
    check(static_cast<bool>(input), "cannot count test TSV rows");
    std::string line;
    std::size_t lines = 0;
    while (std::getline(input, line)) {
        ++lines;
    }
    check(lines >= 1U, "test TSV lacks header");
    return lines - 1U;
}

void set_counts(json_t* summary, std::uint64_t regions, std::uint64_t units, std::uint64_t patterns) {
    JsonPtr counts(json_object());
    json_object_set_new(counts.get(), "regions", json_integer(static_cast<json_int_t>(regions)));
    json_object_set_new(counts.get(), "units", json_integer(static_cast<json_int_t>(units)));
    json_object_set_new(counts.get(), "patterns", json_integer(static_cast<json_int_t>(patterns)));
    json_object_set_new(summary, "row_counts", counts.release());
}

void write_summary_v1(const std::filesystem::path& root, std::uint64_t regions, std::uint64_t units,
                      std::uint64_t patterns) {
    JsonPtr summary(json_object());
    json_object_set_new(summary.get(), "schema_name", json_string("longlineage.regional_compat_summary"));
    json_object_set_new(summary.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(summary.get(), "run_id", json_string("synthetic-regional-validator"));
    json_object_set_new(summary.get(), "profile_id", json_string("PYTHON_V2_DESCRIPTIVE_REGIONAL"));
    json_object_set_new(summary.get(), "state", json_string("READY_FOR_VALIDATION"));
    json_object_set_new(summary.get(), "workers", json_integer(2));
    set_counts(summary.get(), regions, units, patterns);
    JsonPtr census(json_object());
    json_object_set_new(census.get(), "positional_singletons", json_integer(1));
    JsonPtr classes(json_object());
    json_object_set_new(classes.get(), "determined", json_integer(1));
    json_object_set_new(census.get(), "classes", classes.release());
    json_object_set_new(summary.get(), "census", census.release());
    write_json(root / "summary.json", summary.get());
}

void write_summary_v2(const std::filesystem::path& root, std::uint64_t regions, std::uint64_t units,
                      std::uint64_t patterns) {
    JsonPtr summary(json_object());
    json_object_set_new(summary.get(), "schema_name", json_string("longlineage.regional_compat_summary"));
    json_object_set_new(summary.get(), "schema_version", json_string("2.0.0"));
    json_object_set_new(summary.get(), "run_id", json_string("synthetic-production-seven-COLO829"));
    json_object_set_new(summary.get(), "profile_id", json_string("PYTHON_V2_DESCRIPTIVE_REGIONAL"));
    json_object_set_new(summary.get(), "state", json_string("READY_FOR_VALIDATION"));
    json_object_set_new(summary.get(), "dataset_id", json_string("COLO829"));
    json_object_set_new(summary.get(), "dataset_order", json_integer(2));
    json_object_set_new(summary.get(), "source_authority_profile", json_string("PRODUCTION_7_DATASET"));
    check(!g_v2_authority_sha256.empty(), "v2 authority must be created before a v2 fixture");
    json_object_set_new(summary.get(), "source_authority_sha256", json_string(g_v2_authority_sha256.c_str()));
    json_object_set_new(summary.get(), "source_manifest_path", json_string("/synthetic/production-manifest.json"));
    const std::string manifest_sha(64U, 'b');
    json_object_set_new(summary.get(), "source_manifest_sha256", json_string(manifest_sha.c_str()));
    json_object_set_new(summary.get(), "source_manifest_run_id", json_string("synthetic-production-seven"));
    json_object_set_new(summary.get(), "workers", json_integer(2));
    json_object_set_new(summary.get(), "first_region", json_integer(0));
    json_object_set_new(summary.get(), "requested_region_count", json_integer(0));

    JsonPtr parameters(json_object());
    json_object_set_new(parameters.get(), "TIER_R", json_integer(50000));
    json_object_set_new(parameters.get(), "MAX_SNV", json_integer(8));
    json_object_set_new(parameters.get(), "MINREAD", json_integer(3));
    json_object_set_new(parameters.get(), "MAPQ_MIN", json_integer(20));
    json_object_set_new(parameters.get(), "BASEQ_MIN", json_integer(0));
    json_object_set_new(parameters.get(), "EXTRA_NODE_CAP", json_integer(4));
    json_object_set_new(parameters.get(), "PER_LEVEL_BUDGET", json_integer(150000));
    json_object_set_new(summary.get(), "parameters", parameters.release());
    set_counts(summary.get(), regions, units, patterns);

    JsonPtr census(json_object());
    json_object_set_new(census.get(), "scope_sites", json_integer(5));
    json_object_set_new(census.get(), "positional_singletons", json_integer(1));
    json_object_set_new(census.get(), "multi_regions_pre_read", json_integer(2));
    json_object_set_new(census.get(), "multi_region_pre_cap_sites", json_integer(4));
    json_object_set_new(census.get(), "capped_regions", json_integer(0));
    json_object_set_new(census.get(), "cap_excluded_sites", json_integer(0));
    json_object_set_new(census.get(), "retained_selected_sites", json_integer(4));
    json_object_set_new(census.get(), "region_determinacy_all_determined", json_integer(1));
    json_object_set_new(census.get(), "region_determinacy_no_primary_lineage", json_integer(1));
    json_object_set_new(census.get(), "unit_class_determined", json_integer(3));
    json_object_set_new(census.get(), "primary_class_determined", json_integer(1));
    json_object_set_new(summary.get(), "census", census.release());

    JsonPtr verification(json_object());
    json_object_set_new(verification.get(), "method", json_string("PHYSICAL_SHA256"));
    json_object_set_new(verification.get(), "role_count", json_integer(8));
    json_object_set_new(verification.get(), "before_after_identity_stable", json_true());
    json_object_set_new(verification.get(), "truth_fields_seen", json_integer(0));
    json_object_set_new(verification.get(), "embedded_hp_fallback_used", json_false());
    json_object_set_new(summary.get(), "input_verification", verification.release());

    JsonPtr inputs(json_array());
    for (std::size_t index = 0; index < kInputRoles.size(); ++index) {
        JsonPtr row(json_object());
        const std::string role(kInputRoles[index]);
        const std::string path = "/synthetic/COLO829/" + role;
        const std::string digest(64U, static_cast<char>('0' + index));
        json_object_set_new(row.get(), "role", json_string(role.c_str()));
        json_object_set_new(row.get(), "path", json_string(path.c_str()));
        json_object_set_new(row.get(), "canonical_path", json_string(path.c_str()));
        json_object_set_new(row.get(), "size_bytes", json_integer(static_cast<json_int_t>(100U + index)));
        json_object_set_new(row.get(), "sha256", json_string(digest.c_str()));
        json_object_set_new(row.get(), "full_sha256_verified", json_true());
        json_array_append_new(inputs.get(), row.release());
    }
    json_object_set_new(summary.get(), "inputs", inputs.release());

    JsonPtr timing(json_object());
    json_object_set_new(timing.get(), "total_wall_seconds", json_real(4.0));
    json_object_set_new(timing.get(), "input_sha256_seconds", json_real(1.0));
    json_object_set_new(timing.get(), "science_wall_seconds", json_real(2.0));
    json_object_set_new(timing.get(), "worker_open_seconds", json_real(0.1));
    json_object_set_new(timing.get(), "summed_input_seconds", json_real(1.5));
    json_object_set_new(timing.get(), "summed_solver_seconds", json_real(0.2));
    json_object_set_new(summary.get(), "timing", timing.release());

    JsonPtr ceiling(json_object());
    json_object_set_new(ceiling.get(), "formal_m2_topology", json_false());
    json_object_set_new(ceiling.get(), "production_release", json_false());
    json_object_set_new(ceiling.get(), "clone_ancestor_time_order", json_false());
    json_object_set_new(summary.get(), "claim_ceiling", ceiling.release());
    write_json(root / "summary.json", summary.get());
}

void write_base_artifacts(const std::filesystem::path& root, std::string_view schema_version = "1.0.0") {
    std::filesystem::create_directories(root);
    write_text(root / "regions.tsv",
               "region_order\tregion_id\tchrom\tstart1\tend1\tspan\tn_sites_pre_cap\tn_sites_selected\t"
               "n_sites_cap_excluded\tselected_positions\tn_full_cov_reads\tn_families\tn_primary_lineages\t"
               "n_reference_controls\tn_h3_aux\tn_h4_aux\tn_none_units\tdeterminacy\n"
               "0\tregion-0\tchrSynthetic\t101\t111\t10\t2\t2\t0\t101,111\t5\t2\t1\t0\t0\t0\t1\t"
               "all_determined\n"
               "1\tregion-1\tchrSynthetic\t201\t211\t10\t2\t2\t0\t201,211\t3\t1\t0\t1\t0\t0\t0\t"
               "no_primary_lineage\n");
    write_text(root / "units.tsv",
               "region_order\tregion_id\tfamily\trole\tn_reads\tn_full_patterns\tn_supported_patterns\t"
               "mutation_bearing\tn_hidden\tn_trees\tn_feasible_node_sets\tcapped\tclass\n"
               "0\tregion-0\t1\tprimary_mutation_lineage\t5\t1\t1\t1\t0\t1\t1\t0\tdetermined\n"
               "0\tregion-0\tnone\tunphased_auxiliary\t3\t0\t1\t1\t0\t1\t1\t0\tdetermined\n"
               "1\tregion-1\t2\treference_only_control\t3\t1\t1\t0\t0\t1\t1\t0\tdetermined\n");
    write_text(root / "patterns.tsv",
               "region_order\tregion_id\tfamily\tpattern\tpattern_kind\tcount\n"
               "0\tregion-0\t1\tAA\tFULL\t3\n"
               "0\tregion-0\t1\tAA\tSUBREAD\t3\n"
               "0\tregion-0\tnone\tAX\tSUBREAD\t3\n"
               "1\tregion-1\t2\tRR\tFULL\t3\n"
               "1\tregion-1\t2\tRR\tSUBREAD\t3\n");
    if (schema_version == "2.0.0") {
        write_summary_v2(root, 2U, 3U, 5U);
    } else {
        write_summary_v1(root, 2U, 3U, 5U);
    }
}

void seal_producer(const std::filesystem::path& root, std::string_view schema_version = "1.0.0") {
    const std::array<std::string, 4> order = {
        "summary.json",
        "regions.tsv",
        "units.tsv",
        "patterns.tsv",
    };
    std::map<std::string, std::uint64_t> rows = {
        {"summary.json", 1U},
        {"regions.tsv", static_cast<std::uint64_t>(data_rows(root / "regions.tsv"))},
        {"units.tsv", static_cast<std::uint64_t>(data_rows(root / "units.tsv"))},
        {"patterns.tsv", static_cast<std::uint64_t>(data_rows(root / "patterns.tsv"))},
    };
    std::map<std::string, std::string> digests;
    std::string semantic = "PYTHON_V2_DESCRIPTIVE_REGIONAL\t" + std::string(schema_version);
    std::string run_id = "synthetic-regional-validator";
    std::string dataset_id = "COLO829";
    json_int_t dataset_order = 2;
    std::string authority_profile = "PRODUCTION_7_DATASET";
    std::string authority_sha256 = g_v2_authority_sha256;
    if (schema_version == "2.0.0") {
        check(!g_v2_authority_sha256.empty(), "v2 authority must be created before sealing a v2 fixture");
        const JsonPtr summary = load_json(root / "summary.json");
        const auto read_optional_text = [&](const char* key, const std::string& fallback) {
            const json_t* value = json_object_get(summary.get(), key);
            return json_is_string(value)
                       ? std::string(json_string_value(value), static_cast<std::size_t>(json_string_length(value)))
                       : fallback;
        };
        run_id = read_optional_text("run_id", "synthetic-production-seven-COLO829");
        dataset_id = read_optional_text("dataset_id", dataset_id);
        authority_profile = read_optional_text("source_authority_profile", authority_profile);
        authority_sha256 = read_optional_text("source_authority_sha256", authority_sha256);
        const json_t* order_value = json_object_get(summary.get(), "dataset_order");
        if (json_is_integer(order_value)) {
            dataset_order = json_integer_value(order_value);
        }
        semantic += "\t" + dataset_id + "\t" + std::to_string(dataset_order) + "\t" + authority_profile + "\t" +
                    authority_sha256;
    }
    semantic += "\n";
    for (const std::string& path : order) {
        digests.emplace(path, sha256_file(root / path));
        semantic += path + "\t" + std::to_string(rows.at(path)) + "\t" + digests.at(path) + "\n";
    }
    JsonPtr receipt(json_object());
    json_object_set_new(receipt.get(), "schema_name", json_string("longlineage.regional_compat_producer_receipt"));
    json_object_set_new(receipt.get(), "schema_version", json_string(std::string(schema_version).c_str()));
    json_object_set_new(receipt.get(), "run_id", json_stringn(run_id.data(), run_id.size()));
    json_object_set_new(receipt.get(), "profile_id", json_string("PYTHON_V2_DESCRIPTIVE_REGIONAL"));
    if (schema_version == "2.0.0") {
        json_object_set_new(receipt.get(), "dataset_id", json_stringn(dataset_id.data(), dataset_id.size()));
        json_object_set_new(receipt.get(), "dataset_order", json_integer(dataset_order));
        json_object_set_new(receipt.get(), "source_authority_profile",
                            json_stringn(authority_profile.data(), authority_profile.size()));
        json_object_set_new(receipt.get(), "source_authority_sha256",
                            json_stringn(authority_sha256.data(), authority_sha256.size()));
    }
    json_object_set_new(receipt.get(), "summary_sha256", json_string(digests.at("summary.json").c_str()));
    const std::string semantic_sha = sha256_bytes(semantic);
    json_object_set_new(receipt.get(), "semantic_sha256", json_string(semantic_sha.c_str()));
    JsonPtr artifacts(json_array());
    for (const std::string& path : order) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "path", json_string(path.c_str()));
        json_object_set_new(row.get(), "sha256", json_string(digests.at(path).c_str()));
        json_object_set_new(row.get(), "rows", json_integer(static_cast<json_int_t>(rows.at(path))));
        json_array_append_new(artifacts.get(), row.release());
    }
    json_object_set_new(receipt.get(), "artifacts", artifacts.release());
    write_json(root / "producer_receipt.json", receipt.get());

    digests.emplace("producer_receipt.json", sha256_file(root / "producer_receipt.json"));
    std::string checksums;
    for (const std::string path :
         {"patterns.tsv", "producer_receipt.json", "regions.tsv", "summary.json", "units.tsv"}) {
        checksums += digests.at(path) + "  " + path + "\n";
    }
    write_text(root / "checksums.sha256", checksums);
}

void rewrite_checksum_manifest(const std::filesystem::path& root) {
    std::string checksums;
    for (const std::string path :
         {"patterns.tsv", "producer_receipt.json", "regions.tsv", "summary.json", "units.tsv"}) {
        checksums += sha256_file(root / path) + "  " + path + "\n";
    }
    write_text(root / "checksums.sha256", checksums);
}

std::filesystem::path fixture(const std::filesystem::path& base, const std::string& name,
                              std::string_view schema_version = "1.0.0") {
    const std::filesystem::path root = base / name;
    write_base_artifacts(root, schema_version);
    if (schema_version == "2.0.0") {
        bind_v2_summary_to_manifest(root, write_v2_source_manifest(root));
    }
    seal_producer(root, schema_version);
    return root;
}

std::filesystem::path hcc_v2_fixture(const std::filesystem::path& base, const std::string& name) {
    const std::filesystem::path root = fixture(base, name, "2.0.0");
    const std::filesystem::path manifest_path = source_manifest_path(root);
    JsonPtr manifest = load_json(manifest_path);
    json_object_set_new(manifest.get(), "schema_version", json_string("1.1.0"));
    json_object_set_new(manifest.get(), "authority_profile", json_string("HCC1395_DATASET_GATE"));
    json_object_set_new(manifest.get(), "run_id", json_string("synthetic-hcc-gate"));
    json_object_set_new(manifest.get(), "output_root", json_string("/synthetic/.staging/synthetic-hcc-gate"));
    json_t* datasets = json_object_get(manifest.get(), "datasets");
    while (json_array_size(datasets) > 1U) {
        check(json_array_remove(datasets, json_array_size(datasets) - 1U) == 0,
              "cannot reduce synthetic HCC manifest dataset set");
    }
    json_object_set_new(json_object_get(manifest.get(), "contract_bindings"), "dataset_gate_input_authority_sha256",
                        json_string(g_hcc_authority_sha256.c_str()));
    write_json(manifest_path, manifest.get());

    JsonPtr summary = load_json(root / "summary.json");
    json_object_set_new(summary.get(), "run_id", json_string("synthetic-hcc-gate-HCC1395"));
    json_object_set_new(summary.get(), "dataset_id", json_string("HCC1395"));
    json_object_set_new(summary.get(), "dataset_order", json_integer(0));
    json_object_set_new(summary.get(), "source_authority_profile", json_string("HCC1395_DATASET_GATE"));
    json_object_set_new(summary.get(), "source_authority_sha256", json_string(g_hcc_authority_sha256.c_str()));
    json_object_set_new(summary.get(), "source_manifest_run_id", json_string("synthetic-hcc-gate"));
    json_object_set_new(summary.get(), "source_manifest_sha256", json_string(sha256_file(manifest_path).c_str()));
    json_t* inputs = json_object_get(summary.get(), "inputs");
    for (std::size_t index = 0; index < kInputRoles.size(); ++index) {
        const std::string path = "/synthetic/HCC1395/" + std::string(kInputRoles[index]);
        json_object_set_new(json_array_get(inputs, index), "path", json_string(path.c_str()));
        json_object_set_new(json_array_get(inputs, index), "canonical_path", json_string(path.c_str()));
    }
    write_json(root / "summary.json", summary.get());
    seal_producer(root, "2.0.0");
    return root;
}

void mutate_v2_summary(const std::filesystem::path& root, const std::function<void(json_t*)>& mutate) {
    JsonPtr summary = load_json(root / "summary.json");
    mutate(summary.get());
    write_json(root / "summary.json", summary.get());
    seal_producer(root, "2.0.0");
}

void expect_rejected(const std::filesystem::path& root, const std::filesystem::path& executable,
                     const std::string& label, const std::string& expected_check = {},
                     const std::filesystem::path& repository_root = {}) {
    const auto report = longlineage::validation::RegionalCompatValidator::validate_and_freeze(
        {root, executable, true, repository_root});
    check(!report.all_pass, label + " was not rejected");
    if (!expected_check.empty()) {
        check(!report.checks.empty() && report.checks.back().check_id == expected_check,
              label + " failed at an unexpected validator check");
    }
    check(!std::filesystem::exists(root / "validation_receipt.json"), label + " wrote a validation receipt");
    check(!std::filesystem::exists(root / "FROZEN"), label + " wrote a FROZEN marker");
}

}  // namespace

int main() {
    try {
        std::error_code error;
        const std::filesystem::path executable = std::filesystem::canonical("/proc/self/exe", error);
        check(!error, "cannot resolve test executable");
        const std::filesystem::path base =
            std::filesystem::temp_directory_path() /
            ("longlineage-regional-validator-" + std::to_string(static_cast<long long>(::getpid())));
        std::filesystem::remove_all(base);
        const std::filesystem::path v2_repository = create_v2_repository(base);

        const std::filesystem::path check_only = fixture(base, "check-only");
        const auto check_report =
            longlineage::validation::RegionalCompatValidator::validate_and_freeze({check_only, executable, false, {}});
        check(check_report.all_pass, "valid check-only bundle failed");
        check(!std::filesystem::exists(check_only / "validation_receipt.json") &&
                  !std::filesystem::exists(check_only / "FROZEN"),
              "check-only mode wrote outputs");

        const std::filesystem::path valid = fixture(base, "valid");
        const auto valid_report =
            longlineage::validation::RegionalCompatValidator::validate_and_freeze({valid, executable, true, {}});
        check(valid_report.all_pass && valid_report.validation_receipt_written && valid_report.frozen_marker_written,
              "valid bundle did not freeze");
        check(std::filesystem::is_regular_file(valid / "validation_receipt.json") &&
                  std::filesystem::is_regular_file(valid / "FROZEN"),
              "valid bundle lacks frozen outputs");

        const std::filesystem::path valid_v2 = fixture(base, "valid-v2", "2.0.0");
        const auto valid_v2_report = longlineage::validation::RegionalCompatValidator::validate_and_freeze(
            {valid_v2, executable, true, v2_repository});
        check(valid_v2_report.all_pass && valid_v2_report.schema_version == "2.0.0" &&
                  valid_v2_report.dataset_id == "COLO829" && valid_v2_report.dataset_order == 2U &&
                  valid_v2_report.source_authority_profile == "PRODUCTION_7_DATASET" &&
                  valid_v2_report.source_authority_sha256 == g_v2_authority_sha256,
              "valid v2 bundle lost its dataset/authority binding");
        const JsonPtr validation_receipt = load_json(valid_v2 / "validation_receipt.json");
        check(std::string(json_string_value(json_object_get(validation_receipt.get(), "dataset_id"))) == "COLO829" &&
                  json_integer_value(json_object_get(validation_receipt.get(), "dataset_order")) == 2 &&
                  std::string(json_string_value(
                      json_object_get(validation_receipt.get(), "source_authority_profile"))) == "PRODUCTION_7_DATASET",
              "v2 validation receipt lacks dataset binding");
        const json_t* validation_checks = json_object_get(validation_receipt.get(), "checks");
        check(json_is_array(validation_checks) && json_array_size(validation_checks) == 18U &&
                  std::string(json_string_value(json_object_get(json_array_get(validation_checks, 7U), "check_id"))) ==
                      "SOURCE_MANIFEST" &&
                  std::string(json_string_value(json_object_get(json_array_get(validation_checks, 16U), "check_id"))) ==
                      "SOURCE_MANIFEST_STABLE",
              "v2 validation receipt does not contain the ordered 18-check contract");
        const std::string frozen_v2 = read_text(valid_v2 / "FROZEN");
        check(frozen_v2.find("schema_version=2.0.0\n") != std::string::npos &&
                  frozen_v2.find("dataset_id=COLO829\n") != std::string::npos &&
                  frozen_v2.find("dataset_order=2\n") != std::string::npos &&
                  frozen_v2.find("source_authority_profile=PRODUCTION_7_DATASET\n") != std::string::npos &&
                  frozen_v2.find("source_authority_sha256=" + g_v2_authority_sha256 + "\n") != std::string::npos,
              "v2 FROZEN marker lacks dataset/authority binding");

        const std::filesystem::path valid_hcc_v2 = hcc_v2_fixture(base, "valid-hcc-v2");
        const auto valid_hcc_v2_report = longlineage::validation::RegionalCompatValidator::validate_and_freeze(
            {valid_hcc_v2, executable, false, v2_repository});
        check(valid_hcc_v2_report.all_pass && valid_hcc_v2_report.dataset_id == "HCC1395" &&
                  valid_hcc_v2_report.dataset_order == 0U &&
                  valid_hcc_v2_report.source_authority_profile == "HCC1395_DATASET_GATE" &&
                  valid_hcc_v2_report.source_authority_sha256 == g_hcc_authority_sha256,
              "valid v2 HCC dataset-gate manifest did not pass the independent 11-binding replay");

        const std::filesystem::path canonical_target = fixture(base, "v2-canonical-target-drift", "2.0.0");
        mutate_v2_summary(canonical_target, [](json_t* summary) {
            json_object_set_new(json_array_get(json_object_get(summary, "inputs"), 0U), "canonical_path",
                                json_string("/synthetic/canonical-target/COLO829.raw.bam"));
        });
        expect_rejected(canonical_target, executable, "v2 summary canonical path differs from source manifest",
                        "SOURCE_MANIFEST", v2_repository);

        const std::filesystem::path missing_manifest = fixture(base, "v2-missing-source-file", "2.0.0");
        mutate_v2_summary(missing_manifest, [&](json_t* summary) {
            const std::filesystem::path absent = base / "source-manifests" / "absent.json";
            json_object_set_new(summary, "source_manifest_path", json_string(absent.c_str()));
        });
        expect_rejected(missing_manifest, executable, "v2 missing source manifest file", "SOURCE_MANIFEST",
                        v2_repository);

        const std::filesystem::path extra_manifest_field = fixture(base, "v2-extra-manifest-field", "2.0.0");
        mutate_v2_source_manifest(extra_manifest_field,
                                  [](json_t* manifest) { json_object_set_new(manifest, "unexpected", json_true()); });
        expect_rejected(extra_manifest_field, executable, "v2 extra source manifest field", "SOURCE_MANIFEST",
                        v2_repository);

        const std::filesystem::path wrong_manifest_digest = fixture(base, "v2-wrong-manifest-digest", "2.0.0");
        mutate_v2_summary(wrong_manifest_digest, [](json_t* summary) {
            const std::string digest(64U, 'f');
            json_object_set_new(summary, "source_manifest_sha256", json_string(digest.c_str()));
        });
        expect_rejected(wrong_manifest_digest, executable, "v2 wrong source manifest digest", "SOURCE_MANIFEST",
                        v2_repository);

        const std::filesystem::path wrong_manifest_run = fixture(base, "v2-wrong-manifest-run", "2.0.0");
        mutate_v2_source_manifest(wrong_manifest_run, [](json_t* manifest) {
            json_object_set_new(manifest, "run_id", json_string("different-production-run"));
        });
        expect_rejected(wrong_manifest_run, executable, "v2 wrong source manifest run", "SOURCE_MANIFEST",
                        v2_repository);

        const std::filesystem::path wrong_manifest_order = fixture(base, "v2-wrong-manifest-order", "2.0.0");
        mutate_v2_source_manifest(wrong_manifest_order, [](json_t* manifest) {
            json_object_set_new(json_array_get(json_object_get(manifest, "datasets"), 1U), "dataset_order",
                                json_integer(2));
        });
        expect_rejected(wrong_manifest_order, executable, "v2 wrong source manifest dataset order", "SOURCE_MANIFEST",
                        v2_repository);

        const std::filesystem::path wrong_manifest_role = fixture(base, "v2-wrong-manifest-role", "2.0.0");
        mutate_v2_source_manifest(wrong_manifest_role, [](json_t* manifest) {
            json_t* files = json_object_get(json_array_get(json_object_get(manifest, "datasets"), 2U), "files");
            json_object_set_new(json_array_get(files, 1U), "role", json_string("raw_bam"));
        });
        expect_rejected(wrong_manifest_role, executable, "v2 wrong source manifest input role", "SOURCE_MANIFEST",
                        v2_repository);

        const std::filesystem::path wrong_manifest_path = fixture(base, "v2-wrong-manifest-path", "2.0.0");
        mutate_v2_source_manifest(wrong_manifest_path, [](json_t* manifest) {
            json_t* files = json_object_get(json_array_get(json_object_get(manifest, "datasets"), 2U), "files");
            json_object_set_new(json_array_get(files, 0U), "path", json_string("/synthetic/COLO829/different-raw-bam"));
        });
        expect_rejected(wrong_manifest_path, executable, "v2 source manifest input path differs from summary",
                        "SOURCE_MANIFEST", v2_repository);

        const std::filesystem::path wrong_manifest_contract = fixture(base, "v2-wrong-manifest-contract", "2.0.0");
        mutate_v2_source_manifest(wrong_manifest_contract, [](json_t* manifest) {
            const std::string digest(64U, 'f');
            json_object_set_new(json_object_get(manifest, "contract_bindings"), "science_parameters_sha256",
                                json_string(digest.c_str()));
        });
        expect_rejected(wrong_manifest_contract, executable, "v2 wrong source manifest contract digest",
                        "SOURCE_MANIFEST", v2_repository);

        const std::filesystem::path wrong_manifest_workers = fixture(base, "v2-wrong-manifest-workers", "2.0.0");
        mutate_v2_source_manifest(wrong_manifest_workers, [](json_t* manifest) {
            json_object_set_new(json_object_get(manifest, "runtime"), "compute_workers", json_integer(3));
        });
        expect_rejected(wrong_manifest_workers, executable, "v2 source manifest worker mismatch", "SOURCE_MANIFEST",
                        v2_repository);

        const std::filesystem::path wrong_manifest_output = fixture(base, "v2-wrong-manifest-output", "2.0.0");
        mutate_v2_source_manifest(wrong_manifest_output, [](json_t* manifest) {
            json_object_set_new(manifest, "output_root",
                                json_string("/synthetic/not-staging/synthetic-production-seven"));
        });
        expect_rejected(wrong_manifest_output, executable, "v2 source manifest output-root mismatch", "SOURCE_MANIFEST",
                        v2_repository);

        const std::filesystem::path symlink_manifest = fixture(base, "v2-symlink-manifest", "2.0.0");
        const std::filesystem::path symlink_target = source_manifest_path(symlink_manifest);
        const std::filesystem::path symlink_path = symlink_target.parent_path() / "source-manifest-alias.json";
        std::filesystem::create_symlink(symlink_target, symlink_path);
        mutate_v2_summary(symlink_manifest, [&](json_t* summary) {
            json_object_set_new(summary, "source_manifest_path", json_string(symlink_path.c_str()));
        });
        expect_rejected(symlink_manifest, executable, "v2 symlink source manifest", "SOURCE_MANIFEST", v2_repository);

        const std::filesystem::path missing_repo = fixture(base, "v2-missing-repo", "2.0.0");
        expect_rejected(missing_repo, executable, "v2 missing repository trust root", "SOURCE_AUTHORITY");

        const std::filesystem::path wrong_authority = fixture(base, "v2-wrong-authority", "2.0.0");
        mutate_v2_summary(wrong_authority, [](json_t* summary) {
            const std::string digest(64U, 'f');
            json_object_set_new(summary, "source_authority_sha256", json_string(digest.c_str()));
        });
        expect_rejected(wrong_authority, executable, "v2 wrong authority digest", "SOURCE_AUTHORITY", v2_repository);

        const std::filesystem::path authority_input_drift = fixture(base, "v2-authority-input-drift", "2.0.0");
        mutate_v2_summary(authority_input_drift, [](json_t* summary) {
            const std::string digest(64U, 'e');
            json_object_set_new(json_array_get(json_object_get(summary, "inputs"), 0U), "sha256",
                                json_string(digest.c_str()));
        });
        expect_rejected(authority_input_drift, executable, "v2 summary input not in authority", "SOURCE_AUTHORITY",
                        v2_repository);

        const std::filesystem::path census_drift = fixture(base, "v2-census-drift", "2.0.0");
        mutate_v2_summary(census_drift, [](json_t* summary) {
            json_object_set_new(json_object_get(summary, "census"), "unit_class_determined", json_integer(2));
        });
        expect_rejected(census_drift, executable, "v2 output census drift", "OUTPUT_CENSUS", v2_repository);

        const std::filesystem::path embedded_nul = fixture(base, "v2-embedded-nul", "2.0.0");
        mutate_v2_summary(embedded_nul, [](json_t* summary) {
            constexpr char kDatasetWithNul[] = {'C', 'O', 'L', 'O', '8', '2', '9', '\0', 'X'};
            json_object_set_new(summary, "dataset_id", json_stringn(kDatasetWithNul, sizeof(kDatasetWithNul)));
        });
        expect_rejected(embedded_nul, executable, "v2 embedded NUL", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path missing_dataset = fixture(base, "v2-missing-dataset", "2.0.0");
        mutate_v2_summary(missing_dataset, [](json_t* summary) { json_object_del(summary, "dataset_id"); });
        expect_rejected(missing_dataset, executable, "v2 missing dataset", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path missing_parameters = fixture(base, "v2-missing-parameters", "2.0.0");
        mutate_v2_summary(missing_parameters, [](json_t* summary) { json_object_del(summary, "parameters"); });
        expect_rejected(missing_parameters, executable, "v2 missing parameters", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path missing_inputs = fixture(base, "v2-missing-inputs", "2.0.0");
        mutate_v2_summary(missing_inputs, [](json_t* summary) { json_object_del(summary, "inputs"); });
        expect_rejected(missing_inputs, executable, "v2 missing inputs", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path missing_timing = fixture(base, "v2-missing-timing", "2.0.0");
        mutate_v2_summary(missing_timing, [](json_t* summary) { json_object_del(summary, "timing"); });
        expect_rejected(missing_timing, executable, "v2 missing timing", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path missing_source = fixture(base, "v2-missing-source-manifest", "2.0.0");
        mutate_v2_summary(missing_source, [](json_t* summary) { json_object_del(summary, "source_manifest_sha256"); });
        expect_rejected(missing_source, executable, "v2 missing source manifest digest", "SUMMARY_CONTRACT",
                        v2_repository);

        const std::filesystem::path duplicate_role = fixture(base, "v2-duplicate-input-role", "2.0.0");
        mutate_v2_summary(duplicate_role, [](json_t* summary) {
            json_t* inputs = json_object_get(summary, "inputs");
            json_object_set_new(json_array_get(inputs, 1U), "role", json_string("raw_bam"));
        });
        expect_rejected(duplicate_role, executable, "v2 duplicate input role", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path unknown_field = fixture(base, "v2-unknown-field", "2.0.0");
        mutate_v2_summary(unknown_field,
                          [](json_t* summary) { json_object_set_new(summary, "unexpected", json_integer(1)); });
        expect_rejected(unknown_field, executable, "v2 unknown summary field", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path wrong_dataset = fixture(base, "v2-wrong-dataset", "2.0.0");
        mutate_v2_summary(wrong_dataset,
                          [](json_t* summary) { json_object_set_new(summary, "dataset_id", json_string("H1437")); });
        expect_rejected(wrong_dataset, executable, "v2 dataset/order drift", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path wrong_parameter = fixture(base, "v2-wrong-parameter", "2.0.0");
        mutate_v2_summary(wrong_parameter, [](json_t* summary) {
            json_object_set_new(json_object_get(summary, "parameters"), "MINREAD", json_integer(2));
        });
        expect_rejected(wrong_parameter, executable, "v2 changed parameter", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path unverified_input = fixture(base, "v2-unverified-input", "2.0.0");
        mutate_v2_summary(unverified_input, [](json_t* summary) {
            json_object_set_new(json_array_get(json_object_get(summary, "inputs"), 0U), "full_sha256_verified",
                                json_false());
        });
        expect_rejected(unverified_input, executable, "v2 unverified physical SHA", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path uppercase_sha = fixture(base, "v2-uppercase-sha", "2.0.0");
        mutate_v2_summary(uppercase_sha, [](json_t* summary) {
            const std::string digest(64U, 'A');
            json_object_set_new(json_array_get(json_object_get(summary, "inputs"), 0U), "sha256",
                                json_string(digest.c_str()));
        });
        expect_rejected(uppercase_sha, executable, "v2 uppercase physical SHA", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path malformed_timing = fixture(base, "v2-malformed-timing", "2.0.0");
        mutate_v2_summary(malformed_timing, [](json_t* summary) {
            json_object_set_new(json_object_get(summary, "timing"), "science_wall_seconds", json_string("2.0"));
        });
        expect_rejected(malformed_timing, executable, "v2 string timing", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path zero_timing = fixture(base, "v2-zero-timing", "2.0.0");
        mutate_v2_summary(zero_timing, [](json_t* summary) {
            json_object_set_new(json_object_get(summary, "timing"), "science_wall_seconds", json_real(0.0));
        });
        expect_rejected(zero_timing, executable, "v2 zero timing", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path raised_claim = fixture(base, "v2-raised-claim", "2.0.0");
        mutate_v2_summary(raised_claim, [](json_t* summary) {
            json_object_set_new(json_object_get(summary, "claim_ceiling"), "formal_m2_topology", json_true());
        });
        expect_rejected(raised_claim, executable, "v2 raised claim ceiling", "SUMMARY_CONTRACT", v2_repository);

        const std::filesystem::path receipt_swap = fixture(base, "v2-receipt-swap", "2.0.0");
        JsonPtr swapped_receipt = load_json(receipt_swap / "producer_receipt.json");
        json_object_set_new(swapped_receipt.get(), "dataset_id", json_string("H1437"));
        write_json(receipt_swap / "producer_receipt.json", swapped_receipt.get());
        rewrite_checksum_manifest(receipt_swap);
        expect_rejected(receipt_swap, executable, "v2 producer receipt dataset swap", "PRODUCER_RECEIPT",
                        v2_repository);

        const std::filesystem::path corrupt = fixture(base, "corrupt");
        write_text(corrupt / "patterns.tsv", "corrupt\n");
        expect_rejected(corrupt, executable, "corrupt bundle");

        const std::filesystem::path missing = fixture(base, "missing");
        std::filesystem::remove(missing / "units.tsv");
        expect_rejected(missing, executable, "missing-file bundle");

        const std::filesystem::path extra = fixture(base, "extra");
        write_text(extra / "unexpected.txt", "unexpected\n");
        expect_rejected(extra, executable, "extra-file bundle");

        const std::filesystem::path truncated = fixture(base, "truncated");
        const std::string truncated_patterns =
            "region_order\tregion_id\tfamily\tpattern\tpattern_kind\tcount\n"
            "0\tregion-0\t1\tAA\tFULL\t3";
        write_text(truncated / "patterns.tsv", truncated_patterns);
        seal_producer(truncated);
        expect_rejected(truncated, executable, "truncated TSV bundle");

        const std::filesystem::path unordered = fixture(base, "unordered");
        write_text(unordered / "patterns.tsv",
                   "region_order\tregion_id\tfamily\tpattern\tpattern_kind\tcount\n"
                   "0\tregion-0\t1\tAA\tSUBREAD\t3\n"
                   "0\tregion-0\t1\tAA\tFULL\t3\n"
                   "0\tregion-0\tnone\tAX\tSUBREAD\t3\n"
                   "1\tregion-1\t2\tRR\tFULL\t3\n"
                   "1\tregion-1\t2\tRR\tSUBREAD\t3\n");
        seal_producer(unordered);
        expect_rejected(unordered, executable, "out-of-order bundle");

        const std::filesystem::path row_mismatch = fixture(base, "row-mismatch");
        write_summary_v1(row_mismatch, 3U, 3U, 5U);
        seal_producer(row_mismatch);
        expect_rejected(row_mismatch, executable, "row-count mismatch bundle");

        const std::filesystem::path partial = fixture(base, "partial-probe");
        JsonPtr partial_summary = load_json(partial / "summary.json");
        json_object_set_new(partial_summary.get(), "state", json_string("PARTIAL_PROBE"));
        write_json(partial / "summary.json", partial_summary.get());
        seal_producer(partial);
        expect_rejected(partial, executable, "partial-probe bundle");

        std::filesystem::remove_all(base);
        std::cout << "regional compat validator integration PASS\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "regional compat validator integration FAIL: " << failure.what() << '\n';
        return 1;
    }
}
