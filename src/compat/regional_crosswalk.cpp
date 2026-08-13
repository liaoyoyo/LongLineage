// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/compat/regional_crosswalk.hpp"

#include <jansson.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"

namespace longlineage::compat {
namespace {

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};
using JsonPointer = std::unique_ptr<json_t, JsonDeleter>;
using SemanticMap = std::map<std::string, std::string>;

struct PythonMaps {
    std::string dataset_id;
    std::filesystem::path region_view;
    std::string manifest_sha256;
    std::string region_view_sha256;
    std::string authority_sha256;
    SemanticMap regions;
    SemanticMap units;
    SemanticMap patterns;
};

struct CppMaps {
    std::string dataset_id;
    std::string summary_sha256;
    std::string validation_receipt_sha256;
    SemanticMap regions;
    SemanticMap units;
    SemanticMap patterns;
};

struct LockedJson {
    JsonPointer value;
    std::string sha256;
};

[[nodiscard]] ParseResult<LockedJson> load_locked_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return ParseResult<LockedJson>::failure(ParseReason::kIoError,
                                                "cannot open frozen JSON object " + path.string());
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (input.bad()) {
        return ParseResult<LockedJson>::failure(ParseReason::kIoError,
                                                "cannot read frozen JSON object " + path.string());
    }
    const std::string content = bytes.str();
    auto digest = sha256_hex(content);
    if (!digest.ok()) {
        return ParseResult<LockedJson>::failure(digest.reason, digest.detail);
    }
    json_error_t error{};
    JsonPointer value(json_loadb(content.data(), content.size(), JSON_REJECT_DUPLICATES, &error));
    if (!value || !json_is_object(value.get())) {
        std::ostringstream detail;
        detail << "cannot parse frozen JSON object " << path.string() << ':' << error.line << ':' << error.column
               << ": " << error.text;
        return ParseResult<LockedJson>::failure(ParseReason::kMalformedValue, detail.str());
    }
    return ParseResult<LockedJson>::success(LockedJson{std::move(value), std::move(*digest.value)});
}

[[nodiscard]] std::string text_field(const json_t* object, const char* key) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_string(value)) {
        return {};
    }
    const std::string result(json_string_value(value), json_string_length(value));
    return result.find('\0') == std::string::npos ? result : std::string{};
}

[[nodiscard]] bool uint_field(const json_t* object, const char* key, std::uint64_t& output) noexcept {
    const json_t* value = json_object_get(object, key);
    if (!json_is_integer(value) || json_integer_value(value) < 0) {
        return false;
    }
    output = static_cast<std::uint64_t>(json_integer_value(value));
    return true;
}

[[nodiscard]] std::string canonical_json_integer(const json_t* object, const char* key, bool& ok) {
    std::uint64_t value = 0;
    if (!uint_field(object, key, value)) {
        ok = false;
        return {};
    }
    return std::to_string(value);
}

[[nodiscard]] std::string canonical_positions(const json_t* value, bool& ok) {
    if (!json_is_array(value) || json_array_size(value) < 2U || json_array_size(value) > 8U) {
        ok = false;
        return {};
    }
    std::ostringstream output;
    std::uint64_t previous = 0;
    for (std::size_t index = 0; index < json_array_size(value); ++index) {
        const json_t* position = json_array_get(value, index);
        if (!json_is_integer(position) || json_integer_value(position) <= 0) {
            ok = false;
            return {};
        }
        const std::uint64_t parsed = static_cast<std::uint64_t>(json_integer_value(position));
        if (parsed <= previous) {
            ok = false;
            return {};
        }
        if (index != 0U) {
            output << ',';
        }
        output << parsed;
        previous = parsed;
    }
    return output.str();
}

[[nodiscard]] std::string normalized_class(std::string_view value) {
    constexpr std::array<std::string_view, 4> kClasses = {
        "ambiguous_structure",
        "capped",
        "determined",
        "recurrence_required",
    };
    for (const std::string_view candidate : kClasses) {
        if (value == candidate || (value.size() > candidate.size() && value.substr(0, candidate.size()) == candidate &&
                                   value[candidate.size()] == '(')) {
            return std::string(candidate);
        }
    }
    return {};
}

[[nodiscard]] bool is_lower_sha256(std::string_view value) noexcept {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
           });
}

struct PythonAuthorityBinding {
    std::string authority_sha256;
    std::string region_view_basename;
    std::string region_view_sha256;
};

