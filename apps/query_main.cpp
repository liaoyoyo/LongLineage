// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "cli_support.hpp"

namespace {

void usage() {
    std::cout << "Usage: longlineage-query --run-root DIR --artifact ID "
                 "[--key FIELD=VALUE] [--limit N]\n"
              << "Only VALIDATED_FROZEN runs are queryable; the command is read-only.\n";
}

}  // namespace

int main(int argc, char** argv) {
    using longlineage::cli::ExitCode;
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        usage();
        return static_cast<int>(ExitCode::Success);
    }

    std::map<std::string, std::string> options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option != "--run-root" && option != "--artifact" && option != "--key" && option != "--limit") {
            longlineage::cli::emit_error("query", ExitCode::UsageError, "unknown option: " + option);
            return static_cast<int>(ExitCode::UsageError);
        }
        if (index + 1 >= argc || options.count(option) != 0U) {
            longlineage::cli::emit_error("query", ExitCode::UsageError, "missing value or duplicate option: " + option);
            return static_cast<int>(ExitCode::UsageError);
        }
        options.emplace(option, argv[++index]);
    }
    if (options.count("--run-root") == 0U || options.count("--artifact") == 0U) {
        longlineage::cli::emit_error("query", ExitCode::UsageError, "--run-root and --artifact are required");
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }

    const std::filesystem::path run_root = options.at("--run-root");
    const auto gate = longlineage::cli::require_validated_frozen_run(run_root);
    if (!gate.ok) {
        longlineage::cli::emit_error("query", ExitCode::QueryRejected, gate.message);
        return static_cast<int>(ExitCode::QueryRejected);
    }
    longlineage::cli::emit_error("query", ExitCode::QueryRejected,
                                 "indexed record reader is not implemented; no rows were returned or recomputed");
    return static_cast<int>(ExitCode::QueryRejected);
}
