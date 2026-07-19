// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <iostream>
#include <string>

#include "cli_support.hpp"

namespace {

void usage() {
    std::cout << "Usage: longlineage-validate --run-root DIR\n"
              << "The validator is an independent target and never links producer kernels.\n";
}

}  // namespace

int main(int argc, char** argv) {
    using longlineage::cli::ExitCode;
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        usage();
        return static_cast<int>(ExitCode::Success);
    }
    if (argc != 3 || std::string(argv[1]) != "--run-root") {
        longlineage::cli::emit_error("validate", ExitCode::UsageError, "expected exactly --run-root DIR");
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }

    const std::filesystem::path run_root = argv[2];
    longlineage::cli::emit_error(
        "validate", ExitCode::ValidationFailed,
        "independent artifact replay is not implemented; no validation receipt was written for " + run_root.string());
    return static_cast<int>(ExitCode::ValidationFailed);
}