[[nodiscard]] ParseResult<PythonAuthorityBinding> verify_python_authority(const std::filesystem::path& repository_root,
                                                                          const std::string& dataset_id,
                                                                          const std::filesystem::path& manifest_path,
                                                                          const std::string& manifest_sha256) {
    constexpr std::array<std::string_view, 7> kDatasetOrder = {
        "HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954",
    };
    const std::filesystem::path authority_path =
        repository_root / "oracle" / "regional_compat_python_v2_output_authority.json";
    auto authority = load_locked_json(authority_path);
    if (!authority.ok()) {
        return ParseResult<PythonAuthorityBinding>::failure(authority.reason, authority.detail);
    }
    const json_t* root = authority.value->value.get();
    const json_t* constraints = json_object_get(root, "constraints");
    const json_t* datasets = json_object_get(root, "datasets");
    std::uint64_t dataset_count = 0;
    if (json_object_size(root) != 6U ||
        text_field(root, "schema_name") != "longlineage.regional_compat_python_v2_output_authority" ||
        text_field(root, "schema_version") != "1.0.0" ||
        text_field(root, "profile_id") != "PYTHON_V2_DESCRIPTIVE_REGIONAL_7_DATASET" ||
        text_field(root, "source_run_id") != "20260713_layered_reconstruction_v3_raw_all_lps_pass_v5" ||
        !json_is_object(constraints) || json_object_size(constraints) != 5U ||
        !uint_field(constraints, "dataset_count", dataset_count) || dataset_count != kDatasetOrder.size() ||
        !json_is_true(json_object_get(constraints, "dataset_order_exact")) ||
        !json_is_true(json_object_get(constraints, "region_unit_pattern_fields_only")) ||
        !json_is_false(json_object_get(constraints, "copy_number_loh_fields_compared")) ||
        !json_is_false(json_object_get(constraints, "private_source_paths_stored")) || !json_is_array(datasets) ||
        json_array_size(datasets) != kDatasetOrder.size()) {
        return ParseResult<PythonAuthorityBinding>::failure(
            ParseReason::kUnsupportedValue, "frozen Python corpus authority identity/constraints are invalid");
    }
    PythonAuthorityBinding binding;
    binding.authority_sha256 = authority.value->sha256;
    bool selected = false;
    for (std::size_t index = 0; index < kDatasetOrder.size(); ++index) {
        const json_t* row = json_array_get(datasets, index);
        std::uint64_t dataset_order = 0;
        const std::string row_manifest_sha = text_field(row, "output_manifest_sha256");
        const std::string row_region_sha = text_field(row, "region_view_sha256");
        if (!json_is_object(row) || json_object_size(row) != 6U ||
            text_field(row, "dataset_id") != kDatasetOrder[index] || !uint_field(row, "dataset_order", dataset_order) ||
            dataset_order != index || text_field(row, "output_manifest_basename") != "output_manifest.json" ||
            !is_lower_sha256(row_manifest_sha) || !is_lower_sha256(row_region_sha)) {
            return ParseResult<PythonAuthorityBinding>::failure(
                ParseReason::kMalformedValue,
                "frozen Python corpus authority dataset row is invalid at index " + std::to_string(index));
        }
        if (dataset_id == kDatasetOrder[index]) {
            if (selected || manifest_path.filename() != text_field(row, "output_manifest_basename") ||
                manifest_sha256 != row_manifest_sha) {
                return ParseResult<PythonAuthorityBinding>::failure(
                    ParseReason::kUnsupportedValue, "Python output manifest differs from frozen corpus authority");
            }
            selected = true;
            binding.region_view_basename = text_field(row, "region_view_basename");
            binding.region_view_sha256 = row_region_sha;
            if (binding.region_view_basename.empty() ||
                std::filesystem::path(binding.region_view_basename).filename() != binding.region_view_basename) {
                return ParseResult<PythonAuthorityBinding>::failure(ParseReason::kMalformedValue,
                                                                    "Python authority region-view basename is unsafe");
            }
        }
    }
    if (!selected) {
        return ParseResult<PythonAuthorityBinding>::failure(ParseReason::kUnsupportedValue,
                                                            "dataset is absent from frozen Python corpus authority");
    }
    return ParseResult<PythonAuthorityBinding>::success(std::move(binding));
}

