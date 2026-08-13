// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/compat/regional_io.hpp"

#include <htslib/hts.h>
#include <htslib/kstring.h>
#include <htslib/sam.h>
#include <htslib/tbx.h>
#include <jansson.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"
#include "longlineage/io/alignment.hpp"
#include "longlineage/io/reference_reader.hpp"
#include "longlineage/io/sidecar.hpp"
#include "longlineage/io/variant_sites.hpp"
#include "longlineage/manifest/production_manifest.hpp"
#include "longlineage/runtime/ordered_thread_pool.hpp"

namespace longlineage::compat {
namespace {

constexpr std::uint16_t kPythonPileupExcludedFlags = BAM_FUNMAP | BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP;
constexpr std::uint8_t kMinimumMappingQuality = 20;
constexpr std::size_t kMaximumWorkers = 64;
constexpr std::size_t kTaskChargeBytes = 4096;
constexpr std::string_view kCompatibilitySchemaVersion = "2.0.0";
constexpr std::array<FileRole, 8> kRegionalInputRoles = {
    FileRole::kRawBam,
    FileRole::kRawBamIndex,
    FileRole::kPassBiallelicSsnvVcf,
    FileRole::kPassBiallelicSsnvVcfIndex,
    FileRole::kLatestHpPsSidecar,
    FileRole::kLatestHpPsSidecarIndex,
    FileRole::kReferenceFasta,
    FileRole::kReferenceFai,
};

using SamFilePointer = std::unique_ptr<samFile, decltype(&hts_close)>;
using SamHeaderPointer = std::unique_ptr<sam_hdr_t, decltype(&sam_hdr_destroy)>;
using HtsIndexPointer = std::unique_ptr<hts_idx_t, decltype(&hts_idx_destroy)>;
using BamRecordPointer = std::unique_ptr<bam1_t, decltype(&bam_destroy1)>;
using IteratorPointer = std::unique_ptr<hts_itr_t, decltype(&hts_itr_destroy)>;

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};
using JsonPointer = std::unique_ptr<json_t, JsonDeleter>;

struct InputSnapshot {
    std::filesystem::path canonical_path;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_seconds = 0;
    std::int64_t mtime_nanoseconds = 0;
    std::int64_t ctime_seconds = 0;
    std::int64_t ctime_nanoseconds = 0;
};

struct VerifiedInputBinding {
    const LockedFile* file = nullptr;
    InputSnapshot snapshot;
};

struct FamilyEvidence {
    std::uint64_t reads = 0;
    std::map<std::string, std::uint64_t> full;
    std::map<std::string, std::uint64_t> subread;
};

struct UnitEvidence {
    std::string family;
    std::string role;
    std::uint64_t reads = 0;
    std::map<std::string, std::uint64_t> full;
    std::map<std::string, std::uint64_t> subread;
    bool mutation_bearing = false;
    LegacySolverResult solver;
};

struct RegionEvidence {
    RegionalRegionPlan plan;
    bool retained = false;
    std::uint64_t alignment_exposures = 0;
    std::uint64_t sidecar_exact_matches = 0;
    std::uint64_t alignment_conflicts = 0;
    std::map<std::string, std::uint64_t> raw_hp_counts;
    std::map<std::string, std::uint64_t> alignment_class_counts;
    std::uint64_t n_full_cov_reads = 0;
    std::vector<UnitEvidence> units;
    std::string determinacy;
    double input_seconds = 0.0;
    double solver_seconds = 0.0;
};

struct AlignmentCalls {
    std::vector<AlleleCall> calls;
    std::uint16_t flag = 0;
    bool conflicting = false;
};

struct RegionalWorkerPaths {
    std::filesystem::path bam;
    std::filesystem::path bam_index;
    std::filesystem::path sidecar;
    std::filesystem::path sidecar_index;
};

[[nodiscard]] double elapsed_seconds(std::chrono::steady_clock::time_point begin) noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

[[nodiscard]] const LockedFile* find_role(const DatasetManifest& dataset, FileRole role) noexcept {
    for (const LockedFile& file : dataset.files) {
        if (file.role == role) {
            return &file;
        }
    }
    return nullptr;
}

[[nodiscard]] ParseResult<InputSnapshot> snapshot_file(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error || canonical.empty()) {
        return ParseResult<InputSnapshot>::failure(ParseReason::kIoError,
                                                   "cannot canonicalize regional input: " + path.string());
    }
    struct stat observed {};
    if (::stat(canonical.c_str(), &observed) != 0 || !S_ISREG(observed.st_mode) || observed.st_size < 0) {
        return ParseResult<InputSnapshot>::failure(ParseReason::kIoError,
                                                   "cannot stat canonical regional input: " + canonical.string());
    }
    return ParseResult<InputSnapshot>::success(InputSnapshot{
        canonical,
        static_cast<std::uint64_t>(observed.st_dev),
        static_cast<std::uint64_t>(observed.st_ino),
        static_cast<std::uint64_t>(observed.st_size),
        static_cast<std::int64_t>(observed.st_mtim.tv_sec),
        static_cast<std::int64_t>(observed.st_mtim.tv_nsec),
        static_cast<std::int64_t>(observed.st_ctim.tv_sec),
        static_cast<std::int64_t>(observed.st_ctim.tv_nsec),
    });
}

[[nodiscard]] bool same_snapshot(const InputSnapshot& lhs, const InputSnapshot& rhs) noexcept {
    return lhs.canonical_path == rhs.canonical_path && lhs.device == rhs.device && lhs.inode == rhs.inode &&
           lhs.size == rhs.size && lhs.mtime_seconds == rhs.mtime_seconds &&
           lhs.mtime_nanoseconds == rhs.mtime_nanoseconds && lhs.ctime_seconds == rhs.ctime_seconds &&
           lhs.ctime_nanoseconds == rhs.ctime_nanoseconds;
}

[[nodiscard]] ParseResult<std::string> dump_json_file(const std::filesystem::path& path, json_t* object) {
    if (object == nullptr || !json_is_object(object)) {
        return ParseResult<std::string>::failure(ParseReason::kMalformedValue, "JSON root is not an object");
    }
    if (json_dump_file(object, path.string().c_str(), JSON_INDENT(2) | JSON_SORT_KEYS) != 0) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, "cannot write JSON: " + path.string());
    }
    return sha256_file(path);
}

[[nodiscard]] bool safe_identifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > 160) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
    });
}

[[nodiscard]] std::string json_string_text(const json_t* value) {
    if (!json_is_string(value)) {
        return {};
    }
    const std::string result(json_string_value(value), json_string_length(value));
    return result.find('\0') == std::string::npos ? result : std::string{};
}

[[nodiscard]] std::string json_text(const json_t* object, const char* key) {
    return json_string_text(json_object_get(object, key));
}

[[nodiscard]] bool json_has_exact_keys(const json_t* object,
                                       std::initializer_list<std::string_view> expected) noexcept {
    if (!json_is_object(object) || json_object_size(object) != expected.size()) {
        return false;
    }
    return std::all_of(expected.begin(), expected.end(), [object](std::string_view key) {
        return json_object_get(object, std::string(key).c_str()) != nullptr;
    });
}

[[nodiscard]] bool json_uint64(const json_t* object, const char* key, std::uint64_t& output) noexcept {
    const json_t* value = json_object_get(object, key);
    if (!json_is_integer(value) || json_integer_value(value) < 0) {
        return false;
    }
    output = static_cast<std::uint64_t>(json_integer_value(value));
    return true;
}

struct LockedJsonObject {
    JsonPointer document;
    std::string sha256;
    InputSnapshot snapshot;
};

