// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/validation/regional_compat_validator.hpp"

#include <fcntl.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace longlineage::validation {
namespace {

constexpr std::string_view kProfileId = "PYTHON_V2_DESCRIPTIVE_REGIONAL";
constexpr std::string_view kLegacySchemaVersion = "1.0.0";
constexpr std::string_view kCurrentSchemaVersion = "2.0.0";

constexpr std::array<std::string_view, 7> kProductionDatasetOrder = {
    "HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954",
};

constexpr std::array<std::string_view, 8> kInputRoleOrder = {
    "raw_bam",
    "raw_bam_index",
    "pass_biallelic_ssnv_vcf",
    "pass_biallelic_ssnv_vcf_index",
    "latest_hp_ps_sidecar",
    "latest_hp_ps_sidecar_index",
    "reference_fasta",
    "reference_fai",
};

constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kRepositoryContractBindings = {{
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

constexpr std::array<std::string_view, 18> kRegionHeader = {
    "region_order",
    "region_id",
    "chrom",
    "start1",
    "end1",
    "span",
    "n_sites_pre_cap",
    "n_sites_selected",
    "n_sites_cap_excluded",
    "selected_positions",
    "n_full_cov_reads",
    "n_families",
    "n_primary_lineages",
    "n_reference_controls",
    "n_h3_aux",
    "n_h4_aux",
    "n_none_units",
    "determinacy",
};

constexpr std::array<std::string_view, 13> kUnitHeader = {
    "region_order",
    "region_id",
    "family",
    "role",
    "n_reads",
    "n_full_patterns",
    "n_supported_patterns",
    "mutation_bearing",
    "n_hidden",
    "n_trees",
    "n_feasible_node_sets",
    "capped",
    "class",
};

constexpr std::array<std::string_view, 6> kPatternHeader = {
    "region_order", "region_id", "family", "pattern", "pattern_kind", "count",
};

const std::set<std::string> kProducerFiles = {
    "checksums.sha256", "patterns.tsv", "producer_receipt.json", "regions.tsv", "summary.json", "units.tsv",
};

const std::set<std::string> kGeneratedFiles = {
    "FROZEN",
    "validation_receipt.json",
};

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};

using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

class ValidationFailure final : public std::runtime_error {
   public:
    ValidationFailure(std::string check_id, std::string detail)
        : std::runtime_error(detail), check_id_(std::move(check_id)) {}

    [[nodiscard]] const std::string& check_id() const noexcept { return check_id_; }

   private:
    std::string check_id_;
};

[[noreturn]] void reject(const std::string& check_id, const std::string& detail) {
    throw ValidationFailure(check_id, detail);
}

void add_pass(RegionalCompatValidationReport& report, const std::string& check_id, const std::string& detail) {
    report.checks.push_back({check_id, true, detail});
}

bool is_lower_sha256(std::string_view value) noexcept {
    if (value.size() != 64U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char raw) {
        const auto character = static_cast<unsigned char>(raw);
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

class Sha256 final {
   public:
    Sha256() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            reject("INTERNAL_DIGEST", "cannot initialize SHA-256");
        }
    }

    void update(std::string_view bytes) {
        if (!bytes.empty() && EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1) {
            reject("INTERNAL_DIGEST", "cannot update SHA-256");
        }
    }

    [[nodiscard]] std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> raw{};
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(context_.get(), raw.data(), &size) != 1 || size != 32U) {
            reject("INTERNAL_DIGEST", "cannot finalize SHA-256");
        }
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

std::string sha256_file(const std::filesystem::path& path, const std::string& check_id) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reject(check_id, "cannot open file for SHA-256: " + path.string());
    }
    Sha256 digest;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            digest.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if (!input.eof()) {
        reject(check_id, "I/O error while hashing: " + path.string());
    }
    return digest.finish();
}

std::string read_text_file(const std::filesystem::path& path, const std::string& check_id) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reject(check_id, "cannot open file: " + path.string());
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (!input.eof() && input.fail()) {
        reject(check_id, "I/O error while reading: " + path.string());
    }
    std::string result = bytes.str();
    if (result.empty() || result.back() != '\n' || result.find('\0') != std::string::npos ||
        result.find('\r') != std::string::npos) {
        reject(check_id, "text file must be nonempty LF-terminated UTF-8-compatible bytes: " + path.string());
    }
    return result;
}

JsonPtr load_json_strict(const std::filesystem::path& path, const std::string& check_id) {
    json_error_t error{};
    JsonPtr value(json_load_file(path.c_str(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
    if (!value) {
        std::ostringstream detail;
        detail << path.string() << ':' << error.line << ':' << error.column << ": " << error.text;
        reject(check_id, detail.str());
    }
    if (!json_is_object(value.get())) {
        reject(check_id, path.string() + " must contain a JSON object");
    }
    return value;
}

std::string string_field(const json_t* object, const char* key, const std::string& check_id) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_string(value)) {
        reject(check_id, std::string("expected string field: ") + key);
    }
    const std::string result(json_string_value(value), json_string_length(value));
    if (result.find('\0') != std::string::npos) {
        reject(check_id, std::string("embedded NUL in string field: ") + key);
    }
    return result;
}

std::uint64_t uint_field(const json_t* object, const char* key, const std::string& check_id) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_integer(value) || json_integer_value(value) < 0) {
        reject(check_id, std::string("expected nonnegative integer field: ") + key);
    }
    return static_cast<std::uint64_t>(json_integer_value(value));
}

double positive_number_field(const json_t* object, const char* key, const std::string& check_id) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_number(value)) {
        reject(check_id, std::string("expected numeric field: ") + key);
    }
    const double observed = json_number_value(value);
    if (!std::isfinite(observed) || observed <= 0.0) {
        reject(check_id, std::string("expected finite positive numeric field: ") + key);
    }
    return observed;
}

void require_boolean_value(const json_t* object, const char* key, bool expected, const std::string& check_id,
                           const std::string& role) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_boolean(value) || json_is_true(value) != expected) {
        reject(check_id, role + "." + key + " must be " + (expected ? "true" : "false"));
    }
}

bool contains_truth_token(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char raw : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw))));
    }
    return lowered.find("truth") != std::string::npos;
}

std::string absolute_path_field(const json_t* object, const char* key, const std::string& check_id,
                                const std::string& role) {
    const std::string value = string_field(object, key, check_id);
    if (value.empty() || !std::filesystem::path(value).is_absolute() || contains_truth_token(value)) {
        reject(check_id, role + "." + key + " must be an absolute truth-free path");
    }
    return value;
}

bool is_safe_identifier(std::string_view value, std::size_t maximum_length) noexcept {
    if (value.empty() || value.size() > maximum_length) {
        return false;
    }
    const auto is_alphanumeric = [](char raw) {
        const auto character = static_cast<unsigned char>(raw);
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    if (!is_alphanumeric(value.front())) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(),
                       [&](char raw) { return is_alphanumeric(raw) || raw == '.' || raw == '_' || raw == '-'; });
}

std::filesystem::path normalized_absolute_path(std::string_view value, const std::string& check_id,
                                               const std::string& role) {
    const std::filesystem::path path(value);
    if (value.empty() || !path.is_absolute() || path.lexically_normal().string() != value ||
        contains_truth_token(value)) {
        reject(check_id, role + " must be a normalized absolute truth-free path");
    }
    return path;
}

void require_exact_keys(const json_t* object, const std::set<std::string>& expected, const std::string& check_id,
                        const std::string& role) {
    if (!json_is_object(object)) {
        reject(check_id, role + " must be a JSON object");
    }
    for (const std::string& key : expected) {
        if (json_object_get(object, key.c_str()) == nullptr) {
            reject(check_id, role + " missing required field: " + key);
        }
    }
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(object), key, value) {
        static_cast<void>(value);
        if (expected.count(key) == 0U) {
            reject(check_id, role + " contains unknown field: " + std::string(key));
        }
    }
}

void require_nonnegative_census(const json_t* value, const std::string& path) {
    if (json_is_integer(value)) {
        if (json_integer_value(value) < 0) {
            reject("SUMMARY_CONTRACT", "negative census value at " + path);
        }
        return;
    }
    if (!json_is_object(value)) {
        reject("SUMMARY_CONTRACT", "census leaves must be nonnegative integers at " + path);
    }
    const char* key = nullptr;
    json_t* child = nullptr;
    json_object_foreach(const_cast<json_t*>(value), key, child) { require_nonnegative_census(child, path + "." + key); }
}

std::uint64_t parse_uint(std::string_view value, const std::string& check_id, const std::string& role) {
    if (value.empty() || (value.size() > 1U && value.front() == '0')) {
        reject(check_id, role + " is not a canonical unsigned integer: " + std::string(value));
    }
    std::uint64_t parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto converted = std::from_chars(begin, end, parsed);
    if (converted.ec != std::errc{} || converted.ptr != end) {
        reject(check_id, role + " is not an unsigned integer: " + std::string(value));
    }
    return parsed;
}

std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t found = value.find(delimiter, begin);
        fields.emplace_back(
            value.substr(begin, found == std::string_view::npos ? std::string_view::npos : found - begin));
        if (found == std::string_view::npos) {
            break;
        }
        begin = found + 1U;
    }
    return fields;
}

template <std::size_t Size>
void require_header(const std::vector<std::string>& observed, const std::array<std::string_view, Size>& expected,
                    const std::string& check_id, const std::string& path) {
    if (observed.size() != expected.size()) {
        reject(check_id, path + " header column count differs from contract");
    }
    for (std::size_t index = 0; index < Size; ++index) {
        if (observed[index] != expected[index]) {
            reject(check_id, path + " header mismatch at column " + std::to_string(index + 1U));
        }
    }
}

std::vector<std::vector<std::string>> load_tsv(
    const std::filesystem::path& path, const std::string& check_id, std::size_t expected_columns,
    const std::function<void(const std::vector<std::string>&)>& validate_header) {
    const std::string bytes = read_text_file(path, check_id);
    std::vector<std::vector<std::string>> records;
    std::size_t begin = 0;
    std::size_t line_number = 0;
    while (begin < bytes.size()) {
        const std::size_t end = bytes.find('\n', begin);
        const std::string_view line(bytes.data() + begin, end - begin);
        ++line_number;
        std::vector<std::string> fields = split(line, '\t');
        if (line_number == 1U) {
            validate_header(fields);
        } else {
            if (fields.size() != expected_columns ||
                std::any_of(fields.begin(), fields.end(), [](const std::string& value) { return value.empty(); })) {
                reject(check_id, path.filename().string() + ":" + std::to_string(line_number) +
                                     " has empty or wrong-count fields");
            }
            records.push_back(std::move(fields));
        }
        begin = end + 1U;
    }
    return records;
}

struct FileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_nanoseconds = 0;

    bool operator==(const FileIdentity& other) const noexcept {
        return std::tie(device, inode, size, modified_seconds, modified_nanoseconds, changed_seconds,
                        changed_nanoseconds) == std::tie(other.device, other.inode, other.size, other.modified_seconds,
                                                         other.modified_nanoseconds, other.changed_seconds,
                                                         other.changed_nanoseconds);
    }
};

