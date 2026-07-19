// SPDX-License-Identifier: GPL-3.0-only

#include "cli_support.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

namespace longlineage::cli {
namespace {

bool is_object_with_only_keys(const json_t* object, const std::set<std::string>& allowed,
                              const std::set<std::string>& required, std::string& reason) {
    if (!json_is_object(object)) {
        reason = "expected JSON object";
        return false;
    }

    for (const auto& key : required) {
        if (json_object_get(object, key.c_str()) == nullptr) {
            reason = "missing required field: " + key;
            return false;
        }
    }

    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(object), key, value) {
        static_cast<void>(value);
        if (allowed.count(key) == 0U) {
            reason = "unknown field: " + std::string(key);
            return false;
        }
    }
    return true;
}

bool is_nonempty_string(const json_t* value) { return json_is_string(value) && json_string_length(value) > 0U; }

std::string string_field(const json_t* object, const char* name) {
    const auto* value = json_object_get(object, name);
    return json_is_string(value) ? std::string(json_string_value(value)) : std::string{};
}

bool is_lower_hex_digest(const json_t* value) {
    if (!json_is_string(value) || json_string_length(value) != 64U) {
        return false;
    }
    const std::string digest = json_string_value(value);
    return std::all_of(digest.begin(), digest.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool is_safe_repository_relative(const std::string& value) {
    const std::filesystem::path path(value);
    return !value.empty() && !path.is_absolute() && value.find("..") == std::string::npos &&
           path.lexically_normal() == path;
}

bool is_rfc3339_seconds(const std::string& value) {
    return std::regex_match(value, std::regex("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}"
                                              "(Z|[+-][0-9]{2}:[0-9]{2})$"));
}

CheckResult invalid(std::string message) { return {false, std::move(message)}; }

CheckResult valid(std::string message) { return {true, std::move(message)}; }

}  // namespace

void JsonDeleter::operator()(json_t* value) const noexcept {
    if (value != nullptr) {
        json_decref(value);
    }
}

std::filesystem::path find_repo_root(const std::filesystem::path& explicit_root) {
    std::error_code error;
    auto current =
        explicit_root.empty() ? std::filesystem::current_path(error) : std::filesystem::absolute(explicit_root, error);
    if (error) {
        return {};
    }

    current = current.lexically_normal();
    while (!current.empty()) {
        if (std::filesystem::is_regular_file(current / "AGENTS.md", error) &&
            std::filesystem::is_regular_file(current / "schema" / "catalog.json", error) &&
            std::filesystem::is_regular_file(current / "state" / "phase_ledger.json", error)) {
            return current;
        }
        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return {};
}

JsonPtr load_json_strict(const std::filesystem::path& path, std::string& error) {
    json_error_t json_error{};
    json_t* value = json_load_file(path.c_str(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &json_error);
    if (value == nullptr) {
        std::ostringstream stream;
        stream << path.string() << ':' << json_error.line << ':' << json_error.column << ": " << json_error.text;
        error = stream.str();
        return {};
    }
    error.clear();
    return JsonPtr(value);
}

CheckResult sha256_file_hex(const std::filesystem::path& path, std::string& digest) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return invalid("cannot open file for SHA-256: " + path.string());
    }
    using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return invalid("cannot initialize SHA-256 for: " + path.string());
    }
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes = input.gcount();
        if (bytes > 0 && EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(bytes)) != 1) {
            return invalid("cannot update SHA-256 for: " + path.string());
        }
    }
    if (!input.eof()) {
        return invalid("I/O error while hashing: " + path.string());
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> raw{};
    unsigned int raw_size = 0;
    if (EVP_DigestFinal_ex(context.get(), raw.data(), &raw_size) != 1 || raw_size != 32U) {
        return invalid("cannot finalize SHA-256 for: " + path.string());
    }
    constexpr char kHex[] = "0123456789abcdef";
    digest.clear();
    digest.reserve(64U);
    for (unsigned int index = 0; index < raw_size; ++index) {
        const auto byte = raw[index];
        digest.push_back(kHex[(byte >> 4U) & 0x0fU]);
        digest.push_back(kHex[byte & 0x0fU]);
    }
    return valid("SHA-256 recomputed");
}

