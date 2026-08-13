// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "cli_support.hpp"
#include "longlineage/validation/regional_compat_validator.hpp"

namespace {

void usage() {
    std::cout << "Usage: longlineage-regional-validate --bundle DIR [--repo-root DIR] [--check-only]\n"
              << "Independently validates and freezes a Python-compatible regional topology bundle.\n";
}

}  // namespace

int main(int argc, char** argv) {
    using longlineage::cli::ExitCode;
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        usage();
        return static_cast<int>(ExitCode::Success);
    }
    std::filesystem::path bundle;
    std::filesystem::path repository_root;
    bool check_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--check-only") {
            if (check_only) {
                longlineage::cli::emit_error("regional-validate", ExitCode::UsageError, "duplicate --check-only");
                return static_cast<int>(ExitCode::UsageError);
            }
            check_only = true;
        } else if (option == "--bundle") {
            if (!bundle.empty() || index + 1 >= argc) {
                longlineage::cli::emit_error("regional-validate", ExitCode::UsageError,
                                             "--bundle requires exactly one path");
                return static_cast<int>(ExitCode::UsageError);
            }
            bundle = argv[++index];
        } else if (option == "--repo-root") {
            if (!repository_root.empty() || index + 1 >= argc) {
                longlineage::cli::emit_error("regional-validate", ExitCode::UsageError,
                                             "--repo-root requires exactly one path");
                return static_cast<int>(ExitCode::UsageError);
            }
            repository_root = argv[++index];
        } else {
            longlineage::cli::emit_error("regional-validate", ExitCode::UsageError, "unknown option: " + option);
            return static_cast<int>(ExitCode::UsageError);
        }
    }
    if (bundle.empty()) {
        longlineage::cli::emit_error("regional-validate", ExitCode::UsageError, "--bundle is required");
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    std::error_code error;
    const std::filesystem::path executable = std::filesystem::canonical("/proc/self/exe", error);
    if (error) {
        longlineage::cli::emit_error("regional-validate", ExitCode::InternalError,
                                     "cannot resolve validator executable: " + error.message());
        return static_cast<int>(ExitCode::InternalError);
    }
    const auto report = longlineage::validation::RegionalCompatValidator::validate_and_freeze(
        {bundle, executable, !check_only, repository_root});
    std::cout << longlineage::validation::render_regional_compat_validation_report_json(report);
    return static_cast<int>(report.all_pass ? ExitCode::Success : ExitCode::ValidationFailed);
}