FileIdentity regular_identity(const std::filesystem::path& path, const std::string& check_id) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        reject(check_id, "missing path: " + path.string());
    }
    if (!S_ISREG(status.st_mode)) {
        reject(check_id, "required path is not a regular non-symlink file: " + path.string());
    }
    if (status.st_size < 0) {
        reject(check_id, "negative file size: " + path.string());
    }
    return {
        static_cast<std::uint64_t>(status.st_dev),         static_cast<std::uint64_t>(status.st_ino),
        static_cast<std::uint64_t>(status.st_size),        static_cast<std::int64_t>(status.st_mtim.tv_sec),
        static_cast<std::int64_t>(status.st_mtim.tv_nsec), static_cast<std::int64_t>(status.st_ctim.tv_sec),
        static_cast<std::int64_t>(status.st_ctim.tv_nsec),
    };
}

std::map<std::string, FileIdentity> inspect_layout(const std::filesystem::path& root, bool& has_receipt,
                                                   bool& has_frozen) {
    struct stat root_status {};
    if (::lstat(root.c_str(), &root_status) != 0 || !S_ISDIR(root_status.st_mode) || S_ISLNK(root_status.st_mode)) {
        reject("ROOT_LAYOUT", "bundle root must be a real directory: " + root.string());
    }

    std::map<std::string, FileIdentity> identities;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root, error), end; iterator != end; iterator.increment(error)) {
        if (error) {
            reject("ROOT_LAYOUT", "cannot enumerate bundle root: " + error.message());
        }
        const std::string name = iterator->path().filename().string();
        if (kProducerFiles.count(name) == 0U && kGeneratedFiles.count(name) == 0U) {
            reject("NO_EXTRA_FILES", "unknown bundle entry: " + name);
        }
        const FileIdentity identity = regular_identity(iterator->path(), "ROOT_LAYOUT");
        if (kProducerFiles.count(name) != 0U) {
            identities.emplace(name, identity);
        }
        has_receipt = has_receipt || name == "validation_receipt.json";
        has_frozen = has_frozen || name == "FROZEN";
    }
    if (error) {
        reject("ROOT_LAYOUT", "cannot enumerate bundle root: " + error.message());
    }
    if (identities.size() != kProducerFiles.size()) {
        for (const std::string& name : kProducerFiles) {
            if (identities.count(name) == 0U) {
                reject("REQUIRED_FILES", "missing required producer file: " + name);
            }
        }
    }
    if (has_receipt != has_frozen) {
        reject("OUTPUT_TRANSACTION", "validation_receipt.json and FROZEN must coexist");
    }
    return identities;
}

std::map<std::string, std::string> validate_checksum_manifest(const std::filesystem::path& root) {
    const std::string bytes = read_text_file(root / "checksums.sha256", "CHECKSUM_MANIFEST");
    const std::set<std::string> expected = {
        "patterns.tsv", "producer_receipt.json", "regions.tsv", "summary.json", "units.tsv",
    };
    std::map<std::string, std::string> rows;
    std::string previous;
    std::size_t begin = 0;
    std::size_t line_number = 0;
    while (begin < bytes.size()) {
        const std::size_t end = bytes.find('\n', begin);
        const std::string line = bytes.substr(begin, end - begin);
        ++line_number;
        if (line.size() < 67U || line.substr(64U, 2U) != "  ") {
            reject("CHECKSUM_MANIFEST", "malformed sha256sum row " + std::to_string(line_number));
        }
        const std::string digest = line.substr(0, 64U);
        const std::string path = line.substr(66U);
        if (!is_lower_sha256(digest) || expected.count(path) == 0U || (!previous.empty() && path <= previous) ||
            !rows.emplace(path, digest).second) {
            reject("CHECKSUM_MANIFEST", "unknown, duplicate or out-of-order checksum row: " + path);
        }
        previous = path;
        begin = end + 1U;
    }
    if (rows.size() != expected.size()) {
        reject("CHECKSUM_MANIFEST", "checksum manifest does not cover exactly five producer artifacts");
    }
    for (const auto& row : rows) {
        const std::string observed = sha256_file(root / row.first, "CHECKSUM_MANIFEST");
        if (observed != row.second) {
            reject("CHECKSUM_MANIFEST", "SHA-256 mismatch for " + row.first);
        }
    }
    return rows;
}

struct SummaryContract {
    struct InputBinding {
        std::string role;
        std::string path;
        std::string canonical_path;
        std::uint64_t size_bytes = 0;
        std::string sha256;
    };

    std::string schema_version;
    std::string run_id;
    std::string dataset_id;
    std::uint64_t dataset_order = 0;
    std::string source_authority_profile;
    std::string source_authority_sha256;
    std::filesystem::path source_manifest_path;
    std::string source_manifest_sha256;
    std::string source_manifest_run_id;
    std::uint64_t workers = 0;
    std::uint64_t regions = 0;
    std::uint64_t units = 0;
    std::uint64_t patterns = 0;
    std::vector<InputBinding> inputs;
    std::map<std::string, std::uint64_t> census;
};

void validate_dataset_binding(const SummaryContract& contract, const std::string& check_id) {
    if (contract.source_authority_profile == "PRODUCTION_7_DATASET") {
        if (contract.dataset_order >= kProductionDatasetOrder.size() ||
            contract.dataset_id != kProductionDatasetOrder[contract.dataset_order]) {
            reject(check_id, "dataset ID/order differs from the frozen production authority order");
        }
        return;
    }
    if (contract.source_authority_profile == "HCC1395_DATASET_GATE") {
        if (contract.dataset_id != "HCC1395" || contract.dataset_order != 0U) {
            reject(check_id, "legacy HCC authority only permits HCC1395 at order zero");
        }
        return;
    }
    reject(check_id, "unknown source_authority_profile: " + contract.source_authority_profile);
}

std::map<std::string, std::uint64_t> validate_v2_census(const json_t* census) {
    static const std::set<std::string> kRequired = {
        "cap_excluded_sites",
        "capped_regions",
        "multi_region_pre_cap_sites",
        "multi_regions_pre_read",
        "positional_singletons",
        "retained_selected_sites",
        "scope_sites",
    };
    static const std::set<std::string> kAllowed = {
        "alignment_class_primary",
        "alignment_class_supplementary",
        "alignment_exposures",
        "alignment_identity_allele_conflicts",
        "cap_excluded_sites",
        "capped_regions",
        "multi_region_pre_cap_sites",
        "multi_regions_pre_read",
        "positional_singletons",
        "primary_class_ambiguous_order",
        "primary_class_ambiguous_structure",
        "primary_class_capped",
        "primary_class_determined",
        "primary_class_recurrence_required",
        "primary_class_underdetermined",
        "raw_hp_.",
        "raw_hp_1",
        "raw_hp_1-1",
        "raw_hp_1-2",
        "raw_hp_2",
        "raw_hp_2-1",
        "raw_hp_2-2",
        "raw_hp_3",
        "raw_hp_4",
        "read_unsupported_regions",
        "region_determinacy_all_determined",
        "region_determinacy_has_ambiguous",
        "region_determinacy_has_capped",
        "region_determinacy_has_recurrence",
        "region_determinacy_no_primary_lineage",
        "retained_selected_sites",
        "scope_sites",
        "sidecar_exact_matches",
        "unit_class_ambiguous_order",
        "unit_class_ambiguous_structure",
        "unit_class_capped",
        "unit_class_determined",
        "unit_class_recurrence_required",
        "unit_class_underdetermined",
    };
    if (!json_is_object(census)) {
        reject("SUMMARY_CONTRACT", "census must be a closed JSON object");
    }
    std::map<std::string, std::uint64_t> result;
    for (const std::string& key : kRequired) {
        if (json_object_get(census, key.c_str()) == nullptr) {
            reject("SUMMARY_CONTRACT", "census missing required field: " + key);
        }
    }
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(census), key, value) {
        if (kAllowed.count(key) == 0U || !json_is_integer(value) || json_integer_value(value) < 0) {
            reject("SUMMARY_CONTRACT", "census contains unknown or non-integer field: " + std::string(key));
        }
        result.emplace(key, static_cast<std::uint64_t>(json_integer_value(value)));
    }
    if (result.at("scope_sites") != result.at("positional_singletons") + result.at("multi_region_pre_cap_sites") ||
        result.at("multi_region_pre_cap_sites") !=
            result.at("retained_selected_sites") + result.at("cap_excluded_sites") ||
        result.at("capped_regions") > result.at("multi_regions_pre_read")) {
        reject("SUMMARY_CONTRACT", "census site/region conservation equations do not hold");
    }
    return result;
}

SummaryContract validate_summary_v1(const json_t* summary) {
    const std::set<std::string> required = {
        "schema_name", "schema_version", "run_id", "profile_id", "state", "workers", "row_counts", "census",
    };
    for (const std::string& key : required) {
        if (json_object_get(summary, key.c_str()) == nullptr) {
            reject("SUMMARY_CONTRACT", "summary missing required field: " + key);
        }
    }
    if (string_field(summary, "schema_name", "SUMMARY_CONTRACT") != "longlineage.regional_compat_summary" ||
        string_field(summary, "schema_version", "SUMMARY_CONTRACT") != kLegacySchemaVersion ||
        string_field(summary, "profile_id", "SUMMARY_CONTRACT") != kProfileId ||
        string_field(summary, "state", "SUMMARY_CONTRACT") != "READY_FOR_VALIDATION") {
        reject("SUMMARY_CONTRACT", "summary schema/profile identifier mismatch");
    }
    SummaryContract contract;
    contract.schema_version = std::string(kLegacySchemaVersion);
    contract.run_id = string_field(summary, "run_id", "SUMMARY_CONTRACT");
    contract.workers = uint_field(summary, "workers", "SUMMARY_CONTRACT");
    if (contract.run_id.empty() || contract.workers == 0U) {
        reject("SUMMARY_CONTRACT", "run_id must be nonempty and workers must be positive");
    }
    const json_t* counts = json_object_get(summary, "row_counts");
    require_exact_keys(counts, {"patterns", "regions", "units"}, "SUMMARY_CONTRACT", "row_counts");
    contract.regions = uint_field(counts, "regions", "SUMMARY_CONTRACT");
    contract.units = uint_field(counts, "units", "SUMMARY_CONTRACT");
    contract.patterns = uint_field(counts, "patterns", "SUMMARY_CONTRACT");
    require_nonnegative_census(json_object_get(summary, "census"), "census");
    return contract;
}