[[nodiscard]] ParseResult<PythonMaps> load_python_maps(const std::filesystem::path& repository_root,
                                                       const std::filesystem::path& manifest_path) {
    auto manifest = load_locked_json(manifest_path);
    if (!manifest.ok()) {
        return ParseResult<PythonMaps>::failure(manifest.reason, manifest.detail);
    }
    PythonMaps output;
    output.dataset_id = text_field(manifest.value->value.get(), "sample");
    if (text_field(manifest.value->value.get(), "schema_name") != "intersubmod.layered_sample_output_manifest" ||
        text_field(manifest.value->value.get(), "schema_version") != "1.0.0" || output.dataset_id.empty()) {
        return ParseResult<PythonMaps>::failure(ParseReason::kUnsupportedValue,
                                                "frozen Python output manifest identity is invalid");
    }
    auto authority = verify_python_authority(repository_root, output.dataset_id, manifest_path, manifest.value->sha256);
    if (!authority.ok()) {
        return ParseResult<PythonMaps>::failure(authority.reason, authority.detail);
    }
    output.authority_sha256 = authority.value->authority_sha256;
    const json_t* outputs = json_object_get(manifest.value->value.get(), "outputs");
    if (!json_is_array(outputs)) {
        return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                "frozen Python output manifest lacks outputs");
    }
    std::string relative_region_view;
    std::string expected_region_sha;
    for (std::size_t index = 0; index < json_array_size(outputs); ++index) {
        const json_t* row = json_array_get(outputs, index);
        if (json_is_object(row) && text_field(row, "role") == "layered_region_view") {
            if (!relative_region_view.empty()) {
                return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                        "duplicate layered_region_view authority row");
            }
            relative_region_view = text_field(row, "path");
            expected_region_sha = text_field(row, "sha256");
        }
    }
    if (relative_region_view.empty() || relative_region_view != authority.value->region_view_basename ||
        std::filesystem::path(relative_region_view).is_absolute() ||
        std::filesystem::path(relative_region_view).filename() != relative_region_view) {
        return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                "layered_region_view path differs from frozen corpus authority");
    }
    output.region_view = manifest_path.parent_path() / relative_region_view;
    auto region_view = load_locked_json(output.region_view);
    if (!region_view.ok() || region_view.value->sha256 != expected_region_sha ||
        region_view.value->sha256 != authority.value->region_view_sha256) {
        return ParseResult<PythonMaps>::failure(ParseReason::kIoError,
                                                "frozen Python manifest or region-view SHA verification failed");
    }
    output.manifest_sha256 = manifest.value->sha256;
    output.region_view_sha256 = region_view.value->sha256;

    if (text_field(region_view.value->value.get(), "sample") != output.dataset_id) {
        return ParseResult<PythonMaps>::failure(ParseReason::kUnsupportedValue,
                                                "Python output manifest/region-view sample mismatch");
    }
    const json_t* regions = json_object_get(region_view.value->value.get(), "regions");
    if (!json_is_array(regions)) {
        return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                "frozen Python region view lacks regions");
    }
    for (std::size_t region_index = 0; region_index < json_array_size(regions); ++region_index) {
        const json_t* region = json_array_get(regions, region_index);
        const json_t* lineages = json_object_get(region, "lineages");
        const std::string region_id = text_field(region, "region");
        const std::string chrom = text_field(region, "chrom");
        const std::string determinacy = text_field(region, "region_determinacy");
        bool ok = json_is_object(region) && json_is_array(lineages) && !region_id.empty() && !chrom.empty() &&
                  !determinacy.empty();
        const std::string start = canonical_json_integer(region, "start", ok);
        const std::string end = canonical_json_integer(region, "end", ok);
        const std::string selected_sites = canonical_json_integer(region, "n_sSNV", ok);
        const std::string positions = canonical_positions(json_object_get(region, "positions"), ok);
        const std::string full_reads = canonical_json_integer(region, "n_full_cov_reads", ok);
        const std::string primary = canonical_json_integer(region, "n_primary_lineages", ok);
        const std::string controls = canonical_json_integer(region, "n_reference_only_controls", ok);
        const std::string h3 = canonical_json_integer(region, "n_H3_auxiliary", ok);
        const std::string h4 = canonical_json_integer(region, "n_H4_auxiliary", ok);
        std::uint64_t none_count = 0;
        if (!ok) {
            return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue, "malformed frozen Python region row");
        }
        for (std::size_t lineage_index = 0; lineage_index < json_array_size(lineages); ++lineage_index) {
            const json_t* lineage = json_array_get(lineages, lineage_index);
            const std::string family = text_field(lineage, "family");
            const std::string role = text_field(lineage, "unit_role");
            const std::string classification = normalized_class(text_field(lineage, "L1_class"));
            bool lineage_ok = json_is_object(lineage) && !family.empty() && !role.empty() && !classification.empty();
            const std::string reads = canonical_json_integer(lineage, "n_reads", lineage_ok);
            const std::string full = canonical_json_integer(lineage, "n_full_pops", lineage_ok);
            const std::string partial = canonical_json_integer(lineage, "n_partial", lineage_ok);
            const std::string hidden = canonical_json_integer(lineage, "n_hidden", lineage_ok);
            const std::string trees = canonical_json_integer(lineage, "n_trees", lineage_ok);
            const json_t* mutation = json_object_get(lineage, "mutation_bearing");
            const json_t* capped = json_object_get(lineage, "capped");
            if (!lineage_ok || !json_is_boolean(mutation) || !json_is_boolean(capped)) {
                return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                        "malformed frozen Python lineage row");
            }
            none_count += family == "none" ? 1U : 0U;
            const std::string unit_key = region_id + '\t' + family;
            const std::string unit_value = role + '\t' + reads + '\t' + full + '\t' + partial + '\t' +
                                           (json_is_true(mutation) ? "1" : "0") + '\t' + hidden + '\t' + trees + '\t' +
                                           (json_is_true(capped) ? "1" : "0") + '\t' + classification;
            if (!output.units.emplace(unit_key, unit_value).second) {
                return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                        "duplicate frozen Python region/family key");
            }
            const std::array<std::pair<const char*, std::string_view>, 2> pattern_objects = {{
                {"obs_populations", "FULL"},
                {"obs_subreads", "SUBREAD"},
            }};
            for (const auto& pattern_object : pattern_objects) {
                const json_t* patterns = json_object_get(lineage, pattern_object.first);
                if (json_is_null(patterns)) {
                    continue;
                }
                if (!json_is_object(patterns)) {
                    return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                            "Python lineage pattern map is malformed");
                }
                const char* pattern = nullptr;
                json_t* count = nullptr;
                json_object_foreach(const_cast<json_t*>(patterns), pattern, count) {
                    if (!json_is_integer(count) || json_integer_value(count) < 3) {
                        return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                                "Python supported pattern count is invalid");
                    }
                    const std::string pattern_key =
                        unit_key + '\t' + pattern + '\t' + std::string(pattern_object.second);
                    const std::string pattern_value = std::to_string(json_integer_value(count));
                    if (!output.patterns.emplace(pattern_key, pattern_value).second) {
                        return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue,
                                                                "duplicate frozen Python pattern key");
                    }
                }
            }
        }
        const std::string region_value = chrom + '\t' + start + '\t' + end + '\t' + selected_sites + '\t' + positions +
                                         '\t' + full_reads + '\t' + std::to_string(json_array_size(lineages)) + '\t' +
                                         primary + '\t' + controls + '\t' + h3 + '\t' + h4 + '\t' +
                                         std::to_string(none_count) + '\t' + determinacy;
        if (!output.regions.emplace(region_id, region_value).second) {
            return ParseResult<PythonMaps>::failure(ParseReason::kMalformedValue, "duplicate frozen Python region key");
        }
    }
    return ParseResult<PythonMaps>::success(std::move(output));
}

