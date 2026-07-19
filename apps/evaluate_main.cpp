// SPDX-License-Identifier: GPL-3.0-only

#include <filesystem>
#include <iostream>
#include <string>

#include "cli_support.hpp"

namespace {

void usage() {
    std::cout << "Usage: longlineage-evaluate --run-root DIR --truth-vcf FILE --truth-bed FILE "
                 "--output DIR\n"
              << "Evaluation is posthoc and accepts only VALIDATED_FROZEN production runs.\n";
}

}  // namespace

int main(int argc, char** argv) {
    using longlineage::cli::ExitCode;
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        usage();
        return static_cast<int>(ExitCode::Success);
    }
    if (argc != 9 || std::string(argv[1]) != "--run-root" || std::string(argv[3]) != "--truth-vcf" ||
        std::string(argv[5]) != "--truth-bed" || std::string(argv[7]) != "--output") {
        longlineage::cli::emit_error("evaluate", ExitCode::UsageError,
                                     "expected frozen run, truth inputs and isolated output");
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    const auto gate = longlineage::cli::require_validated_frozen_run(std::filesystem::path(argv[2]));
    if (!gate.ok) {
        longlineage::cli::emit_error("evaluate", ExitCode::QueryRejected, gate.message);
        return static_cast<int>(ExitCode::QueryRejected);
    }
    longlineage::cli::emit_error("evaluate", ExitCode::KernelBlocked,
                                 "posthoc benchmark engine is not implemented; production artifacts remain untouched");
    return static_cast<int>(ExitCode::KernelBlocked);
}