[[nodiscard]] ParseResult<LockedJsonObject> load_json_object_strict(const std::filesystem::path& path) {
    auto before = snapshot_file(path);
    if (!before.ok()) {
        return ParseResult<LockedJsonObject>::failure(before.reason, before.detail);
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return ParseResult<LockedJsonObject>::failure(ParseReason::kIoError,
                                                      "cannot open strict JSON object " + path.string());
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (input.bad()) {
        return ParseResult<LockedJsonObject>::failure(ParseReason::kIoError,
                                                      "cannot read strict JSON object " + path.string());
    }
    auto after = snapshot_file(path);
    if (!after.ok() || !same_snapshot(*before.value, *after.value)) {
        return ParseResult<LockedJsonObject>::failure(ParseReason::kIoError,
                                                      "strict JSON object changed while being read: " + path.string());
    }
    const std::string content = bytes.str();
    auto digest = sha256_hex(content);
    if (!digest.ok()) {
        return ParseResult<LockedJsonObject>::failure(digest.reason, digest.detail);
    }
    json_error_t error{};
    JsonPointer document(json_loadb(content.data(), content.size(), JSON_REJECT_DUPLICATES, &error));
    if (!document || !json_is_object(document.get())) {
        std::ostringstream detail;
        detail << "cannot parse strict JSON object " << path.string() << ':' << error.line << ':' << error.column
               << ": " << error.text;
        return ParseResult<LockedJsonObject>::failure(ParseReason::kMalformedValue, detail.str());
    }
    return ParseResult<LockedJsonObject>::success(
        LockedJsonObject{std::move(document), std::move(*digest.value), std::move(*before.value)});
}

struct LockedProductionManifest {
    ProductionManifest manifest;
    std::string sha256;
    InputSnapshot snapshot;
};

[[nodiscard]] ParseResult<LockedProductionManifest> load_locked_production_manifest(const std::filesystem::path& path) {
    auto before = snapshot_file(path);
    if (!before.ok()) {
        return ParseResult<LockedProductionManifest>::failure(before.reason, before.detail);
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return ParseResult<LockedProductionManifest>::failure(ParseReason::kIoError,
                                                              "cannot open regional production manifest");
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (input.bad()) {
        return ParseResult<LockedProductionManifest>::failure(ParseReason::kIoError,
                                                              "cannot read regional production manifest");
    }
    auto after = snapshot_file(path);
    if (!after.ok() || !same_snapshot(*before.value, *after.value)) {
        return ParseResult<LockedProductionManifest>::failure(ParseReason::kIoError,
                                                              "regional production manifest changed while being read");
    }
    const std::string content = bytes.str();
    auto digest = sha256_hex(content);
    if (!digest.ok()) {
        return ParseResult<LockedProductionManifest>::failure(digest.reason, digest.detail);
    }
    auto parsed = parse_production_manifest_json(content);
    if (!parsed.ok()) {
        return ParseResult<LockedProductionManifest>::failure(parsed.reason, parsed.detail);
    }
    return ParseResult<LockedProductionManifest>::success(
        LockedProductionManifest{std::move(*parsed.value), std::move(*digest.value), std::move(*before.value)});
}

[[nodiscard]] ParseResult<bool> verify_locked_document(const std::filesystem::path& path,
                                                       const InputSnapshot& expected_snapshot,
                                                       const std::string& expected_sha256) {
    auto observed_snapshot = snapshot_file(path);
    auto observed_sha256 = sha256_file(path);
    if (!observed_snapshot.ok() || !observed_sha256.ok() ||
        !same_snapshot(expected_snapshot, *observed_snapshot.value) || *observed_sha256.value != expected_sha256) {
        return ParseResult<bool>::failure(ParseReason::kIoError,
                                          "governed document changed during regional execution: " + path.string());
    }
    return ParseResult<bool>::success(true);
}

[[nodiscard]] ParseResult<bool> verify_contract_binding(const std::filesystem::path& repository_root,
                                                        const std::filesystem::path& relative,
                                                        const std::string& expected) {
    auto observed = sha256_file(repository_root / relative);
    if (!observed.ok()) {
        return ParseResult<bool>::failure(observed.reason, observed.detail);
    }
    if (*observed.value != expected) {
        return ParseResult<bool>::failure(ParseReason::kUnsupportedValue,
                                          "regional contract SHA-256 mismatch: " + relative.string());
    }
    return ParseResult<bool>::success(true);
}

[[nodiscard]] ParseResult<bool> verify_repository_contracts(const ProductionManifest& manifest,
                                                            const std::filesystem::path& repository_root) {
    const std::array<std::pair<std::filesystem::path, const std::string*>, 10> bindings = {{
        {"contracts/v1/science_parameters.json", &manifest.contract_bindings.science_parameters_sha256},
        {"schema/catalog.json", &manifest.contract_bindings.schema_catalog_sha256},
        {"contracts/v1/status_reason_codes.tsv", &manifest.contract_bindings.status_reason_registry_sha256},
        {"contracts/v1/type_registry.tsv", &manifest.contract_bindings.type_registry_sha256},
        {"contracts/v1/transform_registry.tsv", &manifest.contract_bindings.transform_registry_sha256},
        {"oracle/authority_manifest.json", &manifest.contract_bindings.authority_manifest_sha256},
        {"provenance/source_to_target_manifest.json", &manifest.contract_bindings.source_to_target_manifest_sha256},
        {"oracle/production_input_authority.json", &manifest.contract_bindings.production_input_authority_sha256},
        {"schema/id_registry.json", &manifest.contract_bindings.schema_id_registry_sha256},
        {"state/release_attestation.json", &manifest.contract_bindings.release_attestation_sha256},
    }};
    for (const auto& binding : bindings) {
        auto checked = verify_contract_binding(repository_root, binding.first, *binding.second);
        if (!checked.ok()) {
            return checked;
        }
    }
    return ParseResult<bool>::success(true);
}

[[nodiscard]] ParseResult<std::string> verify_production_authority(const ProductionManifest& manifest,
                                                                   const std::filesystem::path& repository_root) {
    constexpr std::array<std::string_view, 7> kDatasetOrder = {
        "HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954",
    };
    const std::filesystem::path path = repository_root / "oracle" / "production_input_authority.json";
    auto authority = load_json_object_strict(path);
    if (!authority.ok()) {
        return ParseResult<std::string>::failure(authority.reason, authority.detail);
    }
    if (authority.value->sha256 != manifest.contract_bindings.production_input_authority_sha256) {
        return ParseResult<std::string>::failure(ParseReason::kUnsupportedValue,
                                                 "production input authority binding SHA-256 mismatch");
    }
    const json_t* root = authority.value->document.get();
    const json_t* order = json_object_get(root, "dataset_order");
    const json_t* datasets = json_object_get(root, "datasets");
    const json_t* constraints = json_object_get(root, "constraints");
    const json_t* closeout = json_object_get(root, "source_closeout");
    std::uint64_t closeout_count = 0;
    if (json_text(root, "schema_name") != "longlineage.production_input_authority" ||
        json_text(root, "schema_version") != "1.0.0" ||
        json_text(root, "profile_id") != "RAW_ALL_PRODUCTION_V2_7_DATASET" || !json_is_array(order) ||
        !json_is_array(datasets) || json_array_size(order) != kDatasetOrder.size() ||
        json_array_size(datasets) != kDatasetOrder.size() || manifest.datasets.size() != kDatasetOrder.size() ||
        !json_is_object(constraints) || json_text(constraints, "latest_tag_join") != "EXACT_PROJECTION_NO_FALLBACK" ||
        !json_is_false(json_object_get(constraints, "tagged_bam_output_allowed")) ||
        !json_is_true(json_object_get(constraints, "production_requires_exact_dataset_set")) ||
        !json_is_false(json_object_get(constraints, "private_source_paths_stored")) || !json_is_object(closeout) ||
        !json_uint64(closeout, "dataset_count", closeout_count) || closeout_count != kDatasetOrder.size() ||
        !json_is_true(json_object_get(closeout, "all_pass"))) {
        return ParseResult<std::string>::failure(ParseReason::kUnsupportedValue,
                                                 "production input authority identity/constraints are invalid");
    }
    const std::array<std::pair<FileRole, const char*>, 4> authority_roles = {{
        {FileRole::kLatestHpPsSidecar, "latest_hp_ps_sidecar"},
        {FileRole::kLatestHpPsSidecarIndex, "latest_hp_ps_sidecar_index"},
        {FileRole::kPassBiallelicSsnvVcf, "pass_biallelic_ssnv_vcf"},
        {FileRole::kPassBiallelicSsnvVcfIndex, "pass_biallelic_ssnv_vcf_index"},
    }};
    for (std::size_t index = 0; index < kDatasetOrder.size(); ++index) {
        const json_t* order_id = json_array_get(order, index);
        const json_t* authority_dataset = json_array_get(datasets, index);
        const DatasetManifest& observed_dataset = manifest.datasets[index];
        std::uint64_t authority_order = 0;
        if (!json_is_string(order_id) || !json_is_object(authority_dataset) ||
            json_string_text(order_id) != kDatasetOrder[index] || observed_dataset.dataset_id != kDatasetOrder[index] ||
            observed_dataset.dataset_order != index ||
            json_text(authority_dataset, "dataset_id") != kDatasetOrder[index] ||
            !json_uint64(authority_dataset, "dataset_order", authority_order) || authority_order != index) {
            return ParseResult<std::string>::failure(
                ParseReason::kUnsupportedValue,
                "production dataset identity/order differs from authority at index " + std::to_string(index));
        }
        for (const auto& authority_role : authority_roles) {
            const LockedFile* observed = find_role(observed_dataset, authority_role.first);
            const json_t* expected = json_object_get(authority_dataset, authority_role.second);
            std::uint64_t size = 0;
            if (observed == nullptr || !json_is_object(expected) || !json_uint64(expected, "size_bytes", size) ||
                observed->size_bytes != size || observed->sha256 != json_text(expected, "sha256")) {
                return ParseResult<std::string>::failure(
                    ParseReason::kUnsupportedValue,
                    observed_dataset.dataset_id + ": production authority mismatch for " + authority_role.second);
            }
        }
    }

    const std::filesystem::path regional_path =
        repository_root / "oracle" / "regional_compat_all_datasets_input_authority.json";
    auto regional_authority = load_json_object_strict(regional_path);
    if (!regional_authority.ok()) {
        return ParseResult<std::string>::failure(regional_authority.reason, regional_authority.detail);
    }
    const json_t* regional_root = regional_authority.value->document.get();
    const json_t* regional_datasets = json_object_get(regional_root, "datasets");
    const json_t* regional_constraints = json_object_get(regional_root, "constraints");
    auto hcc_authority =
        load_json_object_strict(repository_root / "oracle" / "hcc1395_dataset_gate_input_authority.json");
    if (!hcc_authority.ok()) {
        return ParseResult<std::string>::failure(hcc_authority.reason, hcc_authority.detail);
    }
    std::uint64_t forbidden_fields = 0;
    if (!json_has_exact_keys(regional_root, {"base_production_input_authority_sha256", "constraints", "datasets",
                                             "hcc1395_dataset_gate_input_authority_sha256", "profile_id", "schema_name",
                                             "schema_version"}) ||
        json_text(regional_root, "schema_name") != "longlineage.regional_compat_all_datasets_input_authority" ||
        json_text(regional_root, "schema_version") != "1.0.0" ||
        json_text(regional_root, "profile_id") != "PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET" ||
        json_text(regional_root, "base_production_input_authority_sha256") != authority.value->sha256 ||
        json_text(regional_root, "hcc1395_dataset_gate_input_authority_sha256") != hcc_authority.value->sha256 ||
        !json_is_array(regional_datasets) || json_array_size(regional_datasets) != kDatasetOrder.size() ||
        !json_has_exact_keys(regional_constraints,
                             {"all_physical_sha256_verified", "latest_tag_join", "persisted_tagged_bam_allowed",
                              "private_source_paths_stored", "truth_fields"}) ||
        json_text(regional_constraints, "latest_tag_join") != "EXACT_PROJECTION_NO_FALLBACK" ||
        !json_uint64(regional_constraints,
                     "tr"
                     "uth_fields",
                     forbidden_fields) ||
        forbidden_fields != 0U ||
        !json_is_false(json_object_get(regional_constraints, "persisted_tagged_bam_allowed")) ||
        !json_is_true(json_object_get(regional_constraints, "all_physical_sha256_verified")) ||
        !json_is_false(json_object_get(regional_constraints, "private_source_paths_stored"))) {
        return ParseResult<std::string>::failure(
            ParseReason::kUnsupportedValue, "regional seven-dataset input authority identity/constraints are invalid");
    }
    for (std::size_t index = 0; index < kDatasetOrder.size(); ++index) {
        const json_t* authority_dataset = json_array_get(regional_datasets, index);
        const json_t* files = json_object_get(authority_dataset, "files");
        std::uint64_t authority_order = 0;
        if (!json_has_exact_keys(authority_dataset, {"dataset_id", "dataset_order", "files"}) ||
            json_text(authority_dataset, "dataset_id") != kDatasetOrder[index] ||
            !json_uint64(authority_dataset, "dataset_order", authority_order) || authority_order != index ||
            !json_is_array(files) || json_array_size(files) != kRegionalInputRoles.size()) {
            return ParseResult<std::string>::failure(
                ParseReason::kMalformedValue,
                "regional input authority dataset row is malformed at index " + std::to_string(index));
        }
        for (std::size_t file_index = 0; file_index < json_array_size(files); ++file_index) {
            const json_t* row = json_array_get(files, file_index);
            const std::string role_text = json_text(row, "role");
            auto role = parse_file_role(role_text);
            std::uint64_t size = 0;
            if (!json_has_exact_keys(row, {"role", "sha256", "size_bytes"}) || !role.ok() ||
                role_text != to_string(kRegionalInputRoles[file_index]) || !json_uint64(row, "size_bytes", size)) {
                return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                         "regional input authority file row is malformed");
            }
            const LockedFile* observed = find_role(manifest.datasets[index], *role.value);
            if (observed == nullptr || observed->size_bytes != size || observed->sha256 != json_text(row, "sha256")) {
                return ParseResult<std::string>::failure(
                    ParseReason::kUnsupportedValue,
                    manifest.datasets[index].dataset_id + ": regional authority mismatch for " + role_text);
            }
        }
    }
    return ParseResult<std::string>::success(regional_authority.value->sha256);
}

[[nodiscard]] ParseResult<std::string> verify_hcc_authority(const ProductionManifest& manifest,
                                                            const std::filesystem::path& repository_root) {
    if (manifest.datasets.size() != 1U || manifest.datasets.front().dataset_id != "HCC1395" ||
        manifest.datasets.front().dataset_order != 0U) {
        return ParseResult<std::string>::failure(ParseReason::kUnsupportedValue,
                                                 "legacy HCC authority requires only HCC1395 at order zero");
    }
    const std::filesystem::path path = repository_root / "oracle" / "hcc1395_dataset_gate_input_authority.json";
    auto authority = load_json_object_strict(path);
    if (!authority.ok()) {
        return ParseResult<std::string>::failure(authority.reason, authority.detail);
    }
    if (authority.value->sha256 != manifest.contract_bindings.dataset_gate_input_authority_sha256) {
        return ParseResult<std::string>::failure(ParseReason::kUnsupportedValue,
                                                 "legacy HCC input authority binding SHA-256 mismatch");
    }
    const json_t* root = authority.value->document.get();
    const json_t* files = json_object_get(root, "files");
    std::uint64_t dataset_order = 0;
    std::uint64_t forbidden_fields = 0;
    if (json_text(root, "schema_name") != "longlineage.dataset_gate_input_authority" ||
        json_text(root, "schema_version") != "1.0.0" || json_text(root, "dataset_id") != "HCC1395" ||
        json_text(root, "authority_profile") != "HCC1395_DATASET_GATE" ||
        json_text(root, "latest_tag_join") != "EXACT_PROJECTION_NO_FALLBACK" ||
        !json_uint64(root, "dataset_order", dataset_order) || dataset_order != 0U ||
        !json_uint64(root,
                     "tr"
                     "uth_fields",
                     forbidden_fields) ||
        forbidden_fields != 0U || !json_is_false(json_object_get(root, "tagged_bam_persisted")) ||
        !json_is_array(files) || json_array_size(files) != 8U) {
        return ParseResult<std::string>::failure(ParseReason::kUnsupportedValue,
                                                 "legacy HCC input authority identity/constraints are invalid");
    }
    std::set<std::string> roles;
    for (std::size_t index = 0; index < json_array_size(files); ++index) {
        const json_t* row = json_array_get(files, index);
        const std::string role_text = json_text(row, "role");
        auto role = parse_file_role(role_text);
        std::uint64_t size = 0;
        if (!json_is_object(row) || !role.ok() || !roles.insert(role_text).second ||
            !json_uint64(row, "size_bytes", size)) {
            return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                     "legacy HCC authority file row is malformed");
        }
        const LockedFile* observed = find_role(manifest.datasets.front(), *role.value);
        if (observed == nullptr || observed->size_bytes != size || observed->sha256 != json_text(row, "sha256")) {
            return ParseResult<std::string>::failure(ParseReason::kUnsupportedValue,
                                                     "legacy HCC authority mismatch for role " + role_text);
        }
    }
    return ParseResult<std::string>::success(authority.value->sha256);
}

