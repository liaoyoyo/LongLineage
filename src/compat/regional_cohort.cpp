// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/compat/regional_cohort.hpp"

#include <jansson.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"

namespace longlineage::compat {
namespace {

constexpr std::string_view kProfileId = "PYTHON_V2_DESCRIPTIVE_REGIONAL";
constexpr std::string_view kCohortProfileId = "PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET";
constexpr std::string_view kSchemaVersion = "2.0.0";
constexpr std::string_view kAuthorityProfile = "PRODUCTION_7_DATASET";

constexpr std::array<std::string_view, 7> kDatasetOrder = {
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

constexpr std::array<std::string_view, 6> kUnitClasses = {
    "ambiguous_order", "ambiguous_structure", "capped", "determined", "recurrence_required", "underdetermined",
};

constexpr std::array<std::string_view, 5> kRegionClasses = {
    "all_determined", "has_ambiguous", "has_capped", "has_recurrence", "no_primary_lineage",
};

constexpr std::array<std::string_view, 18> kValidationChecks = {
    "ROOT_LAYOUT",
    "REQUIRED_FILES",
    "NO_EXTRA_FILES",
    "VALIDATOR_PROVENANCE",
    "CHECKSUM_MANIFEST",
    "SUMMARY_CONTRACT",
    "SOURCE_AUTHORITY",
    "SOURCE_MANIFEST",
    "PRODUCER_RECEIPT",
    "REGIONS_TSV",
    "UNITS_TSV",
    "OUTPUT_CENSUS",
    "PATTERNS_TSV",
    "ROW_COUNTS",
    "FOREIGN_KEYS",
    "SOURCE_AUTHORITY_STABLE",
    "SOURCE_MANIFEST_STABLE",
    "TOCTOU_STABLE",
};

constexpr std::array<std::string_view, 4> kIncludedCrosswalkFields = {
    "region_membership_and_selected_positions",
    "region_read_family_and_determinacy_census",
    "unit_family_role_read_pattern_hidden_tree_cap_class",
    "supported_full_and_subread_pattern_counts",
};

constexpr std::array<std::string_view, 3> kExcludedCrosswalkFields = {
    "cpp_region_order_and_pre_cap_diagnostics",
    "cpp_feasible_node_set_count",
    "python_post_tree_copy_number_and_loh",
};

const std::set<std::string> kBundleFiles = {
    "FROZEN",      "checksums.sha256", "patterns.tsv", "producer_receipt.json",
    "regions.tsv", "summary.json",     "units.tsv",    "validation_receipt.json",
};

const std::set<std::string> kAllowedCensus = {
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

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};
using JsonPointer = std::unique_ptr<json_t, JsonDeleter>;

struct LockedJsonObject {
    JsonPointer root;
    std::string sha256;
};

class CohortFailure final : public std::runtime_error {
   public:
    CohortFailure(ParseReason reason, std::string detail) : std::runtime_error(std::move(detail)), reason_(reason) {}

    [[nodiscard]] ParseReason reason() const noexcept { return reason_; }

   private:
    ParseReason reason_;
};

[[noreturn]] void reject(ParseReason reason, const std::string& detail) { throw CohortFailure(reason, detail); }

[[nodiscard]] bool is_lower_sha256(std::string_view value) noexcept {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char raw) {
               return (raw >= '0' && raw <= '9') || (raw >= 'a' && raw <= 'f');
           });
}

[[nodiscard]] bool contains_truth_token(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char raw : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw))));
    }
    return lowered.find("truth") != std::string::npos;
}

[[nodiscard]] std::filesystem::path require_real_directory(const std::filesystem::path& path, const std::string& role) {
    if (!path.is_absolute()) {
        reject(ParseReason::kMalformedValue, role + " must be an absolute path");
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
        reject(ParseReason::kIoError, role + " must be a real non-symlink directory: " + path.string());
    }
    const std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error) {
        reject(ParseReason::kIoError, "cannot canonicalize " + role + ": " + error.message());
    }
    return canonical;
}

[[nodiscard]] std::filesystem::path require_regular_file(const std::filesystem::path& path, const std::string& role) {
    if (!path.is_absolute()) {
        reject(ParseReason::kMalformedValue, role + " must be an absolute path");
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
        reject(ParseReason::kIoError, role + " must be a real non-symlink regular file: " + path.string());
    }
    const std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error) {
        reject(ParseReason::kIoError, "cannot canonicalize " + role + ": " + error.message());
    }
    return canonical;
}

void require_bundle_layout(const std::filesystem::path& root) {
    std::set<std::string> observed;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root, error), end; iterator != end; iterator.increment(error)) {
        if (error) {
            reject(ParseReason::kIoError, "cannot enumerate frozen bundle: " + error.message());
        }
        const std::string name = iterator->path().filename().string();
        if (kBundleFiles.count(name) == 0U || !observed.insert(name).second) {
            reject(ParseReason::kUnsupportedValue, "unknown or duplicate frozen bundle entry: " + name);
        }
        static_cast<void>(require_regular_file(iterator->path(), "frozen bundle entry"));
    }
    if (error) {
        reject(ParseReason::kIoError, "cannot enumerate frozen bundle: " + error.message());
    }
    if (observed != kBundleFiles) {
        reject(ParseReason::kMissingRequiredField, "frozen bundle does not contain the exact eight-file contract");
    }
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path, const std::string& role) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reject(ParseReason::kIoError, "cannot open " + role + ": " + path.string());
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if ((!input.eof() && input.fail()) || bytes.str().empty()) {
        reject(ParseReason::kIoError, "cannot read nonempty " + role + ": " + path.string());
    }
    const std::string result = bytes.str();
    if (result.back() != '\n' || result.find('\r') != std::string::npos || result.find('\0') != std::string::npos) {
        reject(ParseReason::kMalformedValue, role + " must be LF-terminated text without CR/NUL");
    }
    return result;
}

[[nodiscard]] LockedJsonObject load_locked_json(const std::filesystem::path& path, const std::string& role) {
    const std::string bytes = read_text_file(path, role);
    json_error_t error{};
    JsonPointer root(json_loadb(bytes.data(), bytes.size(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
    if (!root || !json_is_object(root.get())) {
        std::ostringstream detail;
        detail << role << " is not a strict JSON object at " << path.string() << ':' << error.line << ':'
               << error.column << ": " << error.text;
        reject(ParseReason::kMalformedValue, detail.str());
    }
    auto digest = sha256_hex(bytes);
    if (!digest.ok() || !is_lower_sha256(*digest.value)) {
        reject(ParseReason::kIoError, "cannot SHA-256 verify locked " + role + ": " + path.string());
    }
    return {std::move(root), std::move(*digest.value)};
}

void require_exact_keys(const json_t* object, std::initializer_list<std::string_view> expected,
                        const std::string& role) {
    if (!json_is_object(object)) {
        reject(ParseReason::kMalformedValue, role + " must be a JSON object");
    }
    std::set<std::string> keys;
    for (const std::string_view key : expected) {
        keys.emplace(key);
        if (json_object_get(object, std::string(key).c_str()) == nullptr) {
            reject(ParseReason::kMissingRequiredField, role + " missing required field: " + std::string(key));
        }
    }
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(object), key, value) {
        static_cast<void>(value);
        if (keys.count(key) == 0U) {
            reject(ParseReason::kUnsupportedValue, role + " contains unknown field: " + std::string(key));
        }
    }
}

template <std::size_t Size>
void require_exact_string_array(const json_t* value, const std::array<std::string_view, Size>& expected,
                                const std::string& role) {
    if (!json_is_array(value) || json_array_size(value) != expected.size()) {
        reject(ParseReason::kMalformedValue, role + " must contain the exact ordered field inventory");
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const json_t* item = json_array_get(value, index);
        const std::string_view observed = json_is_string(item)
                                              ? std::string_view(json_string_value(item), json_string_length(item))
                                              : std::string_view{};
        if (!json_is_string(item) || observed.find('\0') != std::string_view::npos || observed != expected[index]) {
            reject(ParseReason::kUnsupportedValue,
                   role + " differs from the frozen inventory at index " + std::to_string(index));
        }
    }
}

[[nodiscard]] std::string text_field(const json_t* object, const char* key, const std::string& role) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_string(value)) {
        reject(ParseReason::kMalformedValue, role + '.' + key + " must be a string");
    }
    std::string result(json_string_value(value), json_string_length(value));
    if (result.find('\0') != std::string::npos) {
        reject(ParseReason::kMalformedValue, role + '.' + key + " must not contain embedded NUL");
    }
    return result;
}