[[nodiscard]] std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return fields;
}

[[nodiscard]] ParseResult<SemanticMap> load_tsv_map(
    const std::filesystem::path& path, std::size_t columns,
    const std::function<ParseResult<std::pair<std::string, std::string>>(const std::vector<std::string>&)>& projector) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return ParseResult<SemanticMap>::failure(ParseReason::kIoError, "cannot open C++ TSV: " + path.string());
    }
    std::string line;
    if (!std::getline(input, line) || line.empty()) {
        return ParseResult<SemanticMap>::failure(ParseReason::kMalformedValue,
                                                 "C++ TSV lacks a nonempty header: " + path.string());
    }
    SemanticMap output;
    while (std::getline(input, line)) {
        if (line.empty() || line.back() == '\r') {
            return ParseResult<SemanticMap>::failure(ParseReason::kMalformedValue,
                                                     "C++ TSV contains an empty/CR line: " + path.string());
        }
        const std::vector<std::string> fields = split_tabs(line);
        if (fields.size() != columns) {
            return ParseResult<SemanticMap>::failure(ParseReason::kMalformedValue,
                                                     "C++ TSV column count mismatch: " + path.string());
        }
        auto projected = projector(fields);
        if (!projected.ok() || !output.emplace(projected.value->first, projected.value->second).second) {
            return ParseResult<SemanticMap>::failure(
                projected.ok() ? ParseReason::kMalformedValue : projected.reason,
                projected.ok() ? "duplicate C++ TSV semantic key" : projected.detail);
        }
    }
    if (!input.eof()) {
        return ParseResult<SemanticMap>::failure(ParseReason::kIoError, "C++ TSV read failed: " + path.string());
    }
    return ParseResult<SemanticMap>::success(std::move(output));
}