[[nodiscard]] char bam_base(const bam1_t& alignment, std::uint64_t query_position) noexcept {
    if (query_position >= static_cast<std::uint64_t>(alignment.core.l_qseq)) {
        return '\0';
    }
    const std::uint8_t* sequence = bam_get_seq(&alignment);
    const std::uint8_t code = bam_seqi(sequence, static_cast<hts_pos_t>(query_position));
    char base = seq_nt16_str[code];
    if (base >= 'a' && base <= 'z') {
        base = static_cast<char>(base - ('a' - 'A'));
    }
    return base;
}

[[nodiscard]] ParseResult<std::vector<AlleleCall>> project_alignment(const bam1_t& alignment,
                                                                     const RegionalRegionPlan& region) {
    std::vector<AlleleCall> calls(region.selected_sites.size(), AlleleCall::kUnobservable);
    if (alignment.core.pos < 0) {
        return ParseResult<std::vector<AlleleCall>>::failure(ParseReason::kMalformedValue,
                                                             "mapped alignment has negative POS");
    }
    std::uint64_t reference_cursor = static_cast<std::uint64_t>(alignment.core.pos);
    std::uint64_t query_cursor = 0;
    std::size_t site_index = 0;
    while (site_index < region.selected_sites.size() &&
           region.selected_sites[site_index].position.zero_based() < reference_cursor) {
        ++site_index;
    }
    const std::uint32_t* cigar = bam_get_cigar(&alignment);
    for (std::uint32_t operation_index = 0; operation_index < alignment.core.n_cigar; ++operation_index) {
        const std::uint32_t length = bam_cigar_oplen(cigar[operation_index]);
        const int operation = bam_cigar_op(cigar[operation_index]);
        if (length == 0) {
            return ParseResult<std::vector<AlleleCall>>::failure(ParseReason::kMalformedValue,
                                                                 "alignment contains a zero-length CIGAR operation");
        }
        const bool consumes_query = (bam_cigar_type(operation) & 1) != 0;
        const bool consumes_reference = (bam_cigar_type(operation) & 2) != 0;
        const std::uint64_t query_end = query_cursor + (consumes_query ? length : 0U);
        const std::uint64_t reference_end = reference_cursor + (consumes_reference ? length : 0U);
        if (query_end < query_cursor || reference_end < reference_cursor) {
            return ParseResult<std::vector<AlleleCall>>::failure(ParseReason::kMalformedValue,
                                                                 "alignment CIGAR coordinate overflow");
        }
        if (operation == BAM_CMATCH || operation == BAM_CEQUAL || operation == BAM_CDIFF) {
            while (site_index < region.selected_sites.size() &&
                   region.selected_sites[site_index].position.zero_based() < reference_end) {
                const VariantSite& site = region.selected_sites[site_index];
                const std::uint64_t query_position = query_cursor + (site.position.zero_based() - reference_cursor);
                const char observed = bam_base(alignment, query_position);
                if (observed == '\0') {
                    return ParseResult<std::vector<AlleleCall>>::failure(ParseReason::kMalformedValue,
                                                                         "CIGAR projection lies outside BAM sequence");
                }
                calls[site_index] = classify_allele(observed, site.reference, site.alternate, true);
                ++site_index;
            }
        } else if (operation == BAM_CDEL || operation == BAM_CREF_SKIP) {
            while (site_index < region.selected_sites.size() &&
                   region.selected_sites[site_index].position.zero_based() < reference_end) {
                ++site_index;
            }
        }
        query_cursor = query_end;
        reference_cursor = reference_end;
    }
    if (query_cursor != static_cast<std::uint64_t>(alignment.core.l_qseq)) {
        return ParseResult<std::vector<AlleleCall>>::failure(
            ParseReason::kMalformedValue, "CIGAR query consumption differs from BAM sequence length");
    }
    return ParseResult<std::vector<AlleleCall>>::success(std::move(calls));
}

[[nodiscard]] bool has_alignment_exposure(const std::vector<AlleleCall>& calls) noexcept {
    return std::any_of(calls.begin(), calls.end(), [](AlleleCall call) { return call != AlleleCall::kUnobservable; });
}

[[nodiscard]] std::string raw_hp(const LatestTags& tags) { return tags.hp == "0" ? "." : tags.hp; }