[[nodiscard]] std::uint64_t uint_field(const json_t* object, const char* key, const std::string& role) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_integer(value) || json_integer_value(value) < 0) {
        reject(ParseReason::kMalformedValue, role + '.' + key + " must be a nonnegative integer");
    }
    return static_cast<std::uint64_t>(json_integer_value(value));
}

[[nodiscard]] double number_field(const json_t* object, const char* key, const std::string& role) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_number(value)) {
        reject(ParseReason::kMalformedValue, role + '.' + key + " must be numeric");
    }
    const double result = json_number_value(value);
    if (!std::isfinite(result) || result < 0.0) {
        reject(ParseReason::kMalformedValue, role + '.' + key + " must be finite and nonnegative");
    }
    return result;
}

void require_boolean(const json_t* object, const char* key, bool expected, const std::string& role) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_boolean(value) || json_is_true(value) != expected) {
        reject(ParseReason::kUnsupportedValue,
               role + '.' + key + " must be " + (expected ? std::string("true") : std::string("false")));
    }
}

[[nodiscard]] std::vector<std::string> split_lines(const std::string& bytes) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        const std::size_t end = bytes.find('\n', begin);
        lines.push_back(bytes.substr(begin, end - begin));
        begin = end + 1U;
    }
    return lines;
}

void checked_add(std::uint64_t& target, std::uint64_t value, const std::string& role);
void checked_add(double& target, double value, const std::string& role);

[[nodiscard]] std::string digest_bytes(std::string_view bytes, const std::string& role) {
    auto digest = sha256_hex(bytes);
    if (!digest.ok() || !is_lower_sha256(*digest.value)) {
        reject(ParseReason::kIoError, "cannot SHA-256 verify " + role);
    }
    return std::move(*digest.value);
}

[[nodiscard]] std::string digest_regular_file(const std::filesystem::path& path, const std::string& role) {
    static_cast<void>(require_regular_file(path, role));
    auto digest = sha256_file(path);
    if (!digest.ok() || !is_lower_sha256(*digest.value)) {
        reject(ParseReason::kIoError, "cannot SHA-256 verify " + role + ": " + path.string());
    }
    return std::move(*digest.value);
}

[[nodiscard]] std::map<std::string, std::string> validate_bundle_checksums(RegionalCohortSample& sample) {
    const std::filesystem::path manifest_path = sample.frozen_bundle / "checksums.sha256";
    const std::string bytes = read_text_file(manifest_path, "checksum manifest");
    sample.checksum_manifest_sha256 = digest_bytes(bytes, "checksum manifest");
    constexpr std::array<std::string_view, 5> kChecksumOrder = {
        "patterns.tsv", "producer_receipt.json", "regions.tsv", "summary.json", "units.tsv",
    };
    const std::vector<std::string> lines = split_lines(bytes);
    if (lines.size() != kChecksumOrder.size()) {
        reject(ParseReason::kMalformedValue, "checksum manifest must contain exactly five rows");
    }
    std::map<std::string, std::string> checksums;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string& line = lines[index];
        if (line.size() != 66U + kChecksumOrder[index].size() || line.substr(64U, 2U) != "  ") {
            reject(ParseReason::kMalformedValue,
                   "checksum manifest row is not canonical at index " + std::to_string(index));
        }
        const std::string digest = line.substr(0U, 64U);
        const std::string name = line.substr(66U);
        if (!is_lower_sha256(digest) || name != kChecksumOrder[index] || !checksums.emplace(name, digest).second) {
            reject(ParseReason::kUnsupportedValue,
                   "checksum manifest order/name/digest mismatch at index " + std::to_string(index));
        }
    }
    sample.patterns_tsv_sha256 = digest_regular_file(sample.frozen_bundle / "patterns.tsv", "patterns.tsv");
    sample.regions_tsv_sha256 = digest_regular_file(sample.frozen_bundle / "regions.tsv", "regions.tsv");
    sample.units_tsv_sha256 = digest_regular_file(sample.frozen_bundle / "units.tsv", "units.tsv");
    if (checksums.at("summary.json") != sample.summary_sha256 ||
        checksums.at("producer_receipt.json") != sample.producer_receipt_sha256 ||
        checksums.at("regions.tsv") != sample.regions_tsv_sha256 ||
        checksums.at("units.tsv") != sample.units_tsv_sha256 ||
        checksums.at("patterns.tsv") != sample.patterns_tsv_sha256) {
        reject(ParseReason::kUnsupportedValue, "checksum manifest does not match current frozen bundle bytes");
    }
    return checksums;
}

[[nodiscard]] std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return fields;
}