SummaryContract validate_summary_v2(const json_t* summary) {
    require_exact_keys(summary,
                       {"census",
                        "claim_ceiling",
                        "dataset_id",
                        "dataset_order",
                        "first_region",
                        "input_verification",
                        "inputs",
                        "parameters",
                        "profile_id",
                        "requested_region_count",
                        "row_counts",
                        "run_id",
                        "schema_name",
                        "schema_version",
                        "source_authority_profile",
                        "source_authority_sha256",
                        "source_manifest_path",
                        "source_manifest_run_id",
                        "source_manifest_sha256",
                        "state",
                        "timing",
                        "workers"},
                       "SUMMARY_CONTRACT", "summary v2");
    if (string_field(summary, "schema_name", "SUMMARY_CONTRACT") != "longlineage.regional_compat_summary" ||
        string_field(summary, "schema_version", "SUMMARY_CONTRACT") != kCurrentSchemaVersion ||
        string_field(summary, "profile_id", "SUMMARY_CONTRACT") != kProfileId ||
        string_field(summary, "state", "SUMMARY_CONTRACT") != "READY_FOR_VALIDATION") {
        reject("SUMMARY_CONTRACT", "summary v2 schema/profile/state mismatch");
    }

    SummaryContract contract;
    contract.schema_version = std::string(kCurrentSchemaVersion);
    contract.run_id = string_field(summary, "run_id", "SUMMARY_CONTRACT");
    contract.dataset_id = string_field(summary, "dataset_id", "SUMMARY_CONTRACT");
    contract.dataset_order = uint_field(summary, "dataset_order", "SUMMARY_CONTRACT");
    contract.source_authority_profile = string_field(summary, "source_authority_profile", "SUMMARY_CONTRACT");
    contract.source_authority_sha256 = string_field(summary, "source_authority_sha256", "SUMMARY_CONTRACT");
    contract.workers = uint_field(summary, "workers", "SUMMARY_CONTRACT");
    if (contract.run_id.empty() || contract.dataset_id.empty() || contract.workers == 0U || contract.workers > 64U ||
        !is_lower_sha256(contract.source_authority_sha256)) {
        reject("SUMMARY_CONTRACT", "summary v2 run/dataset/workers/authority digest is invalid");
    }
    validate_dataset_binding(contract, "SUMMARY_CONTRACT");

    if (uint_field(summary, "first_region", "SUMMARY_CONTRACT") != 0U ||
        uint_field(summary, "requested_region_count", "SUMMARY_CONTRACT") != 0U) {
        reject("SUMMARY_CONTRACT", "a v2 validation candidate must be a full, uncapped regional run");
    }
    contract.source_manifest_path =
        absolute_path_field(summary, "source_manifest_path", "SUMMARY_CONTRACT", "summary v2");
    contract.source_manifest_sha256 = string_field(summary, "source_manifest_sha256", "SUMMARY_CONTRACT");
    contract.source_manifest_run_id = string_field(summary, "source_manifest_run_id", "SUMMARY_CONTRACT");
    if (!is_lower_sha256(contract.source_manifest_sha256) || contract.source_manifest_run_id.empty()) {
        reject("SUMMARY_CONTRACT", "source manifest digest/run binding is invalid");
    }

    const json_t* parameters = json_object_get(summary, "parameters");
    require_exact_keys(parameters,
                       {"BASEQ_MIN", "EXTRA_NODE_CAP", "MAPQ_MIN", "MAX_SNV", "MINREAD", "PER_LEVEL_BUDGET", "TIER_R"},
                       "SUMMARY_CONTRACT", "parameters");
    const std::map<std::string, std::uint64_t> expected_parameters = {
        {"BASEQ_MIN", 0U}, {"EXTRA_NODE_CAP", 4U},        {"MAPQ_MIN", 20U},  {"MAX_SNV", 8U},
        {"MINREAD", 3U},   {"PER_LEVEL_BUDGET", 150000U}, {"TIER_R", 50000U},
    };
    for (const auto& expected : expected_parameters) {
        if (uint_field(parameters, expected.first.c_str(), "SUMMARY_CONTRACT") != expected.second) {
            reject("SUMMARY_CONTRACT", "frozen parameter mismatch: " + expected.first);
        }
    }

    const json_t* counts = json_object_get(summary, "row_counts");
    require_exact_keys(counts, {"patterns", "regions", "units"}, "SUMMARY_CONTRACT", "row_counts");
    contract.regions = uint_field(counts, "regions", "SUMMARY_CONTRACT");
    contract.units = uint_field(counts, "units", "SUMMARY_CONTRACT");
    contract.patterns = uint_field(counts, "patterns", "SUMMARY_CONTRACT");
    contract.census = validate_v2_census(json_object_get(summary, "census"));

    const json_t* verification = json_object_get(summary, "input_verification");
    require_exact_keys(
        verification,
        {"before_after_identity_stable", "embedded_hp_fallback_used", "method", "role_count", "truth_fields_seen"},
        "SUMMARY_CONTRACT", "input_verification");
    if (string_field(verification, "method", "SUMMARY_CONTRACT") != "PHYSICAL_SHA256" ||
        uint_field(verification, "role_count", "SUMMARY_CONTRACT") != kInputRoleOrder.size() ||
        uint_field(verification, "truth_fields_seen", "SUMMARY_CONTRACT") != 0U) {
        reject("SUMMARY_CONTRACT", "input verification method/count/truth boundary mismatch");
    }
    require_boolean_value(verification, "before_after_identity_stable", true, "SUMMARY_CONTRACT", "input_verification");
    require_boolean_value(verification, "embedded_hp_fallback_used", false, "SUMMARY_CONTRACT", "input_verification");

    const json_t* inputs = json_object_get(summary, "inputs");
    if (!json_is_array(inputs) || json_array_size(inputs) != kInputRoleOrder.size()) {
        reject("SUMMARY_CONTRACT", "inputs must contain exactly eight rows");
    }
    std::set<std::string> roles;
    for (std::size_t index = 0; index < kInputRoleOrder.size(); ++index) {
        const json_t* row = json_array_get(inputs, index);
        require_exact_keys(row, {"canonical_path", "full_sha256_verified", "path", "role", "sha256", "size_bytes"},
                           "SUMMARY_CONTRACT", "input row");
        const std::string role = string_field(row, "role", "SUMMARY_CONTRACT");
        const std::string digest = string_field(row, "sha256", "SUMMARY_CONTRACT");
        const std::uint64_t size_bytes = uint_field(row, "size_bytes", "SUMMARY_CONTRACT");
        if (role != kInputRoleOrder[index] || !roles.insert(role).second || size_bytes == 0U ||
            !is_lower_sha256(digest)) {
            reject("SUMMARY_CONTRACT", "input role/order/size/SHA mismatch at row " + std::to_string(index));
        }
        const std::string path = absolute_path_field(row, "path", "SUMMARY_CONTRACT", "input row");
        const std::string canonical_path = absolute_path_field(row, "canonical_path", "SUMMARY_CONTRACT", "input row");
        require_boolean_value(row, "full_sha256_verified", true, "SUMMARY_CONTRACT", "input row");
        contract.inputs.push_back({role, path, canonical_path, size_bytes, digest});
    }

    const json_t* timing = json_object_get(summary, "timing");
    require_exact_keys(timing,
                       {"input_sha256_seconds", "science_wall_seconds", "summed_input_seconds", "summed_solver_seconds",
                        "total_wall_seconds", "worker_open_seconds"},
                       "SUMMARY_CONTRACT", "timing");
    for (const char* key : {"input_sha256_seconds", "science_wall_seconds", "summed_input_seconds",
                            "summed_solver_seconds", "total_wall_seconds", "worker_open_seconds"}) {
        static_cast<void>(positive_number_field(timing, key, "SUMMARY_CONTRACT"));
    }

    const json_t* ceiling = json_object_get(summary, "claim_ceiling");
    require_exact_keys(ceiling, {"clone_ancestor_time_order", "formal_m2_topology", "production_release"},
                       "SUMMARY_CONTRACT", "claim_ceiling");
    for (const char* key : {"clone_ancestor_time_order", "formal_m2_topology", "production_release"}) {
        require_boolean_value(ceiling, key, false, "SUMMARY_CONTRACT", "claim_ceiling");
    }
    return contract;
}

SummaryContract validate_summary(const json_t* summary) {
    const std::string version = string_field(summary, "schema_version", "SUMMARY_CONTRACT");
    if (version == kLegacySchemaVersion) {
        return validate_summary_v1(summary);
    }
    if (version == kCurrentSchemaVersion) {
        return validate_summary_v2(summary);
    }
    reject("SUMMARY_CONTRACT", "unsupported regional compatibility schema version: " + version);
}

struct LockedJsonDocument {
    std::filesystem::path path;
    FileIdentity identity;
    std::string sha256;
    JsonPtr document{nullptr};
};