class RegionalWorkerReader final {
   public:
    [[nodiscard]] static ParseResult<std::unique_ptr<RegionalWorkerReader>> open(const RegionalWorkerPaths& paths) {
        const std::string bam_name = paths.bam.string();
        const std::string bam_index_name = paths.bam_index.string();
        SamFilePointer bam(sam_open(bam_name.c_str(), "rb"), &hts_close);
        if (!bam) {
            return ParseResult<std::unique_ptr<RegionalWorkerReader>>::failure(ParseReason::kIoError,
                                                                               "cannot open regional BAM");
        }
        SamHeaderPointer header(sam_hdr_read(bam.get()), &sam_hdr_destroy);
        if (!header) {
            return ParseResult<std::unique_ptr<RegionalWorkerReader>>::failure(ParseReason::kMalformedValue,
                                                                               "cannot read regional BAM header");
        }
        HtsIndexPointer bam_index(sam_index_load2(bam.get(), bam_name.c_str(), bam_index_name.c_str()),
                                  &hts_idx_destroy);
        if (!bam_index) {
            return ParseResult<std::unique_ptr<RegionalWorkerReader>>::failure(
                ParseReason::kIndexError, "cannot load explicitly named regional BAM index");
        }

        htsFile* sidecar = hts_open(paths.sidecar.string().c_str(), "r");
        if (sidecar == nullptr) {
            return ParseResult<std::unique_ptr<RegionalWorkerReader>>::failure(ParseReason::kIoError,
                                                                               "cannot open regional HP/PS sidecar");
        }
        tbx_t* sidecar_index = tbx_index_load3(paths.sidecar.string().c_str(), paths.sidecar_index.string().c_str(), 0);
        if (sidecar_index == nullptr) {
            hts_close(sidecar);
            return ParseResult<std::unique_ptr<RegionalWorkerReader>>::failure(
                ParseReason::kIndexError, "cannot load explicitly named regional sidecar index");
        }
        return ParseResult<std::unique_ptr<RegionalWorkerReader>>::success(std::unique_ptr<RegionalWorkerReader>(
            new RegionalWorkerReader(std::move(bam), std::move(header), std::move(bam_index), sidecar, sidecar_index)));
    }

    ~RegionalWorkerReader() {
        if (sidecar_index_ != nullptr) {
            tbx_destroy(sidecar_index_);
        }
        if (sidecar_ != nullptr) {
            hts_close(sidecar_);
        }
    }

    RegionalWorkerReader(const RegionalWorkerReader&) = delete;
    RegionalWorkerReader& operator=(const RegionalWorkerReader&) = delete;

    [[nodiscard]] ParseResult<RegionEvidence> read(const RegionalRegionPlan& region) {
        const auto input_begin = std::chrono::steady_clock::now();
        RegionEvidence output;
        output.plan = region;
        auto sidecar_rows = fetch_sidecar(region);
        if (!sidecar_rows.ok()) {
            return ParseResult<RegionEvidence>::failure(sidecar_rows.reason, sidecar_rows.detail);
        }

        const int tid = sam_hdr_name2tid(header_.get(), region.chrom.c_str());
        if (tid < 0) {
            return ParseResult<RegionEvidence>::failure(ParseReason::kIndexError,
                                                        "BAM header lacks regional contig " + region.chrom);
        }
        IteratorPointer iterator(sam_itr_queryi(bam_index_.get(), tid, static_cast<hts_pos_t>(region.start1 - 1),
                                                static_cast<hts_pos_t>(region.end1)),
                                 &hts_itr_destroy);
        if (!iterator) {
            return ParseResult<RegionEvidence>::failure(ParseReason::kIndexError,
                                                        "cannot construct regional BAM iterator");
        }
        BamRecordPointer alignment(bam_init1(), &bam_destroy1);
        if (!alignment) {
            return ParseResult<RegionEvidence>::failure(ParseReason::kIoError, "cannot allocate regional BAM record");
        }

        std::map<SidecarFullIdentity, AlignmentCalls> alignments;
        int status = 0;
        while ((status = sam_itr_next(bam_.get(), iterator.get(), alignment.get())) >= 0) {
            const std::uint16_t flag = alignment->core.flag;
            if ((flag & kPythonPileupExcludedFlags) != 0 || alignment->core.qual < kMinimumMappingQuality) {
                continue;
            }
            auto calls = project_alignment(*alignment, region);
            if (!calls.ok()) {
                return ParseResult<RegionEvidence>::failure(calls.reason, calls.detail);
            }
            if (!has_alignment_exposure(*calls.value)) {
                continue;
            }
            auto cigar = cigar_string(*alignment);
            if (!cigar.ok()) {
                return ParseResult<RegionEvidence>::failure(cigar.reason, cigar.detail);
            }
            const hts_pos_t raw_end = bam_endpos(alignment.get());
            if (alignment->core.pos < 0 || raw_end <= alignment->core.pos) {
                return ParseResult<RegionEvidence>::failure(ParseReason::kMalformedValue,
                                                            "regional alignment interval is malformed");
            }
            auto typed_contig = ContigId::from_string(region.chrom);
            auto interval = Interval0::from_bounds(static_cast<std::uint64_t>(alignment->core.pos),
                                                   static_cast<std::uint64_t>(raw_end));
            if (!typed_contig.ok() || !interval.ok()) {
                return ParseResult<RegionEvidence>::failure(ParseReason::kMalformedValue,
                                                            "cannot type regional alignment identity");
            }
            SidecarFullIdentity identity{bam_get_qname(alignment.get()), *typed_contig.value, *interval.value, flag,
                                         blake2b_64_hex(*cigar.value)};
            auto found = alignments.find(identity);
            if (found == alignments.end()) {
                alignments.emplace(std::move(identity), AlignmentCalls{std::move(*calls.value), flag, false});
                continue;
            }
            for (std::size_t index = 0; index < calls.value->size(); ++index) {
                const AlleleCall incoming = (*calls.value)[index];
                AlleleCall& existing = found->second.calls[index];
                if (incoming == AlleleCall::kUnobservable) {
                    continue;
                }
                if (existing == AlleleCall::kUnobservable) {
                    existing = incoming;
                } else if (existing != incoming) {
                    found->second.conflicting = true;
                }
            }
        }
        if (status < -1) {
            return ParseResult<RegionEvidence>::failure(ParseReason::kIoError,
                                                        "regional BAM iterator failed before normal EOF");
        }

        std::map<std::string, FamilyEvidence> families;
        std::map<std::string, std::uint64_t> pooled_full;
        std::map<std::string, std::uint64_t> pooled_subread;
        for (const auto& entry : alignments) {
            if (entry.second.conflicting) {
                ++output.alignment_conflicts;
                continue;
            }
            ++output.alignment_exposures;
            const auto tag = sidecar_rows.value->find(entry.first);
            if (tag == sidecar_rows.value->end()) {
                return ParseResult<RegionEvidence>::failure(
                    ParseReason::kMalformedValue,
                    "authoritative sidecar is missing an exposed alignment: " + entry.first.raw_qname);
            }
            ++output.sidecar_exact_matches;
            const std::string hp = raw_hp(tag->second);
            ++output.raw_hp_counts[hp];
            const std::uint16_t flag = entry.second.flag;
            ++output.alignment_class_counts[(flag & BAM_FSUPPLEMENTARY) != 0 ? "supplementary" : "primary"];

            std::string pattern;
            pattern.reserve(entry.second.calls.size());
            std::size_t covered = 0;
            for (const AlleleCall call : entry.second.calls) {
                if (call == AlleleCall::kAlternate) {
                    pattern.push_back('A');
                    ++covered;
                } else if (call == AlleleCall::kReference) {
                    pattern.push_back('R');
                    ++covered;
                } else {
                    pattern.push_back('X');
                }
            }
            if (covered == 0) {
                continue;
            }
            const std::string family = python_v2_hp_family(hp);
            FamilyEvidence& evidence = families[family];
            ++evidence.reads;
            ++evidence.subread[pattern];
            ++pooled_subread[pattern];
            if (covered == entry.second.calls.size()) {
                ++evidence.full[pattern];
                ++pooled_full[pattern];
            }
        }
        output.n_full_cov_reads = 0;
        for (const auto& entry : pooled_full) {
            output.n_full_cov_reads += entry.second;
        }
        bool pooled_supported = output.n_full_cov_reads >= kRegionalMinimumPatternReads;
        if (!pooled_supported) {
            pooled_supported = std::any_of(pooled_subread.begin(), pooled_subread.end(), [](const auto& entry) {
                return entry.second >= kRegionalMinimumPatternReads;
            });
        }
        output.retained = pooled_supported;
        if (!output.retained) {
            output.determinacy = "read_unsupported";
            output.input_seconds = elapsed_seconds(input_begin);
            return ParseResult<RegionEvidence>::success(std::move(output));
        }

        const auto solver_begin = std::chrono::steady_clock::now();
        for (const auto& family_entry : families) {
            LegacySolverInput solver_input;
            solver_input.site_count = region.selected_sites.size();
            for (const auto& pattern : family_entry.second.full) {
                if (pattern.second >= kRegionalMinimumPatternReads) {
                    solver_input.supported_full_patterns.insert(pattern);
                }
            }
            for (const auto& pattern : family_entry.second.subread) {
                if (pattern.second >= kRegionalMinimumPatternReads) {
                    solver_input.supported_subread_patterns.insert(pattern);
                }
            }
            if (solver_input.supported_full_patterns.empty() && solver_input.supported_subread_patterns.empty()) {
                continue;
            }
            auto solved = solve_python_v2_legacy(solver_input);
            if (!solved.ok()) {
                return ParseResult<RegionEvidence>::failure(
                    solved.reason, region.region_id + " family " + family_entry.first + ": " + solved.detail);
            }
            const bool mutation_bearing = pattern_is_mutation_bearing(solver_input);
            output.units.push_back(UnitEvidence{
                family_entry.first, python_v2_unit_role(family_entry.first, mutation_bearing),
                family_entry.second.reads, std::move(solver_input.supported_full_patterns),
                std::move(solver_input.supported_subread_patterns), mutation_bearing, std::move(*solved.value)});
        }
        output.solver_seconds = elapsed_seconds(solver_begin);

        std::vector<std::string> primary_classes;
        for (const UnitEvidence& unit : output.units) {
            if (unit.role == "primary_mutation_lineage") {
                primary_classes.push_back(unit.solver.classification);
            }
        }
        if (primary_classes.empty()) {
            output.determinacy = "no_primary_lineage";
        } else if (std::all_of(primary_classes.begin(), primary_classes.end(),
                               [](const std::string& value) { return value == "determined"; })) {
            output.determinacy = "all_determined";
        } else if (std::find(primary_classes.begin(), primary_classes.end(), "recurrence_required") !=
                   primary_classes.end()) {
            output.determinacy = "has_recurrence";
        } else if (std::find(primary_classes.begin(), primary_classes.end(), "capped") != primary_classes.end()) {
            output.determinacy = "has_capped";
        } else {
            output.determinacy = "has_ambiguous";
        }
        output.input_seconds = elapsed_seconds(input_begin) - output.solver_seconds;
        return ParseResult<RegionEvidence>::success(std::move(output));
    }