template <std::size_t Size>
[[nodiscard]] bool is_closed_value(std::string_view value, const std::array<std::string_view, Size>& allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

void replay_class_census(RegionalCohortSample& sample) {
    constexpr std::string_view kRegionHeader =
        "region_order\tregion_id\tchrom\tstart1\tend1\tspan\tn_sites_pre_cap\tn_sites_selected\t"
        "n_sites_cap_excluded\tselected_positions\tn_full_cov_reads\tn_families\tn_primary_lineages\t"
        "n_reference_controls\tn_h3_aux\tn_h4_aux\tn_none_units\tdeterminacy";
    constexpr std::string_view kUnitHeader =
        "region_order\tregion_id\tfamily\trole\tn_reads\tn_full_patterns\tn_supported_patterns\tmutation_bearing\t"
        "n_hidden\tn_trees\tn_feasible_node_sets\tcapped\tclass";

    const std::string region_bytes = read_text_file(sample.frozen_bundle / "regions.tsv", "regions.tsv");
    const std::string unit_bytes = read_text_file(sample.frozen_bundle / "units.tsv", "units.tsv");
    if (digest_bytes(region_bytes, "locked regions.tsv") != sample.regions_tsv_sha256 ||
        digest_bytes(unit_bytes, "locked units.tsv") != sample.units_tsv_sha256) {
        reject(ParseReason::kUnsupportedValue, "regional TSV bytes changed between integrity and census replay");
    }

    const std::vector<std::string> region_lines = split_lines(region_bytes);
    const std::vector<std::string> unit_lines = split_lines(unit_bytes);
    if (region_lines.empty() || unit_lines.empty() || region_lines.front() != kRegionHeader ||
        unit_lines.front() != kUnitHeader) {
        reject(ParseReason::kUnsupportedValue, "regional TSV header differs during cohort census replay");
    }

    std::map<std::string, std::uint64_t> region_census;
    for (const std::string_view value : kRegionClasses) {
        region_census.emplace(std::string(value), 0U);
    }
    for (std::size_t index = 1; index < region_lines.size(); ++index) {
        const std::vector<std::string_view> fields = split_tabs(region_lines[index]);
        if (fields.size() != 18U || !is_closed_value(fields[17], kRegionClasses)) {
            reject(ParseReason::kMalformedValue,
                   "regions.tsv class census replay failed at line " + std::to_string(index + 1U));
        }
        checked_add(region_census[std::string(fields[17])], 1U, "region_determinacy_census replay");
    }

    std::map<std::string, std::uint64_t> unit_census;
    std::map<std::string, std::uint64_t> primary_census;
    for (const std::string_view value : kUnitClasses) {
        unit_census.emplace(std::string(value), 0U);
        primary_census.emplace(std::string(value), 0U);
    }
    for (std::size_t index = 1; index < unit_lines.size(); ++index) {
        const std::vector<std::string_view> fields = split_tabs(unit_lines[index]);
        if (fields.size() != 13U || !is_closed_value(fields[12], kUnitClasses)) {
            reject(ParseReason::kMalformedValue,
                   "units.tsv class census replay failed at line " + std::to_string(index + 1U));
        }
        const std::string classification(fields[12]);
        checked_add(unit_census[classification], 1U, "unit_class_census replay");
        if (fields[3] == "primary_mutation_lineage") {
            checked_add(primary_census[classification], 1U, "primary_class_census replay");
        }
    }
    if (region_lines.size() - 1U != sample.row_counts.regions || unit_lines.size() - 1U != sample.row_counts.units ||
        region_census != sample.region_determinacy_census || unit_census != sample.unit_class_census ||
        primary_census != sample.primary_class_census) {
        reject(ParseReason::kUnsupportedValue, "summary class census differs from locked regions.tsv/units.tsv replay");
    }
}

void checked_add(std::uint64_t& target, std::uint64_t value, const std::string& role) {
    constexpr std::uint64_t kJsonIntegerMaximum = static_cast<std::uint64_t>(std::numeric_limits<json_int_t>::max());
    if (value > kJsonIntegerMaximum || target > kJsonIntegerMaximum - value) {
        reject(ParseReason::kUnsupportedValue, "cohort integer overflow at " + role);
    }
    target += value;
}

void checked_add(double& target, double value, const std::string& role) {
    const double result = target + value;
    if (!std::isfinite(result)) {
        reject(ParseReason::kUnsupportedValue, "cohort floating-point overflow at " + role);
    }
    target = result;
}

template <std::size_t Size>
[[nodiscard]] std::map<std::string, std::uint64_t> extract_census(const json_t* census, std::string_view prefix,
                                                                  const std::array<std::string_view, Size>& suffixes) {
    std::map<std::string, std::uint64_t> result;
    for (const std::string_view suffix : suffixes) {
        const std::string key = std::string(prefix) + std::string(suffix);
        const json_t* value = json_object_get(census, key.c_str());
        result.emplace(std::string(suffix), value == nullptr ? 0U : uint_field(census, key.c_str(), "census"));
    }
    return result;
}

[[nodiscard]] std::uint64_t census_sum(const std::map<std::string, std::uint64_t>& census, const std::string& role) {
    std::uint64_t result = 0;
    for (const auto& row : census) {
        checked_add(result, row.second, role + '.' + row.first);
    }
    return result;
}

template <std::size_t Size>
[[nodiscard]] bool has_exact_census_keys(const std::map<std::string, std::uint64_t>& census,
                                         const std::array<std::string_view, Size>& expected) {
    if (census.size() != expected.size()) {
        return false;
    }
    return std::all_of(expected.begin(), expected.end(),
                       [&census](std::string_view key) { return census.count(std::string(key)) == 1U; });
}

void validate_summary(RegionalCohortSample& sample, const json_t* summary, std::size_t expected_order) {
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
                       "summary");
    sample.dataset_id = text_field(summary, "dataset_id", "summary");
    const std::uint64_t dataset_order = uint_field(summary, "dataset_order", "summary");
    const std::uint64_t workers = uint_field(summary, "workers", "summary");
    sample.run_id = text_field(summary, "run_id", "summary");
    sample.source_authority_profile = text_field(summary, "source_authority_profile", "summary");
    sample.source_authority_sha256 = text_field(summary, "source_authority_sha256", "summary");
    if (text_field(summary, "schema_name", "summary") != "longlineage.regional_compat_summary" ||
        text_field(summary, "schema_version", "summary") != kSchemaVersion ||
        text_field(summary, "profile_id", "summary") != kProfileId ||
        text_field(summary, "state", "summary") != "READY_FOR_VALIDATION" ||
        sample.dataset_id != kDatasetOrder[expected_order] || dataset_order != expected_order ||
        sample.source_authority_profile != kAuthorityProfile || !is_lower_sha256(sample.source_authority_sha256) ||
        sample.run_id.empty() || workers != 24U || uint_field(summary, "first_region", "summary") != 0U ||
        uint_field(summary, "requested_region_count", "summary") != 0U) {
        reject(ParseReason::kUnsupportedValue,
               "summary schema/profile/full-scope/dataset/order/authority/workers contract mismatch");
    }
    sample.dataset_order = static_cast<std::uint32_t>(dataset_order);
    sample.workers = static_cast<std::uint32_t>(workers);

    sample.source_manifest_path = text_field(summary, "source_manifest_path", "summary");
    sample.source_manifest_run_id = text_field(summary, "source_manifest_run_id", "summary");
    sample.source_manifest_sha256 = text_field(summary, "source_manifest_sha256", "summary");
    if (!sample.source_manifest_path.is_absolute() || contains_truth_token(sample.source_manifest_path.string()) ||
        sample.source_manifest_run_id.empty() || !is_lower_sha256(sample.source_manifest_sha256) ||
        sample.run_id != sample.source_manifest_run_id + "-" + sample.dataset_id) {
        reject(ParseReason::kUnsupportedValue, "summary source-manifest binding is invalid");
    }

    const json_t* parameters = json_object_get(summary, "parameters");
    require_exact_keys(parameters,
                       {"BASEQ_MIN", "EXTRA_NODE_CAP", "MAPQ_MIN", "MAX_SNV", "MINREAD", "PER_LEVEL_BUDGET", "TIER_R"},
                       "summary.parameters");
    const std::map<std::string, std::uint64_t> expected_parameters = {
        {"BASEQ_MIN", 0U}, {"EXTRA_NODE_CAP", 4U},        {"MAPQ_MIN", 20U},  {"MAX_SNV", 8U},
        {"MINREAD", 3U},   {"PER_LEVEL_BUDGET", 150000U}, {"TIER_R", 50000U},
    };
    for (const auto& expected : expected_parameters) {
        if (uint_field(parameters, expected.first.c_str(), "summary.parameters") != expected.second) {
            reject(ParseReason::kUnsupportedValue, "frozen regional parameter mismatch: " + expected.first);
        }
    }

    const json_t* counts = json_object_get(summary, "row_counts");
    require_exact_keys(counts, {"patterns", "regions", "units"}, "summary.row_counts");
    sample.row_counts.regions = uint_field(counts, "regions", "summary.row_counts");
    sample.row_counts.units = uint_field(counts, "units", "summary.row_counts");
    sample.row_counts.patterns = uint_field(counts, "patterns", "summary.row_counts");

    const json_t* census = json_object_get(summary, "census");
    if (!json_is_object(census)) {
        reject(ParseReason::kMalformedValue, "summary.census must be an object");
    }
    for (const char* required :
         {"cap_excluded_sites", "capped_regions", "multi_region_pre_cap_sites", "multi_regions_pre_read",
          "positional_singletons", "retained_selected_sites", "scope_sites"}) {
        static_cast<void>(uint_field(census, required, "summary.census"));
    }
    const char* census_key = nullptr;
    json_t* census_value = nullptr;
    json_object_foreach(const_cast<json_t*>(census), census_key, census_value) {
        if (kAllowedCensus.count(census_key) == 0U || !json_is_integer(census_value) ||
            json_integer_value(census_value) < 0) {
            reject(ParseReason::kUnsupportedValue,
                   "summary.census contains an unknown or non-integer field: " + std::string(census_key));
        }
    }
    sample.unit_class_census = extract_census(census, "unit_class_", kUnitClasses);
    sample.primary_class_census = extract_census(census, "primary_class_", kUnitClasses);
    sample.region_determinacy_census = extract_census(census, "region_determinacy_", kRegionClasses);
    if (census_sum(sample.unit_class_census, "unit_class_census") != sample.row_counts.units ||
        census_sum(sample.region_determinacy_census, "region_determinacy_census") != sample.row_counts.regions ||
        census_sum(sample.primary_class_census, "primary_class_census") > sample.row_counts.units) {
        reject(ParseReason::kUnsupportedValue, "summary class census does not conserve validated row counts");
    }

    const json_t* timing = json_object_get(summary, "timing");
    require_exact_keys(timing,
                       {"input_sha256_seconds", "science_wall_seconds", "summed_input_seconds", "summed_solver_seconds",
                        "total_wall_seconds", "worker_open_seconds"},
                       "summary.timing");
    sample.timing.total_wall_seconds = number_field(timing, "total_wall_seconds", "summary.timing");
    sample.timing.input_sha256_seconds = number_field(timing, "input_sha256_seconds", "summary.timing");
    sample.timing.science_wall_seconds = number_field(timing, "science_wall_seconds", "summary.timing");
    sample.timing.worker_open_seconds = number_field(timing, "worker_open_seconds", "summary.timing");
    sample.timing.summed_input_seconds = number_field(timing, "summed_input_seconds", "summary.timing");
    sample.timing.summed_solver_seconds = number_field(timing, "summed_solver_seconds", "summary.timing");
    for (const double observed :
         {sample.timing.total_wall_seconds, sample.timing.input_sha256_seconds, sample.timing.science_wall_seconds,
          sample.timing.worker_open_seconds, sample.timing.summed_input_seconds, sample.timing.summed_solver_seconds}) {
        if (observed <= 0.0) {
            reject(ParseReason::kUnsupportedValue, "full-run summary timing must be strictly positive");
        }
    }

    const json_t* verification = json_object_get(summary, "input_verification");
    require_exact_keys(
        verification,
        {"before_after_identity_stable", "embedded_hp_fallback_used", "method", "role_count", "truth_fields_seen"},
        "summary.input_verification");
    if (text_field(verification, "method", "summary.input_verification") != "PHYSICAL_SHA256" ||
        uint_field(verification, "role_count", "summary.input_verification") != kInputRoleOrder.size() ||
        uint_field(verification, "truth_fields_seen", "summary.input_verification") != 0U) {
        reject(ParseReason::kUnsupportedValue, "summary input verification boundary mismatch");
    }
    require_boolean(verification, "before_after_identity_stable", true, "summary.input_verification");
    require_boolean(verification, "embedded_hp_fallback_used", false, "summary.input_verification");

    const json_t* inputs = json_object_get(summary, "inputs");
    if (!json_is_array(inputs) || json_array_size(inputs) != kInputRoleOrder.size()) {
        reject(ParseReason::kMalformedValue, "summary.inputs must contain exactly eight rows");
    }
    for (std::size_t index = 0; index < kInputRoleOrder.size(); ++index) {
        const json_t* row = json_array_get(inputs, index);
        require_exact_keys(row, {"canonical_path", "full_sha256_verified", "path", "role", "sha256", "size_bytes"},
                           "summary.inputs row");
        const std::string role = text_field(row, "role", "summary.inputs row");
        const std::string path = text_field(row, "path", "summary.inputs row");
        const std::string canonical_path = text_field(row, "canonical_path", "summary.inputs row");
        if (role != kInputRoleOrder[index] || !std::filesystem::path(path).is_absolute() ||
            !std::filesystem::path(canonical_path).is_absolute() || contains_truth_token(path) ||
            contains_truth_token(canonical_path) || uint_field(row, "size_bytes", "summary.inputs row") == 0U ||
            !is_lower_sha256(text_field(row, "sha256", "summary.inputs row"))) {
            reject(ParseReason::kUnsupportedValue, "summary input role/path/size/SHA contract mismatch");
        }
        require_boolean(row, "full_sha256_verified", true, "summary.inputs row");
    }

    const json_t* ceiling = json_object_get(summary, "claim_ceiling");
    require_exact_keys(ceiling, {"clone_ancestor_time_order", "formal_m2_topology", "production_release"},
                       "summary.claim_ceiling");
    for (const char* key : {"clone_ancestor_time_order", "formal_m2_topology", "production_release"}) {
        require_boolean(ceiling, key, false, "summary.claim_ceiling");
    }
}