LockedJsonDocument load_locked_json_document(const std::filesystem::path& path, const std::string& check_id) {
    const FileIdentity before = regular_identity(path, check_id);
    const std::string bytes = read_text_file(path, check_id);
    const FileIdentity after = regular_identity(path, check_id);
    if (!(before == after)) {
        reject(check_id, "governed JSON changed while it was read: " + path.string());
    }
    json_error_t error{};
    JsonPtr document(json_loadb(bytes.data(), bytes.size(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
    if (!document || !json_is_object(document.get())) {
        std::ostringstream detail;
        detail << path.string() << ':' << error.line << ':' << error.column << ": " << error.text;
        reject(check_id, detail.str());
    }
    return {path, before, sha256_bytes(bytes), std::move(document)};
}

struct SourceManifestLock {
    LockedJsonDocument manifest;
};

void validate_manifest_runtime(const json_t* runtime, const SummaryContract& summary) {
    constexpr std::string_view kCheckId = "SOURCE_MANIFEST";
    require_exact_keys(runtime,
                       {"buffer_bytes", "compute_workers", "coordinator_slots", "halo_bp",
                        "max_estimated_alignments_per_block", "max_focal_sites_per_block", "writer_threads"},
                       std::string(kCheckId), "source manifest runtime");
    const std::uint64_t compute_workers = uint_field(runtime, "compute_workers", std::string(kCheckId));
    const std::uint64_t writer_threads = uint_field(runtime, "writer_threads", std::string(kCheckId));
    const std::uint64_t coordinator_slots = uint_field(runtime, "coordinator_slots", std::string(kCheckId));
    const std::uint64_t buffer_bytes = uint_field(runtime, "buffer_bytes", std::string(kCheckId));
    const std::uint64_t maximum_sites = uint_field(runtime, "max_focal_sites_per_block", std::string(kCheckId));
    const std::uint64_t maximum_alignments =
        uint_field(runtime, "max_estimated_alignments_per_block", std::string(kCheckId));
    const std::uint64_t halo_bp = uint_field(runtime, "halo_bp", std::string(kCheckId));
    if (compute_workers != summary.workers || compute_workers == 0U || compute_workers > 40U || writer_threads == 0U ||
        writer_threads > 4U || coordinator_slots != 2U || compute_workers + writer_threads + coordinator_slots > 46U ||
        buffer_bytes < 1048576U || buffer_bytes > 8589934592ULL || maximum_sites == 0U || maximum_sites > 4096U ||
        maximum_alignments == 0U || maximum_alignments > 250000U || halo_bp != 5000U) {
        reject(std::string(kCheckId), "source manifest runtime differs from the closed production contract");
    }
}

void validate_manifest_contract_bindings(const json_t* bindings, const RegionalCompatValidationOptions& options,
                                         bool dataset_gate_profile) {
    constexpr std::string_view kCheckId = "SOURCE_MANIFEST";
    std::set<std::string> expected;
    for (const auto& binding : kRepositoryContractBindings) {
        expected.emplace(binding.first);
    }
    if (dataset_gate_profile) {
        expected.emplace("dataset_gate_input_authority_sha256");
    }
    require_exact_keys(bindings, expected, std::string(kCheckId), "source manifest contract_bindings");

    for (const auto& binding : kRepositoryContractBindings) {
        const std::string expected_sha =
            string_field(bindings, std::string(binding.first).c_str(), std::string(kCheckId));
        if (!is_lower_sha256(expected_sha)) {
            reject(std::string(kCheckId), "invalid contract SHA-256: " + std::string(binding.first));
        }
        const std::filesystem::path source = options.repository_root / std::filesystem::path(binding.second);
        const FileIdentity before = regular_identity(source, std::string(kCheckId));
        const std::string observed_sha = sha256_file(source, std::string(kCheckId));
        const FileIdentity after = regular_identity(source, std::string(kCheckId));
        if (!(before == after) || observed_sha != expected_sha) {
            reject(std::string(kCheckId),
                   "repository contract SHA-256 differs from source manifest: " + std::string(binding.second));
        }
    }

    if (dataset_gate_profile) {
        const std::string expected_sha =
            string_field(bindings, "dataset_gate_input_authority_sha256", std::string(kCheckId));
        if (!is_lower_sha256(expected_sha)) {
            reject(std::string(kCheckId), "invalid dataset-gate authority SHA-256 binding");
        }
        const std::filesystem::path source =
            options.repository_root / "oracle" / "hcc1395_dataset_gate_input_authority.json";
        const FileIdentity before = regular_identity(source, std::string(kCheckId));
        const std::string observed_sha = sha256_file(source, std::string(kCheckId));
        const FileIdentity after = regular_identity(source, std::string(kCheckId));
        if (!(before == after) || observed_sha != expected_sha) {
            reject(std::string(kCheckId), "dataset-gate authority SHA-256 differs from source manifest");
        }
    }
}

SourceManifestLock validate_source_manifest(const RegionalCompatValidationOptions& options,
                                            const SummaryContract& summary) {
    constexpr std::string_view kCheckId = "SOURCE_MANIFEST";
    if (summary.schema_version != kCurrentSchemaVersion) {
        return {};
    }

    const std::filesystem::path manifest_path = summary.source_manifest_path.lexically_normal();
    if (manifest_path != summary.source_manifest_path) {
        reject(std::string(kCheckId), "source manifest path is not lexically normalized");
    }
    std::error_code error;
    const std::filesystem::path canonical_path = std::filesystem::canonical(summary.source_manifest_path, error);
    if (error || canonical_path != summary.source_manifest_path) {
        reject(std::string(kCheckId), "source manifest path is missing or contains a symlink alias");
    }

    SourceManifestLock result;
    result.manifest = load_locked_json_document(summary.source_manifest_path, std::string(kCheckId));
    if (result.manifest.sha256 != summary.source_manifest_sha256) {
        reject(std::string(kCheckId), "source manifest SHA-256 differs from summary binding");
    }

    const json_t* root = result.manifest.document.get();
    require_exact_keys(root,
                       {"authority_profile", "contract_bindings", "datasets", "output_root", "run_id", "runtime",
                        "schema_name", "schema_version"},
                       std::string(kCheckId), "source manifest");
    const std::string authority_profile = string_field(root, "authority_profile", std::string(kCheckId));
    const bool production_profile = authority_profile == "PRODUCTION_7_DATASET";
    const bool dataset_gate_profile = authority_profile == "HCC1395_DATASET_GATE";
    const std::string schema_version = string_field(root, "schema_version", std::string(kCheckId));
    const std::string manifest_run_id = string_field(root, "run_id", std::string(kCheckId));
    if (string_field(root, "schema_name", std::string(kCheckId)) != "longlineage.production_manifest" ||
        (!production_profile && !dataset_gate_profile) || authority_profile != summary.source_authority_profile ||
        (!production_profile && schema_version != "1.1.0") ||
        (production_profile && schema_version != "1.0.0" && schema_version != "1.1.0") ||
        !is_safe_identifier(manifest_run_id, 128U) || manifest_run_id != summary.source_manifest_run_id ||
        summary.run_id != manifest_run_id + "-" + summary.dataset_id) {
        reject(std::string(kCheckId), "source manifest schema/profile/run binding is invalid");
    }

    const std::string output_root_text = string_field(root, "output_root", std::string(kCheckId));
    const std::filesystem::path output_root =
        normalized_absolute_path(output_root_text, std::string(kCheckId), "source manifest output_root");
    if (output_root.filename() != manifest_run_id || output_root.parent_path().filename() != ".staging" ||
        output_root.parent_path().parent_path().empty()) {
        reject(std::string(kCheckId), "source manifest output_root must be /.../.staging/<manifest run ID>");
    }

    const json_t* datasets = json_object_get(root, "datasets");
    const std::size_t expected_datasets = production_profile ? kProductionDatasetOrder.size() : 1U;
    if (!json_is_array(datasets) || json_array_size(datasets) != expected_datasets) {
        reject(std::string(kCheckId), "source manifest contains the wrong dataset count");
    }
    for (std::size_t dataset_index = 0; dataset_index < expected_datasets; ++dataset_index) {
        const json_t* dataset = json_array_get(datasets, dataset_index);
        require_exact_keys(dataset, {"dataset_id", "dataset_order", "files"}, std::string(kCheckId),
                           "source manifest dataset");
        const std::string expected_dataset_id =
            production_profile ? std::string(kProductionDatasetOrder[dataset_index]) : "HCC1395";
        if (string_field(dataset, "dataset_id", std::string(kCheckId)) != expected_dataset_id ||
            uint_field(dataset, "dataset_order", std::string(kCheckId)) != dataset_index) {
            reject(std::string(kCheckId),
                   "source manifest dataset ID/order differs at index " + std::to_string(dataset_index));
        }
        const json_t* files = json_object_get(dataset, "files");
        if (!json_is_array(files) || json_array_size(files) != kInputRoleOrder.size()) {
            reject(std::string(kCheckId), "source manifest dataset must contain exactly eight inputs");
        }
        for (std::size_t file_index = 0; file_index < kInputRoleOrder.size(); ++file_index) {
            const json_t* row = json_array_get(files, file_index);
            require_exact_keys(row, {"path", "role", "sha256", "size_bytes"}, std::string(kCheckId),
                               "source manifest file row");
            const std::string role = string_field(row, "role", std::string(kCheckId));
            const std::string path_text = string_field(row, "path", std::string(kCheckId));
            static_cast<void>(normalized_absolute_path(path_text, std::string(kCheckId), "source manifest input path"));
            const std::uint64_t size_bytes = uint_field(row, "size_bytes", std::string(kCheckId));
            const std::string digest = string_field(row, "sha256", std::string(kCheckId));
            if (role != kInputRoleOrder[file_index] || size_bytes == 0U || !is_lower_sha256(digest)) {
                reject(std::string(kCheckId), "source manifest input role/order/size/SHA is invalid");
            }
            if (dataset_index == summary.dataset_order) {
                const SummaryContract::InputBinding& observed = summary.inputs[file_index];
                if (observed.role != role || observed.path != path_text || observed.canonical_path != path_text ||
                    observed.size_bytes != size_bytes || observed.sha256 != digest) {
                    reject(std::string(kCheckId),
                           "selected source manifest input differs from summary at role " + role);
                }
            }
        }
    }

    validate_manifest_runtime(json_object_get(root, "runtime"), summary);
    validate_manifest_contract_bindings(json_object_get(root, "contract_bindings"), options, dataset_gate_profile);
    return result;
}

void verify_source_manifest_stable(const SourceManifestLock& lock) {
    if (lock.manifest.path.empty()) {
        return;
    }
    if (!(regular_identity(lock.manifest.path, "SOURCE_MANIFEST_STABLE") == lock.manifest.identity) ||
        sha256_file(lock.manifest.path, "SOURCE_MANIFEST_STABLE") != lock.manifest.sha256) {
        reject("SOURCE_MANIFEST_STABLE", "source manifest identity or SHA-256 changed during bundle validation");
    }
}

struct SourceAuthorityLock {
    LockedJsonDocument authority;
    bool has_base_authority = false;
    LockedJsonDocument base_authority;
    bool has_dataset_gate_authority = false;
    LockedJsonDocument dataset_gate_authority;
};

void verify_summary_inputs_against_authority(const json_t* files, const SummaryContract& summary,
                                             const std::string& check_id) {
    if (!json_is_array(files) || json_array_size(files) != kInputRoleOrder.size() ||
        summary.inputs.size() != kInputRoleOrder.size()) {
        reject(check_id, "authority and summary must each bind exactly eight inputs");
    }
    for (std::size_t index = 0; index < kInputRoleOrder.size(); ++index) {
        const json_t* row = json_array_get(files, index);
        require_exact_keys(row, {"role", "sha256", "size_bytes"}, check_id, "authority file row");
        const std::string role = string_field(row, "role", check_id);
        const std::string digest = string_field(row, "sha256", check_id);
        const std::uint64_t size_bytes = uint_field(row, "size_bytes", check_id);
        const SummaryContract::InputBinding& observed = summary.inputs[index];
        if (role != kInputRoleOrder[index] || observed.role != role || size_bytes == 0U ||
            observed.size_bytes != size_bytes || !is_lower_sha256(digest) || observed.sha256 != digest) {
            reject(check_id,
                   "summary input differs from frozen authority at role " + std::string(kInputRoleOrder[index]));
        }
    }
}

SourceAuthorityLock validate_source_authority(const RegionalCompatValidationOptions& options,
                                              const SummaryContract& summary) {
    constexpr std::string_view kCheckId = "SOURCE_AUTHORITY";
    if (summary.schema_version != kCurrentSchemaVersion) {
        return {};
    }
    if (options.repository_root.empty() || !options.repository_root.is_absolute()) {
        reject(std::string(kCheckId), "v2 validation requires an absolute --repo-root");
    }
    struct stat root_status {};
    if (::lstat(options.repository_root.c_str(), &root_status) != 0 || !S_ISDIR(root_status.st_mode) ||
        S_ISLNK(root_status.st_mode)) {
        reject(std::string(kCheckId), "repository root must be a real non-symlink directory");
    }

    if (summary.source_authority_profile == "PRODUCTION_7_DATASET") {
        SourceAuthorityLock result;
        result.authority = load_locked_json_document(
            options.repository_root / "oracle" / "regional_compat_all_datasets_input_authority.json",
            std::string(kCheckId));
        if (result.authority.sha256 != summary.source_authority_sha256) {
            reject(std::string(kCheckId), "regional authority SHA-256 differs from summary binding");
        }
        const json_t* root = result.authority.document.get();
        require_exact_keys(
            root,
            {"base_production_input_authority_sha256", "constraints", "datasets",
             "hcc1395_dataset_gate_input_authority_sha256", "profile_id", "schema_name", "schema_version"},
            std::string(kCheckId), "regional authority");
        if (string_field(root, "schema_name", std::string(kCheckId)) !=
                "longlineage.regional_compat_all_datasets_input_authority" ||
            string_field(root, "schema_version", std::string(kCheckId)) != "1.0.0" ||
            string_field(root, "profile_id", std::string(kCheckId)) != "PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET") {
            reject(std::string(kCheckId), "regional authority identity is invalid");
        }
        const json_t* constraints = json_object_get(root, "constraints");
        require_exact_keys(constraints,
                           {"all_physical_sha256_verified", "latest_tag_join", "persisted_tagged_bam_allowed",
                            "private_source_paths_stored", "truth_fields"},
                           std::string(kCheckId), "regional authority constraints");
        if (string_field(constraints, "latest_tag_join", std::string(kCheckId)) != "EXACT_PROJECTION_NO_FALLBACK" ||
            uint_field(constraints, "truth_fields", std::string(kCheckId)) != 0U) {
            reject(std::string(kCheckId), "regional authority truth/tag constraint is invalid");
        }
        require_boolean_value(constraints, "persisted_tagged_bam_allowed", false, std::string(kCheckId),
                              "regional authority constraints");
        require_boolean_value(constraints, "all_physical_sha256_verified", true, std::string(kCheckId),
                              "regional authority constraints");
        require_boolean_value(constraints, "private_source_paths_stored", false, std::string(kCheckId),
                              "regional authority constraints");

        const std::string base_digest =
            string_field(root, "base_production_input_authority_sha256", std::string(kCheckId));
        if (!is_lower_sha256(base_digest)) {
            reject(std::string(kCheckId), "base production authority digest is invalid");
        }
        result.base_authority = load_locked_json_document(
            options.repository_root / "oracle" / "production_input_authority.json", std::string(kCheckId));
        result.has_base_authority = true;
        if (result.base_authority.sha256 != base_digest) {
            reject(std::string(kCheckId), "base production authority SHA-256 differs from regional binding");
        }
        const std::string dataset_gate_digest =
            string_field(root, "hcc1395_dataset_gate_input_authority_sha256", std::string(kCheckId));
        if (!is_lower_sha256(dataset_gate_digest)) {
            reject(std::string(kCheckId), "HCC1395 dataset-gate authority digest is invalid");
        }
        result.dataset_gate_authority = load_locked_json_document(
            options.repository_root / "oracle" / "hcc1395_dataset_gate_input_authority.json", std::string(kCheckId));
        result.has_dataset_gate_authority = true;
        if (result.dataset_gate_authority.sha256 != dataset_gate_digest) {
            reject(std::string(kCheckId), "HCC1395 dataset-gate authority SHA-256 differs from regional binding");
        }

        const json_t* datasets = json_object_get(root, "datasets");
        if (!json_is_array(datasets) || json_array_size(datasets) != kProductionDatasetOrder.size()) {
            reject(std::string(kCheckId), "regional authority must contain exactly seven datasets");
        }
        for (std::size_t index = 0; index < kProductionDatasetOrder.size(); ++index) {
            const json_t* dataset = json_array_get(datasets, index);
            require_exact_keys(dataset, {"dataset_id", "dataset_order", "files"}, std::string(kCheckId),
                               "authority dataset row");
            if (string_field(dataset, "dataset_id", std::string(kCheckId)) != kProductionDatasetOrder[index] ||
                uint_field(dataset, "dataset_order", std::string(kCheckId)) != index) {
                reject(std::string(kCheckId),
                       "regional authority dataset order differs at index " + std::to_string(index));
            }
            const json_t* files = json_object_get(dataset, "files");
            if (index == summary.dataset_order) {
                verify_summary_inputs_against_authority(files, summary, std::string(kCheckId));
            } else {
                if (!json_is_array(files) || json_array_size(files) != kInputRoleOrder.size()) {
                    reject(std::string(kCheckId), "each regional authority dataset must bind eight inputs");
                }
                for (std::size_t file_index = 0; file_index < kInputRoleOrder.size(); ++file_index) {
                    const json_t* row = json_array_get(files, file_index);
                    require_exact_keys(row, {"role", "sha256", "size_bytes"}, std::string(kCheckId),
                                       "authority file row");
                    const std::string digest = string_field(row, "sha256", std::string(kCheckId));
                    if (string_field(row, "role", std::string(kCheckId)) != kInputRoleOrder[file_index] ||
                        uint_field(row, "size_bytes", std::string(kCheckId)) == 0U || !is_lower_sha256(digest)) {
                        reject(std::string(kCheckId), "regional authority file order/value is invalid");
                    }
                }
            }
        }
        return result;
    }

    if (summary.source_authority_profile == "HCC1395_DATASET_GATE") {
        SourceAuthorityLock result;
        result.authority = load_locked_json_document(
            options.repository_root / "oracle" / "hcc1395_dataset_gate_input_authority.json", std::string(kCheckId));
        if (result.authority.sha256 != summary.source_authority_sha256) {
            reject(std::string(kCheckId), "legacy HCC authority SHA-256 differs from summary binding");
        }
        const json_t* root = result.authority.document.get();
        require_exact_keys(
            root,
            {"allowed_terminal_state", "authority_id", "authority_profile", "claim", "dataset_id", "dataset_order",
             "files", "full_content_freeze", "latest_tag_join", "private_source_paths_stored", "schema_name",
             "schema_version", "tagged_bam_persisted", "truth_fields", "variant_scope"},
            std::string(kCheckId), "legacy HCC authority");
        if (string_field(root, "schema_name", std::string(kCheckId)) != "longlineage.dataset_gate_input_authority" ||
            string_field(root, "schema_version", std::string(kCheckId)) != "1.0.0" ||
            string_field(root, "authority_profile", std::string(kCheckId)) != "HCC1395_DATASET_GATE" ||
            string_field(root, "dataset_id", std::string(kCheckId)) != "HCC1395" ||
            uint_field(root, "dataset_order", std::string(kCheckId)) != 0U ||
            uint_field(root, "truth_fields", std::string(kCheckId)) != 0U ||
            string_field(root, "latest_tag_join", std::string(kCheckId)) != "EXACT_PROJECTION_NO_FALLBACK") {
            reject(std::string(kCheckId), "legacy HCC authority identity/constraint is invalid");
        }
        require_boolean_value(root, "private_source_paths_stored", false, std::string(kCheckId),
                              "legacy HCC authority");
        require_boolean_value(root, "tagged_bam_persisted", false, std::string(kCheckId), "legacy HCC authority");
        const json_t* files = json_object_get(root, "files");
        if (!json_is_array(files) || json_array_size(files) != kInputRoleOrder.size()) {
            reject(std::string(kCheckId), "legacy HCC authority must bind exactly eight inputs");
        }
        for (std::size_t index = 0; index < kInputRoleOrder.size(); ++index) {
            const json_t* row = json_array_get(files, index);
            require_exact_keys(row, {"path_token", "role", "sha256", "size_bytes"}, std::string(kCheckId),
                               "legacy HCC authority file row");
            const std::string digest = string_field(row, "sha256", std::string(kCheckId));
            const SummaryContract::InputBinding& observed = summary.inputs[index];
            if (string_field(row, "role", std::string(kCheckId)) != kInputRoleOrder[index] ||
                observed.role != kInputRoleOrder[index] ||
                uint_field(row, "size_bytes", std::string(kCheckId)) != observed.size_bytes ||
                !is_lower_sha256(digest) || digest != observed.sha256 ||
                string_field(row, "path_token", std::string(kCheckId)).empty()) {
                reject(std::string(kCheckId), "summary input differs from legacy HCC authority");
            }
        }
        return result;
    }
    reject(std::string(kCheckId), "unsupported v2 source authority profile");
}

void verify_source_authority_stable(const SourceAuthorityLock& lock) {
    constexpr std::string_view kCheckId = "SOURCE_AUTHORITY_STABLE";
    if (lock.authority.path.empty()) {
        return;
    }
    if (!(regular_identity(lock.authority.path, std::string(kCheckId)) == lock.authority.identity) ||
        sha256_file(lock.authority.path, std::string(kCheckId)) != lock.authority.sha256) {
        reject(std::string(kCheckId), "source authority changed during bundle validation");
    }
    if (lock.has_base_authority &&
        (!(regular_identity(lock.base_authority.path, std::string(kCheckId)) == lock.base_authority.identity) ||
         sha256_file(lock.base_authority.path, std::string(kCheckId)) != lock.base_authority.sha256)) {
        reject(std::string(kCheckId), "base production authority changed during bundle validation");
    }
    if (lock.has_dataset_gate_authority &&
        (!(regular_identity(lock.dataset_gate_authority.path, std::string(kCheckId)) ==
           lock.dataset_gate_authority.identity) ||
         sha256_file(lock.dataset_gate_authority.path, std::string(kCheckId)) != lock.dataset_gate_authority.sha256)) {
        reject(std::string(kCheckId), "HCC1395 dataset-gate authority changed during bundle validation");
    }
}

struct ArtifactReceiptRow {
    std::string sha256;
    std::uint64_t rows = 0;
};

struct ProducerReceiptContract {
    std::string semantic_sha256;
    std::map<std::string, ArtifactReceiptRow> artifacts;
};

ProducerReceiptContract validate_producer_receipt_v1(const json_t* receipt, const SummaryContract& summary,
                                                     const std::map<std::string, std::string>& checksums) {
    require_exact_keys(
        receipt,
        {"artifacts", "profile_id", "run_id", "schema_name", "schema_version", "semantic_sha256", "summary_sha256"},
        "PRODUCER_RECEIPT", "producer_receipt");
    if (string_field(receipt, "schema_name", "PRODUCER_RECEIPT") != "longlineage.regional_compat_producer_receipt" ||
        string_field(receipt, "schema_version", "PRODUCER_RECEIPT") != kLegacySchemaVersion ||
        string_field(receipt, "profile_id", "PRODUCER_RECEIPT") != kProfileId ||
        string_field(receipt, "run_id", "PRODUCER_RECEIPT") != summary.run_id) {
        reject("PRODUCER_RECEIPT", "producer receipt schema/run/profile mismatch");
    }
    const std::string summary_sha = string_field(receipt, "summary_sha256", "PRODUCER_RECEIPT");
    const std::string semantic_sha = string_field(receipt, "semantic_sha256", "PRODUCER_RECEIPT");
    if (!is_lower_sha256(summary_sha) || summary_sha != checksums.at("summary.json") ||
        !is_lower_sha256(semantic_sha)) {
        reject("PRODUCER_RECEIPT", "producer receipt digest format or summary binding mismatch");
    }
    const json_t* artifacts = json_object_get(receipt, "artifacts");
    if (!json_is_array(artifacts) || json_array_size(artifacts) != 4U) {
        reject("PRODUCER_RECEIPT", "artifacts must contain exactly four rows");
    }
    ProducerReceiptContract contract;
    contract.semantic_sha256 = semantic_sha;
    constexpr std::array<std::string_view, 4> kArtifactOrder = {
        "summary.json",
        "regions.tsv",
        "units.tsv",
        "patterns.tsv",
    };
    for (std::size_t index = 0; index < json_array_size(artifacts); ++index) {
        const json_t* row = json_array_get(artifacts, index);
        require_exact_keys(row, {"path", "rows", "sha256"}, "PRODUCER_RECEIPT", "artifact row");
        const std::string path = string_field(row, "path", "PRODUCER_RECEIPT");
        const std::string sha = string_field(row, "sha256", "PRODUCER_RECEIPT");
        const std::uint64_t rows = uint_field(row, "rows", "PRODUCER_RECEIPT");
        if (path != kArtifactOrder[index] || !is_lower_sha256(sha) || sha != checksums.at(path) ||
            !contract.artifacts.emplace(path, ArtifactReceiptRow{sha, rows}).second) {
            reject("PRODUCER_RECEIPT", "invalid, duplicate or noncanonical artifact row: " + path);
        }
    }
    const std::map<std::string, std::uint64_t> expected_rows = {
        {"patterns.tsv", summary.patterns},
        {"regions.tsv", summary.regions},
        {"summary.json", 1U},
        {"units.tsv", summary.units},
    };
    for (const auto& expected : expected_rows) {
        const auto found = contract.artifacts.find(expected.first);
        if (found == contract.artifacts.end() || found->second.rows != expected.second) {
            reject("PRODUCER_RECEIPT", "artifact row count mismatch for " + expected.first);
        }
    }
    std::string canonical = std::string(kProfileId) + "\t" + std::string(kLegacySchemaVersion) + "\n";
    for (const std::string_view path : kArtifactOrder) {
        const auto& artifact = contract.artifacts.at(std::string(path));
        canonical += std::string(path) + "\t" + std::to_string(artifact.rows) + "\t" + artifact.sha256 + "\n";
    }
    if (sha256_bytes(canonical) != contract.semantic_sha256) {
        reject("PRODUCER_RECEIPT", "semantic_sha256 does not match canonical artifact-set bytes");
    }
    return contract;
}

ProducerReceiptContract validate_producer_receipt_v2(const json_t* receipt, const SummaryContract& summary,
                                                     const std::map<std::string, std::string>& checksums) {
    require_exact_keys(
        receipt,
        {"artifacts", "dataset_id", "dataset_order", "profile_id", "run_id", "schema_name", "schema_version",
         "semantic_sha256", "source_authority_profile", "source_authority_sha256", "summary_sha256"},
        "PRODUCER_RECEIPT", "producer_receipt v2");
    if (string_field(receipt, "schema_name", "PRODUCER_RECEIPT") != "longlineage.regional_compat_producer_receipt" ||
        string_field(receipt, "schema_version", "PRODUCER_RECEIPT") != kCurrentSchemaVersion ||
        string_field(receipt, "profile_id", "PRODUCER_RECEIPT") != kProfileId ||
        string_field(receipt, "run_id", "PRODUCER_RECEIPT") != summary.run_id ||
        string_field(receipt, "dataset_id", "PRODUCER_RECEIPT") != summary.dataset_id ||
        uint_field(receipt, "dataset_order", "PRODUCER_RECEIPT") != summary.dataset_order ||
        string_field(receipt, "source_authority_profile", "PRODUCER_RECEIPT") != summary.source_authority_profile ||
        string_field(receipt, "source_authority_sha256", "PRODUCER_RECEIPT") != summary.source_authority_sha256) {
        reject("PRODUCER_RECEIPT", "producer receipt v2 schema/run/dataset/authority binding mismatch");
    }
    const std::string summary_sha = string_field(receipt, "summary_sha256", "PRODUCER_RECEIPT");
    const std::string semantic_sha = string_field(receipt, "semantic_sha256", "PRODUCER_RECEIPT");
    if (!is_lower_sha256(summary_sha) || summary_sha != checksums.at("summary.json") ||
        !is_lower_sha256(semantic_sha)) {
        reject("PRODUCER_RECEIPT", "producer receipt v2 digest format or summary binding mismatch");
    }

    const json_t* artifacts = json_object_get(receipt, "artifacts");
    if (!json_is_array(artifacts) || json_array_size(artifacts) != 4U) {
        reject("PRODUCER_RECEIPT", "artifacts must contain exactly four rows");
    }
    ProducerReceiptContract contract;
    contract.semantic_sha256 = semantic_sha;
    constexpr std::array<std::string_view, 4> kArtifactOrder = {
        "summary.json",
        "regions.tsv",
        "units.tsv",
        "patterns.tsv",
    };
    for (std::size_t index = 0; index < json_array_size(artifacts); ++index) {
        const json_t* row = json_array_get(artifacts, index);
        require_exact_keys(row, {"path", "rows", "sha256"}, "PRODUCER_RECEIPT", "artifact row");
        const std::string path = string_field(row, "path", "PRODUCER_RECEIPT");
        const std::string sha = string_field(row, "sha256", "PRODUCER_RECEIPT");
        const std::uint64_t rows = uint_field(row, "rows", "PRODUCER_RECEIPT");
        if (path != kArtifactOrder[index] || !is_lower_sha256(sha) || sha != checksums.at(path) ||
            !contract.artifacts.emplace(path, ArtifactReceiptRow{sha, rows}).second) {
            reject("PRODUCER_RECEIPT", "invalid, duplicate or noncanonical artifact row: " + path);
        }
    }
    const std::map<std::string, std::uint64_t> expected_rows = {
        {"patterns.tsv", summary.patterns},
        {"regions.tsv", summary.regions},
        {"summary.json", 1U},
        {"units.tsv", summary.units},
    };
    for (const auto& expected : expected_rows) {
        const auto found = contract.artifacts.find(expected.first);
        if (found == contract.artifacts.end() || found->second.rows != expected.second) {
            reject("PRODUCER_RECEIPT", "artifact row count mismatch for " + expected.first);
        }
    }

    std::string canonical = std::string(kProfileId) + "\t" + std::string(kCurrentSchemaVersion) + "\t" +
                            summary.dataset_id + "\t" + std::to_string(summary.dataset_order) + "\t" +
                            summary.source_authority_profile + "\t" + summary.source_authority_sha256 + "\n";
    for (const std::string_view path : kArtifactOrder) {
        const auto& artifact = contract.artifacts.at(std::string(path));
        canonical += std::string(path) + "\t" + std::to_string(artifact.rows) + "\t" + artifact.sha256 + "\n";
    }
    if (sha256_bytes(canonical) != contract.semantic_sha256) {
        reject("PRODUCER_RECEIPT", "semantic_sha256 does not match v2 dataset-bound canonical bytes");
    }
    return contract;
}

ProducerReceiptContract validate_producer_receipt(const json_t* receipt, const SummaryContract& summary,
                                                  const std::map<std::string, std::string>& checksums) {
    const std::string version = string_field(receipt, "schema_version", "PRODUCER_RECEIPT");
    if (version != summary.schema_version) {
        reject("PRODUCER_RECEIPT", "producer receipt and summary schema versions differ");
    }
    if (version == kLegacySchemaVersion) {
        return validate_producer_receipt_v1(receipt, summary, checksums);
    }
    if (version == kCurrentSchemaVersion) {
        return validate_producer_receipt_v2(receipt, summary, checksums);
    }
    reject("PRODUCER_RECEIPT", "unsupported producer receipt schema version: " + version);
}

struct RegionRow {
    std::uint64_t order = 0;
    std::string id;
    std::uint64_t selected_sites = 0;
    std::uint64_t family_count = 0;
    std::uint64_t primary = 0;
    std::uint64_t controls = 0;
    std::uint64_t h3 = 0;
    std::uint64_t h4 = 0;
    std::uint64_t none = 0;
    std::string determinacy;
};

std::vector<RegionRow> validate_regions(const std::filesystem::path& path) {
    const auto rows = load_tsv(path, "REGIONS_TSV", kRegionHeader.size(), [](const std::vector<std::string>& header) {
        require_header(header, kRegionHeader, "REGIONS_TSV", "regions.tsv");
    });
    std::vector<RegionRow> result;
    result.reserve(rows.size());
    std::set<std::string> ids;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& fields = rows[index];
        RegionRow row;
        row.order = parse_uint(fields[0], "REGIONS_TSV", "region_order");
        row.id = fields[1];
        const std::uint64_t start = parse_uint(fields[3], "REGIONS_TSV", "start1");
        const std::uint64_t end = parse_uint(fields[4], "REGIONS_TSV", "end1");
        const std::uint64_t span = parse_uint(fields[5], "REGIONS_TSV", "span");
        const std::uint64_t pre_cap = parse_uint(fields[6], "REGIONS_TSV", "n_sites_pre_cap");
        row.selected_sites = parse_uint(fields[7], "REGIONS_TSV", "n_sites_selected");
        const std::uint64_t excluded = parse_uint(fields[8], "REGIONS_TSV", "n_sites_cap_excluded");
        static_cast<void>(parse_uint(fields[10], "REGIONS_TSV", "n_full_cov_reads"));
        row.family_count = parse_uint(fields[11], "REGIONS_TSV", "n_families");
        row.primary = parse_uint(fields[12], "REGIONS_TSV", "n_primary_lineages");
        row.controls = parse_uint(fields[13], "REGIONS_TSV", "n_reference_controls");
        row.h3 = parse_uint(fields[14], "REGIONS_TSV", "n_h3_aux");
        row.h4 = parse_uint(fields[15], "REGIONS_TSV", "n_h4_aux");
        row.none = parse_uint(fields[16], "REGIONS_TSV", "n_none_units");
        row.determinacy = fields[17];
        if (row.order != static_cast<std::uint64_t>(index) || row.id.empty() || fields[2].empty() ||
            !ids.insert(row.id).second || start == 0U || end < start || span != end - start || pre_cap < 2U ||
            row.selected_sites < 2U || row.selected_sites > 8U || pre_cap < row.selected_sites ||
            excluded != pre_cap - row.selected_sites ||
            row.family_count != row.primary + row.controls + row.h3 + row.h4 + row.none) {
            reject("REGIONS_TSV", "invalid region row at data line " + std::to_string(index + 2U));
        }
        const std::vector<std::string> positions = split(fields[9], ',');
        if (positions.size() != row.selected_sites) {
            reject("REGIONS_TSV", "selected_positions count mismatch for " + row.id);
        }
        std::uint64_t previous = 0;
        for (const std::string& value : positions) {
            const std::uint64_t position = parse_uint(value, "REGIONS_TSV", "selected position");
            if (position < start || position > end || position <= previous) {
                reject("REGIONS_TSV", "selected_positions are out of interval/order for " + row.id);
            }
            previous = position;
        }
        const std::set<std::string> allowed = {
            "all_determined", "has_ambiguous", "has_capped", "has_recurrence", "no_primary_lineage",
        };
        if (allowed.count(row.determinacy) == 0U ||
            ((row.primary == 0U) != (row.determinacy == "no_primary_lineage"))) {
            reject("REGIONS_TSV", "invalid determinacy/primary relationship for " + row.id);
        }
        result.push_back(std::move(row));
    }
    return result;
}

struct UnitKey {
    std::uint64_t order = 0;
    std::string family;

    bool operator<(const UnitKey& other) const noexcept {
        return std::tie(order, family) < std::tie(other.order, other.family);
    }
};

struct UnitRow {
    std::uint64_t reads = 0;
    std::uint64_t full_patterns = 0;
    std::uint64_t supported_patterns = 0;
    bool mutation_bearing = false;
    bool capped = false;
    bool primary = false;
    std::string class_name;
};

std::map<UnitKey, UnitRow> validate_units(const std::filesystem::path& path, const std::vector<RegionRow>& regions) {
    const auto rows = load_tsv(path, "UNITS_TSV", kUnitHeader.size(), [](const std::vector<std::string>& header) {
        require_header(header, kUnitHeader, "UNITS_TSV", "units.tsv");
    });
    std::map<UnitKey, UnitRow> result;
    std::vector<std::uint64_t> counts(regions.size(), 0U);
    std::vector<std::uint64_t> primary(regions.size(), 0U);
    std::vector<std::uint64_t> controls(regions.size(), 0U);
    std::vector<std::uint64_t> h3(regions.size(), 0U);
    std::vector<std::uint64_t> h4(regions.size(), 0U);
    std::vector<std::uint64_t> none(regions.size(), 0U);
    std::vector<std::vector<std::string>> primary_classes(regions.size());
    UnitKey previous;
    bool have_previous = false;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& fields = rows[index];
        const std::uint64_t order = parse_uint(fields[0], "UNITS_TSV", "region_order");
        if (order >= regions.size() || fields[1] != regions[static_cast<std::size_t>(order)].id) {
            reject("UNIT_REGION_FK",
                   "unit references unknown/mismatched region at data line " + std::to_string(index + 2U));
        }
        const UnitKey key{order, fields[2]};
        if (have_previous && !(previous < key)) {
            reject("UNITS_TSV", "unit keys are duplicate or out of order");
        }
        previous = key;
        have_previous = true;
        if (key.family != "1" && key.family != "2" && key.family != "3" && key.family != "4" && key.family != "none") {
            reject("UNITS_TSV", "unknown family: " + key.family);
        }
        const std::string& role = fields[3];
        UnitRow row;
        row.reads = parse_uint(fields[4], "UNITS_TSV", "n_reads");
        row.full_patterns = parse_uint(fields[5], "UNITS_TSV", "n_full_patterns");
        row.supported_patterns = parse_uint(fields[6], "UNITS_TSV", "n_supported_patterns");
        const std::uint64_t mutation = parse_uint(fields[7], "UNITS_TSV", "mutation_bearing");
        static_cast<void>(parse_uint(fields[8], "UNITS_TSV", "n_hidden"));
        const std::uint64_t trees = parse_uint(fields[9], "UNITS_TSV", "n_trees");
        static_cast<void>(parse_uint(fields[10], "UNITS_TSV", "n_feasible_node_sets"));
        const std::uint64_t capped = parse_uint(fields[11], "UNITS_TSV", "capped");
        row.class_name = fields[12];
        if (mutation > 1U || capped > 1U) {
            reject("UNITS_TSV", "boolean unit fields must be 0 or 1");
        }
        row.mutation_bearing = mutation == 1U;
        row.capped = capped == 1U;
        const bool primary_role =
            (key.family == "1" || key.family == "2") && mutation == 1U && role == "primary_mutation_lineage";
        row.primary = primary_role;
        const bool control_role = key.family != "none" && mutation == 0U && role == "reference_only_control";
        const bool h3_role = key.family == "3" && mutation == 1U && role == "unresolved_H3_auxiliary";
        const bool h4_role = key.family == "4" && mutation == 1U && role == "shared_H4_auxiliary";
        const bool none_role = key.family == "none" && role == "unphased_auxiliary";
        const std::set<std::string> classes = {
            "ambiguous_order", "ambiguous_structure", "capped", "determined", "recurrence_required", "underdetermined",
        };
        const bool class_count_consistent =
            (row.class_name == "determined" && trees == 1U) ||
            ((row.class_name == "ambiguous_order" || row.class_name == "ambiguous_structure") && trees > 1U) ||
            (row.class_name == "underdetermined" && trees == 0U) || row.class_name == "recurrence_required" ||
            row.class_name == "capped";
        if (!(primary_role || control_role || h3_role || h4_role || none_role) || row.reads < 3U ||
            (row.full_patterns == 0U && row.supported_patterns == 0U) || classes.count(row.class_name) == 0U ||
            row.capped != (row.class_name == "capped") || !class_count_consistent) {
            reject("UNITS_TSV", "family/role/class contract mismatch at data line " + std::to_string(index + 2U));
        }
        const std::size_t region_index = static_cast<std::size_t>(order);
        ++counts[region_index];
        primary[region_index] += primary_role ? 1U : 0U;
        if (primary_role) {
            primary_classes[region_index].push_back(row.class_name);
        }
        controls[region_index] += control_role ? 1U : 0U;
        h3[region_index] += h3_role ? 1U : 0U;
        h4[region_index] += h4_role ? 1U : 0U;
        none[region_index] += none_role ? 1U : 0U;
        if (!result.emplace(key, std::move(row)).second) {
            reject("UNITS_TSV", "duplicate unit key");
        }
    }
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const RegionRow& region = regions[index];
        if (counts[index] != region.family_count || primary[index] != region.primary ||
            controls[index] != region.controls || h3[index] != region.h3 || h4[index] != region.h4 ||
            none[index] != region.none) {
            reject("UNIT_REGION_CONSERVATION", "unit role census mismatch for " + region.id);
        }
        std::string expected_determinacy = "no_primary_lineage";
        if (!primary_classes[index].empty()) {
            const auto& classes = primary_classes[index];
            if (std::all_of(classes.begin(), classes.end(),
                            [](const std::string& value) { return value == "determined"; })) {
                expected_determinacy = "all_determined";
            } else if (std::find(classes.begin(), classes.end(), "recurrence_required") != classes.end()) {
                expected_determinacy = "has_recurrence";
            } else if (std::find(classes.begin(), classes.end(), "capped") != classes.end()) {
                expected_determinacy = "has_capped";
            } else {
                expected_determinacy = "has_ambiguous";
            }
        }
        if (region.determinacy != expected_determinacy) {
            reject("UNIT_REGION_CONSERVATION",
                   "region determinacy does not match primary unit classes for " + region.id);
        }
    }
    return result;
}

