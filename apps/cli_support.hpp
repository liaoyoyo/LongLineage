// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <jansson.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace longlineage::cli {

enum class ExitCode : int {
    Success = 0,
    UsageError = 2,
    PreflightRejected = 3,
    IoError = 4,
    SchemaError = 5,
    KernelBlocked = 6,
    ValidationFailed = 7,
    QueryRejected = 8,
    InternalError = 9,
};

struct JsonDeleter {
    void operator()(json_t* value) const noexcept;
};

using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

struct CheckResult {
    bool ok{false};
    std::string message;
};

std::filesystem::path find_repo_root(const std::filesystem::path& explicit_root = {});
JsonPtr load_json_strict(const std::filesystem::path& path, std::string& error);
CheckResult sha256_file_hex(const std::filesystem::path& path, std::string& digest);
CheckResult require_safe_repository_file(const std::filesystem::path& repo_root, const std::string& relative_path);
CheckResult validate_production_manifest(const json_t* manifest);
CheckResult require_release_attestation_ready(const std::filesystem::path& repo_root,
                                              const std::string& expected_sha256);
CheckResult require_validated_frozen_run(const std::filesystem::path& run_root);

std::string json_quote(const std::string& value);
void emit_result(const std::string& command, const CheckResult& result);
void emit_error(const std::string& command, ExitCode code, const std::string& message);
bool is_help_flag(const std::string& value);

}  // namespace longlineage::cli