[[nodiscard]] std::string validate_producer(const RegionalCohortSample& sample, const json_t* producer,
                                            const std::string& summary_sha256,
                                            const std::map<std::string, std::string>& checksums) {
    require_exact_keys(
        producer,
        {"artifacts", "dataset_id", "dataset_order", "profile_id", "run_id", "schema_name", "schema_version",
         "semantic_sha256", "source_authority_profile", "source_authority_sha256", "summary_sha256"},
        "producer_receipt");
    const std::string semantic = text_field(producer, "semantic_sha256", "producer_receipt");
    if (text_field(producer, "schema_name", "producer_receipt") != "longlineage.regional_compat_producer_receipt" ||
        text_field(producer, "schema_version", "producer_receipt") != kSchemaVersion ||
        text_field(producer, "profile_id", "producer_receipt") != kProfileId ||
        text_field(producer, "run_id", "producer_receipt") != sample.run_id ||
        text_field(producer, "dataset_id", "producer_receipt") != sample.dataset_id ||
        uint_field(producer, "dataset_order", "producer_receipt") != sample.dataset_order ||
        text_field(producer, "source_authority_profile", "producer_receipt") != sample.source_authority_profile ||
        text_field(producer, "source_authority_sha256", "producer_receipt") != sample.source_authority_sha256 ||
        text_field(producer, "summary_sha256", "producer_receipt") != summary_sha256 || !is_lower_sha256(semantic)) {
        reject(ParseReason::kUnsupportedValue, "producer receipt identity or summary binding mismatch");
    }
    const json_t* artifacts = json_object_get(producer, "artifacts");
    if (!json_is_array(artifacts) || json_array_size(artifacts) != 4U) {
        reject(ParseReason::kMalformedValue, "producer receipt must contain four artifact rows");
    }
    constexpr std::array<std::string_view, 4> kArtifactOrder = {
        "summary.json",
        "regions.tsv",
        "units.tsv",
        "patterns.tsv",
    };
    const std::array<std::uint64_t, 4> expected_rows = {
        1U,
        sample.row_counts.regions,
        sample.row_counts.units,
        sample.row_counts.patterns,
    };
    for (std::size_t index = 0; index < kArtifactOrder.size(); ++index) {
        const json_t* row = json_array_get(artifacts, index);
        require_exact_keys(row, {"path", "rows", "sha256"}, "producer_receipt.artifact");
        const std::string path = text_field(row, "path", "producer_receipt.artifact");
        const std::string digest = text_field(row, "sha256", "producer_receipt.artifact");
        if (path != kArtifactOrder[index] ||
            uint_field(row, "rows", "producer_receipt.artifact") != expected_rows[index] || !is_lower_sha256(digest) ||
            checksums.at(path) != digest || (index == 0U && digest != summary_sha256)) {
            reject(ParseReason::kUnsupportedValue, "producer receipt artifact binding mismatch");
        }
    }
    return semantic;
}

void validate_validation_receipt(RegionalCohortSample& sample, const json_t* validation,
                                 const std::string& producer_sha256, const std::string& semantic_sha256) {
    require_exact_keys(validation,
                       {"checks", "dataset_id", "dataset_order", "producer_receipt_sha256", "profile_id", "row_counts",
                        "run_id", "schema_name", "schema_version", "semantic_sha256", "source_authority_profile",
                        "source_authority_sha256", "state", "validator_executable_sha256"},
                       "validation_receipt");
    if (text_field(validation, "schema_name", "validation_receipt") !=
            "longlineage.regional_compat_validation_receipt" ||
        text_field(validation, "schema_version", "validation_receipt") != kSchemaVersion ||
        text_field(validation, "profile_id", "validation_receipt") != kProfileId ||
        text_field(validation, "state", "validation_receipt") != "VALIDATED_FROZEN" ||
        text_field(validation, "run_id", "validation_receipt") != sample.run_id ||
        text_field(validation, "dataset_id", "validation_receipt") != sample.dataset_id ||
        uint_field(validation, "dataset_order", "validation_receipt") != sample.dataset_order ||
        text_field(validation, "source_authority_profile", "validation_receipt") != sample.source_authority_profile ||
        text_field(validation, "source_authority_sha256", "validation_receipt") != sample.source_authority_sha256 ||
        text_field(validation, "producer_receipt_sha256", "validation_receipt") != producer_sha256 ||
        text_field(validation, "semantic_sha256", "validation_receipt") != semantic_sha256 ||
        !is_lower_sha256(text_field(validation, "validator_executable_sha256", "validation_receipt"))) {
        reject(ParseReason::kUnsupportedValue, "validation receipt identity or digest binding mismatch");
    }
    sample.validator_executable_sha256 = text_field(validation, "validator_executable_sha256", "validation_receipt");
    const json_t* counts = json_object_get(validation, "row_counts");
    require_exact_keys(counts, {"patterns", "regions", "units"}, "validation_receipt.row_counts");
    if (uint_field(counts, "regions", "validation_receipt.row_counts") != sample.row_counts.regions ||
        uint_field(counts, "units", "validation_receipt.row_counts") != sample.row_counts.units ||
        uint_field(counts, "patterns", "validation_receipt.row_counts") != sample.row_counts.patterns) {
        reject(ParseReason::kUnsupportedValue, "validation receipt row counts differ from summary");
    }
    const json_t* checks = json_object_get(validation, "checks");
    if (!json_is_array(checks) || json_array_size(checks) != kValidationChecks.size()) {
        reject(ParseReason::kMalformedValue, "validation receipt must contain the exact eighteen v2 checks");
    }
    for (std::size_t index = 0; index < kValidationChecks.size(); ++index) {
        const json_t* row = json_array_get(checks, index);
        require_exact_keys(row, {"check_id", "detail", "status"}, "validation_receipt.check");
        if (text_field(row, "check_id", "validation_receipt.check") != kValidationChecks[index] ||
            text_field(row, "status", "validation_receipt.check") != "PASS" ||
            text_field(row, "detail", "validation_receipt.check").empty()) {
            reject(ParseReason::kUnsupportedValue, "validation receipt check order/status/detail mismatch");
        }
    }
}