[[nodiscard]] ParseResult<std::map<std::string, std::string>> load_checksum_manifest(
    const std::filesystem::path& bundle) {
    constexpr std::array<std::string_view, 5> kExpected = {
        "patterns.tsv", "producer_receipt.json", "regions.tsv", "summary.json", "units.tsv",
    };
    std::ifstream input(bundle / "checksums.sha256", std::ios::binary);
    if (!input) {
        return ParseResult<std::map<std::string, std::string>>::failure(ParseReason::kIoError,
                                                                        "cannot open C++ checksum manifest");
    }
    std::map<std::string, std::string> output;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 67U || line[64] != ' ' || line[65] != ' ' || line.back() == '\r') {
            return ParseResult<std::map<std::string, std::string>>::failure(ParseReason::kMalformedValue,
                                                                            "C++ checksum manifest row is malformed");
        }
        const std::string sha = line.substr(0, 64U);
        const std::string path = line.substr(66U);
        if (!is_lower_sha256(sha) || path.empty() || !output.emplace(path, sha).second) {
            return ParseResult<std::map<std::string, std::string>>::failure(
                ParseReason::kMalformedValue, "C++ checksum manifest row is invalid or duplicate");
        }
    }
    if (!input.eof() || output.size() != kExpected.size()) {
        return ParseResult<std::map<std::string, std::string>>::failure(ParseReason::kMalformedValue,
                                                                        "C++ checksum manifest is incomplete");
    }
    for (const std::string_view path : kExpected) {
        const auto found = output.find(std::string(path));
        auto observed = sha256_file(bundle / path);
        if (found == output.end() || !observed.ok() || *observed.value != found->second) {
            return ParseResult<std::map<std::string, std::string>>::failure(
                ParseReason::kIoError, "C++ frozen artifact SHA differs from checksum manifest: " + std::string(path));
        }
    }
    return ParseResult<std::map<std::string, std::string>>::success(std::move(output));
}