CheckResult require_safe_repository_file(const std::filesystem::path& repo_root, const std::string& relative_path) {
    if (!is_safe_repository_relative(relative_path)) {
        return invalid("repository path is not safe and relative: " + relative_path);
    }
    std::error_code error;
    const auto canonical_root = std::filesystem::canonical(repo_root, error);
    if (error) {
        return invalid("cannot canonicalize repository root");
    }
    auto current = repo_root;
    for (const auto& component : std::filesystem::path(relative_path)) {
        current /= component;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error) {
            return invalid("cannot inspect repository path component: " + current.string());
        }
        if (std::filesystem::is_symlink(status)) {
            return invalid("repository path traverses a symlink: " + relative_path);
        }
    }
    if (!std::filesystem::is_regular_file(current, error) || error) {
        return invalid("repository path is not a regular file: " + relative_path);
    }
    const auto canonical_file = std::filesystem::canonical(current, error);
    if (error) {
        return invalid("cannot canonicalize repository file: " + relative_path);
    }
    auto root_component = canonical_root.begin();
    auto file_component = canonical_file.begin();
    for (; root_component != canonical_root.end(); ++root_component, ++file_component) {
        if (file_component == canonical_file.end() || *root_component != *file_component) {
            return invalid("repository file escapes the repository root: " + relative_path);
        }
    }
    return valid("repository file is regular, in-root and symlink-free");
}

