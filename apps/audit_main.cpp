// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

#include "longlineage/audit/hcc1395_determinism.hpp"

#ifndef LONGLINEAGE_VERSION
#define LONGLINEAGE_VERSION "0.1.0"
#endif

namespace {

void print_usage() {
    std::cout << "Usage:\n"
              << "  longlineage-audit hcc1395-determinism \\\n"
              << "    --w24-root ABS_PATH --w24-manifest ABS_PATH \\\n"
              << "    --w40-root ABS_PATH --w40-manifest ABS_PATH \\\n"
              << "    --historical-m1-tsv-gz ABS_PATH \\\n"
              << "    --historical-m1-sha256 LOWER_HEX_64 \\\n"
              << "    --output ABS_PATH\n\n"
              << "The command reads only two frozen run roots, their explicit manifest "
                 "metadata,\n"
              << "and the explicitly SHA-bound historical M1 gzip TSV. It never "
                 "opens BAM,\n"
              << "VCF, sidecar or reference inputs.\n";
}

int run_hcc1395(int argc, char** argv) {
    if ((argc - 2) % 2 != 0) {
        std::cerr << "ERROR[CLI]: every option requires one value\n";
        return 2;
    }
    std::map<std::string, std::string> options;
    for (int index = 2; index < argc; index += 2) {
        const std::string key(argv[index]);
        if (key.rfind("--", 0U) != 0U || !options.emplace(key, argv[index + 1]).second) {
            std::cerr << "ERROR[CLI]: unknown form or duplicate option: " << key << '\n';
            return 2;
        }
    }
    static const std::string required[] = {
        "--w24-root",     "--w24-manifest",         "--w40-root",
        "--w40-manifest", "--historical-m1-tsv-gz", "--historical-m1-sha256",
        "--output",
    };
    if (options.size() != std::size(required)) {
        std::cerr << "ERROR[CLI]: command requires exactly seven named options\n";
        return 2;
    }
    for (const std::string& key : required) {
        if (options.count(key) == 0U || options.at(key).empty()) {
            std::cerr << "ERROR[CLI]: missing required option: " << key << '\n';
            return 2;
        }
    }

    longlineage::audit::Hcc1395DeterminismAuditOptions audit;
    audit.w24_run_root = options.at("--w24-root");
    audit.w24_manifest = options.at("--w24-manifest");
    audit.w40_run_root = options.at("--w40-root");
    audit.w40_manifest = options.at("--w40-manifest");
    audit.historical_m1_tsv_gz = options.at("--historical-m1-tsv-gz");
    audit.historical_m1_sha256 = options.at("--historical-m1-sha256");
    audit.output_receipt = options.at("--output");
    const auto result = longlineage::audit::run_hcc1395_determinism_audit(audit);
    if (!result.ok) {
        std::cerr << "FAIL[" << result.error_code << "]: " << result.detail << '\n';
        return 1;
    }
    std::cout << "PASS HCC1395_DETERMINISM_AUDIT\n"
              << "receipt=" << audit.output_receipt << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "longlineage-audit " << LONGLINEAGE_VERSION << '\n';
        return 0;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "hcc1395-determinism") {
        return run_hcc1395(argc, argv);
    }
    print_usage();
    return 2;
}