void validate_frozen_marker(const RegionalCohortSample& sample, const std::string& semantic_sha256,
                            const std::string& bytes) {
    const std::vector<std::string> expected = {
        "VALIDATED_FROZEN",
        "schema_version=" + std::string(kSchemaVersion),
        "dataset_id=" + sample.dataset_id,
        "dataset_order=" + std::to_string(sample.dataset_order),
        "source_authority_profile=" + sample.source_authority_profile,
        "source_authority_sha256=" + sample.source_authority_sha256,
        "semantic_sha256=" + semantic_sha256,
        "run_id=" + sample.run_id,
        "validation_receipt_sha256=" + sample.validation_receipt_sha256,
        "producer_receipt_sha256=" + sample.producer_receipt_sha256,
    };
    if (split_lines(bytes) != expected) {
        reject(ParseReason::kUnsupportedValue, "FROZEN marker does not exactly bind current v2 receipts");
    }
}

void validate_crosswalk_layer(const json_t* crosswalks, const char* layer, std::uint64_t expected_rows) {
    const json_t* row = json_object_get(crosswalks, layer);
    require_exact_keys(row, {"actual_map_sha256", "expected_map_sha256", "mismatches", "rows_actual", "rows_expected"},
                       std::string("crosswalk.") + layer);
    const std::string expected_digest = text_field(row, "expected_map_sha256", "crosswalk layer");
    const std::string actual_digest = text_field(row, "actual_map_sha256", "crosswalk layer");
    if (uint_field(row, "mismatches", "crosswalk layer") != 0U ||
        uint_field(row, "rows_expected", "crosswalk layer") != expected_rows ||
        uint_field(row, "rows_actual", "crosswalk layer") != expected_rows || !is_lower_sha256(expected_digest) ||
        expected_digest != actual_digest) {
        reject(ParseReason::kUnsupportedValue, std::string("crosswalk mismatch/count/digest failure at ") + layer);
    }
}

void validate_crosswalk(RegionalCohortSample& sample, const json_t* crosswalk) {
    require_exact_keys(crosswalk,
                       {"all_exact", "claim_ceiling", "cpp_frozen_bundle", "crosswalks", "dataset_id",
                        "field_inventory", "profile_id", "python_frozen_authority", "schema_name", "schema_version"},
                       "crosswalk_receipt");
    if (text_field(crosswalk, "schema_name", "crosswalk_receipt") != "longlineage.regional_compat_crosswalk_receipt" ||
        text_field(crosswalk, "schema_version", "crosswalk_receipt") != kSchemaVersion ||
        text_field(crosswalk, "profile_id", "crosswalk_receipt") != kProfileId ||
        text_field(crosswalk, "dataset_id", "crosswalk_receipt") != sample.dataset_id) {
        reject(ParseReason::kUnsupportedValue, "crosswalk schema/profile/dataset identity mismatch");
    }
    require_boolean(crosswalk, "all_exact", true, "crosswalk_receipt");

    const json_t* ceiling = json_object_get(crosswalk, "claim_ceiling");
    require_exact_keys(
        ceiling,
        {"descriptive_python_compatibility_only", "formal_topology_claim_allowed", "production_release_claim_allowed"},
        "crosswalk_receipt.claim_ceiling");
    require_boolean(ceiling, "descriptive_python_compatibility_only", true, "crosswalk_receipt.claim_ceiling");
    require_boolean(ceiling, "formal_topology_claim_allowed", false, "crosswalk_receipt.claim_ceiling");
    require_boolean(ceiling, "production_release_claim_allowed", false, "crosswalk_receipt.claim_ceiling");

    const json_t* inventory = json_object_get(crosswalk, "field_inventory");
    require_exact_keys(inventory, {"comparison_scope", "excluded_nonshared_fields", "included"},
                       "crosswalk_receipt.field_inventory");
    if (text_field(inventory, "comparison_scope", "crosswalk_receipt.field_inventory") !=
        "shared_descriptive_fields_only") {
        reject(ParseReason::kUnsupportedValue, "crosswalk comparison scope differs from the frozen contract");
    }
    require_exact_string_array(json_object_get(inventory, "included"), kIncludedCrosswalkFields,
                               "crosswalk_receipt.field_inventory.included");
    require_exact_string_array(json_object_get(inventory, "excluded_nonshared_fields"), kExcludedCrosswalkFields,
                               "crosswalk_receipt.field_inventory.excluded_nonshared_fields");

    const json_t* cpp = json_object_get(crosswalk, "cpp_frozen_bundle");
    require_exact_keys(cpp, {"bundle", "summary_sha256", "validation_receipt_sha256"},
                       "crosswalk_receipt.cpp_frozen_bundle");
    const std::filesystem::path crosswalk_bundle =
        require_real_directory(std::filesystem::path(text_field(cpp, "bundle", "crosswalk_receipt.cpp_frozen_bundle")),
                               "crosswalk-bound bundle");
    if (crosswalk_bundle != sample.frozen_bundle ||
        text_field(cpp, "summary_sha256", "crosswalk_receipt.cpp_frozen_bundle") != sample.summary_sha256 ||
        text_field(cpp, "validation_receipt_sha256", "crosswalk_receipt.cpp_frozen_bundle") !=
            sample.validation_receipt_sha256) {
        reject(ParseReason::kUnsupportedValue, "crosswalk does not bind the supplied frozen bundle and receipts");
    }

    const json_t* python = json_object_get(crosswalk, "python_frozen_authority");
    require_exact_keys(
        python, {"authority_sha256", "output_manifest", "output_manifest_sha256", "region_view", "region_view_sha256"},
        "crosswalk_receipt.python_frozen_authority");
    const std::string output_manifest = text_field(python, "output_manifest", "python_frozen_authority");
    const std::string region_view = text_field(python, "region_view", "python_frozen_authority");
    sample.python_output_manifest_sha256 = text_field(python, "output_manifest_sha256", "python_frozen_authority");
    sample.python_region_view_sha256 = text_field(python, "region_view_sha256", "python_frozen_authority");
    sample.python_authority_sha256 = text_field(python, "authority_sha256", "python_frozen_authority");
    if (!std::filesystem::path(output_manifest).is_absolute() || !std::filesystem::path(region_view).is_absolute() ||
        !is_lower_sha256(sample.python_output_manifest_sha256) || !is_lower_sha256(sample.python_region_view_sha256) ||
        !is_lower_sha256(sample.python_authority_sha256)) {
        reject(ParseReason::kUnsupportedValue, "crosswalk Python authority path/digest binding is invalid");
    }

    const json_t* layers = json_object_get(crosswalk, "crosswalks");
    require_exact_keys(layers, {"patterns", "regions", "units"}, "crosswalk_receipt.crosswalks");
    validate_crosswalk_layer(layers, "regions", sample.row_counts.regions);
    validate_crosswalk_layer(layers, "units", sample.row_counts.units);
    validate_crosswalk_layer(layers, "patterns", sample.row_counts.patterns);
}

[[nodiscard]] RegionalCohortSample load_sample(const RegionalCohortInput& input, std::size_t expected_order) {
    RegionalCohortSample sample;
    sample.frozen_bundle = require_real_directory(input.frozen_bundle, "frozen bundle");
    sample.crosswalk_receipt = require_regular_file(input.crosswalk_receipt, "crosswalk receipt");
    require_bundle_layout(sample.frozen_bundle);

    const std::filesystem::path summary_path = sample.frozen_bundle / "summary.json";
    const std::filesystem::path producer_path = sample.frozen_bundle / "producer_receipt.json";
    const std::filesystem::path validation_path = sample.frozen_bundle / "validation_receipt.json";
    const std::filesystem::path frozen_path = sample.frozen_bundle / "FROZEN";
    LockedJsonObject summary = load_locked_json(summary_path, "summary");
    LockedJsonObject producer = load_locked_json(producer_path, "producer receipt");
    LockedJsonObject validation = load_locked_json(validation_path, "validation receipt");
    LockedJsonObject crosswalk = load_locked_json(sample.crosswalk_receipt, "crosswalk receipt");
    const std::string frozen_bytes = read_text_file(frozen_path, "FROZEN marker");
    auto frozen_sha256 = sha256_hex(frozen_bytes);
    if (!frozen_sha256.ok() || !is_lower_sha256(*frozen_sha256.value)) {
        reject(ParseReason::kIoError, "cannot SHA-256 verify locked FROZEN marker");
    }

    sample.summary_sha256 = std::move(summary.sha256);
    sample.producer_receipt_sha256 = std::move(producer.sha256);
    sample.validation_receipt_sha256 = std::move(validation.sha256);
    sample.frozen_marker_sha256 = std::move(*frozen_sha256.value);
    sample.crosswalk_receipt_sha256 = std::move(crosswalk.sha256);

    validate_summary(sample, summary.root.get(), expected_order);
    const std::map<std::string, std::string> checksums = validate_bundle_checksums(sample);
    replay_class_census(sample);
    const std::string semantic_sha256 =
        validate_producer(sample, producer.root.get(), sample.summary_sha256, checksums);
    validate_validation_receipt(sample, validation.root.get(), sample.producer_receipt_sha256, semantic_sha256);
    validate_frozen_marker(sample, semantic_sha256, frozen_bytes);
    validate_crosswalk(sample, crosswalk.root.get());
    return sample;
}