CheckResult validate_production_manifest(const json_t* manifest) {
    const std::set<std::string> root_keys = {"schema_name", "schema_version", "authority_profile", "run_id",
                                             "output_root", "datasets",       "runtime",           "contract_bindings"};
    std::string reason;
    if (!is_object_with_only_keys(manifest, root_keys, root_keys, reason)) {
        return invalid(reason);
    }

    const auto* schema_name = json_object_get(manifest, "schema_name");
    const auto* schema_version = json_object_get(manifest, "schema_version");
    if (!json_is_string(schema_name) ||
        std::string(json_string_value(schema_name)) != "longlineage.production_manifest") {
        return invalid("schema_name is not longlineage.production_manifest");
    }
    if (!json_is_string(schema_version) || std::string(json_string_value(schema_version)) != "1.0.0") {
        return invalid("unsupported production manifest schema_version");
    }
    const auto* authority_profile = json_object_get(manifest, "authority_profile");
    if (!json_is_string(authority_profile) ||
        (std::string(json_string_value(authority_profile)) != "PRODUCTION_7_DATASET" &&
         std::string(json_string_value(authority_profile)) != "SYNTHETIC")) {
        return invalid("unknown authority_profile");
    }
    const auto* run_id_value = json_object_get(manifest, "run_id");
    const auto* output_root_value = json_object_get(manifest, "output_root");
    if (!is_nonempty_string(run_id_value) || !is_nonempty_string(output_root_value)) {
        return invalid("run_id and output_root must be non-empty strings");
    }
    const std::string run_id = json_string_value(run_id_value);
    const std::filesystem::path output_root(json_string_value(output_root_value));
    if (!output_root.is_absolute() || output_root.lexically_normal() != output_root ||
        output_root.filename() != run_id || output_root.parent_path().filename() != ".staging") {
        return invalid("output_root must be canonical absolute <base>/.staging/<run_id>");
    }

    const auto* datasets = json_object_get(manifest, "datasets");
    if (!json_is_array(datasets) || json_array_size(datasets) == 0U) {
        return invalid("datasets must be a non-empty array");
    }

    const std::set<std::string> dataset_keys = {"dataset_id", "dataset_order", "files"};
    const std::set<std::string> file_keys = {"role", "path", "size_bytes", "sha256"};
    const std::set<std::string> allowed_roles = {
        "raw_bam",
        "raw_bam_index",
        "pass_biallelic_ssnv_vcf",
        "pass_biallelic_ssnv_vcf_index",
        "latest_hp_ps_sidecar",
        "latest_hp_ps_sidecar_index",
        "reference_fasta",
        "reference_fai",
    };

    for (std::size_t dataset_index = 0; dataset_index < json_array_size(datasets); ++dataset_index) {
        const auto* dataset = json_array_get(datasets, dataset_index);
        if (!is_object_with_only_keys(dataset, dataset_keys, dataset_keys, reason)) {
            return invalid("dataset[" + std::to_string(dataset_index) + "]: " + reason);
        }
        if (!is_nonempty_string(json_object_get(dataset, "dataset_id")) ||
            !json_is_integer(json_object_get(dataset, "dataset_order"))) {
            return invalid("dataset id/order has the wrong type");
        }
        const auto* files = json_object_get(dataset, "files");
        if (!json_is_array(files) || json_array_size(files) != allowed_roles.size()) {
            return invalid("each dataset must bind exactly eight frozen file roles");
        }
        std::set<std::string> observed_roles;
        for (std::size_t file_index = 0; file_index < json_array_size(files); ++file_index) {
            const auto* file = json_array_get(files, file_index);
            if (!is_object_with_only_keys(file, file_keys, file_keys, reason)) {
                return invalid("dataset file[" + std::to_string(file_index) + "]: " + reason);
            }
            const auto* role_value = json_object_get(file, "role");
            const auto* path_value = json_object_get(file, "path");
            const auto* size_value = json_object_get(file, "size_bytes");
            const auto* digest_value = json_object_get(file, "sha256");
            if (!is_nonempty_string(role_value) || !is_nonempty_string(path_value) || !json_is_integer(size_value) ||
                json_integer_value(size_value) <= 0 || !json_is_string(digest_value) ||
                json_string_length(digest_value) != 64U) {
                return invalid("locked file role/path/size/digest is malformed");
            }
            const std::string role = json_string_value(role_value);
            if (allowed_roles.count(role) == 0U || !observed_roles.insert(role).second) {
                return invalid("locked file role is unknown or duplicated: " + role);
            }
        }
        if (observed_roles != allowed_roles) {
            return invalid("dataset does not bind the exact production file-role set");
        }
    }

    const std::set<std::string> runtime_keys = {
        "compute_workers",
        "writer_threads",
        "coordinator_slots",
        "buffer_bytes",
        "max_focal_sites_per_block",
        "max_estimated_alignments_per_block",
        "halo_bp",
    };
    const auto* runtime = json_object_get(manifest, "runtime");
    if (!is_object_with_only_keys(runtime, runtime_keys, runtime_keys, reason)) {
        return invalid("runtime: " + reason);
    }
    for (const auto& key : runtime_keys) {
        if (!json_is_integer(json_object_get(runtime, key.c_str()))) {
            return invalid("runtime field is not an integer: " + key);
        }
    }
    if (json_integer_value(json_object_get(runtime, "compute_workers")) < 1 ||
        json_integer_value(json_object_get(runtime, "compute_workers")) > 40 ||
        json_integer_value(json_object_get(runtime, "writer_threads")) < 1 ||
        json_integer_value(json_object_get(runtime, "writer_threads")) > 4 ||
        json_integer_value(json_object_get(runtime, "coordinator_slots")) != 2 ||
        json_integer_value(json_object_get(runtime, "buffer_bytes")) < 1048576 ||
        json_integer_value(json_object_get(runtime, "buffer_bytes")) > 8589934592LL ||
        json_integer_value(json_object_get(runtime, "max_focal_sites_per_block")) < 1 ||
        json_integer_value(json_object_get(runtime, "max_focal_sites_per_block")) > 4096 ||
        json_integer_value(json_object_get(runtime, "max_estimated_alignments_per_block")) < 1 ||
        json_integer_value(json_object_get(runtime, "max_estimated_alignments_per_block")) > 250000 ||
        json_integer_value(json_object_get(runtime, "halo_bp")) != 5000) {
        return invalid("runtime block/buffer/thread bounds are invalid");
    }

    const std::set<std::string> binding_keys = {
        "science_parameters_sha256",        "schema_catalog_sha256",
        "status_reason_registry_sha256",    "type_registry_sha256",
        "transform_registry_sha256",        "authority_manifest_sha256",
        "source_to_target_manifest_sha256", "production_input_authority_sha256",
        "schema_id_registry_sha256",        "release_attestation_sha256",
    };
    const auto* bindings = json_object_get(manifest, "contract_bindings");
    if (!is_object_with_only_keys(bindings, binding_keys, binding_keys, reason)) {
        return invalid("contract_bindings: " + reason);
    }
    for (const auto& key : binding_keys) {
        const auto* digest = json_object_get(bindings, key.c_str());
        if (!is_lower_hex_digest(digest)) {
            return invalid("contract binding is not a lowercase SHA-256 digest: " + key);
        }
    }

    return valid("production manifest shape and closed vocabulary passed");
}