std::map<std::string, std::uint64_t> census_with_prefix(const std::map<std::string, std::uint64_t>& census,
                                                        const std::string& prefix) {
    std::map<std::string, std::uint64_t> result;
    for (const auto& entry : census) {
        if (entry.first.rfind(prefix, 0U) == 0U && entry.second != 0U) {
            result.emplace(entry.first, entry.second);
        }
    }
    return result;
}

void validate_output_census(const SummaryContract& summary, const std::vector<RegionRow>& regions,
                            const std::map<UnitKey, UnitRow>& units) {
    if (summary.schema_version != kCurrentSchemaVersion) {
        return;
    }
    std::map<std::string, std::uint64_t> region_census;
    std::map<std::string, std::uint64_t> unit_census;
    std::map<std::string, std::uint64_t> primary_census;
    for (const RegionRow& region : regions) {
        ++region_census["region_determinacy_" + region.determinacy];
    }
    for (const auto& entry : units) {
        ++unit_census["unit_class_" + entry.second.class_name];
        if (entry.second.primary) {
            ++primary_census["primary_class_" + entry.second.class_name];
        }
    }
    if (census_with_prefix(summary.census, "region_determinacy_") != region_census ||
        census_with_prefix(summary.census, "unit_class_") != unit_census ||
        census_with_prefix(summary.census, "primary_class_") != primary_census) {
        reject("OUTPUT_CENSUS", "summary class/determinacy census differs from regions.tsv or units.tsv");
    }
    const auto unsupported = summary.census.find("read_unsupported_regions");
    const std::uint64_t unsupported_regions = unsupported == summary.census.end() ? 0U : unsupported->second;
    if (summary.census.at("multi_regions_pre_read") != regions.size() + unsupported_regions) {
        reject("OUTPUT_CENSUS", "pre-read regions are not conserved by emitted plus read-unsupported regions");
    }
}