void add_census(std::map<std::string, std::uint64_t>& target, const std::map<std::string, std::uint64_t>& source,
                const std::string& role) {
    for (const auto& row : source) {
        checked_add(target[row.first], row.second, role + '.' + row.first);
    }
}

void add_timing(RegionalCohortTiming& target, const RegionalCohortTiming& source) {
    checked_add(target.total_wall_seconds, source.total_wall_seconds, "timing.total_wall_seconds");
    checked_add(target.input_sha256_seconds, source.input_sha256_seconds, "timing.input_sha256_seconds");
    checked_add(target.science_wall_seconds, source.science_wall_seconds, "timing.science_wall_seconds");
    checked_add(target.worker_open_seconds, source.worker_open_seconds, "timing.worker_open_seconds");
    checked_add(target.summed_input_seconds, source.summed_input_seconds, "timing.summed_input_seconds");
    checked_add(target.summed_solver_seconds, source.summed_solver_seconds, "timing.summed_solver_seconds");
}

void append_source_set_row(std::ostringstream& output, const RegionalCohortSample& sample) {
    output << sample.dataset_order << '\t' << sample.dataset_id << '\t' << sample.source_manifest_path.string() << '\t'
           << sample.source_manifest_run_id << '\t' << sample.source_manifest_sha256 << '\t'
           << sample.validator_executable_sha256 << '\t' << sample.summary_sha256 << '\t'
           << sample.producer_receipt_sha256 << '\t' << sample.validation_receipt_sha256 << '\t'
           << sample.frozen_marker_sha256 << '\t' << sample.checksum_manifest_sha256 << '\t'
           << sample.regions_tsv_sha256 << '\t' << sample.units_tsv_sha256 << '\t' << sample.patterns_tsv_sha256 << '\t'
           << sample.crosswalk_receipt_sha256 << '\t' << sample.python_authority_sha256 << '\n';
}

void append_chart_payload_row(std::ostringstream& output, const RegionalCohortSample& sample) {
    output << sample.dataset_order << '\t' << sample.dataset_id << '\t' << sample.row_counts.regions << '\t'
           << sample.row_counts.units << '\t' << sample.row_counts.patterns;
    for (const std::string_view key : kUnitClasses) {
        output << '\t' << sample.unit_class_census.at(std::string(key));
    }
    for (const std::string_view key : kUnitClasses) {
        output << '\t' << sample.primary_class_census.at(std::string(key));
    }
    for (const std::string_view key : kRegionClasses) {
        output << '\t' << sample.region_determinacy_census.at(std::string(key));
    }
    output << std::hexfloat << '\t' << sample.timing.total_wall_seconds << '\t' << sample.timing.input_sha256_seconds
           << '\t' << sample.timing.science_wall_seconds << '\t' << sample.timing.worker_open_seconds << '\t'
           << sample.timing.summed_input_seconds << '\t' << sample.timing.summed_solver_seconds << std::defaultfloat
           << '\t' << (sample.peak_rss.available ? 1 : 0) << '\t' << sample.peak_rss.bytes.value_or(0U) << '\t'
           << sample.peak_rss.source << '\n';
}

[[nodiscard]] std::string chart_payload_sha256(const std::vector<RegionalCohortSample>& samples) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << kCohortProfileId << "\tchart_payload\t1.0.0\n";
    for (const RegionalCohortSample& sample : samples) {
        append_chart_payload_row(canonical, sample);
    }
    return digest_bytes(canonical.str(), "chart payload");
}

[[nodiscard]] bool same_timing(const RegionalCohortTiming& left, const RegionalCohortTiming& right) noexcept {
    return left.total_wall_seconds == right.total_wall_seconds &&
           left.input_sha256_seconds == right.input_sha256_seconds &&
           left.science_wall_seconds == right.science_wall_seconds &&
           left.worker_open_seconds == right.worker_open_seconds &&
           left.summed_input_seconds == right.summed_input_seconds &&
           left.summed_solver_seconds == right.summed_solver_seconds;
}

void validate_peak_rss(const RegionalCohortPeakRss& peak, const std::string& role) {
    if (peak.available != peak.bytes.has_value() || (peak.available && peak.source.empty()) ||
        (!peak.available && !peak.source.empty())) {
        reject(ParseReason::kUnsupportedValue, role + " has an internally inconsistent availability contract");
    }
}

