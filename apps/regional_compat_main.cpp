// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cli_support.hpp"
#include "longlineage/compat/regional_cohort.hpp"
#include "longlineage/compat/regional_crosswalk.hpp"
#include "longlineage/compat/regional_io.hpp"

namespace {

using longlineage::cli::ExitCode;

void usage() {
    std::cout << "Usage:\n"
              << "  longlineage-regional-compat run --repo-root DIR --manifest FILE --dataset-id ID --run-id ID "
                 "--output-dir DIR --workers N\n"
              << "  longlineage-regional-compat probe --repo-root DIR --manifest FILE --dataset-id ID --run-id ID "
                 "--output-dir DIR --workers N --first-region N --region-count N\n"
              << "  longlineage-regional-compat replay --dataset-id ID --mlhp FILE "
                 "[--mlhp FILE ...] --output FILE\n"
              << "  longlineage-regional-compat replay-hcc1395 --mlhp FILE "
                 "[--mlhp FILE ...] --output FILE\n"
              << "  longlineage-regional-compat crosswalk --repo-root DIR --bundle DIR "
                 "--python-manifest FILE --output FILE\n"
              << "  longlineage-regional-compat cohort --bundle-root DIR --crosswalk-dir DIR --output FILE\n\n"
              << "Profile: PYTHON_V2_DESCRIPTIVE_REGIONAL (evaluation/descriptive; "
                 "never formal M2 topology).\n";
}

bool parse_size(const std::string& text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    std::size_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool safe_dataset_id(const std::string& value) {
    return !value.empty() && value.size() <= 128U && std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
    });
}

int run_mode(int argc, char** argv, bool partial_probe) {
    if ((!partial_probe && argc != 14) || (partial_probe && argc != 18)) {
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    std::map<std::string, std::string> options;
    for (int index = 2; index < argc; index += 2) {
        const std::string key(argv[index]);
        if ((key != "--repo-root" && key != "--manifest" && key != "--dataset-id" && key != "--run-id" &&
             key != "--output-dir" && key != "--workers" && key != "--first-region" && key != "--region-count") ||
            !options.emplace(key, argv[index + 1]).second) {
            std::cerr << "ERROR[CLI]: unknown/duplicate option: " << key << '\n';
            return static_cast<int>(ExitCode::UsageError);
        }
    }
    std::size_t workers = 0;
    std::size_t first_region = 0;
    std::size_t region_count = 0;
    const std::size_t expected_options = partial_probe ? 8U : 6U;
    if (options.size() != expected_options || options.count("--repo-root") != 1U || options.count("--manifest") != 1U ||
        options.count("--dataset-id") != 1U || options.count("--run-id") != 1U || options.count("--output-dir") != 1U ||
        options.count("--workers") != 1U || !parse_size(options["--workers"], workers) ||
        (partial_probe && (!parse_size(options["--first-region"], first_region) ||
                           !parse_size(options["--region-count"], region_count) || region_count == 0))) {
        std::cerr << "ERROR[CLI]: required named options are invalid\n";
        return static_cast<int>(ExitCode::UsageError);
    }
    longlineage::compat::RegionalCompatibilityOptions request;
    request.repository_root = options["--repo-root"];
    request.source_manifest = options["--manifest"];
    request.dataset_id = options["--dataset-id"];
    request.run_id = options["--run-id"];
    request.output_directory = options["--output-dir"];
    request.workers = workers;
    request.first_region = first_region;
    request.region_count = region_count;
    const auto result = longlineage::compat::run_regional_compatibility(request);
    if (!result.ok()) {
        std::cerr << "FAIL[REGIONAL_COMPAT]: " << result.detail << '\n';
        return static_cast<int>(ExitCode::ValidationFailed);
    }
    std::cout << (partial_probe ? "PASS REGIONAL_COMPAT_PARTIAL_PROBE\n"
                                : "PASS REGIONAL_COMPAT_READY_FOR_VALIDATION\n")
              << "input_manifest=" << request.source_manifest << '\n'
              << "repository_root=" << request.repository_root << '\n'
              << "dataset_id=" << result.value->dataset_id << '\n'
              << "dataset_order=" << result.value->dataset_order << '\n'
              << "output_directory=" << result.value->output_directory << '\n'
              << "workers=" << workers << '\n'
              << "regions=" << result.value->regions << '\n'
              << "units=" << result.value->units << '\n'
              << "patterns=" << result.value->patterns << '\n'
              << "semantic_sha256=" << result.value->semantic_sha256 << '\n'
              << "input_sha256_seconds=" << result.value->input_sha256_seconds << '\n'
              << "science_wall_seconds=" << result.value->science_wall_seconds << '\n'
              << "total_wall_seconds=" << result.value->total_wall_seconds << '\n';
    return static_cast<int>(ExitCode::Success);
}

bool map_equals(const std::map<std::string, std::uint64_t>& observed,
                const std::map<std::string, std::uint64_t>& expected) {
    return observed == expected;
}

void write_count_map(std::ofstream& output, const std::map<std::string, std::uint64_t>& values) {
    bool first = true;
    for (const auto& entry : values) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << '\n' << "    \"" << entry.first << "\": " << entry.second;
    }
    if (!values.empty()) {
        output << '\n' << "  ";
    }
}