struct PatternKey {
    std::uint64_t order = 0;
    std::string family;
    std::uint8_t kind_rank = 0;
    std::string pattern;

    bool operator<(const PatternKey& other) const noexcept {
        return std::tie(order, family, kind_rank, pattern) <
               std::tie(other.order, other.family, other.kind_rank, other.pattern);
    }
};

struct ObservedPatterns {
    std::uint64_t full_rows = 0;
    std::uint64_t subread_rows = 0;
    bool has_alternate = false;
    std::map<std::string, std::uint64_t> full_counts;
    std::map<std::string, std::uint64_t> subread_counts;
};

std::size_t validate_patterns(const std::filesystem::path& path, const std::vector<RegionRow>& regions,
                              const std::map<UnitKey, UnitRow>& units) {
    const auto rows = load_tsv(path, "PATTERNS_TSV", kPatternHeader.size(), [](const std::vector<std::string>& header) {
        require_header(header, kPatternHeader, "PATTERNS_TSV", "patterns.tsv");
    });
    std::map<UnitKey, ObservedPatterns> observed_counts;
    PatternKey previous;
    bool have_previous = false;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& fields = rows[index];
        const std::uint64_t order = parse_uint(fields[0], "PATTERNS_TSV", "region_order");
        if (order >= regions.size() || fields[1] != regions[static_cast<std::size_t>(order)].id) {
            reject("PATTERN_REGION_FK", "pattern references unknown/mismatched region");
        }
        const UnitKey unit_key{order, fields[2]};
        if (units.count(unit_key) == 0U) {
            reject("PATTERN_UNIT_FK", "pattern references unknown unit");
        }
        const std::string& kind = fields[4];
        const std::uint8_t kind_rank = kind == "FULL" ? 0U : kind == "SUBREAD" ? 1U : 2U;
        if (kind_rank == 2U) {
            reject("PATTERNS_TSV", "unknown pattern_kind: " + kind);
        }
        const PatternKey key{order, fields[2], kind_rank, fields[3]};
        if (have_previous && !(previous < key)) {
            reject("PATTERNS_TSV", "pattern keys are duplicate or out of order");
        }
        previous = key;
        have_previous = true;
        const std::uint64_t count = parse_uint(fields[5], "PATTERNS_TSV", "count");
        if (key.pattern.size() != regions[static_cast<std::size_t>(order)].selected_sites ||
            std::any_of(key.pattern.begin(), key.pattern.end(),
                        [](char value) { return value != 'R' && value != 'A' && value != 'X'; }) ||
            key.pattern.find_first_of("RA") == std::string::npos ||
            (kind == "FULL" && key.pattern.find('X') != std::string::npos) || count < 3U ||
            count > units.at(unit_key).reads) {
            reject("PATTERNS_TSV", "pattern payload violates R/A/X or support contract");
        }
        auto& counts = observed_counts[unit_key];
        counts.has_alternate = counts.has_alternate || key.pattern.find('A') != std::string::npos;
        if (kind == "FULL") {
            ++counts.full_rows;
            counts.full_counts.emplace(key.pattern, count);
        } else {
            ++counts.subread_rows;
            counts.subread_counts.emplace(key.pattern, count);
        }
    }
    for (const auto& unit : units) {
        const auto found = observed_counts.find(unit.first);
        const std::uint64_t full = found == observed_counts.end() ? 0U : found->second.full_rows;
        const std::uint64_t supported = found == observed_counts.end() ? 0U : found->second.subread_rows;
        if (full != unit.second.full_patterns || supported != unit.second.supported_patterns ||
            (found != observed_counts.end() && found->second.has_alternate != unit.second.mutation_bearing)) {
            reject("PATTERN_UNIT_CONSERVATION", "pattern census mismatch for a unit");
        }
        if (found != observed_counts.end()) {
            for (const auto& full_pattern : found->second.full_counts) {
                const auto subread = found->second.subread_counts.find(full_pattern.first);
                if (subread == found->second.subread_counts.end() || subread->second != full_pattern.second) {
                    reject("PATTERN_UNIT_CONSERVATION", "every FULL pattern must have an equal-count SUBREAD row");
                }
            }
        }
    }
    return rows.size();
}