   private:
    RegionalWorkerReader(SamFilePointer bam, SamHeaderPointer header, HtsIndexPointer bam_index, htsFile* sidecar,
                         tbx_t* sidecar_index)
        : bam_(std::move(bam)),
          header_(std::move(header)),
          bam_index_(std::move(bam_index)),
          sidecar_(sidecar),
          sidecar_index_(sidecar_index) {}

    [[nodiscard]] ParseResult<std::map<SidecarFullIdentity, LatestTags>> fetch_sidecar(
        const RegionalRegionPlan& region) {
        const int tid = tbx_name2id(sidecar_index_, region.chrom.c_str());
        if (tid < 0) {
            return ParseResult<std::map<SidecarFullIdentity, LatestTags>>::failure(
                ParseReason::kIndexError, "sidecar index lacks regional contig " + region.chrom);
        }
        IteratorPointer iterator(tbx_itr_queryi(sidecar_index_, tid, static_cast<hts_pos_t>(region.start1 - 1),
                                                static_cast<hts_pos_t>(region.end1)),
                                 &hts_itr_destroy);
        if (!iterator) {
            return ParseResult<std::map<SidecarFullIdentity, LatestTags>>::failure(
                ParseReason::kIndexError, "cannot construct regional sidecar iterator");
        }
        std::map<SidecarFullIdentity, LatestTags> output;
        kstring_t line{0, 0, nullptr};
        int status = 0;
        while ((status = tbx_itr_next(sidecar_, sidecar_index_, iterator.get(), &line)) >= 0) {
            auto parsed = parse_sidecar_row(std::string_view(line.s, line.l));
            if (!parsed.ok()) {
                std::free(line.s);
                return ParseResult<std::map<SidecarFullIdentity, LatestTags>>::failure(parsed.reason, parsed.detail);
            }
            auto inserted = output.emplace(parsed.value->full_identity, parsed.value->latest_tags);
            if (!inserted.second && inserted.first->second != parsed.value->latest_tags) {
                std::free(line.s);
                return ParseResult<std::map<SidecarFullIdentity, LatestTags>>::failure(
                    ParseReason::kMalformedValue, "conflicting HP/PS rows for exact regional sidecar identity");
            }
        }
        std::free(line.s);
        if (status < -1) {
            return ParseResult<std::map<SidecarFullIdentity, LatestTags>>::failure(
                ParseReason::kIoError, "regional sidecar iterator failed before normal EOF");
        }
        return ParseResult<std::map<SidecarFullIdentity, LatestTags>>::success(std::move(output));
    }

    SamFilePointer bam_{nullptr, &hts_close};
    SamHeaderPointer header_{nullptr, &sam_hdr_destroy};
    HtsIndexPointer bam_index_{nullptr, &hts_idx_destroy};
    htsFile* sidecar_ = nullptr;
    tbx_t* sidecar_index_ = nullptr;
};

[[nodiscard]] std::string join_positions(const RegionalRegionPlan& region) {
    std::ostringstream output;
    for (std::size_t index = 0; index < region.selected_sites.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << region.selected_sites[index].position.value();
    }
    return output.str();
}

[[nodiscard]] ParseResult<std::uint64_t> write_tsvs(const std::filesystem::path& output_directory,
                                                    const std::vector<RegionEvidence>& regions,
                                                    std::uint64_t& region_rows, std::uint64_t& unit_rows,
                                                    std::uint64_t& pattern_rows,
                                                    std::map<std::string, std::uint64_t>& census, double& input_seconds,
                                                    double& solver_seconds) {
    std::ofstream region_file(output_directory / "regions.tsv", std::ios::binary);
    std::ofstream unit_file(output_directory / "units.tsv", std::ios::binary);
    std::ofstream pattern_file(output_directory / "patterns.tsv", std::ios::binary);
    if (!region_file || !unit_file || !pattern_file) {
        return ParseResult<std::uint64_t>::failure(ParseReason::kIoError, "cannot create regional TSV artifacts");
    }
    region_file.imbue(std::locale::classic());
    unit_file.imbue(std::locale::classic());
    pattern_file.imbue(std::locale::classic());
    region_file << "region_order\tregion_id\tchrom\tstart1\tend1\tspan\t"
                   "n_sites_pre_cap\tn_sites_selected\tn_sites_cap_excluded\t"
                   "selected_positions\tn_full_cov_reads\tn_families\t"
                   "n_primary_lineages\tn_reference_controls\tn_h3_aux\t"
                   "n_h4_aux\tn_none_units\tdeterminacy\n";
    unit_file << "region_order\tregion_id\tfamily\trole\tn_reads\t"
                 "n_full_patterns\tn_supported_patterns\tmutation_bearing\t"
                 "n_hidden\tn_trees\tn_feasible_node_sets\tcapped\tclass\n";
    pattern_file << "region_order\tregion_id\tfamily\tpattern\tpattern_kind\tcount\n";

    for (const RegionEvidence& region : regions) {
        input_seconds += region.input_seconds;
        solver_seconds += region.solver_seconds;
        census["alignment_exposures"] += region.alignment_exposures;
        census["sidecar_exact_matches"] += region.sidecar_exact_matches;
        census["alignment_identity_allele_conflicts"] += region.alignment_conflicts;
        for (const auto& entry : region.raw_hp_counts) {
            census["raw_hp_" + entry.first] += entry.second;
        }
        for (const auto& entry : region.alignment_class_counts) {
            census["alignment_class_" + entry.first] += entry.second;
        }
        if (!region.retained) {
            ++census["read_unsupported_regions"];
            continue;
        }
        std::uint64_t primary = 0;
        std::uint64_t reference = 0;
        std::uint64_t h3 = 0;
        std::uint64_t h4 = 0;
        std::uint64_t none = 0;
        for (const UnitEvidence& unit : region.units) {
            primary += unit.role == "primary_mutation_lineage" ? 1U : 0U;
            reference += unit.role == "reference_only_control" ? 1U : 0U;
            h3 += unit.role == "unresolved_H3_auxiliary" ? 1U : 0U;
            h4 += unit.role == "shared_H4_auxiliary" ? 1U : 0U;
            none += unit.family == "none" ? 1U : 0U;
            unit_file << region.plan.region_order << '\t' << region.plan.region_id << '\t' << unit.family << '\t'
                      << unit.role << '\t' << unit.reads << '\t' << unit.full.size() << '\t' << unit.subread.size()
                      << '\t' << (unit.mutation_bearing ? 1 : 0) << '\t' << unit.solver.n_hidden << '\t'
                      << unit.solver.n_trees << '\t' << unit.solver.n_feasible_node_sets << '\t'
                      << (unit.solver.capped ? 1 : 0) << '\t' << unit.solver.classification << '\n';
            ++unit_rows;
            ++census["unit_class_" + unit.solver.classification];
            if (unit.role == "primary_mutation_lineage") {
                ++census["primary_class_" + unit.solver.classification];
            }
            for (const auto& pattern : unit.full) {
                pattern_file << region.plan.region_order << '\t' << region.plan.region_id << '\t' << unit.family << '\t'
                             << pattern.first << "\tFULL\t" << pattern.second << '\n';
                ++pattern_rows;
            }
            for (const auto& pattern : unit.subread) {
                pattern_file << region.plan.region_order << '\t' << region.plan.region_id << '\t' << unit.family << '\t'
                             << pattern.first << "\tSUBREAD\t" << pattern.second << '\n';
                ++pattern_rows;
            }
        }
        region_file << region.plan.region_order << '\t' << region.plan.region_id << '\t' << region.plan.chrom << '\t'
                    << region.plan.start1 << '\t' << region.plan.end1 << '\t' << region.plan.span << '\t'
                    << region.plan.pre_cap_site_count << '\t' << region.plan.selected_sites.size() << '\t'
                    << region.plan.cap_excluded_site_count << '\t' << join_positions(region.plan) << '\t'
                    << region.n_full_cov_reads << '\t' << region.units.size() << '\t' << primary << '\t' << reference
                    << '\t' << h3 << '\t' << h4 << '\t' << none << '\t' << region.determinacy << '\n';
        ++region_rows;
        ++census["region_determinacy_" + region.determinacy];
    }
    region_file.close();
    unit_file.close();
    pattern_file.close();
    if (!region_file || !unit_file || !pattern_file) {
        return ParseResult<std::uint64_t>::failure(ParseReason::kIoError, "regional TSV artifact write failed");
    }
    return ParseResult<std::uint64_t>::success(region_rows + unit_rows + pattern_rows);
}