void validate_receipt_for_render(const RegionalCohortReceipt& receipt) {
    if (receipt.samples.size() != kDatasetOrder.size() || !receipt.all_sources_validated_frozen ||
        !receipt.all_crosswalk_receipts_exact || receipt.source_authority_profile != kAuthorityProfile ||
        !is_lower_sha256(receipt.source_authority_sha256) || !is_lower_sha256(receipt.python_authority_sha256) ||
        !is_lower_sha256(receipt.source_set_sha256) || !is_lower_sha256(receipt.chart_payload_sha256)) {
        reject(ParseReason::kUnsupportedValue, "cohort receipt is not a complete seven-dataset validated handle");
    }

    RegionalCohortReceipt replay;
    replay.source_authority_profile = receipt.source_authority_profile;
    replay.source_authority_sha256 = receipt.source_authority_sha256;
    replay.python_authority_sha256 = receipt.python_authority_sha256;
    std::set<std::filesystem::path> bundles;
    std::set<std::filesystem::path> crosswalks;
    const RegionalCohortSample& cohort_source = receipt.samples.front();
    std::ostringstream source_set;
    source_set.imbue(std::locale::classic());
    source_set << kCohortProfileId << "\t1.0.0\n";
    for (std::size_t index = 0; index < receipt.samples.size(); ++index) {
        const RegionalCohortSample& sample = receipt.samples[index];
        const std::array<std::string_view, 15> digests = {
            sample.source_authority_sha256,
            sample.source_manifest_sha256,
            sample.validator_executable_sha256,
            sample.summary_sha256,
            sample.producer_receipt_sha256,
            sample.validation_receipt_sha256,
            sample.frozen_marker_sha256,
            sample.crosswalk_receipt_sha256,
            sample.checksum_manifest_sha256,
            sample.regions_tsv_sha256,
            sample.units_tsv_sha256,
            sample.patterns_tsv_sha256,
            sample.python_output_manifest_sha256,
            sample.python_region_view_sha256,
            sample.python_authority_sha256,
        };
        if (sample.dataset_id != kDatasetOrder[index] || sample.dataset_order != index || sample.workers != 24U ||
            sample.run_id != sample.source_manifest_run_id + "-" + sample.dataset_id ||
            !sample.source_manifest_path.is_absolute() || !sample.frozen_bundle.is_absolute() ||
            !sample.crosswalk_receipt.is_absolute() ||
            sample.source_authority_profile != receipt.source_authority_profile ||
            sample.source_authority_sha256 != receipt.source_authority_sha256 ||
            sample.python_authority_sha256 != receipt.python_authority_sha256 ||
            sample.source_manifest_path != cohort_source.source_manifest_path ||
            sample.source_manifest_run_id != cohort_source.source_manifest_run_id ||
            sample.source_manifest_sha256 != cohort_source.source_manifest_sha256 ||
            sample.validator_executable_sha256 != cohort_source.validator_executable_sha256 ||
            !std::all_of(digests.begin(), digests.end(), is_lower_sha256) ||
            !bundles.insert(sample.frozen_bundle).second || !crosswalks.insert(sample.crosswalk_receipt).second ||
            !has_exact_census_keys(sample.unit_class_census, kUnitClasses) ||
            !has_exact_census_keys(sample.primary_class_census, kUnitClasses) ||
            !has_exact_census_keys(sample.region_determinacy_census, kRegionClasses) ||
            census_sum(sample.unit_class_census, "render unit census") != sample.row_counts.units ||
            census_sum(sample.region_determinacy_census, "render region census") != sample.row_counts.regions ||
            census_sum(sample.primary_class_census, "render primary census") > sample.row_counts.units) {
            reject(ParseReason::kUnsupportedValue,
                   "cohort sample invariant failed before rendering at order " + std::to_string(index));
        }
        for (const double timing : {sample.timing.total_wall_seconds, sample.timing.input_sha256_seconds,
                                    sample.timing.science_wall_seconds, sample.timing.worker_open_seconds,
                                    sample.timing.summed_input_seconds, sample.timing.summed_solver_seconds}) {
            if (!std::isfinite(timing) || timing <= 0.0) {
                reject(ParseReason::kUnsupportedValue, "cohort sample timing is invalid before rendering");
            }
        }
        validate_peak_rss(sample.peak_rss, "sample peak RSS");
        checked_add(replay.total_row_counts.regions, sample.row_counts.regions, "render row_counts.regions");
        checked_add(replay.total_row_counts.units, sample.row_counts.units, "render row_counts.units");
        checked_add(replay.total_row_counts.patterns, sample.row_counts.patterns, "render row_counts.patterns");
        add_census(replay.total_unit_class_census, sample.unit_class_census, "render unit census");
        add_census(replay.total_primary_class_census, sample.primary_class_census, "render primary census");
        add_census(replay.total_region_determinacy_census, sample.region_determinacy_census, "render region census");
        add_timing(replay.sequential_timing_totals, sample.timing);
        if (sample.peak_rss.available && (!replay.maximum_peak_rss.available ||
                                          *sample.peak_rss.bytes > replay.maximum_peak_rss.bytes.value_or(0U))) {
            replay.maximum_peak_rss = sample.peak_rss;
        }
        append_source_set_row(source_set, sample);
    }
    const std::string replay_sha256 = digest_bytes(source_set.str(), "render source set");
    validate_peak_rss(receipt.maximum_peak_rss, "maximum peak RSS");
    if (receipt.total_row_counts.regions != replay.total_row_counts.regions ||
        receipt.total_row_counts.units != replay.total_row_counts.units ||
        receipt.total_row_counts.patterns != replay.total_row_counts.patterns ||
        receipt.total_unit_class_census != replay.total_unit_class_census ||
        receipt.total_primary_class_census != replay.total_primary_class_census ||
        receipt.total_region_determinacy_census != replay.total_region_determinacy_census ||
        !same_timing(receipt.sequential_timing_totals, replay.sequential_timing_totals) ||
        receipt.maximum_peak_rss.available != replay.maximum_peak_rss.available ||
        receipt.maximum_peak_rss.bytes != replay.maximum_peak_rss.bytes ||
        receipt.maximum_peak_rss.source != replay.maximum_peak_rss.source ||
        receipt.source_set_sha256 != replay_sha256 ||
        receipt.chart_payload_sha256 != chart_payload_sha256(receipt.samples)) {
        reject(ParseReason::kUnsupportedValue, "cohort totals/source-set digest failed render-time replay");
    }
}

void set_json(json_t* object, const char* key, json_t* value) {
    if (value == nullptr) {
        throw std::runtime_error(std::string("cannot allocate JSON field: ") + key);
    }
    // Jansson steals the new reference on both success and failure.
    if (json_object_set_new(object, key, value) != 0) {
        throw std::runtime_error(std::string("cannot set JSON field: ") + key);
    }
}

void set_text(json_t* object, const char* key, const std::string& value) {
    set_json(object, key, json_string(value.c_str()));
}

void set_uint(json_t* object, const char* key, std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<json_int_t>::max())) {
        throw std::runtime_error(std::string("JSON integer overflow: ") + key);
    }
    set_json(object, key, json_integer(static_cast<json_int_t>(value)));
}

void set_number(json_t* object, const char* key, double value) { set_json(object, key, json_real(value)); }

[[nodiscard]] JsonPointer row_counts_json(const RegionalCohortRowCounts& counts) {
    JsonPointer output(json_object());
    set_uint(output.get(), "regions", counts.regions);
    set_uint(output.get(), "units", counts.units);
    set_uint(output.get(), "patterns", counts.patterns);
    return output;
}

[[nodiscard]] JsonPointer timing_json(const RegionalCohortTiming& timing) {
    JsonPointer output(json_object());
    set_number(output.get(), "total_wall_seconds", timing.total_wall_seconds);
    set_number(output.get(), "input_sha256_seconds", timing.input_sha256_seconds);
    set_number(output.get(), "science_wall_seconds", timing.science_wall_seconds);
    set_number(output.get(), "worker_open_seconds", timing.worker_open_seconds);
    set_number(output.get(), "summed_input_seconds", timing.summed_input_seconds);
    set_number(output.get(), "summed_solver_seconds", timing.summed_solver_seconds);
    return output;
}

[[nodiscard]] JsonPointer census_json(const std::map<std::string, std::uint64_t>& census) {
    JsonPointer output(json_object());
    for (const auto& row : census) {
        set_uint(output.get(), row.first.c_str(), row.second);
    }
    return output;
}

[[nodiscard]] JsonPointer class_census_json(const std::map<std::string, std::uint64_t>& units,
                                            const std::map<std::string, std::uint64_t>& primary,
                                            const std::map<std::string, std::uint64_t>& regions) {
    JsonPointer output(json_object());
    set_text(output.get(), "encoding", "CLOSED_CATEGORIES_ZERO_FILLED_FROM_SPARSE_VALIDATED_SOURCE");
    set_json(output.get(), "unit_classes", census_json(units).release());
    set_json(output.get(), "primary_classes", census_json(primary).release());
    set_json(output.get(), "region_determinacy", census_json(regions).release());
    return output;
}

[[nodiscard]] JsonPointer peak_rss_json(const RegionalCohortPeakRss& peak) {
    JsonPointer output(json_object());
    set_json(output.get(), "available", json_boolean(peak.available));
    if (peak.available && peak.bytes.has_value()) {
        set_uint(output.get(), "bytes", *peak.bytes);
        set_text(output.get(), "source", peak.source);
        set_json(output.get(), "reason", json_null());
    } else {
        set_json(output.get(), "bytes", json_null());
        set_json(output.get(), "source", json_null());
        set_text(output.get(), "reason", "NOT_PRESENT_IN_VALIDATED_REGIONAL_BUNDLE_CONTRACT");
    }
    return output;
}

}  // namespace