std::string dump_json(const json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    if (encoded == nullptr) {
        reject("OUTPUT_TRANSACTION", "cannot encode validation JSON");
    }
    std::string result(encoded);
    std::free(encoded);
    result.push_back('\n');
    return result;
}

JsonPtr validation_receipt_json(const RegionalCompatValidationReport& report) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "schema_name", json_string("longlineage.regional_compat_validation_receipt"));
    json_object_set_new(root.get(), "schema_version", json_string(report.schema_version.c_str()));
    json_object_set_new(root.get(), "run_id", json_string(report.run_id.c_str()));
    json_object_set_new(root.get(), "profile_id", json_string(report.profile_id.c_str()));
    if (report.schema_version == kCurrentSchemaVersion) {
        json_object_set_new(root.get(), "dataset_id", json_string(report.dataset_id.c_str()));
        json_object_set_new(root.get(), "dataset_order", json_integer(static_cast<json_int_t>(report.dataset_order)));
        json_object_set_new(root.get(), "source_authority_profile",
                            json_string(report.source_authority_profile.c_str()));
        json_object_set_new(root.get(), "source_authority_sha256", json_string(report.source_authority_sha256.c_str()));
    }
    json_object_set_new(root.get(), "state", json_string("VALIDATED_FROZEN"));
    json_object_set_new(root.get(), "producer_receipt_sha256", json_string(report.producer_receipt_sha256.c_str()));
    json_object_set_new(root.get(), "semantic_sha256", json_string(report.semantic_sha256.c_str()));
    json_object_set_new(root.get(), "validator_executable_sha256",
                        json_string(report.validator_executable_sha256.c_str()));
    JsonPtr counts(json_object());
    json_object_set_new(counts.get(), "regions", json_integer(static_cast<json_int_t>(report.region_rows)));
    json_object_set_new(counts.get(), "units", json_integer(static_cast<json_int_t>(report.unit_rows)));
    json_object_set_new(counts.get(), "patterns", json_integer(static_cast<json_int_t>(report.pattern_rows)));
    json_object_set_new(root.get(), "row_counts", counts.release());
    JsonPtr checks(json_array());
    for (const auto& check : report.checks) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "check_id", json_string(check.check_id.c_str()));
        json_object_set_new(row.get(), "status", json_string(check.passed ? "PASS" : "FAIL"));
        json_object_set_new(row.get(), "detail", json_string(check.detail.c_str()));
        json_array_append_new(checks.get(), row.release());
    }
    json_object_set_new(root.get(), "checks", checks.release());
    return root;
}