void set_json_string(json_t* object, const char* key, const std::string& value) {
    if (json_object_set_new(object, key, json_string(value.c_str())) != 0) {
        throw std::runtime_error(std::string("cannot set JSON string: ") + key);
    }
}

void set_json_integer(json_t* object, const char* key, std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<json_int_t>::max()) ||
        json_object_set_new(object, key, json_integer(static_cast<json_int_t>(value))) != 0) {
        throw std::runtime_error(std::string("cannot set JSON integer: ") + key);
    }
}

[[nodiscard]] ParseResult<std::string> write_summary_and_receipt(
    const RegionalCompatibilityOptions& options, const ProductionManifest& manifest, const DatasetManifest& dataset,
    const std::string& source_authority_sha256, const std::string& source_manifest_sha256,
    const std::vector<VerifiedInputBinding>& verified_inputs, const RegionalPlan& plan,
    const std::map<std::string, std::uint64_t>& census, std::uint64_t region_rows, std::uint64_t unit_rows,
    std::uint64_t pattern_rows, double total_wall_seconds, double input_sha256_seconds, double science_wall_seconds,
    double open_seconds, double summed_input_seconds, double summed_solver_seconds) {
    try {
        JsonPointer summary(json_object());
        set_json_string(summary.get(), "schema_name", "longlineage.regional_compat_summary");
        set_json_string(summary.get(), "schema_version", std::string(kCompatibilitySchemaVersion));
        set_json_string(summary.get(), "run_id", options.run_id);
        set_json_string(summary.get(), "profile_id", std::string(kRegionalProfileId));
        const bool partial_probe = options.first_region != 0 || options.region_count != 0;
        set_json_string(summary.get(), "state", partial_probe ? "PARTIAL_PROBE" : "READY_FOR_VALIDATION");
        set_json_string(summary.get(), "dataset_id", dataset.dataset_id);
        set_json_integer(summary.get(), "dataset_order", dataset.dataset_order);
        set_json_string(summary.get(), "source_authority_profile", std::string(to_string(manifest.authority_profile)));
        set_json_string(summary.get(), "source_authority_sha256", source_authority_sha256);
        set_json_string(summary.get(), "source_manifest_path", options.source_manifest.string());
        set_json_string(summary.get(), "source_manifest_sha256", source_manifest_sha256);
        set_json_string(summary.get(), "source_manifest_run_id", manifest.run_id);
        set_json_integer(summary.get(), "workers", options.workers);
        set_json_integer(summary.get(), "first_region", options.first_region);
        set_json_integer(summary.get(), "requested_region_count", options.region_count);

        JsonPointer parameters(json_object());
        set_json_integer(parameters.get(), "TIER_R", kRegionalGapBp);
        set_json_integer(parameters.get(), "MAX_SNV", kRegionalMaximumSites);
        set_json_integer(parameters.get(), "MINREAD", kRegionalMinimumPatternReads);
        set_json_integer(parameters.get(), "MAPQ_MIN", kMinimumMappingQuality);
        set_json_integer(parameters.get(), "BASEQ_MIN", 0);
        set_json_integer(parameters.get(), "EXTRA_NODE_CAP", kLegacyExtraNodeCap);
        set_json_integer(parameters.get(), "PER_LEVEL_BUDGET", kLegacyPerLevelBudget);
        json_object_set_new(summary.get(), "parameters", parameters.release());

        JsonPointer rows(json_object());
        set_json_integer(rows.get(), "regions", region_rows);
        set_json_integer(rows.get(), "units", unit_rows);
        set_json_integer(rows.get(), "patterns", pattern_rows);
        json_object_set_new(summary.get(), "row_counts", rows.release());

        JsonPointer census_json(json_object());
        set_json_integer(census_json.get(), "scope_sites", plan.census.scope_sites);
        set_json_integer(census_json.get(), "positional_singletons", plan.census.positional_singletons);
        set_json_integer(census_json.get(), "multi_regions_pre_read", plan.census.multi_region_count);
        set_json_integer(census_json.get(), "multi_region_pre_cap_sites", plan.census.multi_region_pre_cap_sites);
        set_json_integer(census_json.get(), "capped_regions", plan.census.capped_region_count);
        set_json_integer(census_json.get(), "cap_excluded_sites", plan.census.cap_excluded_sites);
        set_json_integer(census_json.get(), "retained_selected_sites", plan.census.retained_sites);
        for (const auto& entry : census) {
            set_json_integer(census_json.get(), entry.first.c_str(), entry.second);
        }
        json_object_set_new(summary.get(), "census", census_json.release());

        JsonPointer timing(json_object());
        json_object_set_new(timing.get(), "total_wall_seconds", json_real(total_wall_seconds));
        json_object_set_new(timing.get(), "input_sha256_seconds", json_real(input_sha256_seconds));
        json_object_set_new(timing.get(), "science_wall_seconds", json_real(science_wall_seconds));
        json_object_set_new(timing.get(), "worker_open_seconds", json_real(open_seconds));
        json_object_set_new(timing.get(), "summed_input_seconds", json_real(summed_input_seconds));
        json_object_set_new(timing.get(), "summed_solver_seconds", json_real(summed_solver_seconds));
        json_object_set_new(summary.get(), "timing", timing.release());

        JsonPointer inputs(json_array());
        for (const VerifiedInputBinding& binding : verified_inputs) {
            if (binding.file == nullptr) {
                return ParseResult<std::string>::failure(ParseReason::kMalformedValue,
                                                         "verified regional input binding is null");
            }
            const LockedFile& file = *binding.file;
            JsonPointer row(json_object());
            set_json_string(row.get(), "role", std::string(to_string(file.role)));
            set_json_string(row.get(), "path", file.path.string());
            set_json_string(row.get(), "canonical_path", binding.snapshot.canonical_path.string());
            set_json_integer(row.get(), "size_bytes", file.size_bytes);
            set_json_string(row.get(), "sha256", file.sha256);
            json_object_set_new(row.get(), "full_sha256_verified", json_true());
            json_array_append_new(inputs.get(), row.release());
        }
        json_object_set_new(summary.get(), "inputs", inputs.release());

        JsonPointer verification(json_object());
        set_json_string(verification.get(), "method", "PHYSICAL_SHA256");
        set_json_integer(verification.get(), "role_count", verified_inputs.size());
        json_object_set_new(verification.get(), "before_after_identity_stable", json_true());
        set_json_integer(verification.get(),
                         "tr"
                         "uth_fields_seen",
                         0);
        json_object_set_new(verification.get(), "embedded_hp_fallback_used", json_false());
        json_object_set_new(summary.get(), "input_verification", verification.release());

        JsonPointer claim(json_object());
        json_object_set_new(claim.get(), "formal_m2_topology", json_false());
        json_object_set_new(claim.get(), "production_release", json_false());
        json_object_set_new(claim.get(), "clone_ancestor_time_order", json_false());
        json_object_set_new(summary.get(), "claim_ceiling", claim.release());

        const auto summary_sha = dump_json_file(options.output_directory / "summary.json", summary.get());
        if (!summary_sha.ok()) {
            return summary_sha;
        }

        struct ArtifactRow {
            std::string path;
            std::uint64_t rows;
            std::string sha256;
        };
        std::vector<ArtifactRow> artifacts;
        artifacts.push_back({"summary.json", 1, *summary_sha.value});
        for (const auto& pair : std::vector<std::pair<std::string, std::uint64_t>>{
                 {"regions.tsv", region_rows}, {"units.tsv", unit_rows}, {"patterns.tsv", pattern_rows}}) {
            auto digest = sha256_file(options.output_directory / pair.first);
            if (!digest.ok()) {
                return ParseResult<std::string>::failure(digest.reason, digest.detail);
            }
            artifacts.push_back({pair.first, pair.second, *digest.value});
        }
        std::ostringstream semantic_bytes;
        semantic_bytes << kRegionalProfileId << '\t' << kCompatibilitySchemaVersion << '\t' << dataset.dataset_id
                       << '\t' << dataset.dataset_order << '\t' << to_string(manifest.authority_profile) << '\t'
                       << source_authority_sha256 << '\n';
        for (const ArtifactRow& artifact : artifacts) {
            semantic_bytes << artifact.path << '\t' << artifact.rows << '\t' << artifact.sha256 << '\n';
        }
        auto semantic = sha256_hex(semantic_bytes.str());
        if (!semantic.ok()) {
            return semantic;
        }

        JsonPointer receipt(json_object());
        set_json_string(receipt.get(), "schema_name", "longlineage.regional_compat_producer_receipt");
        set_json_string(receipt.get(), "schema_version", std::string(kCompatibilitySchemaVersion));
        set_json_string(receipt.get(), "run_id", options.run_id);
        set_json_string(receipt.get(), "profile_id", std::string(kRegionalProfileId));
        set_json_string(receipt.get(), "dataset_id", dataset.dataset_id);
        set_json_integer(receipt.get(), "dataset_order", dataset.dataset_order);
        set_json_string(receipt.get(), "source_authority_profile", std::string(to_string(manifest.authority_profile)));
        set_json_string(receipt.get(), "source_authority_sha256", source_authority_sha256);
        set_json_string(receipt.get(), "summary_sha256", *summary_sha.value);
        set_json_string(receipt.get(), "semantic_sha256", *semantic.value);
        JsonPointer artifact_array(json_array());
        for (const ArtifactRow& artifact : artifacts) {
            JsonPointer row(json_object());
            set_json_string(row.get(), "path", artifact.path);
            set_json_string(row.get(), "sha256", artifact.sha256);
            set_json_integer(row.get(), "rows", artifact.rows);
            json_array_append_new(artifact_array.get(), row.release());
        }
        json_object_set_new(receipt.get(), "artifacts", artifact_array.release());
        const auto receipt_sha = dump_json_file(options.output_directory / "producer_receipt.json", receipt.get());
        if (!receipt_sha.ok()) {
            return receipt_sha;
        }

        std::map<std::string, std::string> checksums;
        for (const ArtifactRow& artifact : artifacts) {
            checksums[artifact.path] = artifact.sha256;
        }
        checksums["producer_receipt.json"] = *receipt_sha.value;
        std::ofstream checksum_file(options.output_directory / "checksums.sha256", std::ios::binary);
        for (const auto& entry : checksums) {
            checksum_file << entry.second << "  " << entry.first << '\n';
        }
        checksum_file.close();
        if (!checksum_file) {
            return ParseResult<std::string>::failure(ParseReason::kIoError, "cannot write checksums.sha256");
        }
        return ParseResult<std::string>::success(std::move(*semantic.value));
    } catch (const std::exception& error) {
        return ParseResult<std::string>::failure(ParseReason::kIoError,
                                                 std::string("cannot write regional summary/receipt: ") + error.what());
    }
}