[[nodiscard]] ParseResult<CppMaps> load_cpp_maps(const std::filesystem::path& bundle) {
    if (!std::filesystem::is_regular_file(bundle / "FROZEN")) {
        return ParseResult<CppMaps>::failure(ParseReason::kMissingRequiredField,
                                             "C++ crosswalk requires an independently frozen bundle");
    }
    auto checksums = load_checksum_manifest(bundle);
    auto summary = load_locked_json(bundle / "summary.json");
    auto producer = load_locked_json(bundle / "producer_receipt.json");
    auto validation = load_locked_json(bundle / "validation_receipt.json");
    if (!checksums.ok() || !summary.ok() || !producer.ok() || !validation.ok()) {
        return ParseResult<CppMaps>::failure(ParseReason::kMalformedValue,
                                             "cannot verify C++ checksum/summary/producer/validation receipt");
    }
    CppMaps output;
    const std::string schema_version = text_field(summary.value->value.get(), "schema_version");
    output.dataset_id = schema_version == "1.0.0" ? "HCC1395" : text_field(summary.value->value.get(), "dataset_id");
    if ((schema_version != "1.0.0" && schema_version != "2.0.0") || output.dataset_id.empty() ||
        text_field(summary.value->value.get(), "profile_id") != "PYTHON_V2_DESCRIPTIVE_REGIONAL" ||
        text_field(validation.value->value.get(), "state") != "VALIDATED_FROZEN" ||
        (schema_version == "2.0.0" && (text_field(producer.value->value.get(), "dataset_id") != output.dataset_id ||
                                       text_field(validation.value->value.get(), "dataset_id") != output.dataset_id))) {
        return ParseResult<CppMaps>::failure(ParseReason::kUnsupportedValue,
                                             "C++ bundle summary/validation identity is invalid");
    }
    const std::string& summary_sha = summary.value->sha256;
    const std::string& producer_sha = producer.value->sha256;
    const std::string& validation_sha = validation.value->sha256;
    if (checksums.value->at("summary.json") != summary_sha ||
        checksums.value->at("producer_receipt.json") != producer_sha ||
        text_field(producer.value->value.get(), "summary_sha256") != summary_sha ||
        text_field(validation.value->value.get(), "producer_receipt_sha256") != producer_sha) {
        return ParseResult<CppMaps>::failure(ParseReason::kIoError, "cannot hash C++ frozen receipts");
    }
    std::ifstream frozen(bundle / "FROZEN", std::ios::binary);
    std::set<std::string> frozen_lines;
    std::string frozen_line;
    while (std::getline(frozen, frozen_line)) {
        if (!frozen_line.empty() && frozen_line.back() != '\r') {
            frozen_lines.insert(frozen_line);
        }
    }
    if (!frozen.eof() || frozen_lines.count("VALIDATED_FROZEN") != 1U ||
        frozen_lines.count("validation_receipt_sha256=" + validation_sha) != 1U ||
        frozen_lines.count("producer_receipt_sha256=" + producer_sha) != 1U) {
        return ParseResult<CppMaps>::failure(ParseReason::kUnsupportedValue,
                                             "C++ FROZEN marker does not bind the current receipts");
    }
    output.summary_sha256 = summary_sha;
    output.validation_receipt_sha256 = validation_sha;

    const json_t* row_counts = json_object_get(summary.value->value.get(), "row_counts");
    std::uint64_t expected_regions = 0;
    std::uint64_t expected_units = 0;
    std::uint64_t expected_patterns = 0;
    const json_t* artifacts = json_object_get(producer.value->value.get(), "artifacts");
    if (!json_is_object(row_counts) || !uint_field(row_counts, "regions", expected_regions) ||
        !uint_field(row_counts, "units", expected_units) || !uint_field(row_counts, "patterns", expected_patterns) ||
        !json_is_array(artifacts) || json_array_size(artifacts) != 4U) {
        return ParseResult<CppMaps>::failure(ParseReason::kMalformedValue,
                                             "C++ summary row counts or producer artifacts are malformed");
    }
    const std::array<std::pair<std::string_view, std::uint64_t>, 4> expected_artifacts = {{
        {"summary.json", 1U},
        {"regions.tsv", expected_regions},
        {"units.tsv", expected_units},
        {"patterns.tsv", expected_patterns},
    }};
    for (std::size_t index = 0; index < expected_artifacts.size(); ++index) {
        const json_t* row = json_array_get(artifacts, index);
        std::uint64_t rows = 0;
        const std::string path = text_field(row, "path");
        if (!json_is_object(row) || json_object_size(row) != 3U || path != expected_artifacts[index].first ||
            !uint_field(row, "rows", rows) || rows != expected_artifacts[index].second ||
            text_field(row, "sha256") != checksums.value->at(path)) {
            return ParseResult<CppMaps>::failure(ParseReason::kUnsupportedValue,
                                                 "C++ producer artifact binding is invalid");
        }
    }

    auto regions = load_tsv_map(bundle / "regions.tsv", 18U, [](const std::vector<std::string>& fields) {
        const std::string value = fields[2] + '\t' + fields[3] + '\t' + fields[4] + '\t' + fields[7] + '\t' +
                                  fields[9] + '\t' + fields[10] + '\t' + fields[11] + '\t' + fields[12] + '\t' +
                                  fields[13] + '\t' + fields[14] + '\t' + fields[15] + '\t' + fields[16] + '\t' +
                                  fields[17];
        return ParseResult<std::pair<std::string, std::string>>::success({fields[1], value});
    });
    auto units = load_tsv_map(bundle / "units.tsv", 13U, [](const std::vector<std::string>& fields) {
        const std::string key = fields[1] + '\t' + fields[2];
        const std::string value = fields[3] + '\t' + fields[4] + '\t' + fields[5] + '\t' + fields[6] + '\t' +
                                  fields[7] + '\t' + fields[8] + '\t' + fields[9] + '\t' + fields[11] + '\t' +
                                  fields[12];
        return ParseResult<std::pair<std::string, std::string>>::success({key, value});
    });
    auto patterns = load_tsv_map(bundle / "patterns.tsv", 6U, [](const std::vector<std::string>& fields) {
        const std::string key = fields[1] + '\t' + fields[2] + '\t' + fields[3] + '\t' + fields[4];
        return ParseResult<std::pair<std::string, std::string>>::success({key, fields[5]});
    });
    if (!regions.ok() || !units.ok() || !patterns.ok()) {
        return ParseResult<CppMaps>::failure(ParseReason::kMalformedValue,
                                             "cannot normalize one or more C++ regional TSV files");
    }
    if (regions.value->size() != expected_regions || units.value->size() != expected_units ||
        patterns.value->size() != expected_patterns) {
        return ParseResult<CppMaps>::failure(ParseReason::kUnsupportedValue,
                                             "C++ TSV maps differ from frozen summary row counts");
    }
    for (const std::string_view path :
         {std::string_view("regions.tsv"), std::string_view("units.tsv"), std::string_view("patterns.tsv")}) {
        auto final_sha = sha256_file(bundle / path);
        if (!final_sha.ok() || *final_sha.value != checksums.value->at(std::string(path))) {
            return ParseResult<CppMaps>::failure(
                ParseReason::kIoError, "C++ TSV changed while crosswalk was reading it: " + std::string(path));
        }
    }
    output.regions = std::move(*regions.value);
    output.units = std::move(*units.value);
    output.patterns = std::move(*patterns.value);
    return ParseResult<CppMaps>::success(std::move(output));
}