void write_sync_file(const std::filesystem::path& path, std::string_view bytes) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
    if (descriptor < 0) {
        reject("OUTPUT_TRANSACTION", "cannot create staging output: " + path.string());
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) {
            static_cast<void>(::close(descriptor));
            reject("OUTPUT_TRANSACTION", "short write for staging output: " + path.string());
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
        reject("OUTPUT_TRANSACTION", "cannot fsync/close staging output: " + path.string());
    }
}

void sync_directory(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0 || ::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
        reject("OUTPUT_TRANSACTION", "cannot fsync bundle directory");
    }
}

void write_outputs(const RegionalCompatValidationOptions& options, RegionalCompatValidationReport& report) {
    const JsonPtr receipt = validation_receipt_json(report);
    const std::string receipt_bytes = dump_json(receipt.get());
    report.validation_receipt_sha256 = sha256_bytes(receipt_bytes);
    std::string frozen_bytes = "VALIDATED_FROZEN\n";
    if (report.schema_version == kCurrentSchemaVersion) {
        frozen_bytes += "schema_version=" + report.schema_version + "\ndataset_id=" + report.dataset_id +
                        "\ndataset_order=" + std::to_string(report.dataset_order) +
                        "\nsource_authority_profile=" + report.source_authority_profile +
                        "\nsource_authority_sha256=" + report.source_authority_sha256 +
                        "\nsemantic_sha256=" + report.semantic_sha256 + "\n";
    }
    frozen_bytes += "run_id=" + report.run_id + "\nvalidation_receipt_sha256=" + report.validation_receipt_sha256 +
                    "\nproducer_receipt_sha256=" + report.producer_receipt_sha256 + "\n";
    const std::string suffix = ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    const std::filesystem::path receipt_temp = options.bundle_root / ("validation_receipt.json" + suffix);
    const std::filesystem::path frozen_temp = options.bundle_root / ("FROZEN" + suffix);
    write_sync_file(receipt_temp, receipt_bytes);
    write_sync_file(frozen_temp, frozen_bytes);
    std::error_code error;
    std::filesystem::rename(receipt_temp, report.validation_receipt_path, error);
    if (error) {
        reject("OUTPUT_TRANSACTION", "cannot publish validation receipt: " + error.message());
    }
    report.validation_receipt_written = true;
    std::filesystem::rename(frozen_temp, report.frozen_marker_path, error);
    if (error) {
        reject("OUTPUT_TRANSACTION", "cannot publish FROZEN marker: " + error.message());
    }
    report.frozen_marker_written = true;
    sync_directory(options.bundle_root);
}

}  // namespace

RegionalCompatValidationReport RegionalCompatValidator::validate_and_freeze(
    const RegionalCompatValidationOptions& options) {
    RegionalCompatValidationReport report;
    report.profile_id = std::string(kProfileId);
    report.validation_receipt_path = options.bundle_root / "validation_receipt.json";
    report.frozen_marker_path = options.bundle_root / "FROZEN";
    try {
        bool has_receipt = false;
        bool has_frozen = false;
        const auto before = inspect_layout(options.bundle_root, has_receipt, has_frozen);
        add_pass(report, "ROOT_LAYOUT", "flat bundle root contains only governed regular files");
        add_pass(report, "REQUIRED_FILES", "six required producer files are present");
        add_pass(report, "NO_EXTRA_FILES", "no unknown bundle entry is present");
        if (has_receipt || has_frozen) {
            reject("OUTPUT_TRANSACTION", "refusing to overwrite an already frozen bundle");
        }

        report.validator_executable_sha256 = sha256_file(options.validator_executable, "VALIDATOR_PROVENANCE");
        add_pass(report, "VALIDATOR_PROVENANCE", "validator executable SHA-256 replayed");

        const auto checksums = validate_checksum_manifest(options.bundle_root);
        report.producer_receipt_sha256 = checksums.at("producer_receipt.json");
        add_pass(report, "CHECKSUM_MANIFEST", "five producer artifact SHA-256 values match");

        const JsonPtr summary_json = load_json_strict(options.bundle_root / "summary.json", "SUMMARY_CONTRACT");
        const SummaryContract summary = validate_summary(summary_json.get());
        report.schema_version = summary.schema_version;
        report.run_id = summary.run_id;
        report.dataset_id = summary.dataset_id;
        report.dataset_order = summary.dataset_order;
        report.source_authority_profile = summary.source_authority_profile;
        report.source_authority_sha256 = summary.source_authority_sha256;
        report.region_rows = summary.regions;
        report.unit_rows = summary.units;
        report.pattern_rows = summary.patterns;
        add_pass(report, "SUMMARY_CONTRACT", "summary schema, profile and row counts are typed");

        const SourceAuthorityLock authority_lock = validate_source_authority(options, summary);
        if (summary.schema_version == kCurrentSchemaVersion) {
            add_pass(report, "SOURCE_AUTHORITY",
                     "repository authority identity, constraints, dataset order and eight input bindings replayed");
        }

        const SourceManifestLock manifest_lock = validate_source_manifest(options, summary);
        if (summary.schema_version == kCurrentSchemaVersion) {
            add_pass(report, "SOURCE_MANIFEST",
                     "same-byte manifest, run/output/runtime, datasets, inputs and repository contracts replayed");
        }

        const JsonPtr producer_json =
            load_json_strict(options.bundle_root / "producer_receipt.json", "PRODUCER_RECEIPT");
        const ProducerReceiptContract producer = validate_producer_receipt(producer_json.get(), summary, checksums);
        report.semantic_sha256 = producer.semantic_sha256;
        add_pass(report, "PRODUCER_RECEIPT", "receipt binds run, summary and four artifacts");

        const std::vector<RegionRow> regions = validate_regions(options.bundle_root / "regions.tsv");
        if (regions.size() != summary.regions) {
            reject("ROW_COUNTS", "regions.tsv row count differs from summary");
        }
        add_pass(report, "REGIONS_TSV", "region header, key order and typed invariants replayed");

        const std::map<UnitKey, UnitRow> units = validate_units(options.bundle_root / "units.tsv", regions);
        if (units.size() != summary.units) {
            reject("ROW_COUNTS", "units.tsv row count differs from summary");
        }
        add_pass(report, "UNITS_TSV", "unit header, key order, role and region FK replayed");
        validate_output_census(summary, regions, units);
        if (summary.schema_version == kCurrentSchemaVersion) {
            add_pass(report, "OUTPUT_CENSUS", "summary class/determinacy census independently recomputed from TSV");
        }

        const std::size_t pattern_rows = validate_patterns(options.bundle_root / "patterns.tsv", regions, units);
        if (pattern_rows != summary.patterns) {
            reject("ROW_COUNTS", "patterns.tsv row count differs from summary");
        }
        add_pass(report, "PATTERNS_TSV", "pattern header, order, R/A/X states and unit FK replayed");
        add_pass(report, "ROW_COUNTS", "summary, receipt and observed row counts agree");
        add_pass(report, "FOREIGN_KEYS", "region-unit-pattern foreign keys and censuses agree");

        verify_source_authority_stable(authority_lock);
        if (summary.schema_version == kCurrentSchemaVersion) {
            add_pass(report, "SOURCE_AUTHORITY_STABLE", "source authority identities and SHA-256 values are stable");
        }
        verify_source_manifest_stable(manifest_lock);
        if (summary.schema_version == kCurrentSchemaVersion) {
            add_pass(report, "SOURCE_MANIFEST_STABLE", "source manifest identity and SHA-256 remained stable");
        }

        const auto final_checksums = validate_checksum_manifest(options.bundle_root);
        bool after_receipt = false;
        bool after_frozen = false;
        const auto after = inspect_layout(options.bundle_root, after_receipt, after_frozen);
        if (before != after || final_checksums != checksums || after_receipt || after_frozen) {
            reject("TOCTOU_STABLE", "producer files changed during validation");
        }
        add_pass(report, "TOCTOU_STABLE", "producer SHA and identity/size/mtime/ctime snapshots are stable");

        if (options.write_frozen_outputs) {
            write_outputs(options, report);
            add_pass(report, "OUTPUT_TRANSACTION", "validation receipt then authoritative FROZEN marker published");
        } else {
            add_pass(report, "OUTPUT_TRANSACTION", "check-only mode wrote no files");
        }
        report.all_pass = true;
    } catch (const ValidationFailure& failure) {
        report.checks.push_back({failure.check_id(), false, failure.what()});
        report.all_pass = false;
    } catch (const std::exception& failure) {
        report.checks.push_back({"INTERNAL_ERROR", false, failure.what()});
        report.all_pass = false;
    }
    return report;
}

std::string render_regional_compat_validation_report_json(const RegionalCompatValidationReport& report) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "all_pass", json_boolean(report.all_pass));
    json_object_set_new(root.get(), "schema_version", json_string(report.schema_version.c_str()));
    json_object_set_new(root.get(), "run_id", json_string(report.run_id.c_str()));
    json_object_set_new(root.get(), "profile_id", json_string(report.profile_id.c_str()));
    json_object_set_new(root.get(), "dataset_id", json_string(report.dataset_id.c_str()));
    json_object_set_new(root.get(), "dataset_order", json_integer(static_cast<json_int_t>(report.dataset_order)));
    json_object_set_new(root.get(), "source_authority_profile", json_string(report.source_authority_profile.c_str()));
    json_object_set_new(root.get(), "source_authority_sha256", json_string(report.source_authority_sha256.c_str()));
    json_object_set_new(root.get(), "producer_receipt_sha256", json_string(report.producer_receipt_sha256.c_str()));
    json_object_set_new(root.get(), "semantic_sha256", json_string(report.semantic_sha256.c_str()));
    json_object_set_new(root.get(), "validator_executable_sha256",
                        json_string(report.validator_executable_sha256.c_str()));
    json_object_set_new(root.get(), "validation_receipt_sha256", json_string(report.validation_receipt_sha256.c_str()));
    json_object_set_new(root.get(), "validation_receipt_written", json_boolean(report.validation_receipt_written));
    json_object_set_new(root.get(), "frozen_marker_written", json_boolean(report.frozen_marker_written));
    JsonPtr checks(json_array());
    for (const auto& check : report.checks) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "check_id", json_string(check.check_id.c_str()));
        json_object_set_new(row.get(), "status", json_string(check.passed ? "PASS" : "FAIL"));
        json_object_set_new(row.get(), "detail", json_string(check.detail.c_str()));
        json_array_append_new(checks.get(), row.release());
    }
    json_object_set_new(root.get(), "checks", checks.release());
    return dump_json(root.get());
}

}  // namespace longlineage::validation
