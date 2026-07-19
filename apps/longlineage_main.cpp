// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cli_support.hpp"
#include "longlineage/common/digest.hpp"
#include "longlineage/io/hts_preflight.hpp"
#include "longlineage/manifest/production_manifest.hpp"

namespace {

using longlineage::cli::CheckResult;
using longlineage::cli::ExitCode;

void print_usage() {
    std::cout << "LongLineage " << LONGLINEAGE_VERSION << "\n"
              << "Usage:\n"
              << "  longlineage preflight --manifest FILE [--repo DIR]\n"
              << "  longlineage run       --manifest FILE [--repo DIR]\n"
              << "  longlineage probe     --manifest FILE --partial-output DIR [--repo DIR]\n"
              << "\n"
              << "Production run is fail-closed until P3, P4 and P5 are VERIFIED.\n"
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
    if (parsed.command != "preflight" && parsed.command != "run" && parsed.command != "probe") {
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

CheckResult check_manifest_and_inputs(const longlineage::ProductionManifest& manifest,
                                      const std::filesystem::path& repo_root) {
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
    }

    for (const auto& dataset : manifest.datasets) {
        for (const auto& file : dataset.files) {
            const auto locked = longlineage::verify_locked_file(file);
            if (!locked.ok()) {
                return {false, dataset.dataset_id + ": " + locked.detail};
            }
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
    return {true, "manifest, ten contract bindings, all input locks and indexed HTS probes passed"};
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

    if (arguments.command == "run") {
        const auto manifest = longlineage::load_production_manifest(arguments.values.at("--manifest"));
        if (!manifest.ok()) {
            longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected, manifest.detail);
            return static_cast<int>(ExitCode::PreflightRejected);
        }
        const auto authority = verify_production_authority(*manifest.value, repo_root);
        if (!authority.ok) {
            longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected, authority.message);
            return static_cast<int>(ExitCode::PreflightRejected);
        }
        const auto gate = longlineage::cli::require_release_attestation_ready(
            repo_root, manifest.value->contract_bindings.release_attestation_sha256);
        if (!gate.ok) {
            longlineage::cli::emit_error(arguments.command, ExitCode::KernelBlocked, gate.message);
            return static_cast<int>(ExitCode::KernelBlocked);
        }
    }

    const auto parsed_manifest = longlineage::load_production_manifest(arguments.values.at("--manifest"));
    if (!parsed_manifest.ok()) {
        longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected, parsed_manifest.detail);
        return static_cast<int>(ExitCode::PreflightRejected);
    }
    const auto manifest_result = check_manifest_and_inputs(*parsed_manifest.value, repo_root);
    if (!manifest_result.ok) {
        longlineage::cli::emit_error(arguments.command, ExitCode::PreflightRejected, manifest_result.message);
        return static_cast<int>(ExitCode::PreflightRejected);
    }

    if (arguments.command == "preflight") {
        longlineage::cli::emit_result(arguments.command, manifest_result);
        return static_cast<int>(ExitCode::Success);
    }

    const std::string scope = arguments.command == "probe" ? "PARTIAL probe" : "production";
    longlineage::cli::emit_error(arguments.command, ExitCode::KernelBlocked,
                                 scope + " engine is not release-enabled in this scaffold");
    return static_cast<int>(ExitCode::KernelBlocked);
}