[[nodiscard]] std::string map_sha256(const SemanticMap& values) {
    std::ostringstream canonical;
    for (const auto& value : values) {
        canonical << value.first << '\t' << value.second << '\n';
    }
    auto digest = sha256_hex(canonical.str());
    return digest.ok() ? std::move(*digest.value) : std::string{};
}

[[nodiscard]] RegionalCrosswalkLayer compare_maps(const SemanticMap& expected, const SemanticMap& actual) {
    RegionalCrosswalkLayer layer;
    layer.expected_rows = static_cast<std::uint64_t>(expected.size());
    layer.actual_rows = static_cast<std::uint64_t>(actual.size());
    layer.expected_sha256 = map_sha256(expected);
    layer.actual_sha256 = map_sha256(actual);
    auto expected_it = expected.begin();
    auto actual_it = actual.begin();
    while (expected_it != expected.end() || actual_it != actual.end()) {
        if (actual_it == actual.end() || (expected_it != expected.end() && expected_it->first < actual_it->first)) {
            ++layer.mismatches;
            ++expected_it;
        } else if (expected_it == expected.end() || actual_it->first < expected_it->first) {
            ++layer.mismatches;
            ++actual_it;
        } else {
            layer.mismatches += expected_it->second == actual_it->second ? 0U : 1U;
            ++expected_it;
            ++actual_it;
        }
    }
    return layer;
}

void set_text(json_t* object, const char* key, const std::string& value) {
    json_object_set_new(object, key, json_string(value.c_str()));
}

void set_uint(json_t* object, const char* key, std::uint64_t value) {
    json_object_set_new(object, key, json_integer(static_cast<json_int_t>(value)));
}

JsonPointer layer_json(const RegionalCrosswalkLayer& layer) {
    JsonPointer output(json_object());
    set_uint(output.get(), "rows_expected", layer.expected_rows);
    set_uint(output.get(), "rows_actual", layer.actual_rows);
    set_uint(output.get(), "mismatches", layer.mismatches);
    set_text(output.get(), "expected_map_sha256", layer.expected_sha256);
    set_text(output.get(), "actual_map_sha256", layer.actual_sha256);
    return output;
}

}  // namespace

ParseResult<RegionalCrosswalkReceipt> compare_frozen_regional_bundle(
    const std::filesystem::path& repository_root, const std::filesystem::path& cpp_bundle,
    const std::filesystem::path& python_output_manifest) {
    auto cpp = load_cpp_maps(cpp_bundle);
    auto python = load_python_maps(repository_root, python_output_manifest);
    if (!cpp.ok()) {
        return ParseResult<RegionalCrosswalkReceipt>::failure(cpp.reason, cpp.detail);
    }
    if (!python.ok()) {
        return ParseResult<RegionalCrosswalkReceipt>::failure(python.reason, python.detail);
    }
    if (cpp.value->dataset_id != python.value->dataset_id) {
        return ParseResult<RegionalCrosswalkReceipt>::failure(ParseReason::kUnsupportedValue,
                                                              "C++/Python crosswalk dataset mismatch");
    }
    RegionalCrosswalkReceipt output;
    output.dataset_id = cpp.value->dataset_id;
    output.cpp_bundle = cpp_bundle;
    output.python_output_manifest = python_output_manifest;
    output.python_region_view = python.value->region_view;
    output.cpp_summary_sha256 = cpp.value->summary_sha256;
    output.cpp_validation_receipt_sha256 = cpp.value->validation_receipt_sha256;
    output.python_output_manifest_sha256 = python.value->manifest_sha256;
    output.python_region_view_sha256 = python.value->region_view_sha256;
    output.python_authority_sha256 = python.value->authority_sha256;
    output.regions = compare_maps(python.value->regions, cpp.value->regions);
    output.units = compare_maps(python.value->units, cpp.value->units);
    output.patterns = compare_maps(python.value->patterns, cpp.value->patterns);
    output.all_exact = output.regions.mismatches == 0U && output.units.mismatches == 0U &&
                       output.patterns.mismatches == 0U && output.regions.expected_rows == output.regions.actual_rows &&
                       output.units.expected_rows == output.units.actual_rows &&
                       output.patterns.expected_rows == output.patterns.actual_rows;
    return ParseResult<RegionalCrosswalkReceipt>::success(std::move(output));
}

