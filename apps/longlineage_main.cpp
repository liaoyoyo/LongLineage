// SPDX-License-Identifier: GPL-3.0-only

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli_support.hpp"
#include "longlineage/artifact/dataset_closeout.hpp"
#include "longlineage/artifact/run_root.hpp"
#include "longlineage/common/digest.hpp"
#include "longlineage/io/hts_preflight.hpp"
#include "longlineage/io/reference_reader.hpp"
#include "longlineage/io/variant_sites.hpp"
#include "longlineage/manifest/production_manifest.hpp"
#include "longlineage/pipeline/dataset_plan.hpp"
#include "longlineage/pipeline/dataset_producer.hpp"

#ifndef LONGLINEAGE_GIT_COMMIT
#define LONGLINEAGE_GIT_COMMIT "0000000000000000000000000000000000000000"
#endif

#ifndef LONGLINEAGE_COMPILER_ID
#define LONGLINEAGE_COMPILER_ID "unknown"
#endif

namespace {

using longlineage::cli::CheckResult;
using longlineage::cli::ExitCode;

void print_usage() {
    std::cout << "LongLineage " << LONGLINEAGE_VERSION << "\n"
              << "Usage:\n"
              << "  longlineage preflight --manifest FILE [--repo DIR]\n"
              << "  longlineage run       --manifest FILE [--repo DIR]\n"
              << "  longlineage dataset-gate --manifest FILE [--repo DIR]\n"
              << "  longlineage probe     --manifest FILE --partial-output DIR [--repo DIR]\n"
              << "\n"
              << "Production run is fail-closed until P3, P4 and P5 are VERIFIED.\n"
              << "HCC1395 dataset-gate writes READY_FOR_VALIDATION staging only;\n"
              << "longlineage-validate must independently validate and freeze it.\n"
              << "Probe outputs are always PARTIAL and cannot become validation evidence.\n";
}

struct Arguments {
    std::string command;
    std::map<std::string, std::string> values;
    bool help{false};
};

bool parse_arguments(int argc, char** argv, Arguments& parsed, std::string& error) {
    if (argc < 2) {
        error = "a subcommand is required";
        return false;
    }
    parsed.command = argv[1];
    if (parsed.command != "preflight" && parsed.command != "run" && parsed.command != "dataset-gate" &&
        parsed.command != "probe") {
        error = "unknown subcommand: " + parsed.command;
        return false;
    }

    for (int index = 2; index < argc; ++index) {
        const std::string token = argv[index];
        if (longlineage::cli::is_help_flag(token)) {
            parsed.help = true;
            continue;
        }
        if (token != "--manifest" && token != "--repo" && token != "--partial-output") {
            error = "unknown option: " + token;
            return false;
        }
        if (index + 1 >= argc) {
            error = "missing value for " + token;
            return false;
        }
        if (parsed.values.count(token) != 0U) {
            error = "duplicate option: " + token;
            return false;
        }
        parsed.values.emplace(token, argv[++index]);
    }

    if (parsed.help) {
        return true;
    }
    if (parsed.values.count("--manifest") == 0U) {
        error = "--manifest is required";
        return false;
    }
    if (parsed.command == "probe") {
        if (parsed.values.count("--partial-output") == 0U) {
            error = "probe requires --partial-output";
            return false;
        }
    } else if (parsed.values.count("--partial-output") != 0U) {
        error = "--partial-output is valid only for probe";
        return false;
    }
    return true;
}

const longlineage::LockedFile* find_role(const longlineage::DatasetManifest& dataset, longlineage::FileRole role) {
    for (const auto& file : dataset.files) {
        if (file.role == role) {
            return &file;
        }
    }
    return nullptr;
}

std::string authority_string(const json_t* object, const char* key) {
    const auto* value = json_object_get(object, key);
    return json_is_string(value) ? std::string(json_string_value(value)) : std::string{};
}

bool authority_uint64(const json_t* object, const char* key, std::uint64_t& output) {
    const auto* value = json_object_get(object, key);
    if (!json_is_integer(value) || json_integer_value(value) < 0) {
        return false;
    }
    output = static_cast<std::uint64_t>(json_integer_value(value));
    return true;
}

CheckResult verify_production_authority(const longlineage::ProductionManifest& manifest,
                                        const std::filesystem::path& repo_root) {
    if (manifest.authority_profile != longlineage::AuthorityProfile::kProductionSevenDataset) {
        return {false, "run requires authority_profile=PRODUCTION_7_DATASET"};
    }

    const auto authority_path = repo_root / "oracle" / "production_input_authority.json";
    const auto authority_digest = longlineage::sha256_file(authority_path);
    if (!authority_digest.ok()) {
        return {false, authority_digest.detail};
    }
    if (*authority_digest.value != manifest.contract_bindings.production_input_authority_sha256) {
        return {false, "production input authority contract binding SHA-256 mismatch"};
    }

    std::string error;
    const auto authority = longlineage::cli::load_json_strict(authority_path, error);
    if (!authority) {
        return {false, error};
    }
    if (authority_string(authority.get(), "schema_name") != "longlineage.production_input_authority" ||
        authority_string(authority.get(), "schema_version") != "1.0.0" ||
        authority_string(authority.get(), "profile_id") != "RAW_ALL_PRODUCTION_V2_7_DATASET") {
        return {false, "production input authority identity is invalid"};
    }

    const auto* expected_order = json_object_get(authority.get(), "dataset_order");
    const auto* expected_datasets = json_object_get(authority.get(), "datasets");
    if (!json_is_array(expected_order) || !json_is_array(expected_datasets) || json_array_size(expected_order) != 7U ||
        json_array_size(expected_datasets) != 7U || manifest.datasets.size() != 7U) {
        return {false, "production profile requires the exact seven-dataset authority set"};
    }

    const auto* closeout = json_object_get(authority.get(), "source_closeout");
    std::uint64_t closeout_dataset_count = 0;
    if (!json_is_object(closeout) || !authority_uint64(closeout, "dataset_count", closeout_dataset_count) ||
        closeout_dataset_count != 7U || !json_is_true(json_object_get(closeout, "all_pass"))) {
        return {false, "production authority closeout is not a seven-dataset PASS"};
    }
    const auto* constraints = json_object_get(authority.get(), "constraints");
    if (!json_is_object(constraints) ||
        authority_string(constraints, "latest_tag_join") != "EXACT_PROJECTION_NO_FALLBACK" ||
        !json_is_false(json_object_get(constraints, "tagged_bam_output_allowed")) ||
        !json_is_true(json_object_get(constraints, "production_requires_exact_dataset_set")) ||
        !json_is_false(json_object_get(constraints, "private_source_paths_stored"))) {
        return {false, "production authority constraints are invalid"};
    }

    const std::vector<std::pair<longlineage::FileRole, std::string>> authority_roles = {
        {longlineage::FileRole::kLatestHpPsSidecar, "latest_hp_ps_sidecar"},
        {longlineage::FileRole::kLatestHpPsSidecarIndex, "latest_hp_ps_sidecar_index"},
        {longlineage::FileRole::kPassBiallelicSsnvVcf, "pass_biallelic_ssnv_vcf"},
        {longlineage::FileRole::kPassBiallelicSsnvVcfIndex, "pass_biallelic_ssnv_vcf_index"},
    };
    for (std::size_t index = 0; index < manifest.datasets.size(); ++index) {
        const auto* expected_id = json_array_get(expected_order, index);
        const auto* expected_dataset = json_array_get(expected_datasets, index);
        if (!json_is_string(expected_id) || !json_is_object(expected_dataset)) {
            return {false, "production authority dataset row is malformed"};
        }
        const auto& observed_dataset = manifest.datasets[index];
        std::uint64_t expected_dataset_order = 0;
        if (observed_dataset.dataset_id != json_string_value(expected_id) ||
            authority_string(expected_dataset, "dataset_id") != observed_dataset.dataset_id ||
            !authority_uint64(expected_dataset, "dataset_order", expected_dataset_order) ||
            expected_dataset_order != index || observed_dataset.dataset_order != index) {
            return {false,
                    "production dataset identity/order differs from authority at index " + std::to_string(index)};
        }

        for (const auto& [role, authority_key] : authority_roles) {
            const auto* observed = find_role(observed_dataset, role);
            const auto* expected = json_object_get(expected_dataset, authority_key.c_str());
            std::uint64_t expected_size = 0;
            if (observed == nullptr || !json_is_object(expected) ||
                !authority_uint64(expected, "size_bytes", expected_size) || expected_size != observed->size_bytes ||
                authority_string(expected, "sha256") != observed->sha256) {
                return {false, observed_dataset.dataset_id + ": authority mismatch for " + authority_key};
            }
        }
    }
    return {true, "production profile matches the exact seven-dataset input authority"};
}

CheckResult verify_hcc1395_dataset_gate_authority(const longlineage::ProductionManifest& manifest,
                                                  const std::filesystem::path& repo_root) {
    if (manifest.authority_profile != longlineage::AuthorityProfile::kHcc1395DatasetGate) {
        return {false, "dataset-gate requires authority_profile=HCC1395_DATASET_GATE"};
    }
    if (manifest.datasets.size() != 1U || manifest.datasets.front().dataset_id != "HCC1395" ||
        manifest.datasets.front().dataset_order != 0U) {
        return {false, "HCC1395 dataset gate requires exactly dataset HCC1395 at order 0"};
    }

    const auto authority_path = repo_root / "oracle" / "hcc1395_dataset_gate_input_authority.json";
    const auto authority_digest = longlineage::sha256_file(authority_path);
    if (!authority_digest.ok()) {
        return {false, authority_digest.detail};
    }
    if (manifest.contract_bindings.dataset_gate_input_authority_sha256 != *authority_digest.value) {
        return {false, "HCC1395 dataset-gate input authority SHA-256 mismatch"};
    }

    std::string error;
    const auto authority = longlineage::cli::load_json_strict(authority_path, error);
    if (!authority) {
        return {false, error};
    }
    std::uint64_t dataset_order = 0;
    std::uint64_t forbidden_evaluation_fields = 0;
    if (authority_string(authority.get(), "schema_name") != "longlineage.dataset_gate_input_authority" ||
        authority_string(authority.get(), "schema_version") != "1.0.0" ||
        authority_string(authority.get(), "authority_id") !=
            "HCC1395_RAW_BAM_LPS_SIDECAR_V2_PASS_SSNV_GRCH38_20260720" ||
        authority_string(authority.get(), "authority_profile") != "HCC1395_DATASET_GATE" ||
        authority_string(authority.get(), "allowed_terminal_state") != "VALIDATED_FROZEN_DATASET_GATE" ||
        authority_string(authority.get(), "dataset_id") != "HCC1395" ||
        !authority_uint64(authority.get(), "dataset_order", dataset_order) || dataset_order != 0U ||
        !authority_uint64(authority.get(),
                          "tr"
                          "uth_fields",
                          forbidden_evaluation_fields) ||
        forbidden_evaluation_fields != 0U ||
        !json_is_false(json_object_get(authority.get(), "private_source_paths_stored")) ||
        !json_is_false(json_object_get(authority.get(), "tagged_bam_persisted")) ||
        authority_string(authority.get(), "latest_tag_join") != "EXACT_PROJECTION_NO_FALLBACK") {
        return {false, "HCC1395 dataset-gate authority identity/boundary is invalid"};
    }
    const auto* claim = json_object_get(authority.get(), "claim");
    if (!json_is_object(claim) || !json_is_true(json_object_get(claim, "dataset_gate_allowed")) ||
        !json_is_false(json_object_get(claim, "production_seven_dataset_claim_allowed")) ||
        !json_is_false(json_object_get(claim, "cross_dataset_generalization_allowed"))) {
        return {false, "HCC1395 dataset-gate claim ceiling is invalid"};
    }
    const auto* scope = json_object_get(authority.get(), "variant_scope");
    std::uint64_t all_pass = 0;
    std::uint64_t autosomal = 0;
    std::uint64_t outside = 0;
    if (!json_is_object(scope) || !authority_uint64(scope, "all_pass_biallelic_ssnv", all_pass) ||
        !authority_uint64(scope, "autosomal_chr1_to_chr22", autosomal) ||
        !authority_uint64(scope, "outside_autosomal_scope", outside) || all_pass != 113061U || autosomal != 79687U ||
        outside != 33374U || autosomal + outside != all_pass) {
        return {false, "HCC1395 frozen variant census is invalid"};
    }

    const auto* files = json_object_get(authority.get(), "files");
    if (!json_is_array(files) || json_array_size(files) != 8U) {
        return {false, "HCC1395 dataset-gate authority must contain exactly eight files"};
    }
    std::set<std::string> observed_roles;
    for (std::size_t index = 0; index < json_array_size(files); ++index) {
        const auto* expected = json_array_get(files, index);
        if (!json_is_object(expected)) {
            return {false, "HCC1395 authority file row is malformed"};
        }
        const std::string role_text = authority_string(expected, "role");
        const auto role = longlineage::parse_file_role(role_text);
        std::uint64_t expected_size = 0;
        if (!role.ok() || !observed_roles.insert(role_text).second ||
            !authority_uint64(expected, "size_bytes", expected_size)) {
            return {false, "HCC1395 authority file role is unknown, duplicated or malformed"};
        }
        const auto* observed = find_role(manifest.datasets.front(), *role.value);
        if (observed == nullptr || observed->size_bytes != expected_size ||
            observed->sha256 != authority_string(expected, "sha256")) {
            return {false, "HCC1395 authority mismatch for role " + role_text};
        }
    }
    if (observed_roles.size() != 8U) {
        return {false, "HCC1395 authority does not bind every required file role"};
    }
    return {true, "HCC1395 raw BAM + exact latest HP/PS sidecar dataset-gate authority passed"};
}

CheckResult verify_contract_binding(const std::filesystem::path& root, const std::filesystem::path& relative,
                                    const std::string& expected) {
    const auto digest = longlineage::sha256_file(root / relative);
    if (!digest.ok()) {
        return {false, digest.detail};
    }
    if (*digest.value != expected) {
        return {false, "contract binding SHA-256 mismatch: " + relative.string()};
    }
    return {true, "contract binding passed"};
}

void append_input_snapshot_row(std::ostringstream& snapshot, const longlineage::DatasetManifest& dataset,
                               const longlineage::LockedFile& file, const std::filesystem::path& canonical_path,
                               std::uint64_t device, std::uint64_t inode, std::uint64_t size_bytes,
                               std::int64_t mtime_seconds, std::int64_t mtime_nanoseconds, std::int64_t ctime_seconds,
                               std::int64_t ctime_nanoseconds) {
    snapshot << dataset.dataset_order << '\t' << dataset.dataset_id << '\t' << longlineage::to_string(file.role) << '\t'
             << canonical_path.string() << '\t' << device << '\t' << inode << '\t' << size_bytes << '\t'
             << mtime_seconds << '\t' << mtime_nanoseconds << '\t' << ctime_seconds << '\t' << ctime_nanoseconds << '\t'
             << file.sha256 << '\n';
}

[[nodiscard]] CheckResult canonical_input_state(const longlineage::ProductionManifest& manifest,
                                                std::string& snapshot_sha256, std::string& input_lock_sha256);

CheckResult check_manifest_and_inputs(const longlineage::ProductionManifest& manifest,
                                      const std::filesystem::path& repo_root, std::string& verified_snapshot_sha256) {
    const auto version = longlineage::require_htslib_version("1.18");
    if (!version.ok()) {
        return {false, version.detail};
    }

    const std::vector<std::pair<std::filesystem::path, std::string>> bindings = {
        {"contracts/v1/science_parameters.json", manifest.contract_bindings.science_parameters_sha256},
        {"schema/catalog.json", manifest.contract_bindings.schema_catalog_sha256},
        {"contracts/v1/status_reason_codes.tsv", manifest.contract_bindings.status_reason_registry_sha256},
        {"contracts/v1/type_registry.tsv", manifest.contract_bindings.type_registry_sha256},
        {"contracts/v1/transform_registry.tsv", manifest.contract_bindings.transform_registry_sha256},
        {"oracle/authority_manifest.json", manifest.contract_bindings.authority_manifest_sha256},
        {"provenance/source_to_target_manifest.json", manifest.contract_bindings.source_to_target_manifest_sha256},
        {"oracle/production_input_authority.json", manifest.contract_bindings.production_input_authority_sha256},
        {"schema/id_registry.json", manifest.contract_bindings.schema_id_registry_sha256},
        {"state/release_attestation.json", manifest.contract_bindings.release_attestation_sha256},
    };
    for (const auto& [relative, expected] : bindings) {
        const auto checked = verify_contract_binding(repo_root, relative, expected);
        if (!checked.ok) {
            return checked;
        }
    }

    if (manifest.authority_profile == longlineage::AuthorityProfile::kProductionSevenDataset) {
        const auto authority = verify_production_authority(manifest, repo_root);
        if (!authority.ok) {
            return authority;
        }
    } else if (manifest.authority_profile == longlineage::AuthorityProfile::kHcc1395DatasetGate) {
        const auto authority = verify_hcc1395_dataset_gate_authority(manifest, repo_root);
        if (!authority.ok) {
            return authority;
        }
    }

    std::ostringstream verified_snapshot;
    verified_snapshot << "longlineage.input_snapshot\t1.1.0\n";
    for (const auto& dataset : manifest.datasets) {
        for (const auto& file : dataset.files) {
            const auto locked = longlineage::verify_locked_file(file);
            if (!locked.ok() || !locked.value.has_value()) {
                return {false, dataset.dataset_id + ": " + locked.detail};
            }
            append_input_snapshot_row(verified_snapshot, dataset, file, locked.value->canonical_path,
                                      locked.value->device, locked.value->inode, locked.value->observed_size_bytes,
                                      locked.value->mtime_seconds, locked.value->mtime_nanoseconds,
                                      locked.value->ctime_seconds, locked.value->ctime_nanoseconds);
        }

        const auto* bam = find_role(dataset, longlineage::FileRole::kRawBam);
        const auto* bam_index = find_role(dataset, longlineage::FileRole::kRawBamIndex);
        const auto* vcf = find_role(dataset, longlineage::FileRole::kPassBiallelicSsnvVcf);
        const auto* vcf_index = find_role(dataset, longlineage::FileRole::kPassBiallelicSsnvVcfIndex);
        const auto* sidecar = find_role(dataset, longlineage::FileRole::kLatestHpPsSidecar);
        const auto* sidecar_index = find_role(dataset, longlineage::FileRole::kLatestHpPsSidecarIndex);
        const auto* reference = find_role(dataset, longlineage::FileRole::kReferenceFasta);
        const auto* reference_index = find_role(dataset, longlineage::FileRole::kReferenceFai);
        if (bam == nullptr || bam_index == nullptr || vcf == nullptr || vcf_index == nullptr || sidecar == nullptr ||
            sidecar_index == nullptr || reference == nullptr || reference_index == nullptr) {
            return {false, dataset.dataset_id + ": required role binding is missing"};
        }

        const auto bam_check = longlineage::preflight_bam(bam->path.string(), bam_index->path.string());
        if (!bam_check.ok() || bam_check.empty()) {
            return {false, dataset.dataset_id + ": BAM preflight failed or observed zero records: " + bam_check.detail};
        }
        const auto vcf_check =
            longlineage::preflight_pass_biallelic_ssnv_vcf(vcf->path.string(), vcf_index->path.string());
        if (!vcf_check.ok() || vcf_check.empty()) {
            return {false, dataset.dataset_id + ": VCF preflight failed or observed zero records: " + vcf_check.detail};
        }
        const auto sidecar_check =
            longlineage::preflight_latest_hp_ps_sidecar(sidecar->path.string(), sidecar_index->path.string());
        if (!sidecar_check.ok() || sidecar_check.empty()) {
            return {false, dataset.dataset_id +
                               ": sidecar preflight failed or observed zero records: " + sidecar_check.detail};
        }
        const auto reference_check =
            longlineage::preflight_reference_fasta(reference->path.string(), reference_index->path.string());
        if (!reference_check.ok() || reference_check.empty()) {
            return {false, dataset.dataset_id + ": reference preflight failed or observed zero indexed bases: " +
                               reference_check.detail};
        }
    }
    const auto verified_digest = longlineage::sha256_hex(verified_snapshot.str());
    if (!verified_digest.ok()) {
        return {false, "cannot digest hash-verified input identities"};
    }
    std::string current_snapshot_sha256;
    std::string current_input_lock_sha256;
    const auto current = canonical_input_state(manifest, current_snapshot_sha256, current_input_lock_sha256);
    if (!current.ok || current_snapshot_sha256 != *verified_digest.value) {
        return {
            false,
            current.ok ? "input identity changed after content hash verification" : current.message,
        };
    }
    verified_snapshot_sha256 = *verified_digest.value;
    return {true, "manifest, profile-specific contract bindings, all input locks and indexed HTS probes passed"};
}

struct ProcessIo {
    std::uint64_t read_bytes{0};
    std::uint64_t write_bytes{0};
};

[[nodiscard]] ProcessIo process_io() {
    ProcessIo output;
    std::ifstream input("/proc/self/io");
    std::string key;
    std::uint64_t value = 0;
    while (input >> key >> value) {
        if (key == "read_bytes:") {
            output.read_bytes = value;
        } else if (key == "write_bytes:") {
            output.write_bytes = value;
        }
    }
    return output;
}

[[nodiscard]] double timeval_seconds(const timeval& value) noexcept {
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1000000.0;
}

[[nodiscard]] std::string rfc3339_utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (now == static_cast<std::time_t>(-1) || gmtime_r(&now, &utc) == nullptr) {
        throw std::runtime_error("cannot observe UTC completion time");
    }
    char encoded[32]{};
    if (std::strftime(encoded, sizeof(encoded), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0U) {
        throw std::runtime_error("cannot encode UTC completion time");
    }
    return encoded;
}

[[nodiscard]] std::string local_hostname() {
    char name[256]{};
    if (::gethostname(name, sizeof(name)) != 0) {
        throw std::runtime_error("cannot observe producer hostname");
    }
    name[sizeof(name) - 1U] = '\0';
    return name;
}

[[nodiscard]] std::string kernel_release() {
    struct utsname identity {};
    if (::uname(&identity) != 0) {
        throw std::runtime_error("cannot observe producer kernel release");
    }
    return identity.release;
}

[[nodiscard]] CheckResult canonical_input_state(const longlineage::ProductionManifest& manifest,
                                                std::string& snapshot_sha256, std::string& input_lock_sha256) {
    std::ostringstream snapshot;
    std::ostringstream locks;
    snapshot << "longlineage.input_snapshot\t1.1.0\n";
    locks << "longlineage.input_lock\t1.0.0\n";
    for (const auto& dataset : manifest.datasets) {
        for (const auto& file : dataset.files) {
            std::error_code error;
            const std::filesystem::path canonical = std::filesystem::canonical(file.path, error);
            if (error) {
                return {false, "cannot canonicalize frozen input: " + file.path.string()};
            }
            struct stat status {};
            if (::stat(canonical.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
                return {false, "cannot stat frozen regular input: " + canonical.string()};
            }
            locks << dataset.dataset_order << '\t' << dataset.dataset_id << '\t' << longlineage::to_string(file.role)
                  << '\t' << canonical.string() << '\t' << file.size_bytes << '\t' << file.sha256 << '\n';
            append_input_snapshot_row(
                snapshot, dataset, file, canonical, static_cast<std::uint64_t>(status.st_dev),
                static_cast<std::uint64_t>(status.st_ino), static_cast<std::uint64_t>(status.st_size),
                static_cast<std::int64_t>(status.st_mtim.tv_sec), static_cast<std::int64_t>(status.st_mtim.tv_nsec),
                static_cast<std::int64_t>(status.st_ctim.tv_sec), static_cast<std::int64_t>(status.st_ctim.tv_nsec));
        }
    }
    const auto snapshot_digest = longlineage::sha256_hex(snapshot.str());
    const auto lock_digest = longlineage::sha256_hex(locks.str());
    if (!snapshot_digest.ok() || !lock_digest.ok()) {
        return {false, "cannot digest frozen input state"};
    }
    snapshot_sha256 = *snapshot_digest.value;
    input_lock_sha256 = *lock_digest.value;
    return {true, "frozen input snapshot and lock digest recorded"};
}

[[nodiscard]] std::string decode_mount_field(std::string value) {
    const std::array<std::pair<std::string_view, char>, 4> escapes{{
        {"\\040", ' '},
        {"\\011", '\t'},
        {"\\012", '\n'},
        {"\\134", '\\'},
    }};
    for (const auto& [encoded, decoded] : escapes) {
        std::size_t offset = 0;
        while ((offset = value.find(encoded, offset)) != std::string::npos) {
            value.replace(offset, encoded.size(), 1U, decoded);
            ++offset;
        }
    }
    return value;
}

struct MountInfo {
    std::filesystem::path mount_point;
    std::string source;
    std::string filesystem;
    std::string options;
    bool readonly{false};
};

[[nodiscard]] bool path_beneath(const std::filesystem::path& path, const std::filesystem::path& parent) {
    const auto relative = path.lexically_relative(parent);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

[[nodiscard]] CheckResult mount_for_path(const std::filesystem::path& path, MountInfo& selected) {
    std::ifstream input("/proc/self/mountinfo");
    if (!input) {
        return {false, "cannot read /proc/self/mountinfo"};
    }
    std::string line;
    std::size_t selected_length = 0;
    while (std::getline(input, line)) {
        std::istringstream parser(line);
        std::vector<std::string> fields;
        std::string field;
        while (parser >> field) {
            fields.push_back(field);
        }
        const auto separator = std::find(fields.begin(), fields.end(), "-");
        if (separator == fields.end() || std::distance(fields.begin(), separator) < 6 ||
            std::distance(separator, fields.end()) < 4) {
            continue;
        }
        const std::filesystem::path mount_point(decode_mount_field(fields[4]));
        if (!(path == mount_point || path_beneath(path, mount_point))) {
            continue;
        }
        const std::size_t length = mount_point.string().size();
        if (length < selected_length) {
            continue;
        }
        const std::size_t separator_index = static_cast<std::size_t>(std::distance(fields.begin(), separator));
        const std::string combined_options = fields[5] + "|" + fields.at(separator_index + 3U);
        const auto has_ro = [](std::string_view options) {
            return options == "ro" || options.rfind("ro,", 0) == 0U || options.find(",ro,") != std::string_view::npos ||
                   (options.size() > 3U && options.substr(options.size() - 3U) == ",ro");
        };
        selected = MountInfo{
            mount_point,
            decode_mount_field(fields.at(separator_index + 2U)),
            fields.at(separator_index + 1U),
            combined_options,
            has_ro(fields[5]) || has_ro(fields.at(separator_index + 3U)),
        };
        selected_length = length;
    }
    if (selected_length == 0U) {
        return {false, "cannot resolve mount identity for " + path.string()};
    }
    return {true, "mount identity resolved"};
}

[[nodiscard]] CheckResult build_mount_identity(const longlineage::ProductionManifest& manifest,
                                               std::vector<longlineage::artifact::InputMountIdentity>& output) {
    for (const auto& dataset : manifest.datasets) {
        for (const auto& file : dataset.files) {
            std::error_code error;
            const auto canonical = std::filesystem::canonical(file.path, error);
            if (error) {
                return {false, "cannot canonicalize input mount path: " + file.path.string()};
            }
            MountInfo mount;
            const auto resolved = mount_for_path(canonical, mount);
            if (!resolved.ok) {
                return resolved;
            }
            const auto options_sha = longlineage::sha256_hex(mount.options);
            if (!options_sha.ok()) {
                return {false, "cannot digest input mount options"};
            }
            output.push_back({dataset.dataset_id, std::string(longlineage::to_string(file.role)), canonical,
                              mount.source, mount.filesystem, mount.readonly, *options_sha.value});
        }
    }
    return {true, "input mount identities recorded"};
}

[[nodiscard]] std::vector<longlineage::artifact::ArtifactInputBinding> manifest_input_bindings(
    const longlineage::ProductionManifest& manifest, const std::string& manifest_sha256) {
    std::vector<longlineage::artifact::ArtifactInputBinding> output;
    for (const auto& dataset : manifest.datasets) {
        for (const auto& file : dataset.files) {
            output.push_back({"MANIFEST_INPUT",
                              dataset.dataset_id + ":" + std::string(longlineage::to_string(file.role)),
                              "PHYSICAL_SHA256", file.sha256});
        }
    }
    output.push_back({"MANIFEST_INPUT", "run_manifest", "PHYSICAL_SHA256", manifest_sha256});
    output.push_back({"CONTRACT", "hcc1395_dataset_gate_input_authority", "PHYSICAL_SHA256",
                      manifest.contract_bindings.dataset_gate_input_authority_sha256});
    return output;
}

[[nodiscard]] std::uint64_t nonnegative_delta(std::uint64_t after, std::uint64_t before) noexcept {
    return after >= before ? after - before : 0U;
}

int execute_hcc1395_dataset_gate(const longlineage::ProductionManifest& manifest, const std::string& manifest_sha256,
                                 const std::filesystem::path& repo_root, const std::string& verified_snapshot_sha256) {
    using longlineage::cli::ExitCode;
    if (std::string_view(LONGLINEAGE_GIT_COMMIT) == "0000000000000000000000000000000000000000") {
        longlineage::cli::emit_error("dataset-gate", ExitCode::PreflightRejected,
                                     "dataset-gate requires a Git-resolved build commit");
        return static_cast<int>(ExitCode::PreflightRejected);
    }
    const auto& dataset = manifest.datasets.front();
    const auto* bam = find_role(dataset, longlineage::FileRole::kRawBam);
    const auto* bam_index = find_role(dataset, longlineage::FileRole::kRawBamIndex);
    const auto* vcf = find_role(dataset, longlineage::FileRole::kPassBiallelicSsnvVcf);
    const auto* vcf_index = find_role(dataset, longlineage::FileRole::kPassBiallelicSsnvVcfIndex);
    const auto* sidecar = find_role(dataset, longlineage::FileRole::kLatestHpPsSidecar);
    const auto* sidecar_index = find_role(dataset, longlineage::FileRole::kLatestHpPsSidecarIndex);
    const auto* reference = find_role(dataset, longlineage::FileRole::kReferenceFasta);
    const auto* reference_index = find_role(dataset, longlineage::FileRole::kReferenceFai);
    if (bam == nullptr || bam_index == nullptr || vcf == nullptr || vcf_index == nullptr || sidecar == nullptr ||
        sidecar_index == nullptr || reference == nullptr || reference_index == nullptr) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::PreflightRejected,
                                     "HCC1395 dataset gate lacks one of eight required roles");
        return static_cast<int>(ExitCode::PreflightRejected);
    }

    std::string snapshot_before;
    std::string input_lock_sha256;
    const auto before = canonical_input_state(manifest, snapshot_before, input_lock_sha256);
    if (!before.ok || snapshot_before != verified_snapshot_sha256) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::PreflightRejected,
                                     before.ok ? "input identity changed between content hash "
                                                 "verification and science startup"
                                               : before.message);
        return static_cast<int>(ExitCode::PreflightRejected);
    }

    const std::filesystem::path output_base = manifest.output_root.parent_path().parent_path();
    auto created = longlineage::artifact::RunRootTransaction::create_exclusive(output_base, manifest.run_id);
    if (!created.result.ok() || !created.transaction ||
        created.transaction->staging_root().lexically_normal() != manifest.output_root.lexically_normal()) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::IoError,
                                     "cannot create exact exclusive staging root: " + created.result.message);
        return static_cast<int>(ExitCode::IoError);
    }

    struct rusage usage_before {};
    if (::getrusage(RUSAGE_SELF, &usage_before) != 0) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::IoError,
                                     "cannot observe producer resource usage before science");
        return static_cast<int>(ExitCode::IoError);
    }
    const ProcessIo io_before = process_io();

    auto reference_reader = longlineage::IndexedReferenceReader::open(reference->path, reference_index->path);
    if (!reference_reader.ok() || !reference_reader.value.has_value()) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::PreflightRejected, reference_reader.detail);
        return static_cast<int>(ExitCode::PreflightRejected);
    }
    auto variants = longlineage::load_variant_sites(vcf->path, vcf_index->path, **reference_reader.value,
                                                    dataset.dataset_order, dataset.dataset_id);
    if (!variants.ok() || !variants.value.has_value() || variants.value->census.vcf_records != 113061U ||
        variants.value->census.pass_biallelic_ssnv != 113061U || variants.value->census.selected_scope != 79687U ||
        variants.value->census.excluded_outside_scope != 33374U) {
        longlineage::cli::emit_error(
            "dataset-gate", ExitCode::PreflightRejected,
            variants.ok() ? "HCC1395 VCF census differs from frozen authority" : variants.detail);
        return static_cast<int>(ExitCode::PreflightRejected);
    }

    longlineage::pipeline::DatasetPlanOptions plan_options;
    plan_options.max_focal_sites_per_block = manifest.runtime.max_focal_sites_per_block;
    plan_options.max_estimated_alignments_per_block = manifest.runtime.max_estimated_alignments_per_block;
    plan_options.max_focal_span_bp = 250000U;
    plan_options.halo_bp = manifest.runtime.halo_bp;
    plan_options.planning_units_per_site = 1U;
    auto plan = longlineage::pipeline::plan_dataset_execution(*variants.value, plan_options);
    if (!plan.ok() || !plan.value.has_value()) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::InternalError, plan.detail);
        return static_cast<int>(ExitCode::InternalError);
    }

    const longlineage::pipeline::DatasetReadPaths read_paths{
        bam->path, bam_index->path, sidecar->path, sidecar_index->path, reference->path, reference_index->path,
    };
    longlineage::pipeline::DatasetProductionOptions production_options;
    production_options.workers = manifest.runtime.compute_workers;
    production_options.task_queue_capacity_bytes = 16U * 1024U * 1024U;
    production_options.reorder_capacity_bytes = static_cast<std::size_t>(manifest.runtime.buffer_bytes);
    production_options.maximum_block_payload_bytes =
        std::min<std::size_t>(production_options.reorder_capacity_bytes, 512ULL * 1024ULL * 1024ULL);
    production_options.bgzf_compression_threads = static_cast<int>(manifest.runtime.writer_threads);
    production_options.representation = longlineage::pipeline::M1Representation::kHistoricalObservedRound6NullRound4;
    production_options.task_type = "B";
    production_options.completeness = "FULL";

    auto produced = longlineage::pipeline::produce_dataset_scientific_artifacts(
        read_paths, *variants.value, *plan.value, created.transaction->staging_root(), manifest.run_id,
        production_options);
    if (!produced.ok() || !produced.value.has_value()) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::InternalError, produced.detail);
        return static_cast<int>(ExitCode::InternalError);
    }

    std::string snapshot_after;
    std::string input_lock_after;
    const auto after = canonical_input_state(manifest, snapshot_after, input_lock_after);
    if (!after.ok || snapshot_before != snapshot_after || input_lock_sha256 != input_lock_after) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::PreflightRejected,
                                     after.ok ? "frozen input identity changed during science" : after.message);
        return static_cast<int>(ExitCode::PreflightRejected);
    }

    std::vector<longlineage::artifact::InputMountIdentity> mounts;
    const auto mount_result = build_mount_identity(manifest, mounts);
    if (!mount_result.ok) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::IoError, mount_result.message);
        return static_cast<int>(ExitCode::IoError);
    }
    std::error_code executable_path_error;
    const auto executable_path = std::filesystem::canonical("/proc/self/exe", executable_path_error);
    if (executable_path_error) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::IoError,
                                     "cannot canonicalize the running executable identity");
        return static_cast<int>(ExitCode::IoError);
    }
    const auto executable_sha256 = longlineage::sha256_file(executable_path);
    const auto phase_ledger_sha256 = longlineage::sha256_file(repo_root / "state/phase_ledger.json");
    if (manifest_sha256.size() != 64U || !executable_sha256.ok() || !phase_ledger_sha256.ok()) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::IoError,
                                     "cannot freeze manifest/executable/phase-ledger identity");
        return static_cast<int>(ExitCode::IoError);
    }

    struct rusage usage_after {};
    if (::getrusage(RUSAGE_SELF, &usage_after) != 0) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::IoError,
                                     "cannot observe producer resource usage after science");
        return static_cast<int>(ExitCode::IoError);
    }
    const ProcessIo io_after = process_io();

    longlineage::artifact::ProducerPerformance performance;
    performance.wall_seconds = produced.value->timing.total_seconds;
    performance.user_seconds = timeval_seconds(usage_after.ru_utime) - timeval_seconds(usage_before.ru_utime);
    performance.system_seconds = timeval_seconds(usage_after.ru_stime) - timeval_seconds(usage_before.ru_stime);
    performance.memory_peak_bytes = static_cast<std::uint64_t>(usage_after.ru_maxrss) * 1024U;
    performance.io_read_bytes = nonnegative_delta(io_after.read_bytes, io_before.read_bytes);
    performance.io_write_bytes = nonnegative_delta(io_after.write_bytes, io_before.write_bytes);
    performance.major_page_faults = nonnegative_delta(static_cast<std::uint64_t>(usage_after.ru_majflt),
                                                      static_cast<std::uint64_t>(usage_before.ru_majflt));
    performance.minor_page_faults = nonnegative_delta(static_cast<std::uint64_t>(usage_after.ru_minflt),
                                                      static_cast<std::uint64_t>(usage_before.ru_minflt));
    performance.peak_threads = produced.value->peak_process_threads;
    performance.queue_wait_seconds = produced.value->timing.queue_wait_seconds;
    performance.reorder_wait_seconds = produced.value->timing.reorder_wait_seconds;
    performance.task_latency_p50_seconds = produced.value->timing.task_latency_p50_seconds;
    performance.task_latency_p95_seconds = produced.value->timing.task_latency_p95_seconds;
    performance.task_latency_p99_seconds = produced.value->timing.task_latency_p99_seconds;
    performance.task_latency_max_seconds = produced.value->timing.task_latency_max_seconds;
    for (const auto& artifact : produced.value->artifacts) {
        performance.logical_records += artifact.logical_rows;
        performance.logical_bytes += artifact.logical_bytes;
        ++performance.final_file_count;
        if (!artifact.index.path.empty()) {
            ++performance.final_file_count;
        }
    }
    performance.final_file_count += 5U;
    performance.cache_condition = "UNKNOWN";

    longlineage::artifact::DatasetCloseoutOptions closeout_options;
    closeout_options.repo_root = repo_root;
    closeout_options.staging_root = created.transaction->staging_root();
    closeout_options.run_id = manifest.run_id;
    closeout_options.executable = {
        LONGLINEAGE_VERSION, LONGLINEAGE_GIT_COMMIT, *executable_sha256.value, LONGLINEAGE_COMPILER_ID, "1.18",
    };
    closeout_options.producer_hostname = local_hostname();
    closeout_options.producer_kernel_release = kernel_release();
    closeout_options.input_mount_identity = std::move(mounts);
    closeout_options.manifest_inputs = manifest_input_bindings(manifest, manifest_sha256);
    closeout_options.manifest_sha256 = manifest_sha256;
    closeout_options.input_snapshot_before_sha256 = snapshot_before;
    closeout_options.input_snapshot_after_sha256 = snapshot_after;
    closeout_options.input_lock_sha256 = input_lock_sha256;
    closeout_options.phase_ledger_sha256 = *phase_ledger_sha256.value;
    closeout_options.performance = performance;
    closeout_options.finished_at = rfc3339_utc_now();
    auto closed = longlineage::artifact::write_dataset_producer_closeout(produced.value->artifacts, closeout_options);
    if (!closed.ok() || !closed.value.has_value()) {
        longlineage::cli::emit_error("dataset-gate", ExitCode::InternalError, closed.detail);
        return static_cast<int>(ExitCode::InternalError);
    }

    const auto& counters = produced.value->counters;
    const auto& timing = produced.value->timing;
    std::cout << "{\"command\":\"dataset-gate\","
              << "\"status\":\"READY_FOR_VALIDATION\","
              << "\"run_id\":" << longlineage::cli::json_quote(manifest.run_id) << ","
              << "\"staging_root\":" << longlineage::cli::json_quote(created.transaction->staging_root().string())
              << ",\"authority_profile\":\"HCC1395_DATASET_GATE\","
              << "\"production_claim_allowed\":false,"
              << "\"site_keys\":" << counters.site_keys << ","
              << "\"site_read_rows\":" << counters.site_read_rows << ","
              << "\"methyl_call_rows\":" << counters.methyl_call_rows << ","
              << "\"m1_evaluable\":" << counters.m1_evaluable << ","
              << "\"m1_stable_assignments\":" << counters.m1_stable_assignments << ","
              << "\"m2_eligible\":" << counters.m2_eligible << ","
              << "\"cooccurrence_pairs\":" << counters.cooccurrence_pairs << ","
              << "\"topology_regions\":" << counters.topology_regions << ","
              << "\"science_semantic_sha256\":" << longlineage::cli::json_quote(produced.value->semantic_sha256)
              << ",\"producer_receipt_sha256\":" << longlineage::cli::json_quote(closed.value->producer_receipt_sha256)
              << ",\"timing\":{\"handle_open_seconds\":" << timing.handle_open_seconds
              << ",\"block_execution_seconds\":" << timing.block_execution_seconds
              << ",\"artifact_stream_seconds\":" << timing.artifact_stream_seconds
              << ",\"global_pair_seconds\":" << timing.global_pair_seconds
              << ",\"topology_second_pass_seconds\":" << timing.topology_second_pass_seconds
              << ",\"finalization_seconds\":" << timing.finalization_seconds
              << ",\"total_seconds\":" << timing.total_seconds
              << ",\"queue_wait_seconds\":" << timing.queue_wait_seconds
              << ",\"reorder_wait_seconds\":" << timing.reorder_wait_seconds << "}}\n";
    return static_cast<int>(ExitCode::Success);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        print_usage();
        return static_cast<int>(ExitCode::Success);
    }
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "longlineage " << LONGLINEAGE_VERSION << '\n';
        return static_cast<int>(ExitCode::Success);
    }

    Arguments arguments;
    std::string error;
    if (!parse_arguments(argc, argv, arguments, error)) {
        longlineage::cli::emit_error("longlineage", ExitCode::UsageError, error);
        print_usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    if (arguments.help) {
        print_usage();
        return static_cast<int>(ExitCode::Success);
    }

    const auto explicit_repo = arguments.values.count("--repo") == 0U
                                   ? std::filesystem::path{}
                                   : std::filesystem::path(arguments.values.at("--repo"));
    const auto repo_root = longlineage::cli::find_repo_root(explicit_repo);
    if (repo_root.empty()) {
        longlineage::cli::emit_error(arguments.command, ExitCode::IoError, "cannot locate repository contract root");
        return static_cast<int>(ExitCode::IoError);
    }

    const auto manifest_snapshot = longlineage::load_production_manifest_snapshot(arguments.values.at("--manifest"));
    if (!manifest_snapshot.ok() || !manifest_snapshot.value.has_value()) {
        longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected, manifest_snapshot.detail);
        return static_cast<int>(ExitCode::PreflightRejected);
    }
    const longlineage::ProductionManifest& parsed_manifest = manifest_snapshot.value->manifest;

    if (arguments.command == "run") {
        const auto authority = verify_production_authority(parsed_manifest, repo_root);
        if (!authority.ok) {
            longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected, authority.message);
            return static_cast<int>(ExitCode::PreflightRejected);
        }
        const auto gate = longlineage::cli::require_release_attestation_ready(
            repo_root, parsed_manifest.contract_bindings.release_attestation_sha256);
        if (!gate.ok) {
            longlineage::cli::emit_error(arguments.command, ExitCode::KernelBlocked, gate.message);
            return static_cast<int>(ExitCode::KernelBlocked);
        }
    } else if (arguments.command == "dataset-gate") {
        const auto authority = verify_hcc1395_dataset_gate_authority(parsed_manifest, repo_root);
        if (!authority.ok) {
            longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected, authority.message);
            return static_cast<int>(ExitCode::PreflightRejected);
        }
    } else if (arguments.command == "probe" &&
               parsed_manifest.authority_profile == longlineage::AuthorityProfile::kHcc1395DatasetGate) {
        longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected,
                                     "HCC1395_DATASET_GATE cannot be weakened through probe");
        return static_cast<int>(ExitCode::PreflightRejected);
    }

    std::string verified_snapshot_sha256;
    const auto manifest_result = check_manifest_and_inputs(parsed_manifest, repo_root, verified_snapshot_sha256);
    if (!manifest_result.ok) {
        longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected, manifest_result.message);
        return static_cast<int>(ExitCode::PreflightRejected);
    }

    if (arguments.command == "preflight") {
        longlineage::cli::emit_result(arguments.command, manifest_result);
        return static_cast<int>(ExitCode::Success);
    }
    if (arguments.command == "dataset-gate") {
        return execute_hcc1395_dataset_gate(parsed_manifest, manifest_snapshot.value->physical_sha256, repo_root,
                                            verified_snapshot_sha256);
    }

    const std::string scope = arguments.command == "probe" ? "PARTIAL probe" : "production";
    longlineage::cli::emit_error(arguments.command, ExitCode::KernelBlocked,
                                 scope + " engine is not release-enabled in this scaffold");
    return static_cast<int>(ExitCode::KernelBlocked);
}
