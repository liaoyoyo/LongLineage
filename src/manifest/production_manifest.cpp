// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/manifest/production_manifest.hpp"

#include <fcntl.h>
#include <jansson.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"

namespace longlineage {
namespace {

using JsonPointer = std::unique_ptr<json_t, decltype(&json_decref)>;

class FileDescriptor final {
   public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }

   private:
    int value_;
};

[[nodiscard]] bool same_file_identity(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino && left.st_mode == right.st_mode &&
           left.st_size == right.st_size && left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec && left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

constexpr std::array<FileRole, 8> kRequiredRoles = {
    FileRole::kRawBam,
    FileRole::kRawBamIndex,
    FileRole::kPassBiallelicSsnvVcf,
    FileRole::kPassBiallelicSsnvVcfIndex,
    FileRole::kLatestHpPsSidecar,
    FileRole::kLatestHpPsSidecarIndex,
    FileRole::kReferenceFasta,
    FileRole::kReferenceFai,
};

[[nodiscard]] bool contains_truth_token(std::string_view value) {
    constexpr std::string_view kToken = "truth";
    if (value.size() < kToken.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset + kToken.size() <= value.size(); ++offset) {
        bool match = true;
        for (std::size_t index = 0; index < kToken.size(); ++index) {
            const auto character = static_cast<unsigned char>(value[offset + index]);
            if (static_cast<char>(std::tolower(character)) != kToken[index]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reject_truth_recursive(const json_t* node, std::string_view path, std::size_t depth,
                                          std::string& detail) {
    if (depth > 64) {
        detail = "JSON nesting exceeds the production-manifest limit at " + std::string(path);
        return false;
    }
    if (json_is_object(node)) {
        const char* key = nullptr;
        json_t* value = nullptr;
        json_object_foreach(const_cast<json_t*>(node), key, value) {
            if (contains_truth_token(key)) {
                detail = "Truth-bearing key is prohibited in production manifest at " + std::string(path) + "." + key;
                return false;
            }
            if (!reject_truth_recursive(value, std::string(path) + "." + key, depth + 1, detail)) {
                return false;
            }
        }
    } else if (json_is_array(node)) {
        const std::size_t size = json_array_size(node);
        for (std::size_t index = 0; index < size; ++index) {
            if (!reject_truth_recursive(json_array_get(node, index),
                                        std::string(path) + "[" + std::to_string(index) + "]", depth + 1, detail)) {
                return false;
            }
        }
    } else if (json_is_string(node)) {
        const char* raw = json_string_value(node);
        const std::size_t size = json_string_length(node);
        const std::string_view value(raw, size);
        if (contains_truth_token(value)) {
            detail = "Truth-bearing string value is prohibited in production manifest at " + std::string(path);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_only_keys(const json_t* object, std::initializer_list<std::string_view> allowed,
                                 std::initializer_list<std::string_view> required, std::string_view path,
                                 std::string& detail) {
    if (!json_is_object(object)) {
        detail = std::string(path) + " must be an object";
        return false;
    }
    std::set<std::string_view> allowed_set(allowed);
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(object), key, value) {
        static_cast<void>(value);
        if (allowed_set.count(key) == 0) {
            detail = std::string(path) + " contains unknown field: " + key;
            return false;
        }
    }
    for (const std::string_view key_name : required) {
        if (json_object_get(object, std::string(key_name).c_str()) == nullptr) {
            detail = std::string(path) + " is missing required field: " + std::string(key_name);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool read_string(const json_t* object, const char* key, std::string_view path, std::string& output,
                               std::string& detail) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_string(value)) {
        detail = std::string(path) + "." + key + " must be a string";
        return false;
    }
    const char* raw = json_string_value(value);
    const std::size_t size = json_string_length(value);
    if (std::char_traits<char>::length(raw) != size) {
        detail = std::string(path) + "." + key + " contains an embedded NUL";
        return false;
    }
    output.assign(raw, size);
    return true;
}

[[nodiscard]] bool read_unsigned(const json_t* object, const char* key, std::uint64_t minimum, std::uint64_t maximum,
                                 std::string_view path, std::uint64_t& output, std::string& detail) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_integer(value)) {
        detail = std::string(path) + "." + key + " must be an integer";
        return false;
    }
    const json_int_t parsed = json_integer_value(value);
    if (parsed < 0 || static_cast<std::uint64_t>(parsed) < minimum || static_cast<std::uint64_t>(parsed) > maximum) {
        detail = std::string(path) + "." + key + " is outside the permitted range";
        return false;
    }
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

[[nodiscard]] bool is_safe_id(std::string_view value, std::size_t max_size) {
    if (value.empty() || value.size() > max_size) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (!std::isalnum(first)) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) || character == '.' || character == '_' || character == '-';
    });
}

[[nodiscard]] bool is_lower_sha256(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool validate_absolute_non_alias_path(const std::filesystem::path& path, std::string_view field_path,
                                                    std::string& detail) {
    if (!path.is_absolute() || path.empty() || path.lexically_normal() != path) {
        detail = std::string(field_path) + " must be an absolute, lexically-normal path without '.' or '..'";
        return false;
    }
    std::filesystem::path current;
    for (const auto& component : path) {
        current /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (!error && std::filesystem::is_symlink(status)) {
            detail =
                std::string(field_path) + " must not traverse an existing symbolic-link alias: " + current.string();
            return false;
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            detail = std::string(field_path) + " cannot be checked for symbolic-link aliases: " + current.string();
            return false;
        }
    }
    return true;
}

[[nodiscard]] ParseResult<LockedFile> parse_locked_file(const json_t* node, std::string_view path) {
    std::string detail;
    if (!has_only_keys(node, {"role", "path", "size_bytes", "sha256"}, {"role", "path", "size_bytes", "sha256"}, path,
                       detail)) {
        return ParseResult<LockedFile>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    std::string role_text;
    std::string file_path;
    std::string sha256;
    if (!read_string(node, "role", path, role_text, detail) || !read_string(node, "path", path, file_path, detail) ||
        !read_string(node, "sha256", path, sha256, detail)) {
        return ParseResult<LockedFile>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    const auto role = parse_file_role(role_text);
    if (!role.ok()) {
        return ParseResult<LockedFile>::failure(role.reason, std::string(path) + ".role: " + role.detail);
    }
    std::uint64_t size_bytes = 0;
    if (!read_unsigned(node, "size_bytes", 1, static_cast<std::uint64_t>(std::numeric_limits<json_int_t>::max()), path,
                       size_bytes, detail)) {
        return ParseResult<LockedFile>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    if (file_path.empty()) {
        return ParseResult<LockedFile>::failure(ParseReason::kMalformedValue,
                                                std::string(path) + ".path must not be empty");
    }
    const std::filesystem::path parsed_path(file_path);
    if (!validate_absolute_non_alias_path(parsed_path, std::string(path) + ".path", detail)) {
        return ParseResult<LockedFile>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    if (!is_lower_sha256(sha256)) {
        return ParseResult<LockedFile>::failure(ParseReason::kMalformedValue,
                                                std::string(path) + ".sha256 must be 64 lowercase hex digits");
    }
    return ParseResult<LockedFile>::success(LockedFile{*role.value, parsed_path, size_bytes, std::move(sha256)});
}

[[nodiscard]] ParseResult<DatasetManifest> parse_dataset(const json_t* node, std::size_t array_index) {
    const std::string path = "$.datasets[" + std::to_string(array_index) + "]";
    std::string detail;
    if (!has_only_keys(node, {"dataset_id", "dataset_order", "files"}, {"dataset_id", "dataset_order", "files"}, path,
                       detail)) {
        return ParseResult<DatasetManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    std::string dataset_id;
    if (!read_string(node, "dataset_id", path, dataset_id, detail) || !is_safe_id(dataset_id, 64)) {
        if (detail.empty()) {
            detail = path + ".dataset_id does not match the safe identifier contract";
        }
        return ParseResult<DatasetManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    std::uint64_t dataset_order = 0;
    if (!read_unsigned(node, "dataset_order", 0, std::numeric_limits<std::uint32_t>::max(), path, dataset_order,
                       detail)) {
        return ParseResult<DatasetManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    if (dataset_order != array_index) {
        return ParseResult<DatasetManifest>::failure(
            ParseReason::kMalformedValue, path + ".dataset_order must equal its frozen zero-based array position");
    }

    const json_t* files = json_object_get(node, "files");
    if (!json_is_array(files) || json_array_size(files) != kRequiredRoles.size()) {
        return ParseResult<DatasetManifest>::failure(ParseReason::kMalformedValue,
                                                     path + ".files must contain exactly eight locked files");
    }
    std::vector<LockedFile> parsed_files;
    parsed_files.reserve(kRequiredRoles.size());
    std::set<FileRole> roles;
    for (std::size_t index = 0; index < json_array_size(files); ++index) {
        auto file = parse_locked_file(json_array_get(files, index), path + ".files[" + std::to_string(index) + "]");
        if (!file.ok()) {
            return ParseResult<DatasetManifest>::failure(file.reason, std::move(file.detail));
        }
        if (!roles.insert(file.value->role).second) {
            return ParseResult<DatasetManifest>::failure(
                ParseReason::kMalformedValue,
                path + ".files contains duplicate role: " + std::string(to_string(file.value->role)));
        }
        parsed_files.push_back(std::move(*file.value));
    }
    for (const FileRole required_role : kRequiredRoles) {
        if (roles.count(required_role) == 0) {
            return ParseResult<DatasetManifest>::failure(
                ParseReason::kMissingRequiredField,
                path + ".files is missing role: " + std::string(to_string(required_role)));
        }
    }
    return ParseResult<DatasetManifest>::success(
        DatasetManifest{std::move(dataset_id), static_cast<std::uint32_t>(dataset_order), std::move(parsed_files)});
}

[[nodiscard]] ParseResult<RuntimeManifest> parse_runtime(const json_t* node) {
    constexpr std::string_view kPath = "$.runtime";
    std::string detail;
    if (!has_only_keys(node,
                       {"compute_workers", "writer_threads", "coordinator_slots", "buffer_bytes",
                        "max_focal_sites_per_block", "max_estimated_alignments_per_block", "halo_bp"},
                       {"compute_workers", "writer_threads", "coordinator_slots", "buffer_bytes",
                        "max_focal_sites_per_block", "max_estimated_alignments_per_block", "halo_bp"},
                       kPath, detail)) {
        return ParseResult<RuntimeManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    std::uint64_t compute_workers = 0;
    std::uint64_t writer_threads = 0;
    std::uint64_t coordinator_slots = 0;
    std::uint64_t buffer_bytes = 0;
    std::uint64_t max_sites = 0;
    std::uint64_t max_alignments = 0;
    std::uint64_t halo_bp = 0;
    if (!read_unsigned(node, "compute_workers", 1, 40, kPath, compute_workers, detail) ||
        !read_unsigned(node, "writer_threads", 1, 4, kPath, writer_threads, detail) ||
        !read_unsigned(node, "coordinator_slots", 2, 2, kPath, coordinator_slots, detail) ||
        !read_unsigned(node, "buffer_bytes", 1048576, 8589934592ULL, kPath, buffer_bytes, detail) ||
        !read_unsigned(node, "max_focal_sites_per_block", 1, 4096, kPath, max_sites, detail) ||
        !read_unsigned(node, "max_estimated_alignments_per_block", 1, 250000, kPath, max_alignments, detail) ||
        !read_unsigned(node, "halo_bp", 5000, 5000, kPath, halo_bp, detail)) {
        return ParseResult<RuntimeManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    return ParseResult<RuntimeManifest>::success(RuntimeManifest{
        static_cast<std::uint32_t>(compute_workers), static_cast<std::uint32_t>(writer_threads),
        static_cast<std::uint32_t>(coordinator_slots), buffer_bytes, static_cast<std::uint32_t>(max_sites),
        static_cast<std::uint32_t>(max_alignments), static_cast<std::uint32_t>(halo_bp)});
}

[[nodiscard]] ParseResult<ContractBindings> parse_contract_bindings(const json_t* node) {
    constexpr std::string_view kPath = "$.contract_bindings";
    std::string detail;
    if (!has_only_keys(
            node,
            {"science_parameters_sha256", "schema_catalog_sha256", "status_reason_registry_sha256",
             "type_registry_sha256", "transform_registry_sha256", "authority_manifest_sha256",
             "source_to_target_manifest_sha256", "production_input_authority_sha256",
             "dataset_gate_input_authority_sha256", "schema_id_registry_sha256", "release_attestation_sha256"},
            {"science_parameters_sha256", "schema_catalog_sha256", "status_reason_registry_sha256",
             "type_registry_sha256", "transform_registry_sha256", "authority_manifest_sha256",
             "source_to_target_manifest_sha256", "production_input_authority_sha256", "schema_id_registry_sha256",
             "release_attestation_sha256"},
            kPath, detail)) {
        return ParseResult<ContractBindings>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    ContractBindings bindings;
    if (!read_string(node, "science_parameters_sha256", kPath, bindings.science_parameters_sha256, detail) ||
        !read_string(node, "schema_catalog_sha256", kPath, bindings.schema_catalog_sha256, detail) ||
        !read_string(node, "status_reason_registry_sha256", kPath, bindings.status_reason_registry_sha256, detail) ||
        !read_string(node, "type_registry_sha256", kPath, bindings.type_registry_sha256, detail) ||
        !read_string(node, "transform_registry_sha256", kPath, bindings.transform_registry_sha256, detail) ||
        !read_string(node, "authority_manifest_sha256", kPath, bindings.authority_manifest_sha256, detail) ||
        !read_string(node, "source_to_target_manifest_sha256", kPath, bindings.source_to_target_manifest_sha256,
                     detail) ||
        !read_string(node, "production_input_authority_sha256", kPath, bindings.production_input_authority_sha256,
                     detail) ||
        !read_string(node, "schema_id_registry_sha256", kPath, bindings.schema_id_registry_sha256, detail) ||
        !read_string(node, "release_attestation_sha256", kPath, bindings.release_attestation_sha256, detail)) {
        return ParseResult<ContractBindings>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    const json_t* dataset_gate_authority = json_object_get(node, "dataset_gate_input_authority_sha256");
    if (dataset_gate_authority != nullptr && !read_string(node, "dataset_gate_input_authority_sha256", kPath,
                                                          bindings.dataset_gate_input_authority_sha256, detail)) {
        return ParseResult<ContractBindings>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    const std::array<std::pair<std::string_view, const std::string*>, 11> values = {{
        {"science_parameters_sha256", &bindings.science_parameters_sha256},
        {"schema_catalog_sha256", &bindings.schema_catalog_sha256},
        {"status_reason_registry_sha256", &bindings.status_reason_registry_sha256},
        {"type_registry_sha256", &bindings.type_registry_sha256},
        {"transform_registry_sha256", &bindings.transform_registry_sha256},
        {"authority_manifest_sha256", &bindings.authority_manifest_sha256},
        {"source_to_target_manifest_sha256", &bindings.source_to_target_manifest_sha256},
        {"production_input_authority_sha256", &bindings.production_input_authority_sha256},
        {"dataset_gate_input_authority_sha256", &bindings.dataset_gate_input_authority_sha256},
        {"schema_id_registry_sha256", &bindings.schema_id_registry_sha256},
        {"release_attestation_sha256", &bindings.release_attestation_sha256},
    }};
    for (const auto& [name, value] : values) {
        if (name == "dataset_gate_input_authority_sha256" && value->empty()) {
            continue;
        }
        if (!is_lower_sha256(*value)) {
            return ParseResult<ContractBindings>::failure(
                ParseReason::kMalformedValue,
                std::string(kPath) + "." + std::string(name) + " must be 64 lowercase hex digits");
        }
    }
    return ParseResult<ContractBindings>::success(std::move(bindings));
}

}  // namespace

std::string_view to_string(AuthorityProfile profile) noexcept {
    switch (profile) {
        case AuthorityProfile::kProductionSevenDataset:
            return "PRODUCTION_7_DATASET";
        case AuthorityProfile::kHcc1395DatasetGate:
            return "HCC1395_DATASET_GATE";
        case AuthorityProfile::kSynthetic:
            return "SYNTHETIC";
    }
    return "SYNTHETIC";
}

ParseResult<AuthorityProfile> parse_authority_profile(std::string_view value) {
    if (value == "PRODUCTION_7_DATASET") {
        return ParseResult<AuthorityProfile>::success(AuthorityProfile::kProductionSevenDataset);
    }
    if (value == "HCC1395_DATASET_GATE") {
        return ParseResult<AuthorityProfile>::success(AuthorityProfile::kHcc1395DatasetGate);
    }
    if (value == "SYNTHETIC") {
        return ParseResult<AuthorityProfile>::success(AuthorityProfile::kSynthetic);
    }
    return ParseResult<AuthorityProfile>::failure(
        ParseReason::kUnsupportedValue,
        "authority_profile must be PRODUCTION_7_DATASET, HCC1395_DATASET_GATE or SYNTHETIC");
}

std::string_view to_string(FileRole role) noexcept {
    switch (role) {
        case FileRole::kRawBam:
            return "raw_bam";
        case FileRole::kRawBamIndex:
            return "raw_bam_index";
        case FileRole::kPassBiallelicSsnvVcf:
            return "pass_biallelic_ssnv_vcf";
        case FileRole::kPassBiallelicSsnvVcfIndex:
            return "pass_biallelic_ssnv_vcf_index";
        case FileRole::kLatestHpPsSidecar:
            return "latest_hp_ps_sidecar";
        case FileRole::kLatestHpPsSidecarIndex:
            return "latest_hp_ps_sidecar_index";
        case FileRole::kReferenceFasta:
            return "reference_fasta";
        case FileRole::kReferenceFai:
            return "reference_fai";
    }
    return "unsupported";
}

ParseResult<FileRole> parse_file_role(std::string_view value) {
    for (const FileRole role : kRequiredRoles) {
        if (value == to_string(role)) {
            return ParseResult<FileRole>::success(role);
        }
    }
    return ParseResult<FileRole>::failure(ParseReason::kUnsupportedValue,
                                          "Unknown or non-production file role: " + std::string(value));
}

ParseResult<ProductionManifest> parse_production_manifest_json(std::string_view json) {
    json_error_t error{};
    JsonPointer root(json_loadb(json.data(), json.size(), JSON_REJECT_DUPLICATES, &error), &json_decref);
    if (!root) {
        std::ostringstream detail;
        detail << "Invalid JSON at line " << error.line << ", column " << error.column << ": " << error.text;
        return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue, detail.str());
    }

    std::string detail;
    if (!reject_truth_recursive(root.get(), "$", 0, detail)) {
        return ParseResult<ProductionManifest>::failure(ParseReason::kUnsupportedValue, std::move(detail));
    }
    if (!has_only_keys(root.get(),
                       {"schema_name", "schema_version", "run_id", "output_root", "datasets", "runtime",
                        "contract_bindings", "authority_profile"},
                       {"schema_name", "schema_version", "run_id", "output_root", "datasets", "runtime",
                        "contract_bindings", "authority_profile"},
                       "$", detail)) {
        return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }

    ProductionManifest manifest;
    if (!read_string(root.get(), "schema_name", "$", manifest.schema_name, detail) ||
        !read_string(root.get(), "schema_version", "$", manifest.schema_version, detail) ||
        !read_string(root.get(), "run_id", "$", manifest.run_id, detail)) {
        return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    std::string output_root;
    if (!read_string(root.get(), "output_root", "$", output_root, detail)) {
        return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    manifest.output_root = std::filesystem::path(std::move(output_root));
    std::string authority_profile;
    if (!read_string(root.get(), "authority_profile", "$", authority_profile, detail)) {
        return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue, std::move(detail));
    }
    auto parsed_profile = parse_authority_profile(authority_profile);
    if (!parsed_profile.ok()) {
        return ParseResult<ProductionManifest>::failure(parsed_profile.reason, std::move(parsed_profile.detail));
    }
    manifest.authority_profile = *parsed_profile.value;
    if (manifest.schema_name != "longlineage.production_manifest" ||
        (manifest.schema_version != "1.0.0" && manifest.schema_version != "1.1.0")) {
        return ParseResult<ProductionManifest>::failure(
            ParseReason::kUnsupportedValue,
            "Production manifest must declare longlineage.production_manifest schema version 1.0.0 or 1.1.0");
    }
    if (!is_safe_id(manifest.run_id, 128)) {
        return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue,
                                                        "$.run_id does not match the safe identifier contract");
    }
    if (manifest.output_root.empty()) {
        return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue,
                                                        "$.output_root must not be empty");
    }
    if (!validate_absolute_non_alias_path(manifest.output_root, "$.output_root", detail) ||
        manifest.output_root.filename() != manifest.run_id ||
        manifest.output_root.parent_path().filename() != ".staging") {
        return ParseResult<ProductionManifest>::failure(
            ParseReason::kMalformedValue, detail.empty()
                                              ? "$.output_root must be absolute <base>/.staging/<run_id> with exact "
                                                "run_id basename"
                                              : std::move(detail));
    }

    const json_t* datasets = json_object_get(root.get(), "datasets");
    if (!json_is_array(datasets) || json_array_size(datasets) == 0) {
        return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue,
                                                        "$.datasets must be a non-empty array");
    }
    std::set<std::string> dataset_ids;
    manifest.datasets.reserve(json_array_size(datasets));
    for (std::size_t index = 0; index < json_array_size(datasets); ++index) {
        auto dataset = parse_dataset(json_array_get(datasets, index), index);
        if (!dataset.ok()) {
            return ParseResult<ProductionManifest>::failure(dataset.reason, std::move(dataset.detail));
        }
        if (!dataset_ids.insert(dataset.value->dataset_id).second) {
            return ParseResult<ProductionManifest>::failure(ParseReason::kMalformedValue,
                                                            "Duplicate dataset_id: " + dataset.value->dataset_id);
        }
        manifest.datasets.push_back(std::move(*dataset.value));
    }

    auto runtime = parse_runtime(json_object_get(root.get(), "runtime"));
    if (!runtime.ok()) {
        return ParseResult<ProductionManifest>::failure(runtime.reason, std::move(runtime.detail));
    }
    manifest.runtime = std::move(*runtime.value);
    auto contract_bindings = parse_contract_bindings(json_object_get(root.get(), "contract_bindings"));
    if (!contract_bindings.ok()) {
        return ParseResult<ProductionManifest>::failure(contract_bindings.reason, std::move(contract_bindings.detail));
    }
    manifest.contract_bindings = std::move(*contract_bindings.value);
    if (manifest.authority_profile == AuthorityProfile::kHcc1395DatasetGate) {
        if (manifest.schema_version != "1.1.0") {
            return ParseResult<ProductionManifest>::failure(
                ParseReason::kUnsupportedValue,
                "HCC1395_DATASET_GATE requires production manifest schema version 1.1.0");
        }
        if (manifest.contract_bindings.dataset_gate_input_authority_sha256.empty()) {
            return ParseResult<ProductionManifest>::failure(
                ParseReason::kMissingRequiredField,
                "$.contract_bindings.dataset_gate_input_authority_sha256 is required for HCC1395_DATASET_GATE");
        }
    } else if (!manifest.contract_bindings.dataset_gate_input_authority_sha256.empty()) {
        return ParseResult<ProductionManifest>::failure(
            ParseReason::kUnsupportedValue,
            "$.contract_bindings.dataset_gate_input_authority_sha256 is forbidden outside HCC1395_DATASET_GATE");
    }
    return ParseResult<ProductionManifest>::success(std::move(manifest));
}

ParseResult<ProductionManifest> load_production_manifest(const std::filesystem::path& path) {
    auto snapshot = load_production_manifest_snapshot(path);
    if (!snapshot.ok() || !snapshot.value.has_value()) {
        return ParseResult<ProductionManifest>::failure(snapshot.reason, std::move(snapshot.detail));
    }
    return ParseResult<ProductionManifest>::success(std::move(snapshot.value->manifest));
}

ParseResult<ProductionManifestSnapshot> load_production_manifest_snapshot(const std::filesystem::path& path) {
    FileDescriptor descriptor(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.get() < 0) {
        return ParseResult<ProductionManifestSnapshot>::failure(ParseReason::kIoError,
                                                                "Cannot open production manifest: " + path.string());
    }
    struct stat before {};
    constexpr std::uint64_t kMaximumManifestBytes = 16U * 1024U * 1024U;
    if (::fstat(descriptor.get(), &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > kMaximumManifestBytes) {
        return ParseResult<ProductionManifestSnapshot>::failure(
            ParseReason::kIoError, "Production manifest is not a bounded regular file: " + path.string());
    }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return ParseResult<ProductionManifestSnapshot>::failure(
                ParseReason::kIoError, "Short read while capturing production manifest: " + path.string());
        }
        offset += static_cast<std::size_t>(count);
    }
    char trailing = '\0';
    ssize_t trailing_count = 0;
    do {
        trailing_count = ::read(descriptor.get(), &trailing, 1U);
    } while (trailing_count < 0 && errno == EINTR);
    struct stat after {};
    struct stat path_after {};
    if (trailing_count != 0 || ::fstat(descriptor.get(), &after) != 0 || ::stat(path.c_str(), &path_after) != 0 ||
        !same_file_identity(before, after) || !same_file_identity(after, path_after)) {
        return ParseResult<ProductionManifestSnapshot>::failure(
            ParseReason::kIoError, "Production manifest identity changed during immutable snapshot: " + path.string());
    }
    auto parsed = parse_production_manifest_json(bytes);
    if (!parsed.ok() || !parsed.value.has_value()) {
        return ParseResult<ProductionManifestSnapshot>::failure(parsed.reason, std::move(parsed.detail));
    }
    auto digest = sha256_hex(bytes);
    if (!digest.ok() || !digest.value.has_value()) {
        return ParseResult<ProductionManifestSnapshot>::failure(digest.reason, std::move(digest.detail));
    }
    return ParseResult<ProductionManifestSnapshot>::success(
        ProductionManifestSnapshot{std::move(*parsed.value), std::move(*digest.value)});
}

ParseResult<FileLockCheck> verify_locked_file(const LockedFile& locked_file) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::canonical(locked_file.path, error);
    if (error || canonical.empty()) {
        return ParseResult<FileLockCheck>::failure(ParseReason::kIoError,
                                                   "Cannot canonicalize locked input: " + locked_file.path.string());
    }
    FileDescriptor descriptor(::open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.get() < 0) {
        return ParseResult<FileLockCheck>::failure(ParseReason::kIoError,
                                                   "Cannot open canonical locked input: " + canonical.string());
    }
    struct stat before {};
    if (::fstat(descriptor.get(), &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0) {
        return ParseResult<FileLockCheck>::failure(ParseReason::kIoError,
                                                   "Cannot fstat locked regular input: " + canonical.string());
    }
    const auto size = static_cast<std::uint64_t>(before.st_size);
    if (size != locked_file.size_bytes) {
        return ParseResult<FileLockCheck>::failure(
            ParseReason::kIoError, "Locked input size mismatch for " + locked_file.path.string() + ": expected " +
                                       std::to_string(locked_file.size_bytes) + ", observed " + std::to_string(size));
    }
    const std::filesystem::path descriptor_path =
        std::filesystem::path("/proc/self/fd") / std::to_string(descriptor.get());
    auto digest = sha256_file(descriptor_path);
    if (!digest.ok()) {
        return ParseResult<FileLockCheck>::failure(digest.reason, std::move(digest.detail));
    }
    struct stat after {};
    struct stat path_after {};
    if (::fstat(descriptor.get(), &after) != 0 || ::stat(canonical.c_str(), &path_after) != 0 ||
        !same_file_identity(before, after) || !same_file_identity(after, path_after)) {
        return ParseResult<FileLockCheck>::failure(
            ParseReason::kIoError, "Locked input identity changed during SHA-256 freeze: " + canonical.string());
    }
    if (*digest.value != locked_file.sha256) {
        return ParseResult<FileLockCheck>::failure(ParseReason::kIoError,
                                                   "Locked input SHA-256 mismatch for " + locked_file.path.string());
    }
    return ParseResult<FileLockCheck>::success(FileLockCheck{
        size,
        std::move(*digest.value),
        canonical,
        static_cast<std::uint64_t>(after.st_dev),
        static_cast<std::uint64_t>(after.st_ino),
        static_cast<std::int64_t>(after.st_mtim.tv_sec),
        static_cast<std::int64_t>(after.st_mtim.tv_nsec),
        static_cast<std::int64_t>(after.st_ctim.tv_sec),
        static_cast<std::int64_t>(after.st_ctim.tv_nsec),
    });
}

}  // namespace longlineage