std::string render_regional_crosswalk_receipt_json(const RegionalCrosswalkReceipt& receipt) {
    JsonPointer root(json_object());
    set_text(root.get(), "schema_name", "longlineage.regional_compat_crosswalk_receipt");
    set_text(root.get(), "schema_version", "2.0.0");
    set_text(root.get(), "profile_id", "PYTHON_V2_DESCRIPTIVE_REGIONAL");
    set_text(root.get(), "dataset_id", receipt.dataset_id);
    json_object_set_new(root.get(), "all_exact", json_boolean(receipt.all_exact));
    JsonPointer claim(json_object());
    json_object_set_new(claim.get(), "descriptive_python_compatibility_only", json_true());
    json_object_set_new(claim.get(), "formal_topology_claim_allowed", json_false());
    json_object_set_new(claim.get(), "production_release_claim_allowed", json_false());
    json_object_set_new(root.get(), "claim_ceiling", claim.release());
    JsonPointer cpp(json_object());
    set_text(cpp.get(), "bundle", receipt.cpp_bundle.string());
    set_text(cpp.get(), "summary_sha256", receipt.cpp_summary_sha256);
    set_text(cpp.get(), "validation_receipt_sha256", receipt.cpp_validation_receipt_sha256);
    json_object_set_new(root.get(), "cpp_frozen_bundle", cpp.release());
    JsonPointer python(json_object());
    set_text(python.get(), "output_manifest", receipt.python_output_manifest.string());
    set_text(python.get(), "output_manifest_sha256", receipt.python_output_manifest_sha256);
    set_text(python.get(), "region_view", receipt.python_region_view.string());
    set_text(python.get(), "region_view_sha256", receipt.python_region_view_sha256);
    set_text(python.get(), "authority_sha256", receipt.python_authority_sha256);
    json_object_set_new(root.get(), "python_frozen_authority", python.release());
    JsonPointer inventory(json_object());
    set_text(inventory.get(), "comparison_scope", "shared_descriptive_fields_only");
    JsonPointer included(json_array());
    for (const std::string_view field : {
             std::string_view("region_membership_and_selected_positions"),
             std::string_view("region_read_family_and_determinacy_census"),
             std::string_view("unit_family_role_read_pattern_hidden_tree_cap_class"),
             std::string_view("supported_full_and_subread_pattern_counts"),
         }) {
        json_array_append_new(included.get(), json_stringn(field.data(), field.size()));
    }
    json_object_set_new(inventory.get(), "included", included.release());
    JsonPointer excluded(json_array());
    for (const std::string_view field : {
             std::string_view("cpp_region_order_and_pre_cap_diagnostics"),
             std::string_view("cpp_feasible_node_set_count"),
             std::string_view("python_post_tree_copy_number_and_loh"),
         }) {
        json_array_append_new(excluded.get(), json_stringn(field.data(), field.size()));
    }
    json_object_set_new(inventory.get(), "excluded_nonshared_fields", excluded.release());
    json_object_set_new(root.get(), "field_inventory", inventory.release());
    JsonPointer layers(json_object());
    json_object_set_new(layers.get(), "regions", layer_json(receipt.regions).release());
    json_object_set_new(layers.get(), "units", layer_json(receipt.units).release());
    json_object_set_new(layers.get(), "patterns", layer_json(receipt.patterns).release());
    json_object_set_new(root.get(), "crosswalks", layers.release());
    char* encoded = json_dumps(root.get(), JSON_INDENT(2) | JSON_SORT_KEYS);
    if (encoded == nullptr) {
        return {};
    }
    std::string output(encoded);
    std::free(encoded);
    output.push_back('\n');
    return output;
}

}  // namespace longlineage::compat