int replay_mode(int argc, char** argv, bool enforce_hcc1395_golden) {
    std::vector<std::filesystem::path> parts;
    std::filesystem::path output_path;
    std::string dataset_id;
    for (int index = 2; index < argc; ++index) {
        const std::string option(argv[index]);
        if (index + 1 >= argc) {
            std::cerr << "ERROR[CLI]: missing value for " << option << '\n';
            return static_cast<int>(ExitCode::UsageError);
        }
        if (option == "--mlhp") {
            parts.emplace_back(argv[++index]);
        } else if (option == "--output" && output_path.empty()) {
            output_path = argv[++index];
        } else if (option == "--dataset-id" && dataset_id.empty()) {
            dataset_id = argv[++index];
        } else {
            std::cerr << "ERROR[CLI]: unknown/duplicate option: " << option << '\n';
            return static_cast<int>(ExitCode::UsageError);
        }
    }
    if (enforce_hcc1395_golden && dataset_id.empty()) {
        dataset_id = "HCC1395";
    }
    if (parts.empty() || output_path.empty() || !safe_dataset_id(dataset_id) || std::filesystem::exists(output_path)) {
        std::cerr << "ERROR[CLI]: dataset ID, MLHP parts and a new --output path are required\n";
        return static_cast<int>(ExitCode::UsageError);
    }
    const auto replay = longlineage::compat::replay_frozen_mlhp_oracle(parts);
    if (!replay.ok()) {
        std::cerr << "FAIL[ORACLE_REPLAY]: " << replay.detail << '\n';
        return static_cast<int>(ExitCode::ValidationFailed);
    }
    const std::map<std::string, std::uint64_t> expected_all = {
        {"ambiguous_structure", 6395},
        {"capped", 956},
        {"determined", 12738},
        {"recurrence_required", 30},
    };
    const std::map<std::string, std::uint64_t> expected_primary = {
        {"ambiguous_structure", 5875},
        {"capped", 784},
        {"determined", 4217},
        {"recurrence_required", 28},
    };
    const bool exact = !enforce_hcc1395_golden ||
                       (dataset_id == "HCC1395" && replay.value->regions == 8222 && replay.value->units == 20119 &&
                        replay.value->patterns == 106559 && map_equals(replay.value->all_unit_classes, expected_all) &&
                        map_equals(replay.value->primary_unit_classes, expected_primary));
    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        std::cerr << "FAIL[ORACLE_REPLAY]: cannot create output receipt\n";
        return static_cast<int>(ExitCode::IoError);
    }
    output << "{\n"
           << "  \"schema_name\": \"longlineage.regional_oracle_replay\",\n"
           << "  \"schema_version\": \"1.0.0\",\n"
           << "  \"profile_id\": \"PYTHON_V2_DESCRIPTIVE_REGIONAL\",\n"
           << "  \"dataset_id\": \"" << dataset_id << "\",\n"
           << "  \"regions\": " << replay.value->regions << ",\n"
           << "  \"units\": " << replay.value->units << ",\n"
           << "  \"patterns\": " << replay.value->patterns << ",\n"
           << "  \"all_unit_classes\": {";
    write_count_map(output, replay.value->all_unit_classes);
    output << "},\n  \"primary_unit_classes\": {";
    write_count_map(output, replay.value->primary_unit_classes);
    output << "},\n"
           << "  \"unit_semantic_sha256\": \"" << replay.value->unit_semantic_sha256 << "\",\n"
           << "  \"hcc1395_golden_enforced\": " << (enforce_hcc1395_golden ? "true" : "false") << ",\n"
           << "  \"hcc1395_golden_exact\": " << (exact ? "true" : "false") << "\n}\n";
    output.close();
    if (!output || !exact) {
        std::cerr << "FAIL[ORACLE_REPLAY]: HCC1395 solver census mismatch or receipt write failed\n";
        return static_cast<int>(ExitCode::ValidationFailed);
    }
    std::cout << (enforce_hcc1395_golden ? "PASS HCC1395_REGIONAL_ORACLE_REPLAY\n" : "PASS REGIONAL_ORACLE_REPLAY\n")
              << "dataset_id=" << dataset_id << '\n'
              << "input_parts=" << parts.size() << '\n'
              << "output=" << output_path << '\n'
              << "regions=" << replay.value->regions << '\n'
              << "units=" << replay.value->units << '\n'
              << "patterns=" << replay.value->patterns << '\n'
              << "unit_semantic_sha256=" << replay.value->unit_semantic_sha256 << '\n';
    return static_cast<int>(ExitCode::Success);
}