CheckResult require_release_attestation_ready(const std::filesystem::path& repo_root,
                                              const std::string& expected_sha256) {
    if (!std::regex_match(expected_sha256, std::regex("^[0-9a-f]{64}$"))) {
        return invalid("release attestation binding is not a lowercase SHA-256");
    }
    const auto path = repo_root / "state" / "release_attestation.json";
    const auto attestation_path_check = require_safe_repository_file(repo_root, "state/release_attestation.json");
    if (!attestation_path_check.ok) {
        return attestation_path_check;
    }
    std::string observed_sha256;
    const auto digest = sha256_file_hex(path, observed_sha256);
    if (!digest.ok) {
        return digest;
    }
    if (observed_sha256 != expected_sha256) {
        return invalid("release attestation SHA-256 differs from manifest binding");
    }

    std::string error;
    const auto attestation = load_json_strict(path, error);
    if (!attestation) {
        return invalid(error);
    }
    const std::set<std::string> root_keys = {"schema_name", "schema_version", "attestation_id", "attestation_state",
                                             "issued_at",   "phases",         "blockers"};
    if (!is_object_with_only_keys(attestation.get(), root_keys, root_keys, error)) {
        return invalid("release attestation: " + error);
    }
    if (string_field(attestation.get(), "schema_name") != "longlineage.release_attestation" ||
        string_field(attestation.get(), "schema_version") != "1.0.0" ||
        !std::regex_match(string_field(attestation.get(), "attestation_id"),
                          std::regex("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")) ||
        !is_rfc3339_seconds(string_field(attestation.get(), "issued_at"))) {
        return invalid("release attestation identity is invalid");
    }

    const auto* phases = json_object_get(attestation.get(), "phases");
    const auto* blockers = json_object_get(attestation.get(), "blockers");
    if (!json_is_array(phases) || json_array_size(phases) != 3U || !json_is_array(blockers)) {
        return invalid("release attestation must contain P3/P4/P5 and blocker arrays");
    }
    std::set<std::string> observed_blockers;
    for (std::size_t index = 0; index < json_array_size(blockers); ++index) {
        const auto* blocker = json_array_get(blockers, index);
        if (!json_is_string(blocker) ||
            !std::regex_match(json_string_value(blocker), std::regex("^[A-Z0-9][A-Z0-9_]+$")) ||
            !observed_blockers.insert(json_string_value(blocker)).second) {
            return invalid("release attestation blocker is malformed or duplicated");
        }
    }
    const std::array<std::string, 3> required = {"P3", "P4", "P5"};
    const std::set<std::string> evidence_roles = {"TEST_LOG", "RECEIPT", "AUDIT"};
    for (std::size_t index = 0; index < required.size(); ++index) {
        const auto* phase = json_array_get(phases, index);
        const std::set<std::string> phase_keys = {"id", "status", "evidence"};
        if (!is_object_with_only_keys(phase, phase_keys, phase_keys, error) ||
            string_field(phase, "id") != required[index] || !json_is_array(json_object_get(phase, "evidence"))) {
            return invalid("release attestation phase order/shape is invalid at " + required[index]);
        }
        const std::string status = string_field(phase, "status");
        if (status != "BLOCKED" && status != "VERIFIED") {
            return invalid("release attestation phase status is invalid: " + required[index] + "=" + status);
        }
        const auto* evidence = json_object_get(phase, "evidence");
        for (std::size_t evidence_index = 0; evidence_index < json_array_size(evidence); ++evidence_index) {
            const auto* row = json_array_get(evidence, evidence_index);
            const std::set<std::string> evidence_keys = {"role", "path", "sha256", "command", "exit_code"};
            if (!is_object_with_only_keys(row, evidence_keys, evidence_keys, error)) {
                return invalid(required[index] + " evidence: " + error);
            }
            const std::string relative = string_field(row, "path");
            const auto* sha = json_object_get(row, "sha256");
            const auto* exit_code = json_object_get(row, "exit_code");
            if (evidence_roles.count(string_field(row, "role")) == 0U || !is_safe_repository_relative(relative) ||
                !is_lower_hex_digest(sha) || !is_nonempty_string(json_object_get(row, "command")) ||
                !json_is_integer(exit_code) || json_integer_value(exit_code) != 0) {
                return invalid(required[index] + " evidence is not replayable");
            }
            const auto path_check = require_safe_repository_file(repo_root, relative);
            if (!path_check.ok) {
                return invalid(required[index] + " evidence path is unsafe: " + path_check.message);
            }
            std::string observed;
            const auto checked = sha256_file_hex(repo_root / relative, observed);
            if (!checked.ok || observed != json_string_value(sha)) {
                return invalid(required[index] + " evidence path/digest cannot be verified: " + relative);
            }
        }
    }

    const std::string state = string_field(attestation.get(), "attestation_state");
    if (state != "READY") {
        if (state != "NOT_READY" || json_array_size(blockers) == 0U) {
            return invalid("release attestation state/blockers are invalid");
        }
        return invalid("release attestation is NOT_READY");
    }
    if (json_array_size(blockers) != 0U) {
        return invalid("READY release attestation cannot carry blockers");
    }

    for (std::size_t index = 0; index < required.size(); ++index) {
        const auto* phase = json_array_get(phases, index);
        const auto* evidence = json_object_get(phase, "evidence");
        if (string_field(phase, "status") != "VERIFIED" || json_array_size(evidence) == 0U) {
            return invalid("READY release attestation lacks complete evidence for " + required[index]);
        }
    }
    return valid("manifest-bound release attestation is READY for P3/P4/P5");
}