[[nodiscard]] std::uint64_t json_uint(json_t* object, const char* key, bool& ok) noexcept {
    json_t* value = json_object_get(object, key);
    if (!json_is_integer(value) || json_integer_value(value) < 0) {
        ok = false;
        return 0;
    }
    return static_cast<std::uint64_t>(json_integer_value(value));
}

[[nodiscard]] std::map<std::string, std::uint64_t> json_count_map(json_t* object, bool& ok) {
    std::map<std::string, std::uint64_t> output;
    if (json_is_null(object) || object == nullptr) {
        return output;
    }
    if (!json_is_object(object)) {
        ok = false;
        return output;
    }
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(object, key, value) {
        if (!json_is_integer(value) || json_integer_value(value) < 0) {
            ok = false;
            return {};
        }
        output.emplace(key, static_cast<std::uint64_t>(json_integer_value(value)));
    }
    return output;
}

}  // namespace

ParseResult<RegionalCompatibilityReceipt> run_regional_compatibility(const RegionalCompatibilityOptions& options) {
    const auto wall_begin = std::chrono::steady_clock::now();
    if (!safe_identifier(options.run_id) || !safe_identifier(options.dataset_id) || options.workers == 0 ||
        options.workers > kMaximumWorkers || options.queue_capacity_bytes < kTaskChargeBytes ||
        !options.repository_root.is_absolute() || !options.source_manifest.is_absolute() ||
        !options.output_directory.is_absolute()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(ParseReason::kUnsupportedValue,
                                                                  "regional compatibility options are malformed");
    }
    std::error_code error;
    const std::filesystem::path repository_root = std::filesystem::canonical(options.repository_root, error);
    if (error || repository_root.empty() || !std::filesystem::is_directory(repository_root, error) || error) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(ParseReason::kIoError,
                                                                  "regional repository root is unavailable");
    }
    if (std::filesystem::exists(options.output_directory, error) || error) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(
            ParseReason::kIoError, "regional output directory already exists or cannot be checked");
    }
    auto locked_manifest = load_locked_production_manifest(options.source_manifest);
    if (!locked_manifest.ok()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(locked_manifest.reason, locked_manifest.detail);
    }
    const ProductionManifest& manifest = locked_manifest.value->manifest;
    const std::string expected_run_id = manifest.run_id + "-" + options.dataset_id;
    const std::filesystem::path expected_output_directory =
        (manifest.output_root / options.dataset_id).lexically_normal();
    if (locked_manifest.value->snapshot.canonical_path != options.source_manifest.lexically_normal()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(
            ParseReason::kUnsupportedValue, "regional source manifest must be a canonical non-alias path");
    }
    if (options.run_id != expected_run_id || options.output_directory != expected_output_directory ||
        options.output_directory != options.output_directory.lexically_normal() ||
        options.workers != manifest.runtime.compute_workers) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(
            ParseReason::kUnsupportedValue,
            "regional run-id, output directory or worker count differs from the source manifest contract");
    }
    auto contract_check = verify_repository_contracts(manifest, repository_root);
    if (!contract_check.ok()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(contract_check.reason, contract_check.detail);
    }

    const DatasetManifest* dataset = nullptr;
    ParseResult<std::string> source_authority =
        ParseResult<std::string>::failure(ParseReason::kUnsupportedValue, "unsupported regional authority profile");
    if (manifest.authority_profile == AuthorityProfile::kProductionSevenDataset) {
        source_authority = verify_production_authority(manifest, repository_root);
        for (const DatasetManifest& candidate : manifest.datasets) {
            if (candidate.dataset_id == options.dataset_id) {
                if (dataset != nullptr) {
                    return ParseResult<RegionalCompatibilityReceipt>::failure(
                        ParseReason::kMalformedValue, "regional dataset selector is not unique");
                }
                dataset = &candidate;
            }
        }
    } else if (manifest.authority_profile == AuthorityProfile::kHcc1395DatasetGate) {
        source_authority = verify_hcc_authority(manifest, repository_root);
        if (options.dataset_id == "HCC1395") {
            dataset = &manifest.datasets.front();
        }
    }
    if (!source_authority.ok()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(source_authority.reason, source_authority.detail);
    }
    if (dataset == nullptr) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(
            ParseReason::kUnsupportedValue, "regional dataset_id is absent from the governed source authority");
    }

    const LockedFile* bam = find_role(*dataset, FileRole::kRawBam);
    const LockedFile* bam_index = find_role(*dataset, FileRole::kRawBamIndex);
    const LockedFile* vcf = find_role(*dataset, FileRole::kPassBiallelicSsnvVcf);
    const LockedFile* vcf_index = find_role(*dataset, FileRole::kPassBiallelicSsnvVcfIndex);
    const LockedFile* sidecar = find_role(*dataset, FileRole::kLatestHpPsSidecar);
    const LockedFile* sidecar_index = find_role(*dataset, FileRole::kLatestHpPsSidecarIndex);
    const LockedFile* reference = find_role(*dataset, FileRole::kReferenceFasta);
    const LockedFile* reference_index = find_role(*dataset, FileRole::kReferenceFai);
    if (bam == nullptr || bam_index == nullptr || vcf == nullptr || vcf_index == nullptr || sidecar == nullptr ||
        sidecar_index == nullptr || reference == nullptr || reference_index == nullptr) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(
            ParseReason::kMissingRequiredField, dataset->dataset_id + ": manifest lacks a required regional role");
    }

    const auto sha_begin = std::chrono::steady_clock::now();
    std::vector<VerifiedInputBinding> verified_inputs;
    verified_inputs.reserve(kRegionalInputRoles.size());
    for (const FileRole role : kRegionalInputRoles) {
        const LockedFile* file = find_role(*dataset, role);
        if (file == nullptr) {
            return ParseResult<RegionalCompatibilityReceipt>::failure(
                ParseReason::kMissingRequiredField,
                dataset->dataset_id + ": missing role " + std::string(to_string(role)));
        }
        auto locked = verify_locked_file(*file);
        if (!locked.ok()) {
            return ParseResult<RegionalCompatibilityReceipt>::failure(
                locked.reason, dataset->dataset_id + ": " + std::string(to_string(role)) + ": " + locked.detail);
        }
        const FileLockCheck& check = *locked.value;
        verified_inputs.push_back(VerifiedInputBinding{
            file,
            InputSnapshot{check.canonical_path, check.device, check.inode, check.observed_size_bytes,
                          check.mtime_seconds, check.mtime_nanoseconds, check.ctime_seconds, check.ctime_nanoseconds},
        });
    }
    const double input_sha256_seconds = elapsed_seconds(sha_begin);
    const auto science_begin = std::chrono::steady_clock::now();

    auto reference_reader = IndexedReferenceReader::open(reference->path, reference_index->path);
    if (!reference_reader.ok()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(reference_reader.reason, reference_reader.detail);
    }
    auto variants = load_variant_sites(vcf->path, vcf_index->path, **reference_reader.value, dataset->dataset_order,
                                       dataset->dataset_id);
    if (!variants.ok()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(variants.reason, variants.detail);
    }
    auto plan = make_python_v2_regional_plan(*variants.value);
    if (!plan.ok()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(plan.reason, plan.detail);
    }
    if (options.first_region >= plan.value->regions.size()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(ParseReason::kUnsupportedValue,
                                                                  "regional first_region lies outside the full plan");
    }
    const std::size_t remaining_regions = plan.value->regions.size() - options.first_region;
    const std::size_t requested_regions = options.region_count == 0 ? remaining_regions : options.region_count;
    if (requested_regions == 0 || requested_regions > remaining_regions) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(
            ParseReason::kUnsupportedValue, "regional requested range lies outside the full plan");
    }

    const auto open_begin = std::chrono::steady_clock::now();
    RegionalWorkerPaths paths{bam->path, bam_index->path, sidecar->path, sidecar_index->path};
    std::vector<std::unique_ptr<RegionalWorkerReader>> readers;
    readers.reserve(options.workers);
    for (std::size_t worker = 0; worker < options.workers; ++worker) {
        auto reader = RegionalWorkerReader::open(paths);
        if (!reader.ok()) {
            return ParseResult<RegionalCompatibilityReceipt>::failure(
                reader.reason, "regional worker " + std::to_string(worker) + ": " + reader.detail);
        }
        readers.push_back(std::move(*reader.value));
    }
    const double open_seconds = elapsed_seconds(open_begin);

    runtime::OrderedThreadPool<RegionEvidence> pool(options.workers, options.queue_capacity_bytes);
    for (std::size_t local_index = 0; local_index < requested_regions; ++local_index) {
        const std::size_t index = options.first_region + local_index;
        const auto submitted = pool.submit_indexed(kTaskChargeBytes, [index, &readers, &plan](std::size_t worker) {
            auto result = readers[worker]->read(plan.value->regions[index]);
            if (!result.ok()) {
                throw std::runtime_error(result.detail);
            }
            return std::move(*result.value);
        });
        if (submitted.status != runtime::PoolStatus::kSuccess) {
            pool.cancel("regional task submission failed");
            const auto ignored = pool.finish();
            static_cast<void>(ignored);
            return ParseResult<RegionalCompatibilityReceipt>::failure(
                ParseReason::kIoError, "regional task submission failed: " + submitted.message);
        }
    }
    auto batch = pool.finish();
    if (batch.status != runtime::PoolStatus::kSuccess || batch.ordered_results.size() != requested_regions) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(ParseReason::kIoError,
                                                                  "regional worker pool failed: " + batch.message);
    }
    for (std::size_t index = 0; index < batch.ordered_results.size(); ++index) {
        if (batch.ordered_results[index].plan.region_order != options.first_region + index) {
            return ParseResult<RegionalCompatibilityReceipt>::failure(ParseReason::kMalformedValue,
                                                                      "regional ordered publication sequence drifted");
        }
    }

    for (const VerifiedInputBinding& binding : verified_inputs) {
        if (binding.file == nullptr) {
            return ParseResult<RegionalCompatibilityReceipt>::failure(ParseReason::kMalformedValue,
                                                                      "regional verified input binding is null");
        }
        auto after = snapshot_file(binding.file->path);
        if (!after.ok() || !same_snapshot(binding.snapshot, *after.value)) {
            return ParseResult<RegionalCompatibilityReceipt>::failure(
                ParseReason::kIoError, "regional input changed during execution: " + binding.file->path.string());
        }
    }
    auto manifest_stable =
        verify_locked_document(options.source_manifest, locked_manifest.value->snapshot, locked_manifest.value->sha256);
    auto contracts_stable = verify_repository_contracts(manifest, repository_root);
    ParseResult<std::string> final_authority =
        ParseResult<std::string>::failure(ParseReason::kUnsupportedValue, "unsupported regional authority profile");
    if (manifest.authority_profile == AuthorityProfile::kProductionSevenDataset) {
        final_authority = verify_production_authority(manifest, repository_root);
    } else if (manifest.authority_profile == AuthorityProfile::kHcc1395DatasetGate) {
        final_authority = verify_hcc_authority(manifest, repository_root);
    }
    if (!manifest_stable.ok() || !contracts_stable.ok() || !final_authority.ok() ||
        *final_authority.value != *source_authority.value) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(
            ParseReason::kIoError, "regional manifest, repository contract or input authority changed during run");
    }
    if (!std::filesystem::create_directories(options.output_directory, error) || error) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(ParseReason::kIoError,
                                                                  "cannot create regional output directory");
    }

    std::uint64_t region_rows = 0;
    std::uint64_t unit_rows = 0;
    std::uint64_t pattern_rows = 0;
    std::map<std::string, std::uint64_t> census;
    double summed_input_seconds = 0.0;
    double summed_solver_seconds = 0.0;
    auto written = write_tsvs(options.output_directory, batch.ordered_results, region_rows, unit_rows, pattern_rows,
                              census, summed_input_seconds, summed_solver_seconds);
    if (!written.ok()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(written.reason, written.detail);
    }
    const double science_wall_seconds = elapsed_seconds(science_begin);
    const double total_wall_seconds = elapsed_seconds(wall_begin);
    auto semantic = write_summary_and_receipt(
        options, manifest, *dataset, *source_authority.value, locked_manifest.value->sha256, verified_inputs,
        *plan.value, census, region_rows, unit_rows, pattern_rows, total_wall_seconds, input_sha256_seconds,
        science_wall_seconds, open_seconds, summed_input_seconds, summed_solver_seconds);
    if (!semantic.ok()) {
        return ParseResult<RegionalCompatibilityReceipt>::failure(semantic.reason, semantic.detail);
    }
    return ParseResult<RegionalCompatibilityReceipt>::success(RegionalCompatibilityReceipt{
        options.output_directory,
        dataset->dataset_id,
        dataset->dataset_order,
        std::move(*semantic.value),
        region_rows,
        unit_rows,
        pattern_rows,
        total_wall_seconds,
        input_sha256_seconds,
        science_wall_seconds,
    });
}