int crosswalk_mode(int argc, char** argv) {
    if (argc != 10) {
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    std::map<std::string, std::string> options;
    for (int index = 2; index < argc; index += 2) {
        const std::string key(argv[index]);
        if ((key != "--repo-root" && key != "--bundle" && key != "--python-manifest" && key != "--output") ||
            !options.emplace(key, argv[index + 1]).second) {
            std::cerr << "ERROR[CLI]: unknown/duplicate crosswalk option: " << key << '\n';
            return static_cast<int>(ExitCode::UsageError);
        }
    }
    if (options.size() != 4U || options.count("--repo-root") != 1U || options.count("--bundle") != 1U ||
        options.count("--python-manifest") != 1U || options.count("--output") != 1U) {
        std::cerr << "ERROR[CLI]: crosswalk requires repository, bundle, Python manifest and output\n";
        return static_cast<int>(ExitCode::UsageError);
    }
    const std::filesystem::path repository_root = options.at("--repo-root");
    const std::filesystem::path bundle = options.at("--bundle");
    const std::filesystem::path python_manifest = options.at("--python-manifest");
    const std::filesystem::path output_path = options.at("--output");
    if (!repository_root.is_absolute() || !std::filesystem::is_directory(repository_root) || !bundle.is_absolute() ||
        !python_manifest.is_absolute() || !output_path.is_absolute() || std::filesystem::exists(output_path)) {
        std::cerr << "ERROR[CLI]: crosswalk paths must be absolute and output must be new\n";
        return static_cast<int>(ExitCode::UsageError);
    }
    const auto receipt = longlineage::compat::compare_frozen_regional_bundle(repository_root, bundle, python_manifest);
    if (!receipt.ok()) {
        std::cerr << "FAIL[REGIONAL_CROSSWALK]: " << receipt.detail << '\n';
        return static_cast<int>(ExitCode::ValidationFailed);
    }
    const std::string json = longlineage::compat::render_regional_crosswalk_receipt_json(*receipt.value);
    std::ofstream output(output_path, std::ios::binary);
    output << json;
    output.close();
    if (!output || json.empty()) {
        std::cerr << "FAIL[REGIONAL_CROSSWALK]: cannot write crosswalk receipt\n";
        return static_cast<int>(ExitCode::IoError);
    }
    std::cout << (receipt.value->all_exact ? "PASS REGIONAL_CROSSWALK_EXACT\n" : "FAIL REGIONAL_CROSSWALK_MISMATCH\n")
              << "dataset_id=" << receipt.value->dataset_id << '\n'
              << "repository_root=" << repository_root << '\n'
              << "cpp_bundle=" << bundle << '\n'
              << "python_manifest=" << python_manifest << '\n'
              << "output=" << output_path << '\n'
              << "regions=" << receipt.value->regions.actual_rows << ",mismatches=" << receipt.value->regions.mismatches
              << '\n'
              << "units=" << receipt.value->units.actual_rows << ",mismatches=" << receipt.value->units.mismatches
              << '\n'
              << "patterns=" << receipt.value->patterns.actual_rows
              << ",mismatches=" << receipt.value->patterns.mismatches << '\n';
    return static_cast<int>(receipt.value->all_exact ? ExitCode::Success : ExitCode::ValidationFailed);
}

int cohort_mode(int argc, char** argv) {
    if (argc != 8) {
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    std::map<std::string, std::string> options;
    for (int index = 2; index < argc; index += 2) {
        const std::string key(argv[index]);
        if ((key != "--bundle-root" && key != "--crosswalk-dir" && key != "--output") ||
            !options.emplace(key, argv[index + 1]).second) {
            std::cerr << "ERROR[CLI]: unknown/duplicate cohort option: " << key << '\n';
            return static_cast<int>(ExitCode::UsageError);
        }
    }
    if (options.size() != 3U || options.count("--bundle-root") != 1U || options.count("--crosswalk-dir") != 1U ||
        options.count("--output") != 1U) {
        std::cerr << "ERROR[CLI]: cohort requires bundle root, crosswalk directory and output\n";
        return static_cast<int>(ExitCode::UsageError);
    }
    const std::filesystem::path bundle_root = options.at("--bundle-root");
    const std::filesystem::path crosswalk_dir = options.at("--crosswalk-dir");
    const std::filesystem::path output_path = options.at("--output");
    if (!bundle_root.is_absolute() || !crosswalk_dir.is_absolute() || !output_path.is_absolute() ||
        !std::filesystem::is_directory(bundle_root) || !std::filesystem::is_directory(crosswalk_dir) ||
        std::filesystem::exists(output_path)) {
        std::cerr << "ERROR[CLI]: cohort input directories must exist, paths must be absolute and output must be new\n";
        return static_cast<int>(ExitCode::UsageError);
    }
    constexpr std::array<std::string_view, 7> kDatasets = {
        "HCC1395", "HCC1395_DORADO", "COLO829", "H1437", "H2009", "HCC1937", "HCC1954",
    };
    std::vector<longlineage::compat::RegionalCohortInput> inputs;
    inputs.reserve(kDatasets.size());
    for (const std::string_view dataset : kDatasets) {
        inputs.push_back({bundle_root / dataset, crosswalk_dir / (std::string(dataset) + ".crosswalk.json")});
    }
    const auto receipt = longlineage::compat::aggregate_frozen_regional_cohort(inputs);
    if (!receipt.ok()) {
        std::cerr << "FAIL[REGIONAL_COHORT]: " << receipt.detail << '\n';
        return static_cast<int>(ExitCode::ValidationFailed);
    }
    const std::string json = longlineage::compat::render_regional_cohort_receipt_json(*receipt.value);
    std::ofstream output(output_path, std::ios::binary);
    output << json;
    output.close();
    if (!output || json.empty()) {
        std::cerr << "FAIL[REGIONAL_COHORT]: cannot write cohort receipt\n";
        return static_cast<int>(ExitCode::IoError);
    }
    std::cout << "PASS REGIONAL_COHORT_EXACT_RECEIPTS\n"
              << "bundle_root=" << bundle_root << '\n'
              << "crosswalk_dir=" << crosswalk_dir << '\n'
              << "output=" << output_path << '\n'
              << "datasets=" << receipt.value->samples.size() << '\n'
              << "regions=" << receipt.value->total_row_counts.regions << '\n'
              << "units=" << receipt.value->total_row_counts.units << '\n'
              << "patterns=" << receipt.value->total_row_counts.patterns << '\n'
              << "source_set_sha256=" << receipt.value->source_set_sha256 << '\n';
    return static_cast<int>(ExitCode::Success);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        usage();
        return static_cast<int>(ExitCode::Success);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "run") {
        return run_mode(argc, argv, false);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "probe") {
        return run_mode(argc, argv, true);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "replay-hcc1395") {
        return replay_mode(argc, argv, true);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "replay") {
        return replay_mode(argc, argv, false);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "crosswalk") {
        return crosswalk_mode(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "cohort") {
        return cohort_mode(argc, argv);
    }
    usage();
    return static_cast<int>(ExitCode::UsageError);
}
