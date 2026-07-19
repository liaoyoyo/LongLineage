// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <iostream>
#include <string>

#include "cli_support.hpp"

namespace {

void usage() {
    std::cout << "Usage: longlineage-export-legacy --run-root DIR --output DIR\n"
              << "Export is a schema-only transform of VALIDATED_FROZEN native records.\n";
}

}  // namespace

int main(int argc, char** argv) {
    using longlineage::cli::ExitCode;
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        usage();
        return static_cast<int>(ExitCode::Success);
    }
    if (argc != 5 || std::string(argv[1]) != "--run-root" || std::string(argv[3]) != "--output") {
        longlineage::cli::emit_error("export-legacy", ExitCode::UsageError, "expected --run-root DIR --output DIR");
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    const auto gate = longlineage::cli::require_validated_frozen_run(std::filesystem::path(argv[2]));
    if (!gate.ok) {
        longlineage::cli::emit_error("export-legacy", ExitCode::QueryRejected, gate.message);
        return static_cast<int>(ExitCode::QueryRejected);
    }
    longlineage::cli::emit_error("export-legacy", ExitCode::KernelBlocked,
                                 "legacy layout writer is not implemented; output directory was not created");
    return static_cast<int>(ExitCode::KernelBlocked);
}