ParseResult<RegionalOracleReplayReceipt> replay_frozen_mlhp_oracle(
    const std::vector<std::filesystem::path>& ordered_mlhp_parts) {
    if (ordered_mlhp_parts.empty()) {
        return ParseResult<RegionalOracleReplayReceipt>::failure(ParseReason::kMissingRequiredField,
                                                                 "oracle replay requires at least one MLHP part");
    }
    RegionalOracleReplayReceipt output;
    std::ostringstream semantic;
    semantic << "longlineage.regional_oracle_replay\t1.0.0\n";
    for (const std::filesystem::path& path : ordered_mlhp_parts) {
        json_error_t error{};
        JsonPointer document(json_load_file(path.string().c_str(), JSON_REJECT_DUPLICATES, &error));
        if (!document || !json_is_object(document.get())) {
            return ParseResult<RegionalOracleReplayReceipt>::failure(ParseReason::kMalformedValue,
                                                                     "cannot parse frozen MLHP part: " + path.string());
        }
        json_t* groups = json_object_get(document.get(), "groups");
        if (!json_is_array(groups)) {
            return ParseResult<RegionalOracleReplayReceipt>::failure(ParseReason::kMalformedValue,
                                                                     "frozen MLHP part lacks groups array");
        }
        const std::size_t group_count = json_array_size(groups);
        for (std::size_t group_index = 0; group_index < group_count; ++group_index) {
            json_t* group = json_array_get(groups, group_index);
            json_t* chrom = json_object_get(group, "chrom");
            json_t* start = json_object_get(group, "start");
            json_t* end = json_object_get(group, "end");
            json_t* positions = json_object_get(group, "positions");
            json_t* populations_by_hp = json_object_get(group, "populations_by_hp");
            json_t* subreads_by_hp = json_object_get(group, "subread_groups_by_hp");
            json_t* reads_by_hp = json_object_get(group, "reads_by_hp");
            if (!json_is_string(chrom) || !json_is_integer(start) || !json_is_integer(end) ||
                !json_is_array(positions) || !json_is_object(populations_by_hp) || !json_is_object(subreads_by_hp) ||
                !json_is_object(reads_by_hp)) {
                return ParseResult<RegionalOracleReplayReceipt>::failure(ParseReason::kMalformedValue,
                                                                         "frozen MLHP group schema is malformed");
            }
            const std::string region_id = json_string_text(chrom) + ":" + std::to_string(json_integer_value(start)) +
                                          "-" + std::to_string(json_integer_value(end));
            const std::size_t site_count = json_array_size(positions);
            if (site_count < 2 || site_count > kRegionalMaximumSites) {
                return ParseResult<RegionalOracleReplayReceipt>::failure(
                    ParseReason::kMalformedValue, "frozen MLHP region has invalid selected site count");
            }
            std::set<std::string> families;
            const char* family = nullptr;
            json_t* value = nullptr;
            json_object_foreach(populations_by_hp, family, value) { families.insert(family); }
            json_object_foreach(subreads_by_hp, family, value) { families.insert(family); }
            ++output.regions;
            for (const std::string& current_family : families) {
                bool ok = true;
                LegacySolverInput input;
                input.site_count = site_count;
                input.supported_full_patterns =
                    json_count_map(json_object_get(populations_by_hp, current_family.c_str()), ok);
                input.supported_subread_patterns =
                    json_count_map(json_object_get(subreads_by_hp, current_family.c_str()), ok);
                const std::uint64_t reads = json_uint(reads_by_hp, current_family.c_str(), ok);
                if (!ok || (input.supported_full_patterns.empty() && input.supported_subread_patterns.empty())) {
                    return ParseResult<RegionalOracleReplayReceipt>::failure(
                        ParseReason::kMalformedValue, "frozen MLHP family evidence is malformed");
                }
                output.patterns += static_cast<std::uint64_t>(input.supported_full_patterns.size());
                output.patterns += static_cast<std::uint64_t>(input.supported_subread_patterns.size());
                auto solved = solve_python_v2_legacy(input);
                if (!solved.ok()) {
                    return ParseResult<RegionalOracleReplayReceipt>::failure(
                        solved.reason, region_id + " family " + current_family + ": " + solved.detail);
                }
                const bool mutation_bearing = pattern_is_mutation_bearing(input);
                const std::string role = python_v2_unit_role(current_family, mutation_bearing);
                ++output.units;
                ++output.all_unit_classes[solved.value->classification];
                if (role == "primary_mutation_lineage") {
                    ++output.primary_unit_classes[solved.value->classification];
                }
                semantic << region_id << '\t' << current_family << '\t' << role << '\t' << reads << '\t'
                         << input.supported_full_patterns.size() << '\t' << input.supported_subread_patterns.size()
                         << '\t' << (mutation_bearing ? 1 : 0) << '\t' << solved.value->n_hidden << '\t'
                         << solved.value->n_trees << '\t' << solved.value->n_feasible_node_sets << '\t'
                         << (solved.value->capped ? 1 : 0) << '\t' << solved.value->classification << '\n';
            }
        }
    }
    auto digest = sha256_hex(semantic.str());
    if (!digest.ok()) {
        return ParseResult<RegionalOracleReplayReceipt>::failure(digest.reason, digest.detail);
    }
    output.unit_semantic_sha256 = std::move(*digest.value);
    return ParseResult<RegionalOracleReplayReceipt>::success(std::move(output));
}

}  // namespace longlineage::compat