CheckResult require_validated_frozen_run(const std::filesystem::path& run_root) {
    std::string error;
    const auto receipt = load_json_strict(run_root / "run_receipt.json", error);
    if (!receipt) {
        return invalid(error);
    }
    const auto* state = json_object_get(receipt.get(), "state");
    if (!json_is_string(state) || std::string(json_string_value(state)) != "VALIDATED_FROZEN") {
        return invalid("run_receipt state is not VALIDATED_FROZEN");
    }
    const auto* fields_seen = json_object_get(receipt.get(), "truth_fields_seen");
    if (!json_is_integer(fields_seen) || json_integer_value(fields_seen) != 0) {
        return invalid("run_receipt production-boundary counter is not zero");
    }
    return valid("run is VALIDATED_FROZEN and production-boundary counter is zero");
}

std::string json_quote(const std::string& value) {
    JsonPtr string_value(json_stringn(value.data(), value.size()));
    if (!string_value) {
        return "\"<encoding-error>\"";
    }
    char* encoded = json_dumps(string_value.get(), JSON_ENCODE_ANY | JSON_COMPACT);
    if (encoded == nullptr) {
        return "\"<encoding-error>\"";
    }
    const std::string output(encoded);
    std::free(encoded);
    return output;
}

void emit_result(const std::string& command, const CheckResult& result) {
    std::cout << "{\"command\":" << json_quote(command) << ",\"status\":\"" << (result.ok ? "PASS" : "FAIL")
              << "\",\"message\":" << json_quote(result.message) << "}\n";
}

void emit_error(const std::string& command, ExitCode code, const std::string& message) {
    std::cerr << "{\"command\":" << json_quote(command) << ",\"status\":\"ERROR\","
              << "\"exit_code\":" << static_cast<int>(code) << ",\"message\":" << json_quote(message) << "}\n";
}

bool is_help_flag(const std::string& value) { return value == "-h" || value == "--help"; }

}  // namespace longlineage::cli
