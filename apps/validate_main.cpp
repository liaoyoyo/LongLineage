// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "cli_support.hpp"
#include "longlineage/validation/artifact_validator.hpp"

namespace {

void usage() {
    std::cout << "Usage: longlineage-validate --run-root DIR --manifest FILE [--repo DIR] [--check-only]\n"
              << "       longlineage-validate --run-root DIR --manifest FILE --freeze --output-base DIR [--repo DIR]\n"
              << "The validator is an independent target and never links producer kernels.\n";
}

}  // namespace

int main(int argc, char** argv) {
    using longlineage::cli::ExitCode;
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        usage();
        return static_cast<int>(ExitCode::Success);
    }

    std::map<std::string, std::string> options;
    bool check_only = false;
    bool freeze = false;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--check-only") {
            if (check_only) {
                longlineage::cli::emit_error("validate", ExitCode::UsageError, "duplicate --check-only");
                return static_cast<int>(ExitCode::UsageError);
            }
            check_only = true;
            continue;
        }
        if (option == "--freeze") {
            if (freeze) {
                longlineage::cli::emit_error("validate", ExitCode::UsageError, "duplicate --freeze");
                return static_cast<int>(ExitCode::UsageError);
            }
            freeze = true;
            continue;
        }
        if (option != "--run-root" && option != "--repo" && option != "--output-base" && option != "--manifest") {
            longlineage::cli::emit_error("validate", ExitCode::UsageError, "unknown option: " + option);
            return static_cast<int>(ExitCode::UsageError);
        }
        if (index + 1 >= argc || options.count(option) != 0U) {
            longlineage::cli::emit_error("validate", ExitCode::UsageError,
                                         "missing value or duplicate option: " + option);
            return static_cast<int>(ExitCode::UsageError);
        }
        options.emplace(option, argv[++index]);
    }
    if (options.count("--run-root") == 0U) {
        longlineage::cli::emit_error("validate", ExitCode::UsageError, "--run-root DIR is required");
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    if ((freeze && (check_only || options.count("--output-base") == 0U || options.count("--manifest") == 0U)) ||
        (!freeze && options.count("--output-base") != 0U)) {
        longlineage::cli::emit_error(
            "validate", ExitCode::UsageError,
            "--freeze requires --manifest and --output-base and is incompatible with --check-only");
        return static_cast<int>(ExitCode::UsageError);
    }

    const std::filesystem::path explicit_repo =
        options.count("--repo") == 0U ? std::filesystem::path{} : std::filesystem::path(options.at("--repo"));
    const std::filesystem::path repo_root = longlineage::cli::find_repo_root(explicit_repo);
    if (repo_root.empty()) {
        longlineage::cli::emit_error("validate", ExitCode::IoError, "cannot locate repository contract root");
        return static_cast<int>(ExitCode::IoError);
    }
    std::error_code error;
    const std::filesystem::path run_root = std::filesystem::absolute(options.at("--run-root"), error);
    if (error) {
        longlineage::cli::emit_error("validate", ExitCode::IoError, "cannot resolve run root");
        return static_cast<int>(ExitCode::IoError);
    }
    const std::filesystem::path executable = std::filesystem::canonical("/proc/self/exe", error);
    if (error) {
        longlineage::cli::emit_error("validate", ExitCode::IoError, "cannot resolve validator executable");
        return static_cast<int>(ExitCode::IoError);
    }
    std::filesystem::path manifest_path;
    if (options.count("--manifest") != 0U) {
        manifest_path = std::filesystem::absolute(options.at("--manifest"), error);
        if (error) {
            longlineage::cli::emit_error("validate", ExitCode::IoError, "cannot resolve production manifest");
            return static_cast<int>(ExitCode::IoError);
        }
    }

    if (freeze) {
        const std::filesystem::path output_base = std::filesystem::absolute(options.at("--output-base"), error);
        if (error) {
            longlineage::cli::emit_error("validate", ExitCode::IoError, "cannot resolve output base");
            return static_cast<int>(ExitCode::IoError);
        }
        const auto report = longlineage::validation::ArtifactValidator::validate_and_freeze(
            {{repo_root, run_root.lexically_normal(), executable, true, manifest_path.lexically_normal()},
             output_base.lexically_normal(),
             false,
             {}});
        std::cout << longlineage::validation::render_finalize_report_json(report) << '\n';
        return static_cast<int>(report.dataset_gate_frozen ? ExitCode::Success : ExitCode::ValidationFailed);
    }
    const auto report = longlineage::validation::ArtifactValidator::validate(
        {repo_root, run_root.lexically_normal(), executable, !check_only, manifest_path.lexically_normal()});
    std::cout << longlineage::validation::render_report_json(report) << '\n';
    return static_cast<int>(report.all_pass ? ExitCode::Success : ExitCode::ValidationFailed);
}