ParseResult<RegionalCohortReceipt> aggregate_frozen_regional_cohort(const std::vector<RegionalCohortInput>& inputs) {
    try {
        if (inputs.size() != kDatasetOrder.size()) {
            return ParseResult<RegionalCohortReceipt>::failure(ParseReason::kMissingRequiredField,
                                                               "regional cohort requires exactly seven paired inputs");
        }
        RegionalCohortReceipt output;
        std::set<std::filesystem::path> bundle_paths;
        std::set<std::filesystem::path> crosswalk_paths;
        std::ostringstream source_set;
        source_set.imbue(std::locale::classic());
        source_set << kCohortProfileId << "\t1.0.0\n";
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            RegionalCohortSample sample = load_sample(inputs[index], index);
            if (!bundle_paths.insert(sample.frozen_bundle).second ||
                !crosswalk_paths.insert(sample.crosswalk_receipt).second) {
                reject(ParseReason::kUnsupportedValue, "duplicate cohort bundle or crosswalk path");
            }
            if (index == 0U) {
                output.source_authority_profile = sample.source_authority_profile;
                output.source_authority_sha256 = sample.source_authority_sha256;
                output.python_authority_sha256 = sample.python_authority_sha256;
            } else {
                const RegionalCohortSample& cohort_source = output.samples.front();
                if (sample.source_authority_profile != output.source_authority_profile ||
                    sample.source_authority_sha256 != output.source_authority_sha256 ||
                    sample.python_authority_sha256 != output.python_authority_sha256 ||
                    sample.source_manifest_path != cohort_source.source_manifest_path ||
                    sample.source_manifest_run_id != cohort_source.source_manifest_run_id ||
                    sample.source_manifest_sha256 != cohort_source.source_manifest_sha256 ||
                    sample.validator_executable_sha256 != cohort_source.validator_executable_sha256) {
                    reject(ParseReason::kUnsupportedValue,
                           "seven cohort samples do not share one production, Python corpus, source manifest and "
                           "validator authority");
                }
            }
            checked_add(output.total_row_counts.regions, sample.row_counts.regions, "row_counts.regions");
            checked_add(output.total_row_counts.units, sample.row_counts.units, "row_counts.units");
            checked_add(output.total_row_counts.patterns, sample.row_counts.patterns, "row_counts.patterns");
            add_census(output.total_unit_class_census, sample.unit_class_census, "unit_class_census");
            add_census(output.total_primary_class_census, sample.primary_class_census, "primary_class_census");
            add_census(output.total_region_determinacy_census, sample.region_determinacy_census,
                       "region_determinacy_census");
            add_timing(output.sequential_timing_totals, sample.timing);
            if (sample.peak_rss.available && sample.peak_rss.bytes.has_value() &&
                (!output.maximum_peak_rss.available ||
                 *sample.peak_rss.bytes > output.maximum_peak_rss.bytes.value_or(0U))) {
                output.maximum_peak_rss = sample.peak_rss;
            }
            append_source_set_row(source_set, sample);
            output.samples.push_back(std::move(sample));
        }
        auto source_set_sha256 = sha256_hex(source_set.str());
        if (!source_set_sha256.ok()) {
            return ParseResult<RegionalCohortReceipt>::failure(source_set_sha256.reason, source_set_sha256.detail);
        }
        output.source_set_sha256 = std::move(*source_set_sha256.value);
        output.chart_payload_sha256 = chart_payload_sha256(output.samples);
        output.all_sources_validated_frozen = true;
        output.all_crosswalk_receipts_exact = true;
        return ParseResult<RegionalCohortReceipt>::success(std::move(output));
    } catch (const CohortFailure& failure) {
        return ParseResult<RegionalCohortReceipt>::failure(failure.reason(), failure.what());
    } catch (const std::exception& error) {
        return ParseResult<RegionalCohortReceipt>::failure(
            ParseReason::kIoError, "regional cohort aggregation failed: " + std::string(error.what()));
    }
}

std::string render_regional_cohort_receipt_json(const RegionalCohortReceipt& receipt) {
    try {
        validate_receipt_for_render(receipt);
        JsonPointer root(json_object());
        set_text(root.get(), "schema_name", "longlineage.regional_compat_cohort_receipt");
        set_text(root.get(), "schema_version", "1.0.0");
        set_text(root.get(), "profile_id", std::string(kCohortProfileId));
        set_text(root.get(), "state", "VALIDATED_FROZEN_BUNDLES_WITH_EXACT_CROSSWALK_RECEIPTS_AGGREGATED");
        set_uint(root.get(), "dataset_count", receipt.samples.size());
        set_text(root.get(), "source_authority_profile", receipt.source_authority_profile);
        set_text(root.get(), "source_authority_sha256", receipt.source_authority_sha256);
        set_text(root.get(), "python_authority_sha256", receipt.python_authority_sha256);
        set_text(root.get(), "source_set_sha256", receipt.source_set_sha256);
        set_text(root.get(), "chart_payload_sha256", receipt.chart_payload_sha256);
        set_json(root.get(), "all_sources_validated_frozen", json_boolean(receipt.all_sources_validated_frozen));
        set_json(root.get(), "all_crosswalk_receipts_exact", json_boolean(receipt.all_crosswalk_receipts_exact));
        set_json(root.get(), "crosswalk_receipts_independently_frozen", json_false());
        set_text(root.get(), "crosswalk_evidence_scope",
                 "CPLUSPLUS_RECEIPT_BOUND_TO_FROZEN_BUNDLE_AND_FROZEN_PYTHON_CORPUS_AUTHORITY");

        JsonPointer claim(json_object());
        set_json(claim.get(), "descriptive_python_compatibility_only", json_true());
        set_json(claim.get(), "formal_m2_topology", json_false());
        set_json(claim.get(), "production_release", json_false());
        set_json(claim.get(), "clone_ancestor_time_order", json_false());
        set_json(claim.get(), "crosswalk_mismatch_tolerance", json_false());
        set_json(root.get(), "claim_ceiling", claim.release());

        JsonPointer samples(json_array());
        for (const RegionalCohortSample& sample : receipt.samples) {
            JsonPointer row(json_object());
            set_text(row.get(), "dataset_id", sample.dataset_id);
            set_uint(row.get(), "dataset_order", sample.dataset_order);
            set_text(row.get(), "run_id", sample.run_id);
            set_uint(row.get(), "workers", sample.workers);
            set_json(row.get(), "row_counts", row_counts_json(sample.row_counts).release());
            set_json(row.get(), "class_census",
                     class_census_json(sample.unit_class_census, sample.primary_class_census,
                                       sample.region_determinacy_census)
                         .release());
            set_json(row.get(), "timing", timing_json(sample.timing).release());
            set_json(row.get(), "peak_rss", peak_rss_json(sample.peak_rss).release());

            JsonPointer source(json_object());
            set_text(source.get(), "frozen_bundle", sample.frozen_bundle.string());
            set_text(source.get(), "crosswalk_receipt", sample.crosswalk_receipt.string());
            set_text(source.get(), "source_authority_profile", sample.source_authority_profile);
            set_text(source.get(), "source_authority_sha256", sample.source_authority_sha256);
            set_text(source.get(), "source_manifest_path", sample.source_manifest_path.string());
            set_text(source.get(), "source_manifest_run_id", sample.source_manifest_run_id);
            set_text(source.get(), "source_manifest_sha256", sample.source_manifest_sha256);
            set_text(source.get(), "validator_executable_sha256", sample.validator_executable_sha256);
            set_text(source.get(), "summary_sha256", sample.summary_sha256);
            set_text(source.get(), "producer_receipt_sha256", sample.producer_receipt_sha256);
            set_text(source.get(), "validation_receipt_sha256", sample.validation_receipt_sha256);
            set_text(source.get(), "frozen_marker_sha256", sample.frozen_marker_sha256);
            set_text(source.get(), "crosswalk_receipt_sha256", sample.crosswalk_receipt_sha256);
            set_text(source.get(), "checksum_manifest_sha256", sample.checksum_manifest_sha256);
            set_text(source.get(), "regions_tsv_sha256", sample.regions_tsv_sha256);
            set_text(source.get(), "units_tsv_sha256", sample.units_tsv_sha256);
            set_text(source.get(), "patterns_tsv_sha256", sample.patterns_tsv_sha256);
            set_text(source.get(), "python_output_manifest_sha256", sample.python_output_manifest_sha256);
            set_text(source.get(), "python_region_view_sha256", sample.python_region_view_sha256);
            set_text(source.get(), "python_authority_sha256", sample.python_authority_sha256);
            set_json(row.get(), "source_bindings", source.release());

            JsonPointer crosswalk(json_object());
            set_json(crosswalk.get(), "receipt_reports_all_exact", json_true());
            set_json(crosswalk.get(), "receipt_independently_frozen", json_false());
            set_uint(crosswalk.get(), "regions_mismatches", 0U);
            set_uint(crosswalk.get(), "units_mismatches", 0U);
            set_uint(crosswalk.get(), "patterns_mismatches", 0U);
            set_json(row.get(), "crosswalk", crosswalk.release());
            if (json_array_append_new(samples.get(), row.release()) != 0) {
                throw std::runtime_error("cannot append cohort sample JSON");
            }
        }
        set_json(root.get(), "samples", samples.release());

        JsonPointer totals(json_object());
        set_json(totals.get(), "row_counts", row_counts_json(receipt.total_row_counts).release());
        set_json(totals.get(), "class_census",
                 class_census_json(receipt.total_unit_class_census, receipt.total_primary_class_census,
                                   receipt.total_region_determinacy_census)
                     .release());
        set_json(totals.get(), "sequential_timing", timing_json(receipt.sequential_timing_totals).release());
        set_json(totals.get(), "maximum_peak_rss", peak_rss_json(receipt.maximum_peak_rss).release());
        set_json(root.get(), "cohort_totals", totals.release());

        char* encoded = json_dumps(root.get(), JSON_INDENT(2) | JSON_SORT_KEYS | JSON_REAL_PRECISION(17));
        if (encoded == nullptr) {
            return {};
        }
        std::string output(encoded);
        std::free(encoded);
        output.push_back('\n');
        return output;
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace longlineage::compat
