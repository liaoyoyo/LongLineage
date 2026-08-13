// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cli_support.hpp"

namespace {

using longlineage::cli::CheckResult;
using longlineage::cli::ExitCode;
using longlineage::cli::JsonPtr;

CheckResult pass(std::string message) { return {true, std::move(message)}; }

CheckResult fail(std::string message) { return {false, std::move(message)}; }

std::string string_field(const json_t* object, const char* name) {
    const auto* value = json_object_get(object, name);
    return json_is_string(value) ? std::string(json_string_value(value)) : std::string{};
}

bool object_has_exact_keys(const json_t* object, const std::set<std::string>& expected, std::string& error) {
    if (!json_is_object(object)) {
        error = "expected object";
        return false;
    }
    for (const auto& name : expected) {
        if (json_object_get(object, name.c_str()) == nullptr) {
            error = "missing field: " + name;
            return false;
        }
    }
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(object), key, value) {
        static_cast<void>(value);
        if (expected.count(key) == 0U) {
            error = "unknown field: " + std::string(key);
            return false;
        }
    }
    return true;
}

bool array_contains_string(const json_t* array, const std::string& expected) {
    if (!json_is_array(array)) {
        return false;
    }
    for (std::size_t index = 0; index < json_array_size(array); ++index) {
        const auto* value = json_array_get(array, index);
        if (json_is_string(value) && std::string(json_string_value(value)) == expected) {
            return true;
        }
    }
    return false;
}

CheckResult check_policy(const std::filesystem::path& root) {
    std::string error;
    const auto policy = longlineage::cli::load_json_strict(root / "governance" / "repository_policy.json", error);
    if (!policy) {
        return fail(error);
    }

    const std::set<std::string> root_keys = {
        "schema_name",
        "schema_version",
        "additional_properties_allowed",
        "permanent_branches",
        "temporary_branch_prefixes",
        "required_root_files",
        "protected_decisions",
        "sensitive_data_policy",
        "build_policy",
        "truth_aware_targets",
        "contract_sources",
        "presentation_source_root",
        "release_requires_explicit_public_authorization",
    };
    if (!object_has_exact_keys(policy.get(), root_keys, error)) {
        return fail("repository policy: " + error);
    }
    if (string_field(policy.get(), "schema_name") != "longlineage.repository_policy" ||
        string_field(policy.get(), "schema_version") != "1.0.0" ||
        !json_is_false(json_object_get(policy.get(), "additional_properties_allowed")) ||
        !json_is_true(json_object_get(policy.get(), "release_requires_explicit_public_authorization"))) {
        return fail("repository policy identity/closed-world/public-authorization rule is invalid");
    }
    const auto* permanent = json_object_get(policy.get(), "permanent_branches");
    if (!json_is_array(permanent) || json_array_size(permanent) != 1U || !array_contains_string(permanent, "main")) {
        return fail("main must be the only permanent branch");
    }

    const auto* required_files = json_object_get(policy.get(), "required_root_files");
    if (!json_is_array(required_files)) {
        return fail("required_root_files is not an array");
    }
    for (std::size_t index = 0; index < json_array_size(required_files); ++index) {
        const auto* entry = json_array_get(required_files, index);
        if (!json_is_string(entry)) {
            return fail("required_root_files contains a non-string");
        }
        const std::filesystem::path relative = json_string_value(entry);
        if (relative.is_absolute() || relative.string().find("..") != std::string::npos ||
            !std::filesystem::is_regular_file(root / relative)) {
            return fail("required root file is absent or unsafe: " + relative.string());
        }
    }

    const auto* sensitive = json_object_get(policy.get(), "sensitive_data_policy");
    const std::set<std::string> sensitive_keys = {
        "maximum_tracked_file_bytes",     "git_lfs_allowed",
        "real_genomics_payload_allowed",  "real_coordinates_allowed",
        "absolute_private_paths_allowed", "synthetic_contig_prefix",
    };
    if (!object_has_exact_keys(sensitive, sensitive_keys, error) ||
        !json_is_integer(json_object_get(sensitive, "maximum_tracked_file_bytes")) ||
        json_integer_value(json_object_get(sensitive, "maximum_tracked_file_bytes")) != 1048576 ||
        !json_is_false(json_object_get(sensitive, "git_lfs_allowed")) ||
        !json_is_false(json_object_get(sensitive, "real_genomics_payload_allowed")) ||
        !json_is_false(json_object_get(sensitive, "real_coordinates_allowed")) ||
        !json_is_false(json_object_get(sensitive, "absolute_private_paths_allowed")) ||
        string_field(sensitive, "synthetic_contig_prefix") != "synchr") {
        return fail("sensitive-data policy differs from the machine hygiene gate");
    }

    const auto* protected_decisions = json_object_get(policy.get(), "protected_decisions");
    for (const auto& decision : {
             "PRODUCTION_TRUTH_ISOLATION",
             "LATEST_SIDECAR_ONLY_HP_PS",
             "NO_PYTHON_SCIENCE",
             "NO_WINNER_FROM_INCOMPLETE_FAMILY",
             "LINEAGE_COMPATIBLE_FAMILY_CLAIM_CEILING",
         }) {
        if (!array_contains_string(protected_decisions, decision)) {
            return fail("protected decision is missing: " + std::string(decision));
        }
    }

    const auto* build = json_object_get(policy.get(), "build_policy");
    const std::set<std::string> build_keys = {
        "minimum_cmake_version",
        "cpp_standard",
        "production_htslib_exact_version",
        "forbidden_compiler_flags",
        "network_fetch_during_release_configure",
    };
    if (!object_has_exact_keys(build, build_keys, error) || string_field(build, "minimum_cmake_version") != "3.22" ||
        !json_is_integer(json_object_get(build, "cpp_standard")) ||
        json_integer_value(json_object_get(build, "cpp_standard")) != 17 ||
        string_field(build, "production_htslib_exact_version") != "1.18" ||
        !json_is_false(json_object_get(build, "network_fetch_during_release_configure")) ||
        !array_contains_string(json_object_get(build, "forbidden_compiler_flags"), "-ffast-math") ||
        !array_contains_string(json_object_get(build, "forbidden_compiler_flags"), "-march=native")) {
        return fail("build policy does not match the frozen CMake/C++/HTSlib/flag contract");
    }
    const auto* aware_targets = json_object_get(policy.get(), "truth_aware_targets");
    if (!json_is_array(aware_targets) || json_array_size(aware_targets) != 1U ||
        !array_contains_string(aware_targets, "longlineage-evaluate")) {
        return fail("evaluation executable must be the sole truth-aware target");
    }
    const auto* contract_sources = json_object_get(policy.get(), "contract_sources");
    if (!json_is_array(contract_sources) || json_array_size(contract_sources) == 0U) {
        return fail("contract_sources must be a non-empty array");
    }
    for (std::size_t index = 0; index < json_array_size(contract_sources); ++index) {
        const auto* item = json_array_get(contract_sources, index);
        if (!json_is_string(item)) {
            return fail("contract_sources contains a non-string");
        }
        const std::filesystem::path relative = json_string_value(item);
        if (relative.is_absolute() || relative.string().find("..") != std::string::npos ||
            !std::filesystem::is_regular_file(root / relative)) {
            return fail("contract source is absent or unsafe: " + relative.string());
        }
    }
    return pass("repository policy is closed, complete and bound to required root files");
}

bool path_is_safe_relative(const std::string& value);
std::vector<std::string> split_tab(const std::string& line);

std::string trim_copy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::vector<std::string> split_markdown_row(const std::string& line) {
    std::vector<std::string> cells;
    if (line.empty() || line.front() != '|') {
        return cells;
    }
    std::size_t begin = 1U;
    while (begin < line.size()) {
        const auto end = line.find('|', begin);
        if (end == std::string::npos) {
            return {};
        }
        cells.push_back(trim_copy(line.substr(begin, end - begin)));
        begin = end + 1U;
    }
    return cells;
}

CheckResult load_phase_status_mirror(const std::filesystem::path& path, std::map<std::string, std::string>& statuses) {
    std::ifstream input(path);
    if (!input) {
        return fail("cannot open phase status mirror: " + path.string());
    }
    std::size_t phase_column = 0;
    std::size_t status_column = 0;
    bool found_header = false;
    std::string line;
    while (std::getline(input, line)) {
        const auto cells = split_markdown_row(line);
        if (cells.empty()) {
            continue;
        }
        if (!found_header) {
            const auto phase = std::find(cells.begin(), cells.end(), "Phase");
            const auto status = std::find(cells.begin(), cells.end(), "Status");
            if (phase != cells.end() && status != cells.end()) {
                phase_column = static_cast<std::size_t>(std::distance(cells.begin(), phase));
                status_column = static_cast<std::size_t>(std::distance(cells.begin(), status));
                found_header = true;
            }
            continue;
        }
        if (phase_column >= cells.size() || status_column >= cells.size()) {
            continue;
        }
        const auto& id = cells[phase_column];
        if (!std::regex_match(id, std::regex("^P[0-8]$"))) {
            continue;
        }
        if (!statuses.emplace(id, cells[status_column]).second) {
            return fail(path.string() + ": duplicate phase status row: " + id);
        }
    }
    if (!found_header || statuses.size() != 9U) {
        return fail(path.string() + ": phase status mirror must contain exactly P0 through P8");
    }
    return pass("phase status mirror parsed");
}

bool is_lower_sha256_text(const std::string& value) { return std::regex_match(value, std::regex("^[0-9a-f]{64}$")); }

bool is_rfc3339_seconds(const std::string& value) {
    return std::regex_match(value, std::regex("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}"
                                              "(Z|[+-][0-9]{2}:[0-9]{2})$"));
}

std::int64_t days_from_civil(int year, unsigned int month, unsigned int day) {
    year -= month <= 2U ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned int year_of_era = static_cast<unsigned int>(year - era * 400);
    const unsigned int adjusted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned int day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

std::optional<std::int64_t> parse_rfc3339_epoch_seconds(const std::string& value) {
    if (!is_rfc3339_seconds(value)) {
        return std::nullopt;
    }
    const int year = std::stoi(value.substr(0, 4));
    const unsigned int month = static_cast<unsigned int>(std::stoul(value.substr(5, 2)));
    const unsigned int day = static_cast<unsigned int>(std::stoul(value.substr(8, 2)));
    const unsigned int hour = static_cast<unsigned int>(std::stoul(value.substr(11, 2)));
    const unsigned int minute = static_cast<unsigned int>(std::stoul(value.substr(14, 2)));
    const unsigned int second = static_cast<unsigned int>(std::stoul(value.substr(17, 2)));
    const bool leap_year = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const std::array<unsigned int, 12> month_days = {
        31U, leap_year ? 29U : 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U,
    };
    if (month == 0U || month > month_days.size() || day == 0U || day > month_days[month - 1U] || hour > 23U ||
        minute > 59U || second > 59U) {
        return std::nullopt;
    }
    std::int64_t offset_seconds = 0;
    if (value.back() != 'Z') {
        const unsigned int offset_hour = static_cast<unsigned int>(std::stoul(value.substr(20, 2)));
        const unsigned int offset_minute = static_cast<unsigned int>(std::stoul(value.substr(23, 2)));
        if (offset_hour > 23U || offset_minute > 59U) {
            return std::nullopt;
        }
        offset_seconds = static_cast<std::int64_t>(offset_hour) * 3600 + static_cast<std::int64_t>(offset_minute) * 60;
        if (value[19] == '-') {
            offset_seconds = -offset_seconds;
        }
    }
    const std::int64_t local_seconds = days_from_civil(year, month, day) * 86400 +
                                       static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 +
                                       static_cast<std::int64_t>(second);
    return local_seconds - offset_seconds;
}

bool path_is_ancestor_or_equal(const std::filesystem::path& ancestor, const std::filesystem::path& descendant) {
    auto ancestor_part = ancestor.begin();
    auto descendant_part = descendant.begin();
    for (; ancestor_part != ancestor.end(); ++ancestor_part, ++descendant_part) {
        if (descendant_part == descendant.end() || *ancestor_part != *descendant_part) {
            return false;
        }
    }
    return true;
}

struct ActiveWriteClaim {
    std::string task_id;
    std::string owner_agent_id;
    std::filesystem::path path;
    bool directory_tree = false;
};

struct TaskRelation {
    std::string status;
    std::optional<std::string> parent_task_id;
    std::set<std::string> depends_on;
    bool archived = false;
};

bool validate_write_claim_path(const std::filesystem::path& root, const std::string& value, const std::string& kind,
                               std::filesystem::path& normalized, std::string& error) {
    const std::filesystem::path declared(value);
    normalized = declared.lexically_normal();
    if (value.empty() || declared.is_absolute() || declared == "." || normalized != declared || normalized.empty() ||
        normalized == ".") {
        error = "write claim is not a normalized non-root relative path: " + value;
        return false;
    }
    for (const auto& component : normalized) {
        if (component == "." || component == ".." || component.empty()) {
            error = "write claim has an unsafe component: " + value;
            return false;
        }
    }
    if (value.find_first_of("?*[]{}") != std::string::npos || (kind != "FILE" && kind != "DIRECTORY_TREE")) {
        error = "write claim has a glob or unknown kind: " + value;
        return false;
    }

    auto candidate = root;
    for (const auto& component : normalized) {
        candidate /= component;
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(candidate, status_error);
        if (!status_error && std::filesystem::is_symlink(status)) {
            error = "write claim traverses a symlink: " + value;
            return false;
        }
        if (status_error && status_error != std::errc::no_such_file_or_directory) {
            error = "write claim path cannot be inspected: " + value;
            return false;
        }
    }

    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(root / normalized, status_error);
    if (!status_error && std::filesystem::exists(status)) {
        if ((kind == "FILE" && !std::filesystem::is_regular_file(status)) ||
            (kind == "DIRECTORY_TREE" && !std::filesystem::is_directory(status))) {
            error = "write claim kind differs from the existing target: " + value;
            return false;
        }
    } else if (status_error && status_error != std::errc::no_such_file_or_directory) {
        error = "write claim target cannot be inspected: " + value;
        return false;
    }
    return true;
}

struct AuditEnvelopeInfo {
    std::string snapshot_id;
    std::string task_id;
    std::string scope_key;
    std::set<std::string> supersedes;
    std::optional<std::string> superseded_by;
    bool all_commands_pass = true;
};

CheckResult load_audit_envelope(const std::filesystem::path& path, AuditEnvelopeInfo& info) {
    std::string error;
    const auto envelope = longlineage::cli::load_json_strict(path, error);
    if (!envelope) {
        return fail(error);
    }
    const std::set<std::string> root_keys = {
        "schema_name",          "schema_version",       "snapshot_id",        "task_id",       "scope",
        "source_snapshot_kind", "source_commit",        "source_tree_sha256", "source_dirty",  "commands",
        "captured_at",          "captured_by_agent_id", "supersedes",         "superseded_by", "supersession_reason"};
    if (!object_has_exact_keys(envelope.get(), root_keys, error) ||
        string_field(envelope.get(), "schema_name") != "longlineage.audit_evidence" ||
        string_field(envelope.get(), "schema_version") != "1.0.0") {
        return fail(path.string() + ": audit envelope identity/closed-world shape is invalid: " + error);
    }
    info.snapshot_id = string_field(envelope.get(), "snapshot_id");
    info.task_id = string_field(envelope.get(), "task_id");
    if (!std::regex_match(info.snapshot_id, std::regex("^[0-9]{8}-[a-z0-9][a-z0-9-]*-[0-9]{3}$")) ||
        path.stem() != info.snapshot_id ||
        !std::regex_match(info.task_id, std::regex("^[0-9]{8}-[a-z0-9][a-z0-9-]*$")) ||
        !is_lower_sha256_text(string_field(envelope.get(), "source_tree_sha256")) ||
        !json_is_boolean(json_object_get(envelope.get(), "source_dirty")) ||
        !is_rfc3339_seconds(string_field(envelope.get(), "captured_at")) ||
        !std::regex_match(string_field(envelope.get(), "captured_by_agent_id"),
                          std::regex("^[a-z][a-z0-9_-]{1,31}:[A-Za-z0-9][A-Za-z0-9._/-]{0,126}$"))) {
        return fail(path.string() + ": audit snapshot/task/tree/capture identity is invalid");
    }
    const auto captured_epoch = parse_rfc3339_epoch_seconds(string_field(envelope.get(), "captured_at"));
    if (!captured_epoch.has_value()) {
        return fail(path.string() + ": audit captured_at cannot be parsed");
    }
    const std::string source_kind = string_field(envelope.get(), "source_snapshot_kind");
    const auto* source_commit = json_object_get(envelope.get(), "source_commit");
    const std::set<std::string> source_kinds = {"GIT_COMMIT", "GIT_TREE", "UNCOMMITTED_TREE", "VALIDATED_FROZEN_RUN"};
    if (source_kinds.count(source_kind) == 0U ||
        ((source_kind == "GIT_COMMIT" || source_kind == "GIT_TREE") &&
         (!json_is_string(source_commit) ||
          !std::regex_match(json_string_value(source_commit), std::regex("^(?:[0-9a-f]{40}|[0-9a-f]{64})$")))) ||
        ((source_kind == "UNCOMMITTED_TREE" || source_kind == "VALIDATED_FROZEN_RUN") &&
         !json_is_null(source_commit))) {
        return fail(path.string() + ": audit source snapshot kind/commit binding is invalid");
    }
    const auto* scope = json_object_get(envelope.get(), "scope");
    const std::set<std::string> scope_keys = {"kind", "completeness", "description", "included_paths",
                                              "excluded_paths"};
    if (!object_has_exact_keys(scope, scope_keys, error) || string_field(scope, "description").empty()) {
        return fail(path.string() + ": audit scope is malformed: " + error);
    }
    for (const auto& field : {"included_paths", "excluded_paths"}) {
        const auto* paths = json_object_get(scope, field);
        if (!json_is_array(paths) || (std::string(field) == "included_paths" && json_array_size(paths) == 0U)) {
            return fail(path.string() + ": audit scope path list is malformed");
        }
        std::set<std::string> unique_paths;
        for (std::size_t index = 0; index < json_array_size(paths); ++index) {
            const auto* value = json_array_get(paths, index);
            if (!json_is_string(value) || !path_is_safe_relative(json_string_value(value)) ||
                !unique_paths.insert(json_string_value(value)).second) {
                return fail(path.string() + ": audit scope path is unsafe or duplicated");
            }
        }
    }
    info.scope_key = string_field(scope, "kind") + "\n" + string_field(scope, "completeness") + "\n" +
                     string_field(scope, "description");

    const auto* commands = json_object_get(envelope.get(), "commands");
    if (!json_is_array(commands) || json_array_size(commands) == 0U) {
        return fail(path.string() + ": audit commands must be non-empty");
    }
    std::set<std::string> command_ids;
    for (std::size_t index = 0; index < json_array_size(commands); ++index) {
        const auto* command = json_array_get(commands, index);
        const std::set<std::string> command_keys = {"command_id", "argv",          "working_directory",
                                                    "exit_code",  "stdout_sha256", "stderr_sha256",
                                                    "started_at", "completed_at"};
        if (!object_has_exact_keys(command, command_keys, error) ||
            !command_ids.insert(string_field(command, "command_id")).second ||
            !path_is_safe_relative(string_field(command, "working_directory")) ||
            !json_is_integer(json_object_get(command, "exit_code")) ||
            json_integer_value(json_object_get(command, "exit_code")) < 0 ||
            json_integer_value(json_object_get(command, "exit_code")) > 255) {
            return fail(path.string() + ": audit command row is malformed: " + error);
        }
        const auto* argv = json_object_get(command, "argv");
        if (!json_is_array(argv) || json_array_size(argv) == 0U) {
            return fail(path.string() + ": audit command argv is empty");
        }
        for (std::size_t argument = 0; argument < json_array_size(argv); ++argument) {
            const auto* value = json_array_get(argv, argument);
            if (!json_is_string(value) || json_string_length(value) == 0U) {
                return fail(path.string() + ": audit command argv has an empty/non-string argument");
            }
        }
        for (const auto& digest_name : {"stdout_sha256", "stderr_sha256"}) {
            const auto* digest = json_object_get(command, digest_name);
            if (!json_is_null(digest) &&
                (!json_is_string(digest) || !is_lower_sha256_text(json_string_value(digest)))) {
                return fail(path.string() + ": audit command output digest is malformed");
            }
        }
        const auto started = parse_rfc3339_epoch_seconds(string_field(command, "started_at"));
        const auto completed = parse_rfc3339_epoch_seconds(string_field(command, "completed_at"));
        if (!started.has_value() || !completed.has_value() || *started > *completed || *completed > *captured_epoch) {
            return fail(path.string() + ": audit command time order is invalid");
        }
        info.all_commands_pass =
            info.all_commands_pass && json_integer_value(json_object_get(command, "exit_code")) == 0;
    }

    const auto* supersedes = json_object_get(envelope.get(), "supersedes");
    if (!json_is_array(supersedes)) {
        return fail(path.string() + ": audit supersedes must be an array");
    }
    for (std::size_t index = 0; index < json_array_size(supersedes); ++index) {
        const auto* value = json_array_get(supersedes, index);
        if (!json_is_string(value) || info.snapshot_id == json_string_value(value) ||
            !info.supersedes.insert(json_string_value(value)).second) {
            return fail(path.string() + ": audit supersedes contains a malformed, duplicate or self edge");
        }
    }
    const auto* superseded_by = json_object_get(envelope.get(), "superseded_by");
    if (json_is_string(superseded_by)) {
        info.superseded_by = std::string(json_string_value(superseded_by));
        if (*info.superseded_by == info.snapshot_id) {
            return fail(path.string() + ": audit superseded_by is a self edge");
        }
    } else if (!json_is_null(superseded_by)) {
        return fail(path.string() + ": audit superseded_by is neither null nor an ID");
    }
    const auto* reason = json_object_get(envelope.get(), "supersession_reason");
    if ((!info.supersedes.empty() || info.superseded_by.has_value()) &&
        (!json_is_string(reason) || json_string_length(reason) == 0U)) {
        return fail(path.string() + ": audit supersession edge lacks a reason");
    }
    if (info.supersedes.empty() && !info.superseded_by.has_value() && !json_is_null(reason)) {
        return fail(path.string() + ": audit without supersession edges carries a reason");
    }
    return pass("audit envelope passed");
}

CheckResult check_state(const std::filesystem::path& root) {
    std::string error;
    std::vector<std::pair<std::string, std::string>> phase_audit_references;
    std::vector<std::pair<std::string, std::string>> task_audit_references;
    const auto project = longlineage::cli::load_json_strict(root / "state" / "project_state.json", error);
    if (!project) {
        return fail(error);
    }
    const auto project_schema =
        longlineage::cli::load_json_strict(root / "governance" / "project_state.schema.json", error);
    if (!project_schema) {
        return fail(error);
    }
    if (string_field(project_schema.get(), "$id") != "https://longlineage.local/governance/project_state-1.0.0.json" ||
        !json_is_false(json_object_get(project_schema.get(), "additionalProperties"))) {
        return fail("project state schema identity/closed-world rule is invalid");
    }
    const auto ledger = longlineage::cli::load_json_strict(root / "state" / "phase_ledger.json", error);
    if (!ledger) {
        return fail(error);
    }
    const auto phase_schema =
        longlineage::cli::load_json_strict(root / "governance" / "phase_ledger.schema.json", error);
    if (!phase_schema) {
        return fail(error);
    }
    if (string_field(phase_schema.get(), "$id") != "https://longlineage.local/governance/phase_ledger-1.0.0.json" ||
        !json_is_false(json_object_get(phase_schema.get(), "additionalProperties"))) {
        return fail("phase ledger schema identity/closed-world rule is invalid");
    }
    const std::set<std::string> project_keys = {
        "schema_name", "schema_version",      "project",      "task_type",       "scope", "active_milestone", "goals",
        "open_gates",  "protected_decisions", "next_actions", "last_verified_at"};
    if (!object_has_exact_keys(project.get(), project_keys, error) ||
        string_field(project.get(), "schema_name") != "longlineage.project_state" ||
        string_field(project.get(), "schema_version") != "1.0.0" ||
        string_field(project.get(), "project") != "LongLineage") {
        return fail("project state identity/closed-world shape is invalid: " + error);
    }
    const std::set<std::string> task_types = {
        "A_EXPLORATORY_PILOT",  "B_COMPREHENSIVE_VALIDATION",
        "C_PRODUCTION_RELEASE", "D_EXTERNAL_HANDOFF",
        "E_HOTFIX_BUGFIX",      "F_DEMO",
    };
    const std::string task_type = string_field(project.get(), "task_type");
    if (task_types.count(task_type) == 0U || task_type != "B_COMPREHENSIVE_VALIDATION") {
        return fail("unknown task_type in project state: " + task_type);
    }
    const auto* scope = json_object_get(project.get(), "scope");
    const std::set<std::string> scope_keys = {"datasets", "biological_samples", "genome", "partial"};
    if (!object_has_exact_keys(scope, scope_keys, error) || !json_is_integer(json_object_get(scope, "datasets")) ||
        json_integer_value(json_object_get(scope, "datasets")) != 7 ||
        !json_is_integer(json_object_get(scope, "biological_samples")) ||
        json_integer_value(json_object_get(scope, "biological_samples")) != 6 ||
        string_field(scope, "genome") != "chr1-22") {
        return fail("project state B scope is not fixed at 7 datasets/6 samples/chr1-22");
    }
    const auto* partial = json_object_get(scope, "partial");
    if ((task_type == "B_COMPREHENSIVE_VALIDATION" || task_type == "C_PRODUCTION_RELEASE" ||
         task_type == "D_EXTERNAL_HANDOFF") &&
        !json_is_false(partial)) {
        return fail("B/C/D task type cannot carry partial=true");
    }
    const std::set<std::string> allowed_milestones = {
        "P0_P1_FOUNDATION",           "P2_RUNTIME_FOUNDATION", "P3_M1_PARITY",
        "P4_M2_COOCCURRENCE_PARITY",  "P5_TOPOLOGY_PARITY",    "P6_INDEPENDENT_VALIDATION",
        "P7_FULL_DATASET_VALIDATION", "P8_RELEASE_CANDIDATE"};
    if (allowed_milestones.count(string_field(project.get(), "active_milestone")) == 0U) {
        return fail("project state active_milestone is outside the closed vocabulary");
    }
    const std::set<std::string> expected_goals = {"LL-G1", "LL-G2", "LL-G3", "LL-G4", "LL-G5"};
    std::set<std::string> observed_project_goals;
    const auto* project_goals = json_object_get(project.get(), "goals");
    if (!json_is_array(project_goals)) {
        return fail("project state goals is not an array");
    }
    for (std::size_t index = 0; index < json_array_size(project_goals); ++index) {
        const auto* goal = json_array_get(project_goals, index);
        if (!json_is_string(goal) || !observed_project_goals.insert(json_string_value(goal)).second) {
            return fail("project state goals contains a non-string or duplicate");
        }
    }
    if (observed_project_goals != expected_goals) {
        return fail("project state must bind LL-G1 through LL-G5 exactly");
    }
    const std::set<std::string> allowed_open_gates = {"P0_AUTHORITY_AND_PROVENANCE",
                                                      "P1_TYPED_IO",
                                                      "P2_DETERMINISM",
                                                      "P3_M1_PARITY",
                                                      "P4_M2_COOCCURRENCE_PARITY",
                                                      "P5_TOPOLOGY_PARITY",
                                                      "P6_INDEPENDENT_VALIDATOR",
                                                      "P7_FULL_DATASET",
                                                      "P8_RELEASE"};
    const auto* open_gates = json_object_get(project.get(), "open_gates");
    std::set<std::string> observed_open_gates;
    if (!json_is_array(open_gates)) {
        return fail("project state open_gates is not an array");
    }
    for (std::size_t index = 0; index < json_array_size(open_gates); ++index) {
        const auto* gate = json_array_get(open_gates, index);
        if (!json_is_string(gate) || allowed_open_gates.count(json_string_value(gate)) == 0U ||
            !observed_open_gates.insert(json_string_value(gate)).second) {
            return fail("project state open gate is unknown or duplicated");
        }
    }
    const std::set<std::string> expected_decisions = {"PRODUCTION_TRUTH_ISOLATION", "LATEST_SIDECAR_ONLY_HP_PS",
                                                      "NO_PYTHON_SCIENCE", "NO_WINNER_FROM_INCOMPLETE_FAMILY",
                                                      "LINEAGE_COMPATIBLE_FAMILY_CLAIM_CEILING"};
    const auto* project_decisions = json_object_get(project.get(), "protected_decisions");
    std::set<std::string> observed_decisions;
    if (!json_is_array(project_decisions)) {
        return fail("project state protected_decisions is not an array");
    }
    for (std::size_t index = 0; index < json_array_size(project_decisions); ++index) {
        const auto* decision = json_array_get(project_decisions, index);
        if (!json_is_string(decision) || !observed_decisions.insert(json_string_value(decision)).second) {
            return fail("project state protected decision is malformed or duplicated");
        }
    }
    if (observed_decisions != expected_decisions) {
        return fail("project state protected decisions differ from policy");
    }
    const auto* next_actions = json_object_get(project.get(), "next_actions");
    if (!json_is_array(next_actions) || json_array_size(next_actions) == 0U) {
        return fail("project state next_actions must be non-empty");
    }
    for (std::size_t index = 0; index < json_array_size(next_actions); ++index) {
        const auto* action = json_array_get(next_actions, index);
        if (!json_is_string(action) || json_string_length(action) == 0U) {
            return fail("project state next_actions contains an empty/non-string row");
        }
    }
    if (!is_rfc3339_seconds(string_field(project.get(), "last_verified_at"))) {
        return fail("project state last_verified_at is not RFC3339 with seconds");
    }

    const std::set<std::string> ledger_keys = {"schema_name", "schema_version", "updated_at", "phases"};
    if (!object_has_exact_keys(ledger.get(), ledger_keys, error) ||
        string_field(ledger.get(), "schema_name") != "longlineage.phase_ledger" ||
        string_field(ledger.get(), "schema_version") != "1.0.0" ||
        !is_rfc3339_seconds(string_field(ledger.get(), "updated_at"))) {
        return fail("phase ledger identity/closed-world shape is invalid: " + error);
    }

    const auto* phases = json_object_get(ledger.get(), "phases");
    if (!json_is_array(phases) || json_array_size(phases) != 9U) {
        return fail("phase ledger must contain exactly P0 through P8");
    }
    const std::set<std::string> allowed_status = {"NOT_STARTED", "IN_PROGRESS", "VERIFIED", "BLOCKED", "FAILED"};
    const std::array<std::string, 9> phase_names = {
        "authority_and_provenance",
        "typed_io_and_preflight",
        "reader_threading_and_packed_writer",
        "m1_parity",
        "m2_and_cooccurrence_parity",
        "topology_parity",
        "independent_validator_export_and_query",
        "seven_dataset_full_validation",
        "presentation_and_release_candidate",
    };
    const std::set<std::string> evidence_roles = {"CONTRACT", "AUDIT", "TEST_LOG", "RECEIPT", "REPORT"};
    std::map<std::string, std::string> phase_statuses;
    for (std::size_t index = 0; index < json_array_size(phases); ++index) {
        const auto* phase = json_array_get(phases, index);
        const std::set<std::string> phase_keys = {"id", "name", "status", "predecessors", "evidence", "blockers"};
        if (!object_has_exact_keys(phase, phase_keys, error)) {
            return fail("phase row is not closed at index " + std::to_string(index) + ": " + error);
        }
        const std::string id = string_field(phase, "id");
        const std::string status = string_field(phase, "status");
        const std::string expected_id = "P" + std::to_string(index);
        if (id != expected_id || string_field(phase, "name") != phase_names[index] ||
            allowed_status.count(status) == 0U || !phase_statuses.emplace(id, status).second) {
            return fail("phase identity/order/name/status is invalid at index " + std::to_string(index));
        }
        const auto* predecessors = json_object_get(phase, "predecessors");
        const auto* evidence = json_object_get(phase, "evidence");
        const auto* blockers = json_object_get(phase, "blockers");
        if (!json_is_array(predecessors) || !json_is_array(evidence) || !json_is_array(blockers)) {
            return fail(id + " predecessors/evidence/blockers must be arrays");
        }
        if ((index == 0U && json_array_size(predecessors) != 0U) ||
            (index > 0U &&
             (json_array_size(predecessors) != 1U || !json_is_string(json_array_get(predecessors, 0U)) ||
              std::string(json_string_value(json_array_get(predecessors, 0U))) != "P" + std::to_string(index - 1U)))) {
            return fail(id + " predecessor chain is not the exact P0-P8 sequence");
        }

        for (std::size_t evidence_index = 0; evidence_index < json_array_size(evidence); ++evidence_index) {
            const auto* row = json_array_get(evidence, evidence_index);
            const std::set<std::string> evidence_keys = {"role", "path", "sha256", "command", "exit_code"};
            if (!object_has_exact_keys(row, evidence_keys, error)) {
                return fail(id + " evidence row is not closed: " + error);
            }
            const std::string role = string_field(row, "role");
            const std::string relative = string_field(row, "path");
            const auto* sha = json_object_get(row, "sha256");
            const auto* command = json_object_get(row, "command");
            const auto* exit_code = json_object_get(row, "exit_code");
            if (evidence_roles.count(role) == 0U || !path_is_safe_relative(relative)) {
                return fail(id + " evidence role/path is invalid or absent: " + relative);
            }
            if (role == "AUDIT") {
                if (relative.rfind("state/audits/", 0) != 0 || !json_is_string(sha) || !json_is_string(command) ||
                    !json_is_integer(exit_code) || json_integer_value(exit_code) != 0) {
                    return fail(id + " AUDIT evidence must bind a passing machine envelope under state/audits");
                }
                phase_audit_references.emplace_back(id, relative);
            }
            const auto evidence_path_check = longlineage::cli::require_safe_repository_file(root, relative);
            if (!evidence_path_check.ok) {
                return fail(id + " evidence path is unsafe: " + evidence_path_check.message);
            }
            if (!json_is_null(sha) && (!json_is_string(sha) || !is_lower_sha256_text(json_string_value(sha)))) {
                return fail(id + " evidence digest is neither null nor lowercase SHA-256");
            }
            if ((!json_is_null(command) && !json_is_string(command)) ||
                (json_is_string(command) && json_string_length(command) == 0U) ||
                (!json_is_null(exit_code) && (!json_is_integer(exit_code) || json_integer_value(exit_code) < 0 ||
                                              json_integer_value(exit_code) > 255)) ||
                (json_is_null(command) != json_is_null(exit_code))) {
                return fail(id + " evidence command/exit_code pair is malformed");
            }
            if (json_is_string(sha)) {
                std::string observed;
                const auto digest = longlineage::cli::sha256_file_hex(root / relative, observed);
                if (!digest.ok || observed != json_string_value(sha)) {
                    return fail(id + " evidence SHA-256 mismatch: " + relative);
                }
            }
            if (status == "VERIFIED" && (!json_is_string(sha) || !json_is_string(command) ||
                                         !json_is_integer(exit_code) || json_integer_value(exit_code) != 0)) {
                return fail(id + " VERIFIED evidence is not complete/replayable");
            }
        }

        std::set<std::string> observed_blockers;
        for (std::size_t blocker_index = 0; blocker_index < json_array_size(blockers); ++blocker_index) {
            const auto* blocker = json_array_get(blockers, blocker_index);
            if (!json_is_string(blocker) ||
                !std::regex_match(json_string_value(blocker), std::regex("^[A-Z0-9][A-Z0-9_]+$")) ||
                !observed_blockers.insert(json_string_value(blocker)).second) {
                return fail(id + " blocker is malformed or duplicated");
            }
        }
        if (status == "VERIFIED" && json_array_size(evidence) == 0U) {
            return fail(id + " cannot be VERIFIED without replayable evidence");
        }
        if (status == "VERIFIED" && json_array_size(blockers) != 0U) {
            return fail(id + " cannot be VERIFIED while carrying blockers");
        }
        if ((status == "BLOCKED" || status == "FAILED") && json_array_size(blockers) == 0U) {
            return fail(id + " cannot be " + status + " without a blocker");
        }
    }
    for (std::size_t index = 1U; index < phase_names.size(); ++index) {
        const std::string id = "P" + std::to_string(index);
        const std::string predecessor = "P" + std::to_string(index - 1U);
        if (phase_statuses.at(id) == "VERIFIED" && phase_statuses.at(predecessor) != "VERIFIED") {
            return fail(id + " cannot be VERIFIED before " + predecessor);
        }
    }
    const std::array<std::string, 9> phase_gate_ids = {
        "P0_AUTHORITY_AND_PROVENANCE",
        "P1_TYPED_IO",
        "P2_DETERMINISM",
        "P3_M1_PARITY",
        "P4_M2_COOCCURRENCE_PARITY",
        "P5_TOPOLOGY_PARITY",
        "P6_INDEPENDENT_VALIDATOR",
        "P7_FULL_DATASET",
        "P8_RELEASE",
    };
    std::set<std::string> derived_open_gates;
    for (std::size_t index = 0; index < phase_gate_ids.size(); ++index) {
        if (phase_statuses.at("P" + std::to_string(index)) != "VERIFIED") {
            derived_open_gates.insert(phase_gate_ids[index]);
        }
    }
    if (observed_open_gates != derived_open_gates) {
        return fail("project state open_gates must exactly equal every non-VERIFIED phase gate");
    }
    for (const auto& mirror_path : {root / "ROADMAP.md", root / "docs" / "CURRENT_FOCUS.md"}) {
        std::map<std::string, std::string> mirror;
        const auto parsed = load_phase_status_mirror(mirror_path, mirror);
        if (!parsed.ok) {
            return parsed;
        }
        if (mirror != phase_statuses) {
            return fail(mirror_path.string() + ": phase statuses drift from ledger");
        }
    }

    const auto task_schema = longlineage::cli::load_json_strict(root / "governance" / "agent_task.schema.json", error);
    if (!task_schema) {
        return fail(error);
    }
    const auto* required_task_fields = json_object_get(task_schema.get(), "required");
    const auto* task_properties = json_object_get(task_schema.get(), "properties");
    if (!json_is_array(required_task_fields) || !json_is_object(task_properties)) {
        return fail("agent task schema lacks required/properties");
    }

    std::size_t active_tasks = 0;
    std::set<std::string> active_task_ids;
    std::set<std::string> active_owner_ids;
    std::vector<ActiveWriteClaim> active_write_claims;
    std::map<std::string, TaskRelation> task_relations;
    constexpr std::int64_t maximum_lease_seconds = 86400;
    constexpr std::int64_t heartbeat_max_age_seconds = 3600;
    constexpr std::int64_t future_clock_skew_seconds = 300;
    const auto now_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto active_root = root / "state" / "tasks" / "active";
    if (!std::filesystem::is_directory(active_root)) {
        return fail("state/tasks/active directory is absent");
    }
    for (const auto& entry : std::filesystem::directory_iterator(active_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto task = longlineage::cli::load_json_strict(entry.path(), error);
        if (!task || !json_is_object(task.get())) {
            return fail(task ? entry.path().string() + ": expected object" : error);
        }
        for (std::size_t index = 0; index < json_array_size(required_task_fields); ++index) {
            const auto* required = json_array_get(required_task_fields, index);
            if (!json_is_string(required) || json_object_get(task.get(), json_string_value(required)) == nullptr) {
                return fail(entry.path().string() + ": required task field is absent");
            }
        }
        const char* key = nullptr;
        json_t* value = nullptr;
        json_object_foreach(task.get(), key, value) {
            static_cast<void>(value);
            if (json_object_get(task_properties, key) == nullptr) {
                return fail(entry.path().string() + ": unknown task field: " + key);
            }
        }
        if (string_field(task.get(), "schema_name") != "longlineage.agent_task" ||
            string_field(task.get(), "schema_version") != "1.0.0") {
            return fail(entry.path().string() + ": agent task identity is invalid");
        }
        const std::string task_id = string_field(task.get(), "task_id");
        if (!std::regex_match(task_id, std::regex("^[0-9]{8}-[a-z0-9][a-z0-9-]*$")) || entry.path().stem() != task_id) {
            return fail(entry.path().string() + ": task_id is malformed or differs from filename");
        }
        const std::set<std::string> short_task_types = {"A", "B", "C", "D", "E", "F"};
        const std::string short_type = string_field(task.get(), "task_type");
        if (short_task_types.count(short_type) == 0U) {
            return fail(entry.path().string() + ": invalid task_type");
        }
        const auto task_status = string_field(task.get(), "status");
        const std::set<std::string> task_statuses = {"PLANNED", "IN_PROGRESS", "BLOCKED", "FAILED", "VERIFIED"};
        if (task_statuses.count(task_status) == 0U) {
            return fail(entry.path().string() + ": invalid task status");
        }
        if (task_status == "VERIFIED") {
            return fail(entry.path().string() + ": VERIFIED task belongs in state/tasks/archive");
        }
        if (!active_task_ids.insert(task_id).second) {
            return fail(entry.path().string() + ": duplicate active task_id");
        }

        const auto* owner = json_object_get(task.get(), "owner_agent_id");
        const auto* parent = json_object_get(task.get(), "parent_task_id");
        const auto* allowed_paths = json_object_get(task.get(), "allowed_paths");
        const auto* depends_on = json_object_get(task.get(), "depends_on");
        const auto* lease_expires = json_object_get(task.get(), "lease_expires_at");
        const auto* heartbeat = json_object_get(task.get(), "heartbeat_at");
        const std::string lease_state = string_field(task.get(), "lease_state");
        if ((!json_is_null(parent) &&
             (!json_is_string(parent) ||
              !std::regex_match(json_string_value(parent), std::regex("^[0-9]{8}-[a-z0-9][a-z0-9-]*$")))) ||
            (json_is_string(parent) && task_id == json_string_value(parent))) {
            return fail(entry.path().string() + ": parent_task_id is malformed or self-referential");
        }
        if (!json_is_array(depends_on)) {
            return fail(entry.path().string() + ": depends_on must be an array");
        }
        std::set<std::string> dependency_ids;
        for (std::size_t dependency_index = 0; dependency_index < json_array_size(depends_on); ++dependency_index) {
            const auto* dependency = json_array_get(depends_on, dependency_index);
            if (!json_is_string(dependency) ||
                !std::regex_match(json_string_value(dependency), std::regex("^[0-9]{8}-[a-z0-9][a-z0-9-]*$")) ||
                task_id == json_string_value(dependency) ||
                !dependency_ids.insert(json_string_value(dependency)).second) {
                return fail(entry.path().string() + ": depends_on contains a malformed, duplicate or self task");
            }
        }
        TaskRelation relation;
        relation.status = task_status;
        if (json_is_string(parent)) {
            relation.parent_task_id = std::string(json_string_value(parent));
        }
        relation.depends_on = dependency_ids;
        if (!task_relations.emplace(task_id, std::move(relation)).second) {
            return fail(entry.path().string() + ": duplicate task_id across task registry");
        }
        if (!json_is_array(allowed_paths)) {
            return fail(entry.path().string() + ": allowed_paths must be an array");
        }
        std::set<std::string> task_claim_paths;
        for (std::size_t claim_index = 0; claim_index < json_array_size(allowed_paths); ++claim_index) {
            const auto* claim = json_array_get(allowed_paths, claim_index);
            const std::set<std::string> claim_keys = {"path", "kind"};
            if (!object_has_exact_keys(claim, claim_keys, error)) {
                return fail(entry.path().string() + ": malformed allowed_paths row: " + error);
            }
            const std::string claimed_path = string_field(claim, "path");
            const std::string claim_kind = string_field(claim, "kind");
            std::filesystem::path normalized_claim;
            error.clear();
            if (!validate_write_claim_path(root, claimed_path, claim_kind, normalized_claim, error) ||
                !task_claim_paths.insert(normalized_claim.generic_string()).second) {
                return fail(entry.path().string() + ": unsafe, duplicate or malformed allowed path: " + claimed_path +
                            (error.empty() ? std::string{} : " (" + error + ")"));
            }
        }
        const auto owner_is_valid =
            json_is_string(owner) &&
            std::regex_match(json_string_value(owner),
                             std::regex("^[a-z][a-z0-9_-]{1,31}:[A-Za-z0-9][A-Za-z0-9._/-]{0,126}$"));
        if (task_status == "PLANNED") {
            if (!json_is_null(owner) || lease_state != "UNASSIGNED" || !json_is_null(lease_expires) ||
                !json_is_null(heartbeat)) {
                return fail(entry.path().string() + ": PLANNED task has an invalid unassigned lease");
            }
        } else {
            if (!owner_is_valid || !json_is_string(lease_expires) || !json_is_string(heartbeat)) {
                return fail(entry.path().string() + ": assigned task owner or lease timestamps are malformed");
            }
            const auto lease_epoch = parse_rfc3339_epoch_seconds(json_string_value(lease_expires));
            const auto heartbeat_epoch = parse_rfc3339_epoch_seconds(json_string_value(heartbeat));
            if (!lease_epoch.has_value() || !heartbeat_epoch.has_value() || *heartbeat_epoch > *lease_epoch) {
                return fail(entry.path().string() + ": task lease is malformed or precedes heartbeat");
            }
            const auto updated_epoch = parse_rfc3339_epoch_seconds(string_field(task.get(), "updated_at"));
            if (!updated_epoch.has_value() || *updated_epoch < *heartbeat_epoch ||
                *heartbeat_epoch > now_seconds + future_clock_skew_seconds ||
                *lease_epoch - *heartbeat_epoch > maximum_lease_seconds) {
                return fail(entry.path().string() + ": task heartbeat/update/lease window violates policy");
            }
            if (task_status == "IN_PROGRESS") {
                if (lease_state != "ACTIVE" || *lease_epoch <= now_seconds || json_array_size(allowed_paths) == 0U ||
                    now_seconds - *heartbeat_epoch > heartbeat_max_age_seconds ||
                    !active_owner_ids.insert(json_string_value(owner)).second) {
                    return fail(entry.path().string() +
                                ": IN_PROGRESS task needs a recent heartbeat, future unique ACTIVE lease and "
                                "non-empty write set");
                }
                for (std::size_t claim_index = 0; claim_index < json_array_size(allowed_paths); ++claim_index) {
                    const auto* claim = json_array_get(allowed_paths, claim_index);
                    active_write_claims.push_back(
                        {task_id, json_string_value(owner),
                         std::filesystem::path(string_field(claim, "path")).lexically_normal(),
                         string_field(claim, "kind") == "DIRECTORY_TREE"});
                }
            } else if ((lease_state != "RELEASED" && lease_state != "EXPIRED") ||
                       (lease_state == "EXPIRED" && *lease_epoch > now_seconds)) {
                return fail(entry.path().string() +
                            ": terminal/blocked task must release its lease or carry a past expiry");
            }
        }
        const auto* goals = json_object_get(task.get(), "goals");
        std::set<std::string> observed_goals;
        const std::set<std::string> allowed_goals = {"LL-G1", "LL-G2", "LL-G3", "LL-G4", "LL-G5"};
        if (!json_is_array(goals) || json_array_size(goals) == 0U) {
            return fail(entry.path().string() + ": goals must be a non-empty array");
        }
        for (std::size_t index = 0; index < json_array_size(goals); ++index) {
            const auto* goal = json_array_get(goals, index);
            if (!json_is_string(goal) || !observed_goals.insert(json_string_value(goal)).second ||
                allowed_goals.count(json_string_value(goal)) == 0U) {
                return fail(entry.path().string() + ": goal is invalid or duplicated");
            }
        }

        const auto* task_scope = json_object_get(task.get(), "scope");
        const std::set<std::string> task_scope_keys = {"completeness", "description"};
        if (!object_has_exact_keys(task_scope, task_scope_keys, error) ||
            (string_field(task_scope, "completeness") != "PARTIAL" &&
             string_field(task_scope, "completeness") != "FULL") ||
            string_field(task_scope, "description").empty()) {
            return fail(entry.path().string() + ": invalid closed-world task scope");
        }
        if ((short_type == "B" || short_type == "C" || short_type == "D") &&
            string_field(task_scope, "completeness") != "FULL") {
            return fail(entry.path().string() + ": B/C/D task must declare FULL scope");
        }

        for (const auto& array_name :
             {"assumptions", "inputs", "expected_outputs", "step_verify", "evidence", "deviations", "blockers"}) {
            if (!json_is_array(json_object_get(task.get(), array_name))) {
                return fail(entry.path().string() + ": task field must be an array: " + array_name);
            }
        }
        const auto* step_verify = json_object_get(task.get(), "step_verify");
        if (json_array_size(step_verify) == 0U) {
            return fail(entry.path().string() + ": Step→Verify array is empty");
        }
        for (std::size_t index = 0; index < json_array_size(step_verify); ++index) {
            const auto* item = json_array_get(step_verify, index);
            const std::set<std::string> step_keys = {"step", "verify"};
            if (!object_has_exact_keys(item, step_keys, error) || string_field(item, "step").empty() ||
                string_field(item, "verify").empty()) {
                return fail(entry.path().string() + ": malformed Step→Verify row");
            }
        }

        for (const auto& array_name : {"inputs", "expected_outputs"}) {
            const auto* references = json_object_get(task.get(), array_name);
            for (std::size_t index = 0; index < json_array_size(references); ++index) {
                const auto* reference = json_array_get(references, index);
                const std::set<std::string> reference_keys = {"role", "path", "sha256"};
                if (!object_has_exact_keys(reference, reference_keys, error)) {
                    return fail(entry.path().string() + ": malformed file reference");
                }
                const std::string relative = string_field(reference, "path");
                const std::filesystem::path relative_path(relative);
                const auto* digest = json_object_get(reference, "sha256");
                if (string_field(reference, "role").empty() || relative.empty() || relative_path.is_absolute() ||
                    relative.find("..") != std::string::npos ||
                    (!json_is_null(digest) && (!json_is_string(digest) || json_string_length(digest) != 64U))) {
                    return fail(entry.path().string() + ": unsafe path or malformed digest in file reference");
                }
                if (json_is_string(digest) &&
                    !std::regex_match(json_string_value(digest), std::regex("^[0-9a-f]{64}$"))) {
                    return fail(entry.path().string() + ": file reference digest is not lowercase SHA-256");
                }
            }
        }

        const auto* task_evidence = json_object_get(task.get(), "evidence");
        const auto* task_blockers = json_object_get(task.get(), "blockers");
        std::set<std::string> task_evidence_ids;
        const std::set<std::string> task_evidence_kinds = {"TEST_LOG", "AUDIT", "RECEIPT", "REPORT"};
        for (std::size_t index = 0; index < json_array_size(task_evidence); ++index) {
            const auto* evidence = json_array_get(task_evidence, index);
            const std::set<std::string> evidence_keys = {"evidence_id",     "evidence_path", "evidence_kind",
                                                         "command",         "exit_code",     "output_excerpt",
                                                         "evidence_sha256", "recorded_at"};
            const auto* exit_code = json_object_get(evidence, "exit_code");
            const auto* digest = json_object_get(evidence, "evidence_sha256");
            const std::string evidence_id = string_field(evidence, "evidence_id");
            const std::string evidence_path = string_field(evidence, "evidence_path");
            if (!object_has_exact_keys(evidence, evidence_keys, error) ||
                !std::regex_match(evidence_id, std::regex("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")) ||
                !task_evidence_ids.insert(evidence_id).second || !path_is_safe_relative(evidence_path) ||
                task_evidence_kinds.count(string_field(evidence, "evidence_kind")) == 0U ||
                string_field(evidence, "command").empty() || !json_is_integer(exit_code) ||
                json_integer_value(exit_code) < 0 || json_integer_value(exit_code) > 255 ||
                !json_is_string(json_object_get(evidence, "output_excerpt")) || !json_is_string(digest) ||
                json_string_length(digest) != 64U || !is_rfc3339_seconds(string_field(evidence, "recorded_at"))) {
                return fail(entry.path().string() + ": malformed replay evidence row");
            }
            if (string_field(evidence, "evidence_kind") == "AUDIT") {
                if (evidence_path.rfind("state/audits/", 0) != 0) {
                    return fail(entry.path().string() + ": AUDIT task evidence must live under state/audits");
                }
                task_audit_references.emplace_back(task_id, evidence_path);
            }
            const auto evidence_path_check = longlineage::cli::require_safe_repository_file(root, evidence_path);
            if (!evidence_path_check.ok) {
                return fail(entry.path().string() + ": evidence path is unsafe: " + evidence_path_check.message);
            }
            if (!std::regex_match(json_string_value(digest), std::regex("^[0-9a-f]{64}$"))) {
                return fail(entry.path().string() + ": evidence digest is not lowercase SHA-256");
            }
            std::string observed;
            const auto digest_check = longlineage::cli::sha256_file_hex(root / evidence_path, observed);
            if (!digest_check.ok || observed != json_string_value(digest)) {
                return fail(entry.path().string() + ": evidence file SHA-256 mismatch: " + evidence_path);
            }
            if (task_status == "VERIFIED" && json_integer_value(exit_code) != 0) {
                return fail(entry.path().string() + ": VERIFIED task evidence exit_code is nonzero");
            }
        }
        if (task_status == "VERIFIED" && (!json_is_array(task_evidence) || json_array_size(task_evidence) == 0U)) {
            return fail(entry.path().string() + ": VERIFIED task lacks evidence");
        }
        if ((task_status == "BLOCKED" || task_status == "FAILED") &&
            (!json_is_array(task_blockers) || json_array_size(task_blockers) == 0U)) {
            return fail(entry.path().string() + ": blocked/failed task lacks blockers");
        }
        if (!is_rfc3339_seconds(string_field(task.get(), "updated_at"))) {
            return fail(entry.path().string() + ": updated_at is not RFC3339 with seconds");
        }
        ++active_tasks;
    }

    const auto archive_root = root / "state" / "tasks" / "archive";
    if (!std::filesystem::is_directory(archive_root)) {
        return fail("state/tasks/archive directory is absent");
    }
    for (const auto& entry : std::filesystem::directory_iterator(archive_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto task = longlineage::cli::load_json_strict(entry.path(), error);
        if (!task || !json_is_object(task.get())) {
            return fail(task ? entry.path().string() + ": expected object" : error);
        }
        for (std::size_t index = 0; index < json_array_size(required_task_fields); ++index) {
            const auto* required = json_array_get(required_task_fields, index);
            if (!json_is_string(required) || json_object_get(task.get(), json_string_value(required)) == nullptr) {
                return fail(entry.path().string() + ": required archived task field is absent");
            }
        }
        const char* key = nullptr;
        json_t* value = nullptr;
        json_object_foreach(task.get(), key, value) {
            static_cast<void>(value);
            if (json_object_get(task_properties, key) == nullptr) {
                return fail(entry.path().string() + ": unknown archived task field: " + key);
            }
        }
        const std::string task_id = string_field(task.get(), "task_id");
        if (string_field(task.get(), "schema_name") != "longlineage.agent_task" ||
            string_field(task.get(), "schema_version") != "1.0.0" ||
            !std::regex_match(task_id, std::regex("^[0-9]{8}-[a-z0-9][a-z0-9-]*$")) || entry.path().stem() != task_id ||
            string_field(task.get(), "status") != "VERIFIED" ||
            (string_field(task.get(), "lease_state") != "RELEASED" &&
             string_field(task.get(), "lease_state") != "EXPIRED")) {
            return fail(entry.path().string() + ": archived task identity/status/lease is invalid");
        }
        const auto* parent = json_object_get(task.get(), "parent_task_id");
        const auto* dependencies = json_object_get(task.get(), "depends_on");
        TaskRelation relation;
        relation.status = "VERIFIED";
        relation.archived = true;
        if (json_is_string(parent)) {
            relation.parent_task_id = std::string(json_string_value(parent));
        } else if (!json_is_null(parent)) {
            return fail(entry.path().string() + ": archived parent_task_id is malformed");
        }
        if (!json_is_array(dependencies)) {
            return fail(entry.path().string() + ": archived depends_on is not an array");
        }
        for (std::size_t index = 0; index < json_array_size(dependencies); ++index) {
            const auto* dependency = json_array_get(dependencies, index);
            if (!json_is_string(dependency) || task_id == json_string_value(dependency) ||
                !relation.depends_on.insert(json_string_value(dependency)).second) {
                return fail(entry.path().string() + ": archived dependency is malformed, duplicated or self");
            }
        }
        const auto* evidence = json_object_get(task.get(), "evidence");
        if (!json_is_array(evidence) || json_array_size(evidence) == 0U) {
            return fail(entry.path().string() + ": archived VERIFIED task lacks evidence");
        }
        for (std::size_t index = 0; index < json_array_size(evidence); ++index) {
            const auto* row = json_array_get(evidence, index);
            const std::string evidence_path = string_field(row, "evidence_path");
            const std::string declared_sha = string_field(row, "evidence_sha256");
            if (!path_is_safe_relative(evidence_path) || !is_lower_sha256_text(declared_sha) ||
                !json_is_integer(json_object_get(row, "exit_code")) ||
                json_integer_value(json_object_get(row, "exit_code")) != 0) {
                return fail(entry.path().string() + ": archived task evidence is malformed or non-passing");
            }
            std::string observed_sha;
            const auto digest = longlineage::cli::sha256_file_hex(root / evidence_path, observed_sha);
            if (!digest.ok || observed_sha != declared_sha) {
                return fail(entry.path().string() + ": archived task evidence digest mismatch");
            }
        }
        if (!task_relations.emplace(task_id, std::move(relation)).second) {
            return fail(entry.path().string() + ": duplicate task_id across active/archive registry");
        }
    }

    if (active_tasks == 0U) {
        return fail("no machine-readable active AI task record exists");
    }
    for (const auto& [task_id, relation] : task_relations) {
        if (relation.parent_task_id.has_value() && task_relations.count(*relation.parent_task_id) == 0U) {
            return fail("task parent does not exist: " + task_id + " -> " + *relation.parent_task_id);
        }
        for (const auto& dependency : relation.depends_on) {
            const auto dependency_row = task_relations.find(dependency);
            if (dependency_row == task_relations.end()) {
                return fail("task dependency does not exist: " + task_id + " -> " + dependency);
            }
            if (relation.status == "IN_PROGRESS" && dependency_row->second.status != "VERIFIED") {
                return fail("IN_PROGRESS task dependency is not VERIFIED: " + task_id + " -> " + dependency);
            }
        }
    }
    const auto graph_is_acyclic = [&](bool include_parent, bool include_dependencies) {
        std::map<std::string, int> color;
        std::function<bool(const std::string&)> visit = [&](const std::string& task_id) {
            const int state = color[task_id];
            if (state == 1) {
                return false;
            }
            if (state == 2) {
                return true;
            }
            color[task_id] = 1;
            const auto& relation = task_relations.at(task_id);
            if (include_parent && relation.parent_task_id.has_value() && !visit(*relation.parent_task_id)) {
                return false;
            }
            if (include_dependencies) {
                for (const auto& dependency : relation.depends_on) {
                    if (!visit(dependency)) {
                        return false;
                    }
                }
            }
            color[task_id] = 2;
            return true;
        };
        for (const auto& [task_id, relation] : task_relations) {
            static_cast<void>(relation);
            if (!visit(task_id)) {
                return false;
            }
        }
        return true;
    };
    if (!graph_is_acyclic(true, false) || !graph_is_acyclic(false, true) || !graph_is_acyclic(true, true)) {
        return fail("task parent/dependency registry contains a cycle");
    }

    const auto audit_root = root / "state" / "audits";
    if (!std::filesystem::is_directory(audit_root)) {
        return fail("state/audits directory is absent");
    }
    std::map<std::string, AuditEnvelopeInfo> audit_envelopes;
    std::map<std::string, std::string> audit_paths;
    for (const auto& entry : std::filesystem::directory_iterator(audit_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        AuditEnvelopeInfo info;
        const auto loaded = load_audit_envelope(entry.path(), info);
        if (!loaded.ok) {
            return loaded;
        }
        if (task_relations.count(info.task_id) == 0U ||
            !audit_paths.emplace(std::filesystem::relative(entry.path(), root).generic_string(), info.snapshot_id)
                 .second ||
            !audit_envelopes.emplace(info.snapshot_id, std::move(info)).second) {
            return fail(entry.path().string() + ": audit task is unknown or audit path/snapshot ID is duplicated");
        }
    }
    std::map<std::string, std::set<std::string>> superseded_by_derived;
    for (const auto& [snapshot_id, info] : audit_envelopes) {
        for (const auto& older : info.supersedes) {
            const auto old = audit_envelopes.find(older);
            if (old == audit_envelopes.end() || old->second.task_id != info.task_id ||
                old->second.scope_key != info.scope_key) {
                return fail("audit supersedes edge is unresolved or crosses task/scope: " + snapshot_id + " -> " +
                            older);
            }
            superseded_by_derived[older].insert(snapshot_id);
        }
    }
    std::map<std::string, int> audit_color;
    std::function<bool(const std::string&)> visit_audit = [&](const std::string& snapshot_id) {
        const int state = audit_color[snapshot_id];
        if (state == 1) {
            return false;
        }
        if (state == 2) {
            return true;
        }
        audit_color[snapshot_id] = 1;
        for (const auto& older : audit_envelopes.at(snapshot_id).supersedes) {
            if (!visit_audit(older)) {
                return false;
            }
        }
        audit_color[snapshot_id] = 2;
        return true;
    };
    for (const auto& [snapshot_id, info] : audit_envelopes) {
        static_cast<void>(info);
        if (!visit_audit(snapshot_id)) {
            return fail("audit supersession graph contains a cycle");
        }
    }
    std::map<std::string, std::size_t> audit_tips;
    for (const auto& [snapshot_id, info] : audit_envelopes) {
        const auto reverse = superseded_by_derived.find(snapshot_id);
        if (info.superseded_by.has_value() &&
            (reverse == superseded_by_derived.end() || reverse->second.count(*info.superseded_by) == 0U)) {
            return fail("audit superseded_by hint disagrees with derived forward edges: " + snapshot_id);
        }
        if (reverse == superseded_by_derived.end() || reverse->second.empty()) {
            ++audit_tips[info.task_id + "\n" + info.scope_key];
        }
    }
    for (const auto& [audit_scope, tips] : audit_tips) {
        static_cast<void>(audit_scope);
        if (tips != 1U) {
            return fail("audit task/scope must have exactly one current non-superseded tip");
        }
    }
    std::set<std::string> phases_with_audits;
    for (const auto& [phase_id, relative] : phase_audit_references) {
        const auto path = audit_paths.find(relative);
        if (path == audit_paths.end() || !audit_envelopes.at(path->second).all_commands_pass) {
            return fail(phase_id + " AUDIT reference does not resolve to an all-passing envelope");
        }
        phases_with_audits.insert(phase_id);
    }
    for (const auto& [task_id, relative] : task_audit_references) {
        const auto path = audit_paths.find(relative);
        if (path == audit_paths.end() || audit_envelopes.at(path->second).task_id != task_id ||
            !audit_envelopes.at(path->second).all_commands_pass) {
            return fail(task_id + " AUDIT evidence does not resolve to its own all-passing envelope");
        }
    }
    for (const auto& [phase_id, status] : phase_statuses) {
        if (status == "VERIFIED" && phases_with_audits.count(phase_id) == 0U) {
            return fail(phase_id + " VERIFIED phase lacks a bound machine AUDIT envelope");
        }
    }

    for (std::size_t left = 0; left < active_write_claims.size(); ++left) {
        for (std::size_t right = left + 1U; right < active_write_claims.size(); ++right) {
            const auto& lhs = active_write_claims[left];
            const auto& rhs = active_write_claims[right];
            const bool overlaps = lhs.path == rhs.path || path_is_ancestor_or_equal(lhs.path, rhs.path) ||
                                  path_is_ancestor_or_equal(rhs.path, lhs.path);
            if (overlaps) {
                return fail("active task write-set overlap: " + lhs.task_id + " (" + lhs.path.string() + ") and " +
                            rhs.task_id + " (" + rhs.path.string() + ")");
            }
        }
    }
    return pass("project state, P0-P8 ledger and " + std::to_string(active_tasks) +
                " active AI task record(s), owner leases and non-overlapping write sets are internally consistent");
}

bool path_is_safe_relative(const std::string& value) {
    const std::filesystem::path path(value);
    return !value.empty() && !path.is_absolute() && value.find("..") == std::string::npos;
}

CheckResult check_tabular_record_schema(const json_t* schema, const std::string& path) {
    const auto* header = json_object_get(schema, "header");
    const auto* fields = json_object_get(schema, "fields");
    if (header == nullptr) {
        return pass("non-tabular schema");
    }
    if (!json_is_array(header) || !json_is_array(fields) || json_array_size(header) != json_array_size(fields)) {
        return fail(path + ": header and fields must be equal-length arrays");
    }
    std::set<std::string> names;
    for (std::size_t index = 0; index < json_array_size(header); ++index) {
        const auto* header_value = json_array_get(header, index);
        const auto* field = json_array_get(fields, index);
        const auto* field_name = json_object_get(field, "name");
        if (!json_is_string(header_value) || !json_is_string(field_name) ||
            std::string(json_string_value(header_value)) != json_string_value(field_name)) {
            return fail(path + ": header order differs from field definitions");
        }
        if (!names.insert(json_string_value(header_value)).second) {
            return fail(path + ": duplicate tabular field name");
        }
    }
    return pass("tabular schema header/field order passed");
}

bool valid_inline_enum_type(const std::string& type) {
    if (type.rfind("enum:", 0) != 0 || type.size() <= 5U) {
        return false;
    }
    std::set<std::string> values;
    std::size_t begin = 5U;
    while (true) {
        const auto end = type.find('|', begin);
        const std::string value = type.substr(begin, end - begin);
        if (value.empty() || !values.insert(value).second) {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return true;
}

CheckResult check_tabular_field_contract(const json_t* schema, const std::string& path,
                                         const std::set<std::string>& registered_types) {
    const auto* fields = json_object_get(schema, "fields");
    if (!json_is_array(fields)) {
        return pass("non-tabular field contract");
    }
    const std::set<std::string> allowed_field_keys = {"name", "type", "required", "null", "const", "unit"};
    for (std::size_t index = 0; index < json_array_size(fields); ++index) {
        const auto* field = json_array_get(fields, index);
        if (!json_is_object(field)) {
            return fail(path + ": field row is not an object");
        }
        const char* key = nullptr;
        json_t* value = nullptr;
        json_object_foreach(const_cast<json_t*>(field), key, value) {
            static_cast<void>(value);
            if (allowed_field_keys.count(key) == 0U) {
                return fail(path + ": unknown field-contract key: " + key);
            }
        }
        const std::string name = string_field(field, "name");
        const std::string type = string_field(field, "type");
        const auto* required = json_object_get(field, "required");
        const auto* null_value = json_object_get(field, "null");
        const auto* constant = json_object_get(field, "const");
        const auto* unit = json_object_get(field, "unit");
        if (name.empty() || type.empty() || !json_is_boolean(required) ||
            (registered_types.count(type) == 0U && !valid_inline_enum_type(type))) {
            return fail(path + ": field name/type/required is invalid: " + name);
        }
        if (json_is_true(required)) {
            if (null_value != nullptr) {
                return fail(path + ": required field declares a null token: " + name);
            }
        } else if (!json_is_string(null_value) || std::string(json_string_value(null_value)) != ".") {
            return fail(path + ": optional field must declare null token '.': " + name);
        }
        if (constant != nullptr &&
            (!json_is_true(required) || (!json_is_string(constant) && !json_is_integer(constant) &&
                                         !json_is_real(constant) && !json_is_boolean(constant)))) {
            return fail(path + ": const is not a required scalar field: " + name);
        }
        if (unit != nullptr && (!json_is_string(unit) || json_string_length(unit) == 0U)) {
            return fail(path + ": unit is not a non-empty string: " + name);
        }
    }
    return pass("tabular fields use registered types and closed null/const/unit rules");
}

bool schema_declares_dot_path(const json_t* schema, const json_t* document_root, const std::vector<std::string>& parts,
                              std::size_t part_index, const std::map<std::string, const json_t*>& schemas_by_id) {
    const auto* reference = json_object_get(schema, "$ref");
    if (json_is_string(reference)) {
        const std::string id = json_string_value(reference);
        if (id.empty() || id.front() == '#') {
            return false;
        }
        const auto fragment = id.find('#');
        if (fragment != std::string::npos) {
            return false;
        }
        const auto target = schemas_by_id.find(id);
        return target != schemas_by_id.end() &&
               schema_declares_dot_path(target->second, target->second, parts, part_index, schemas_by_id);
    }
    if (part_index == parts.size()) {
        return true;
    }
    const auto* properties = json_object_get(schema, "properties");
    if (!json_is_object(properties)) {
        static_cast<void>(document_root);
        return false;
    }
    const auto* child = json_object_get(properties, parts[part_index].c_str());
    return child != nullptr && schema_declares_dot_path(child, document_root, parts, part_index + 1U, schemas_by_id);
}

std::vector<std::string> split_dot_path(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const auto end = value.find('.', begin);
        const std::string part = value.substr(begin, end - begin);
        if (part.empty()) {
            return {};
        }
        parts.push_back(part);
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return parts;
}

CheckResult check_catalog_key_array(const json_t* keys, const std::string& label, const std::string& format,
                                    const json_t* schema, const std::string& schema_path,
                                    const std::map<std::string, const json_t*>& schemas_by_id) {
    if (!json_is_array(keys)) {
        return fail(label + " is not an array");
    }
    std::set<std::string> observed;
    std::set<std::string> tabular_fields;
    const auto* header = json_object_get(schema, "header");
    if (json_is_array(header)) {
        for (std::size_t index = 0; index < json_array_size(header); ++index) {
            const auto* value = json_array_get(header, index);
            if (json_is_string(value)) {
                tabular_fields.insert(json_string_value(value));
            }
        }
    }
    if (tabular_fields.empty()) {
        const auto* fields = json_object_get(schema, "fields");
        if (json_is_array(fields) && (format == "SHA256SUM" || format == "TSV" || format == "TSV_BGZF")) {
            for (std::size_t index = 0; index < json_array_size(fields); ++index) {
                const auto* field = json_array_get(fields, index);
                const std::string name = string_field(field, "name");
                if (!name.empty()) {
                    tabular_fields.insert(name);
                }
            }
        }
    }
    for (std::size_t index = 0; index < json_array_size(keys); ++index) {
        const auto* value = json_array_get(keys, index);
        if (!json_is_string(value) || !observed.insert(json_string_value(value)).second) {
            return fail(label + " contains a non-string or duplicate");
        }
        const std::string key = json_string_value(value);
        if (!tabular_fields.empty()) {
            if (tabular_fields.count(key) == 0U) {
                return fail(label + " names an unknown tabular field: " + key);
            }
        } else if (format == "LLM_BGZF") {
            if (key != "dataset_order" && key != "site_order") {
                return fail(label + " names an unknown LLM frame key: " + key);
            }
        } else {
            const auto parts = split_dot_path(key);
            if (parts.empty() || !schema_declares_dot_path(schema, schema, parts, 0U, schemas_by_id)) {
                return fail(label + " names an unknown JSON dot path in " + schema_path + ": " + key);
            }
        }
    }
    return pass(label + " resolves to declared schema fields");
}

CheckResult check_status_domain_bindings(const json_t* schema, const std::string& path,
                                         const std::set<std::string>& status_domains) {
    const auto* fields = json_object_get(schema, "fields");
    if (!json_is_array(fields)) {
        return pass("non-tabular status binding");
    }
    const std::set<std::string> domain_bound_types = {"status_code", "reason_code", "axis_status"};
    std::map<std::string, std::string> bound_fields;
    for (std::size_t index = 0; index < json_array_size(fields); ++index) {
        const auto* field = json_array_get(fields, index);
        const std::string type = string_field(field, "type");
        if (domain_bound_types.count(type) != 0U) {
            bound_fields.emplace(string_field(field, "name"), type);
        }
    }

    const auto* bindings = json_object_get(schema, "status_domain_bindings");
    if (bound_fields.empty()) {
        if (bindings != nullptr && (!json_is_object(bindings) || json_object_size(bindings) != 0U)) {
            return fail(path + ": status_domain_bindings exists without typed fields");
        }
        return pass("no status-bound tabular fields");
    }
    if (!json_is_object(bindings) || json_object_size(bindings) != bound_fields.size()) {
        return fail(path + ": every status/reason/axis field needs exactly one domain binding");
    }
    const char* field_name = nullptr;
    json_t* domain_value = nullptr;
    json_object_foreach(const_cast<json_t*>(bindings), field_name, domain_value) {
        if (bound_fields.count(field_name) == 0U || !json_is_string(domain_value)) {
            return fail(path + ": binding names a missing or wrongly typed field: " + field_name);
        }
        const std::string domain = json_string_value(domain_value);
        if (status_domains.count(domain) == 0U) {
            return fail(path + ": binding names an unknown status registry domain: " + domain);
        }
    }
    return pass("status-domain bindings passed");
}

void collect_external_schema_refs(const json_t* value, std::vector<std::string>& references) {
    if (json_is_object(value)) {
        const char* key = nullptr;
        json_t* child = nullptr;
        json_object_foreach(const_cast<json_t*>(value), key, child) {
            if (std::string(key) == "$ref" && json_is_string(child)) {
                const std::string reference = json_string_value(child);
                if (!reference.empty() && reference.front() != '#') {
                    references.push_back(reference);
                }
            }
            collect_external_schema_refs(child, references);
        }
    } else if (json_is_array(value)) {
        for (std::size_t index = 0; index < json_array_size(value); ++index) {
            collect_external_schema_refs(json_array_get(value, index), references);
        }
    }
}

CheckResult verify_declared_file_sha256(const std::filesystem::path& root, const std::string& relative,
                                        const json_t* declared, const std::string& context) {
    if (!path_is_safe_relative(relative) || !json_is_string(declared) ||
        !is_lower_sha256_text(json_string_value(declared))) {
        return fail(context + ": path/digest declaration is invalid: " + relative);
    }
    const auto path_check = longlineage::cli::require_safe_repository_file(root, relative);
    if (!path_check.ok) {
        return fail(context + ": " + path_check.message);
    }
    std::string observed;
    const auto digest = longlineage::cli::sha256_file_hex(root / relative, observed);
    if (!digest.ok) {
        return fail(context + ": " + digest.message);
    }
    if (observed != json_string_value(declared)) {
        return fail(context + ": SHA-256 mismatch: " + relative);
    }
    return pass(context + ": SHA-256 passed");
}

CheckResult load_unique_string_set(const json_t* object, const char* field, std::set<std::string>& values) {
    const auto* array = json_object_get(object, field);
    if (!json_is_array(array)) {
        return fail(std::string(field) + " must be an array");
    }
    for (std::size_t index = 0; index < json_array_size(array); ++index) {
        const auto* value = json_array_get(array, index);
        if (!json_is_string(value) || !values.insert(json_string_value(value)).second) {
            return fail(std::string(field) + " contains a non-string or duplicate");
        }
    }
    return pass(std::string(field) + " is a unique string set");
}

std::string declared_record_schema_name(const json_t* schema) {
    const std::string direct = string_field(schema, "schema_name");
    if (!direct.empty()) {
        return direct;
    }
    const auto* properties = json_object_get(schema, "properties");
    const auto* schema_name = json_is_object(properties) ? json_object_get(properties, "schema_name") : nullptr;
    const auto* constant = json_is_object(schema_name) ? json_object_get(schema_name, "const") : nullptr;
    return json_is_string(constant) ? std::string(json_string_value(constant)) : std::string{};
}

CheckResult check_semantic_groups(const json_t* schema, const std::string& path) {
    const auto* groups = json_object_get(schema, "semantic_groups");
    const bool is_site_reads = string_field(schema, "schema_name") == "longlineage.site_reads";
    if (groups == nullptr) {
        return is_site_reads ? fail(path + ": site_reads must declare its Interval0 semantic group")
                             : pass("no semantic groups");
    }
    if (!json_is_array(groups) || json_array_size(groups) == 0U) {
        return fail(path + ": semantic_groups must be a non-empty array");
    }
    std::map<std::string, std::string> field_types;
    const auto* fields = json_object_get(schema, "fields");
    if (!json_is_array(fields)) {
        return fail(path + ": semantic_groups require tabular fields");
    }
    for (std::size_t index = 0; index < json_array_size(fields); ++index) {
        const auto* field = json_array_get(fields, index);
        field_types.emplace(string_field(field, "name"), string_field(field, "type"));
    }
    std::set<std::string> group_ids;
    for (std::size_t index = 0; index < json_array_size(groups); ++index) {
        const auto* group = json_array_get(groups, index);
        std::string error;
        const std::set<std::string> keys = {"group_id", "semantic_type", "coordinate_system", "required",
                                            "members",  "constraints",   "enforcement"};
        if (!object_has_exact_keys(group, keys, error) || !group_ids.insert(string_field(group, "group_id")).second ||
            string_field(group, "semantic_type") != "Interval0" ||
            string_field(group, "coordinate_system") != "ZERO_BASED_HALF_OPEN" ||
            !json_is_true(json_object_get(group, "required"))) {
            return fail(path + ": semantic group identity/type/required shape is invalid: " + error);
        }
        const auto* members = json_object_get(group, "members");
        const std::set<std::string> member_keys = {"begin", "end"};
        if (!object_has_exact_keys(members, member_keys, error)) {
            return fail(path + ": Interval0 members are not closed: " + error);
        }
        const std::string begin = string_field(members, "begin");
        const std::string end = string_field(members, "end");
        if (begin == end || field_types[begin] != "uint64" || field_types[end] != "uint64") {
            return fail(path + ": Interval0 members must name distinct uint64 fields");
        }
        const auto* constraints = json_object_get(group, "constraints");
        if (!json_is_array(constraints) || json_array_size(constraints) != 1U) {
            return fail(path + ": Interval0 requires exactly one LT constraint");
        }
        const auto* constraint = json_array_get(constraints, 0U);
        const std::set<std::string> constraint_keys = {"operator", "left_field", "right_field"};
        if (!object_has_exact_keys(constraint, constraint_keys, error) ||
            string_field(constraint, "operator") != "LT" || string_field(constraint, "left_field") != begin ||
            string_field(constraint, "right_field") != end) {
            return fail(path + ": Interval0 constraint must be LT(begin,end)");
        }
        std::set<std::string> enforcement;
        const auto loaded = load_unique_string_set(group, "enforcement", enforcement);
        if (!loaded.ok || enforcement != std::set<std::string>{"INDEPENDENT_VALIDATOR", "PRODUCER"}) {
            return fail(path + ": Interval0 must be enforced by producer and independent validator");
        }
    }
    if (is_site_reads && group_ids != std::set<std::string>{"alignment_reference_interval"}) {
        return fail(path + ": site_reads Interval0 group identity differs from the frozen contract");
    }
    return pass("machine-readable semantic groups passed");
}

CheckResult check_embedded_json_bindings(const std::filesystem::path& root, const json_t* schema,
                                         const std::string& path,
                                         const std::map<std::string, std::string>& registered_schema_paths) {
    std::map<std::string, std::pair<std::string, bool>> field_contracts;
    const auto* fields = json_object_get(schema, "fields");
    if (!json_is_array(fields)) {
        return pass("non-tabular embedded JSON contract");
    }
    for (std::size_t index = 0; index < json_array_size(fields); ++index) {
        const auto* field = json_array_get(fields, index);
        field_contracts.emplace(
            string_field(field, "name"),
            std::make_pair(string_field(field, "type"), json_is_true(json_object_get(field, "required"))));
    }
    std::set<std::string> canonical_json_fields;
    for (const auto& [name, contract] : field_contracts) {
        if (contract.first == "canonical_json") {
            canonical_json_fields.insert(name);
        }
    }
    const auto* bindings = json_object_get(schema, "embedded_json_bindings");
    if (canonical_json_fields.empty()) {
        if (bindings != nullptr && (!json_is_object(bindings) || json_object_size(bindings) != 0U)) {
            return fail(path + ": embedded_json_bindings exists without canonical_json fields");
        }
        return pass("no embedded canonical JSON fields");
    }
    if (!json_is_object(bindings) || json_object_size(bindings) != canonical_json_fields.size()) {
        return fail(path + ": every canonical_json field needs exactly one nested schema binding");
    }
    const std::set<std::string> operators = {
        "ARRAY_LENGTH_EQUALS_FIELD",    "MATRIX_TOTAL_EQUALS_FIELD",           "MATRIX_COLUMN_SUM_EQUALS_FIELD",
        "MINIMUM_ROW_SUM_EQUALS_FIELD", "CANONICAL_JSON_SHA256_EQUALS_FIELD",  "VALUE_PRESENT_IFF_FIELD_EQUALS",
        "NULLABILITY_EQUALS_FIELD",     "PARTNER_SITE_ORDER_SPACING_AT_LEAST",
    };
    const char* field_name = nullptr;
    json_t* binding = nullptr;
    json_object_foreach(const_cast<json_t*>(bindings), field_name, binding) {
        std::string error;
        const std::set<std::string> binding_keys = {"schema_id", "schema_path", "schema_sha256", "null_policy",
                                                    "constraints"};
        if (canonical_json_fields.count(field_name) == 0U || !object_has_exact_keys(binding, binding_keys, error)) {
            return fail(path + ": malformed or unknown embedded JSON binding: " + std::string(field_name));
        }
        const std::string schema_id = string_field(binding, "schema_id");
        const std::string schema_path = string_field(binding, "schema_path");
        const auto registration = registered_schema_paths.find(schema_id);
        if (registration == registered_schema_paths.end() || registration->second != schema_path) {
            return fail(path + ": embedded JSON schema ID/path is not registered offline: " + schema_id);
        }
        const auto digest = verify_declared_file_sha256(root, schema_path, json_object_get(binding, "schema_sha256"),
                                                        path + " embedded schema " + field_name);
        if (!digest.ok) {
            return digest;
        }
        const bool required = field_contracts.at(field_name).second;
        if ((required && string_field(binding, "null_policy") != "FORBIDDEN") ||
            (!required && string_field(binding, "null_policy") != "DOT_TOKEN_ONLY")) {
            return fail(path + ": embedded JSON null policy differs from parent field nullability");
        }
        const auto* constraints = json_object_get(binding, "constraints");
        if (!json_is_array(constraints) || json_array_size(constraints) == 0U) {
            return fail(path + ": embedded JSON binding lacks conservation constraints");
        }
        for (std::size_t index = 0; index < json_array_size(constraints); ++index) {
            const auto* constraint = json_array_get(constraints, index);
            const std::string operation = string_field(constraint, "operator");
            if (!json_is_object(constraint) || operators.count(operation) == 0U) {
                return fail(path + ": embedded JSON binding uses an unknown constraint operator");
            }
            const auto* companion = json_object_get(constraint, "field");
            if (companion != nullptr &&
                (!json_is_string(companion) || field_contracts.count(json_string_value(companion)) == 0U)) {
                return fail(path + ": embedded JSON constraint names an unknown companion field");
            }
            if (operation == "MATRIX_COLUMN_SUM_EQUALS_FIELD") {
                const auto* column = json_object_get(constraint, "column");
                if (!json_is_integer(column) || json_integer_value(column) < 0 || json_integer_value(column) > 1) {
                    return fail(path + ": matrix column constraint is outside the closed two-column shape");
                }
            }
            if (operation == "PARTNER_SITE_ORDER_SPACING_AT_LEAST") {
                const auto* distance = json_object_get(constraint, "distance_bp");
                if (!json_is_integer(distance) || json_integer_value(distance) != 20 ||
                    string_field(constraint, "lookup_artifact") != "cooccurrence_pairs") {
                    return fail(path + ": joint partner spacing contract differs from 20 bp/cooccurrence_pairs");
                }
            }
        }
    }
    return pass("embedded canonical JSON fields are bound to offline hash-locked nested schemas");
}

std::set<std::string> load_gate_ids_unchecked(const std::filesystem::path& root) {
    std::set<std::string> ids;
    std::ifstream input(root / "governance" / "gate_registry.tsv");
    std::string line;
    bool first = true;
    while (std::getline(input, line)) {
        if (first) {
            first = false;
            continue;
        }
        const auto fields = split_tab(line);
        if (!fields.empty() && !fields[0].empty()) {
            ids.insert(fields[0]);
        }
    }
    return ids;
}

CheckResult check_lifecycle_registry(const std::filesystem::path& root, const std::set<std::string>& gate_ids) {
    {
        std::ifstream input(root / "contracts" / "v1" / "lifecycle_codes.tsv");
        std::string line;
        if (!std::getline(input, line) ||
            split_tab(line) != std::vector<std::string>{"dimension", "code", "rank", "description"}) {
            return fail("lifecycle registry header differs from the closed contract");
        }
        std::set<std::string> observed;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            const auto fields = split_tab(line);
            if (fields.size() != 4U || fields[3].empty() ||
                !observed.insert(fields[0] + ":" + fields[1] + ":" + fields[2]).second) {
                return fail("lifecycle registry row is malformed or duplicated");
            }
        }
        const std::set<std::string> expected = {
            "implementation:PLANNED:0",    "implementation:SKELETON:1",        "implementation:IMPLEMENTED:2",
            "verification:NOT_VERIFIED:0", "verification:CONTRACT_VERIFIED:1", "verification:PARITY_VERIFIED:2",
        };
        if (observed != expected) {
            return fail("lifecycle registry differs from the closed implementation/verification axes");
        }
    }

    const std::set<std::string> implementation = {"PLANNED", "SKELETON", "IMPLEMENTED"};
    const std::set<std::string> verification = {"NOT_VERIFIED", "CONTRACT_VERIFIED", "PARITY_VERIFIED"};
    for (const auto& specification : std::vector<std::pair<std::filesystem::path, std::vector<std::string>>>{
             {root / "contracts" / "v1" / "transform_registry.tsv",
              {"transform_id", "executable", "trust_domain", "implementation_status", "verification_status",
               "evidence_gate_id", "description"}},
             {root / "contracts" / "v1" / "query_operators.tsv",
              {"operator", "operand_shape", "value_domain", "index_mode", "scan_policy", "implementation_status",
               "verification_status", "description"}},
         }) {
        std::ifstream input(specification.first);
        std::string line;
        if (!std::getline(input, line) || split_tab(line) != specification.second) {
            return fail(specification.first.string() + ": header differs from the closed contract");
        }
        const auto implementation_column = static_cast<std::size_t>(std::distance(
            specification.second.begin(),
            std::find(specification.second.begin(), specification.second.end(), "implementation_status")));
        const auto verification_column = static_cast<std::size_t>(
            std::distance(specification.second.begin(),
                          std::find(specification.second.begin(), specification.second.end(), "verification_status")));
        const auto evidence = std::find(specification.second.begin(), specification.second.end(), "evidence_gate_id");
        const std::optional<std::size_t> evidence_column =
            evidence == specification.second.end() ? std::nullopt
                                                   : std::optional<std::size_t>(static_cast<std::size_t>(
                                                         std::distance(specification.second.begin(), evidence)));
        std::set<std::string> row_ids;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            const auto fields = split_tab(line);
            if (fields.size() != specification.second.size() || fields[0].empty() ||
                !row_ids.insert(fields[0]).second || implementation.count(fields[implementation_column]) == 0U ||
                verification.count(fields[verification_column]) == 0U) {
                return fail(specification.first.string() + ": row is malformed, duplicated or has unknown lifecycle");
            }
            if (fields[verification_column] != "NOT_VERIFIED") {
                if (fields[implementation_column] != "IMPLEMENTED" || !evidence_column.has_value() ||
                    fields[*evidence_column] == "." || gate_ids.count(fields[*evidence_column]) == 0U) {
                    return fail(specification.first.string() +
                                ": verified lifecycle row lacks IMPLEMENTED state and resolvable gate");
                }
            } else if (evidence_column.has_value() && fields[*evidence_column] != ".") {
                return fail(specification.first.string() + ": unverified lifecycle row carries evidence");
            }
        }
        if (row_ids.empty()) {
            return fail(specification.first.string() + ": registry has no rows");
        }
    }
    return pass("lifecycle, transform and query registries passed");
}

CheckResult check_contract_registry_bindings(const std::filesystem::path& root, const json_t* catalog,
                                             const std::set<std::string>& catalog_artifact_ids) {
    std::string error;
    const auto binding =
        longlineage::cli::load_json_strict(root / "schema" / "core" / "contract_registry_bindings.schema.json", error);
    if (!binding) {
        return fail(error);
    }
    if (string_field(binding.get(), "$id") !=
        "https://longlineage.local/schema/contract_registry_bindings-1.0.1.json") {
        return fail("contract registry binding schema has the wrong current ID");
    }
    const auto* properties = json_object_get(binding.get(), "properties");
    const auto* catalog_const = json_object_get(json_object_get(properties, "catalog"), "const");
    const auto catalog_digest =
        verify_declared_file_sha256(root, string_field(catalog_const, "path"), json_object_get(catalog_const, "sha256"),
                                    "contract-bound schema catalog");
    if (!catalog_digest.ok || string_field(catalog_const, "path") != "schema/catalog.json") {
        return catalog_digest.ok ? fail("contract binding names an unexpected catalog path") : catalog_digest;
    }

    const auto* registries_const = json_object_get(json_object_get(properties, "registries"), "const");
    const std::map<std::string, std::pair<std::string, std::string>> expected_registries = {
        {"artifact_roles", {"contracts/v1/artifact_roles.tsv", "CATALOG_EXACT_ROW_SET"}},
        {"lifecycle_codes", {"contracts/v1/lifecycle_codes.tsv", "CLOSED_VOCABULARY_SHA256_LOCK"}},
        {"query_operators", {"contracts/v1/query_operators.tsv", "SHA256_LOCK"}},
        {"transform_registry", {"contracts/v1/transform_registry.tsv", "SHA256_LOCK"}},
    };
    if (!json_is_object(registries_const) || json_object_size(registries_const) != expected_registries.size()) {
        return fail("contract registry SHA lock does not cover the exact required registry set");
    }
    for (const auto& [name, expected] : expected_registries) {
        const auto* row = json_object_get(registries_const, name.c_str());
        const std::set<std::string> keys = {"path", "sha256", "binding_mode"};
        if (!object_has_exact_keys(row, keys, error) || string_field(row, "path") != expected.first ||
            string_field(row, "binding_mode") != expected.second) {
            return fail("contract registry binding row is malformed: " + name);
        }
        const auto digest = verify_declared_file_sha256(root, expected.first, json_object_get(row, "sha256"),
                                                        "contract registry " + name);
        if (!digest.ok) {
            return digest;
        }
    }

    std::map<std::string, std::tuple<std::string, std::string, bool>> catalog_contracts;
    const auto* artifacts = json_object_get(catalog, "artifacts");
    for (std::size_t index = 0; index < json_array_size(artifacts); ++index) {
        const auto* artifact = json_array_get(artifacts, index);
        const auto schema = longlineage::cli::load_json_strict(root / string_field(artifact, "record_schema"), error);
        if (!schema) {
            return fail(error);
        }
        catalog_contracts.emplace(
            string_field(artifact, "artifact_id"),
            std::make_tuple(declared_record_schema_name(schema.get()), string_field(artifact, "format"),
                            json_is_true(json_object_get(artifact, "queryable"))));
    }

    std::ifstream roles(root / "contracts" / "v1" / "artifact_roles.tsv");
    std::string line;
    const std::vector<std::string> role_header = {
        "role", "schema_name", "format", "binding_scope", "catalog_binding", "production_authority", "queryable"};
    if (!std::getline(roles, line) || split_tab(line) != role_header) {
        return fail("artifact role registry header differs from the closed contract");
    }
    std::set<std::string> artifact_roles;
    std::set<std::string> subordinate_roles;
    std::set<std::string> role_ids;
    while (std::getline(roles, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = split_tab(line);
        if (fields.size() != role_header.size() || !role_ids.insert(fields[0]).second ||
            (fields[5] != "true" && fields[5] != "false") || (fields[6] != "true" && fields[6] != "false")) {
            return fail("artifact role registry row is malformed or duplicated");
        }
        if (fields[3] == "ARTIFACT") {
            artifact_roles.insert(fields[0]);
            const auto contract = catalog_contracts.find(fields[0]);
            if (contract == catalog_contracts.end() || fields[4] != "EXACT_CATALOG_ARTIFACT_ID" ||
                fields[1] != std::get<0>(contract->second) || fields[2] != std::get<1>(contract->second) ||
                (fields[6] == "true") != std::get<2>(contract->second)) {
                return fail("artifact role row drifts from catalog metadata: " + fields[0]);
            }
        } else if (fields[3] == "SUBORDINATE_INDEX") {
            subordinate_roles.insert(fields[0]);
            if (fields[0] != "site_index" || fields[4] != "CATALOG_SITE_INDEX_SCHEMA") {
                return fail("unknown subordinate artifact role");
            }
        } else {
            return fail("artifact role uses an unknown binding scope");
        }
    }
    if (artifact_roles != catalog_artifact_ids || subordinate_roles != std::set<std::string>{"site_index"}) {
        return fail("artifact role registry is not the exact catalog/subordinate row set");
    }

    const auto* relation_const = json_object_get(json_object_get(properties, "artifact_role_relation"), "const");
    std::set<std::string> relation_artifacts;
    std::set<std::string> relation_subordinates;
    const auto relation_artifact_check =
        load_unique_string_set(relation_const, "catalog_artifact_ids", relation_artifacts);
    const auto relation_subordinate_check =
        load_unique_string_set(relation_const, "subordinate_index_role_ids", relation_subordinates);
    if (!relation_artifact_check.ok || !relation_subordinate_check.ok || relation_artifacts != catalog_artifact_ids ||
        relation_subordinates != std::set<std::string>{"site_index"}) {
        return fail("contract binding const row-set differs from catalog/artifact roles");
    }

    return check_lifecycle_registry(root, load_gate_ids_unchecked(root));
}

CheckResult check_source_to_target_lifecycle(const std::filesystem::path& root) {
    std::string error;
    const auto manifest =
        longlineage::cli::load_json_strict(root / "provenance" / "source_to_target_manifest.json", error);
    if (!manifest) {
        return fail(error);
    }
    const std::set<std::string> root_keys = {"$schema",           "schema_name",   "schema_version",
                                             "origin_repository", "origin_commit", "license_review_status",
                                             "mappings"};
    if (!object_has_exact_keys(manifest.get(), root_keys, error) ||
        string_field(manifest.get(), "$schema") !=
            "https://longlineage.local/schema/source_to_target_manifest-1.1.0.json" ||
        string_field(manifest.get(), "schema_name") != "longlineage.source_to_target_manifest" ||
        string_field(manifest.get(), "schema_version") != "1.1.0" ||
        string_field(manifest.get(), "origin_repository") != "InterSubMod" ||
        !std::regex_match(string_field(manifest.get(), "origin_commit"), std::regex("^[0-9a-f]{40}$"))) {
        return fail("source-to-target manifest identity/closed-world shape is invalid: " + error);
    }
    const auto* mappings = json_object_get(manifest.get(), "mappings");
    if (!json_is_array(mappings) || json_array_size(mappings) == 0U) {
        return fail("source-to-target manifest mappings must be non-empty");
    }
    const auto gate_ids = load_gate_ids_unchecked(root);
    std::set<std::string> origin_ids;
    for (std::size_t index = 0; index < json_array_size(mappings); ++index) {
        const auto* mapping = json_array_get(mappings, index);
        const std::set<std::string> keys = {"origin_id",
                                            "source_path",
                                            "source_sha256",
                                            "target",
                                            "target_kind",
                                            "target_presence",
                                            "target_digest_kind",
                                            "target_sha256",
                                            "implementation_status",
                                            "verification_status",
                                            "verified_evidence_id",
                                            "transformation",
                                            "reuse"};
        if (!object_has_exact_keys(mapping, keys, error)) {
            return fail("source-to-target mapping is not closed: " + error);
        }
        const std::string origin_id = string_field(mapping, "origin_id");
        const std::string source_path = string_field(mapping, "source_path");
        const std::string target = string_field(mapping, "target");
        const std::string target_kind = string_field(mapping, "target_kind");
        const std::string target_presence = string_field(mapping, "target_presence");
        const std::string implementation = string_field(mapping, "implementation_status");
        const std::string verification = string_field(mapping, "verification_status");
        const auto* target_digest_kind = json_object_get(mapping, "target_digest_kind");
        const auto* target_sha = json_object_get(mapping, "target_sha256");
        const auto* evidence = json_object_get(mapping, "verified_evidence_id");
        if (!std::regex_match(origin_id, std::regex("^[a-z0-9][a-z0-9-]*$")) || !origin_ids.insert(origin_id).second ||
            !path_is_safe_relative(source_path) || !is_lower_sha256_text(string_field(mapping, "source_sha256")) ||
            !path_is_safe_relative(target) || (target_kind != "FILE" && target_kind != "DIRECTORY") ||
            (target_presence != "ABSENT" && target_presence != "PRESENT") ||
            (implementation != "PLANNED" && implementation != "SKELETON" && implementation != "IMPLEMENTED") ||
            (verification != "NOT_VERIFIED" && verification != "CONTRACT_VERIFIED" &&
             verification != "PARITY_VERIFIED")) {
            return fail("source-to-target mapping identity/lifecycle is malformed: " + origin_id);
        }
        const auto target_path = root / target;
        std::error_code status_error;
        const auto target_status = std::filesystem::symlink_status(target_path, status_error);
        const bool target_exists = !status_error && std::filesystem::exists(target_status);
        if (target_presence == "ABSENT") {
            if (target_exists || implementation != "PLANNED" || !json_is_null(target_digest_kind) ||
                !json_is_null(target_sha) || verification != "NOT_VERIFIED" || !json_is_null(evidence)) {
                return fail("PLANNED/ABSENT source mapping carries a target, digest or evidence: " + origin_id);
            }
            continue;
        }
        if (!target_exists || std::filesystem::is_symlink(target_status) ||
            (target_kind == "FILE" && !std::filesystem::is_regular_file(target_status)) ||
            (target_kind == "DIRECTORY" && !std::filesystem::is_directory(target_status)) ||
            !json_is_string(target_digest_kind) || !json_is_string(target_sha) ||
            !is_lower_sha256_text(json_string_value(target_sha))) {
            return fail("PRESENT source mapping target/type/digest is invalid: " + origin_id);
        }
        if (target_kind == "FILE") {
            if (string_field(mapping, "target_digest_kind") != "FILE_SHA256") {
                return fail("file target mapping must use FILE_SHA256: " + origin_id);
            }
            const auto digest =
                verify_declared_file_sha256(root, target, target_sha, "source mapping target " + origin_id);
            if (!digest.ok) {
                return digest;
            }
        } else {
            if (string_field(mapping, "target_digest_kind") != "TREE_SHA256") {
                return fail("directory target mapping must use TREE_SHA256: " + origin_id);
            }
            return fail("directory target TREE_SHA256 replay is not implemented; mapping cannot be PRESENT: " +
                        origin_id);
        }
        if (implementation == "PLANNED" ||
            (implementation == "SKELETON" && (verification != "NOT_VERIFIED" || !json_is_null(evidence)))) {
            return fail("present source mapping lifecycle contradicts implementation state: " + origin_id);
        }
        if (verification == "NOT_VERIFIED") {
            if (!json_is_null(evidence)) {
                return fail("unverified source mapping carries evidence: " + origin_id);
            }
        } else if (implementation != "IMPLEMENTED" || !json_is_string(evidence) ||
                   gate_ids.count(json_string_value(evidence)) == 0U) {
            return fail("verified source mapping lacks IMPLEMENTED state/resolvable evidence: " + origin_id);
        }
    }
    return pass("source-to-target lifecycle and present target digests passed " + std::to_string(origin_ids.size()) +
                " mappings");
}

CheckResult check_catalog(const std::filesystem::path& root) {
    std::string error;
    const auto catalog = longlineage::cli::load_json_strict(root / "schema" / "catalog.json", error);
    if (!catalog) {
        return fail(error);
    }
    if (string_field(catalog.get(), "schema_name") != "longlineage.artifact_schema_catalog" ||
        string_field(catalog.get(), "schema_version").empty()) {
        return fail("catalog identity/version is missing");
    }
    const std::set<std::string> catalog_keys = {"schema_name",
                                                "schema_version",
                                                "coordinate_types",
                                                "null_token",
                                                "schema_id_registry",
                                                "type_registry",
                                                "status_reason_registry",
                                                "site_index_schema",
                                                "site_index_schema_sha256",
                                                "run_membership",
                                                "artifacts"};
    if (!object_has_exact_keys(catalog.get(), catalog_keys, error)) {
        return fail("catalog is not closed-world: " + error);
    }

    for (const char* registry_name :
         {"schema_id_registry", "type_registry", "status_reason_registry", "site_index_schema"}) {
        const std::string relative = string_field(catalog.get(), registry_name);
        if (!path_is_safe_relative(relative) || !std::filesystem::is_regular_file(root / relative)) {
            return fail(std::string(registry_name) + " path is absent or unsafe: " + relative);
        }
    }
    const auto site_index_digest =
        verify_declared_file_sha256(root, string_field(catalog.get(), "site_index_schema"),
                                    json_object_get(catalog.get(), "site_index_schema_sha256"), "site index schema");
    if (!site_index_digest.ok) {
        return site_index_digest;
    }

    const std::string schema_registry_path = string_field(catalog.get(), "schema_id_registry");
    const auto schema_registry = longlineage::cli::load_json_strict(root / schema_registry_path, error);
    if (!schema_registry) {
        return fail(error);
    }
    const std::set<std::string> schema_registry_keys = {"schema_name", "schema_version", "schemas",
                                                        "catalog_schema_locks", "resolution"};
    if (!object_has_exact_keys(schema_registry.get(), schema_registry_keys, error) ||
        string_field(schema_registry.get(), "schema_name") != "longlineage.schema_id_registry" ||
        string_field(schema_registry.get(), "schema_version") != "1.0.0") {
        return fail("schema ID registry identity/closed-world shape is invalid: " + error);
    }
    const auto* resolution = json_object_get(schema_registry.get(), "resolution");
    const std::set<std::string> resolution_keys = {"network_allowed", "unknown_id_policy", "duplicate_id_policy",
                                                   "registry_path_policy"};
    if (!object_has_exact_keys(resolution, resolution_keys, error) ||
        !json_is_false(json_object_get(resolution, "network_allowed")) ||
        string_field(resolution, "unknown_id_policy") != "FAIL_CLOSED" ||
        string_field(resolution, "duplicate_id_policy") != "FAIL_CLOSED" ||
        string_field(resolution, "registry_path_policy") != "REPOSITORY_RELATIVE_NORMALIZED") {
        return fail("schema ID resolution policy is not fail-closed/offline");
    }

    std::set<std::string> status_domains;
    {
        std::ifstream status_registry(root / string_field(catalog.get(), "status_reason_registry"));
        std::string row;
        bool first = true;
        while (std::getline(status_registry, row)) {
            if (first) {
                first = false;
                continue;
            }
            const auto tab = row.find('\t');
            if (tab != std::string::npos && tab > 0U) {
                status_domains.insert(row.substr(0, tab));
            }
        }
    }
    if (status_domains.empty()) {
        return fail("status registry declares no domains");
    }

    std::set<std::string> registered_types;
    {
        std::ifstream type_registry(root / string_field(catalog.get(), "type_registry"));
        std::string row;
        if (!std::getline(type_registry, row) ||
            split_tab(row) != std::vector<std::string>{"type_name", "physical_encoding", "constraints", "null_rule",
                                                       "unit_or_semantics"}) {
            return fail("type registry header differs from the closed five-column contract");
        }
        while (std::getline(type_registry, row)) {
            if (row.empty()) {
                continue;
            }
            const auto fields = split_tab(row);
            if (fields.size() != 5U ||
                std::any_of(fields.begin(), fields.end(), [](const std::string& value) { return value.empty(); }) ||
                !registered_types.insert(fields[0]).second) {
                return fail("type registry row is empty, duplicated or malformed");
            }
        }
    }
    for (const auto& required :
         {"uint8", "uint16", "uint32", "uint64", "int64", "float64", "bool", "string", "Position1", "sha256",
          "status_code", "reason_code", "axis_status", "hp_state", "enum:<A|B>"}) {
        if (registered_types.count(required) == 0U) {
            return fail("type registry lacks required type: " + std::string(required));
        }
    }

    const auto* registered_schemas = json_object_get(schema_registry.get(), "schemas");
    if (!json_is_array(registered_schemas) || json_array_size(registered_schemas) == 0U) {
        return fail("schema ID registry schemas must be a non-empty array");
    }
    std::set<std::string> registered_ids;
    std::set<std::string> registered_paths;
    std::map<std::string, std::string> registered_schema_paths;
    std::vector<JsonPtr> loaded_registered_schemas;
    for (std::size_t index = 0; index < json_array_size(registered_schemas); ++index) {
        const auto* registration = json_array_get(registered_schemas, index);
        const std::set<std::string> registration_keys = {"id", "path", "sha256"};
        if (!object_has_exact_keys(registration, registration_keys, error)) {
            return fail("schema ID registration row is not closed: " + error);
        }
        const std::string id = string_field(registration, "id");
        const std::string path = string_field(registration, "path");
        if (id.rfind("https://longlineage.local/", 0) != 0 || !registered_ids.insert(id).second ||
            !path_is_safe_relative(path) || !registered_paths.insert(path).second ||
            !std::filesystem::is_regular_file(root / path)) {
            return fail("schema ID/path is unsafe, absent or duplicated: " + id + " -> " + path);
        }
        registered_schema_paths.emplace(id, path);
        const auto digest_check =
            verify_declared_file_sha256(root, path, json_object_get(registration, "sha256"), "registered schema " + id);
        if (!digest_check.ok) {
            return digest_check;
        }
        auto schema = longlineage::cli::load_json_strict(root / path, error);
        if (!schema) {
            return fail(error);
        }
        if (string_field(schema.get(), "$id") != id) {
            return fail("registered schema $id differs from registry: " + path);
        }
        loaded_registered_schemas.push_back(std::move(schema));
    }
    for (const auto& schema : loaded_registered_schemas) {
        std::vector<std::string> external_refs;
        collect_external_schema_refs(schema.get(), external_refs);
        for (const auto& reference : external_refs) {
            const auto fragment = reference.find('#');
            const std::string base_id = reference.substr(0, fragment);
            if (registered_ids.count(base_id) == 0U) {
                return fail("external $ref cannot resolve offline through schema registry: " + reference);
            }
        }
    }
    std::map<std::string, const json_t*> schemas_by_id;
    for (const auto& schema : loaded_registered_schemas) {
        schemas_by_id.emplace(string_field(schema.get(), "$id"), schema.get());
    }

    const auto* catalog_locks = json_object_get(schema_registry.get(), "catalog_schema_locks");
    if (!json_is_array(catalog_locks) || json_array_size(catalog_locks) == 0U) {
        return fail("schema ID registry catalog_schema_locks must be non-empty");
    }
    std::set<std::string> locked_catalog_paths;
    for (std::size_t index = 0; index < json_array_size(catalog_locks); ++index) {
        const auto* lock = json_array_get(catalog_locks, index);
        const std::set<std::string> lock_keys = {"path", "sha256"};
        if (!object_has_exact_keys(lock, lock_keys, error)) {
            return fail("catalog schema lock row is not closed: " + error);
        }
        const std::string path = string_field(lock, "path");
        if (!locked_catalog_paths.insert(path).second) {
            return fail("catalog schema lock path is duplicated: " + path);
        }
        const auto digest_check =
            verify_declared_file_sha256(root, path, json_object_get(lock, "sha256"), "catalog schema lock");
        if (!digest_check.ok) {
            return digest_check;
        }
    }

    const auto* artifacts = json_object_get(catalog.get(), "artifacts");
    if (!json_is_array(artifacts) || json_array_size(artifacts) == 0U) {
        return fail("catalog artifacts must be a non-empty array");
    }
    std::set<std::string> ids;
    std::set<std::string> relative_paths;
    std::set<std::string> expected_catalog_schema_paths = {string_field(catalog.get(), "site_index_schema")};
    const std::set<std::string> allowed_formats = {"JSON", "TSV_BGZF", "JSONL_BGZF", "LLM_BGZF", "TSV", "SHA256SUM"};
    for (std::size_t index = 0; index < json_array_size(artifacts); ++index) {
        const auto* artifact = json_array_get(artifacts, index);
        const std::set<std::string> artifact_keys = {
            "artifact_id", "relative_path", "format", "record_schema", "record_schema_sha256",
            "primary_key", "sort_key",      "index",  "index_key",     "producer",
            "validator",   "queryable"};
        if (!object_has_exact_keys(artifact, artifact_keys, error)) {
            return fail("catalog artifact row is not closed: " + error);
        }
        const std::string id = string_field(artifact, "artifact_id");
        const std::string relative = string_field(artifact, "relative_path");
        const std::string schema_path = string_field(artifact, "record_schema");
        if (id.empty() || !ids.insert(id).second) {
            return fail("catalog artifact_id is empty or duplicated: " + id);
        }
        if (!path_is_safe_relative(relative) || !relative_paths.insert(relative).second) {
            return fail("artifact relative_path is unsafe or duplicated: " + relative);
        }
        if (!path_is_safe_relative(schema_path) || !std::filesystem::is_regular_file(root / schema_path)) {
            return fail("record schema is absent or unsafe for artifact " + id);
        }
        expected_catalog_schema_paths.insert(schema_path);
        const auto record_digest = verify_declared_file_sha256(
            root, schema_path, json_object_get(artifact, "record_schema_sha256"), "record schema for " + id);
        if (!record_digest.ok) {
            return record_digest;
        }
        if (!json_is_array(json_object_get(artifact, "primary_key")) ||
            !json_is_array(json_object_get(artifact, "sort_key")) ||
            !json_is_boolean(json_object_get(artifact, "queryable")) ||
            allowed_formats.count(string_field(artifact, "format")) == 0U ||
            string_field(artifact, "producer").empty() || string_field(artifact, "validator").empty()) {
            return fail("artifact contract is incomplete: " + id);
        }
        const auto* index_value = json_object_get(artifact, "index");
        const auto* index_key = json_object_get(artifact, "index_key");
        if (!json_is_null(index_value) &&
            (!json_is_string(index_value) || !path_is_safe_relative(json_string_value(index_value)))) {
            return fail("artifact index path is malformed: " + id);
        }
        if (json_is_null(index_value) != json_is_null(index_key)) {
            return fail("artifact index and index_key nullability differ: " + id);
        }
        if (!json_is_null(index_value)) {
            const auto* primary_key = json_object_get(artifact, "primary_key");
            if (!json_is_array(index_key) || json_array_size(index_key) != 2U ||
                json_array_size(primary_key) < json_array_size(index_key)) {
                return fail("indexed artifact needs a two-field index_key: " + id);
            }
            for (std::size_t key_index = 0; key_index < json_array_size(index_key); ++key_index) {
                const auto* indexed_field = json_array_get(index_key, key_index);
                const auto* primary_field = json_array_get(primary_key, key_index);
                if (!json_is_string(indexed_field) || !json_is_string(primary_field) ||
                    std::string(json_string_value(indexed_field)) != json_string_value(primary_field)) {
                    return fail("artifact index_key is not a primary-key prefix: " + id);
                }
            }
            if (std::string(json_string_value(json_array_get(index_key, 0U))) != "dataset_order") {
                return fail("artifact index_key must begin with dataset_order: " + id);
            }
        }

        const auto schema = longlineage::cli::load_json_strict(root / schema_path, error);
        if (!schema) {
            return fail(error);
        }
        const auto table_check = check_tabular_record_schema(schema.get(), schema_path);
        if (!table_check.ok) {
            return table_check;
        }
        const auto type_check = check_tabular_field_contract(schema.get(), schema_path, registered_types);
        if (!type_check.ok) {
            return type_check;
        }
        const auto domain_check = check_status_domain_bindings(schema.get(), schema_path, status_domains);
        if (!domain_check.ok) {
            return domain_check;
        }
        const auto semantic_group_check = check_semantic_groups(schema.get(), schema_path);
        if (!semantic_group_check.ok) {
            return semantic_group_check;
        }
        const auto embedded_check =
            check_embedded_json_bindings(root, schema.get(), schema_path, registered_schema_paths);
        if (!embedded_check.ok) {
            return embedded_check;
        }
        const auto primary_check =
            check_catalog_key_array(json_object_get(artifact, "primary_key"), id + " primary_key",
                                    string_field(artifact, "format"), schema.get(), schema_path, schemas_by_id);
        if (!primary_check.ok) {
            return primary_check;
        }
        const auto sort_check =
            check_catalog_key_array(json_object_get(artifact, "sort_key"), id + " sort_key",
                                    string_field(artifact, "format"), schema.get(), schema_path, schemas_by_id);
        if (!sort_check.ok) {
            return sort_check;
        }
    }
    if (locked_catalog_paths != expected_catalog_schema_paths) {
        return fail("catalog_schema_locks must exactly cover catalog record/index schemas");
    }

    const auto* membership = json_object_get(catalog.get(), "run_membership");
    const std::set<std::string> membership_keys = {
        "scientific_artifact_ids",      "artifact_catalog_row_artifact_ids",  "data_lineage_output_artifact_ids",
        "semantic_digest_artifact_ids", "producer_receipt_artifact_ids",      "run_receipt_artifact_ids",
        "checksums_artifact_ids",       "checksums_include_declared_indexes", "checksums_excluded_artifact_ids",
    };
    if (!object_has_exact_keys(membership, membership_keys, error) ||
        !json_is_true(json_object_get(membership, "checksums_include_declared_indexes"))) {
        return fail("run_membership is not closed or index inclusion is not required: " + error);
    }
    std::set<std::string> scientific;
    std::set<std::string> artifact_catalog_rows;
    std::set<std::string> data_lineage_rows;
    std::set<std::string> semantic_digests;
    std::set<std::string> producer_receipt;
    std::set<std::string> run_receipt;
    std::set<std::string> checksums;
    std::set<std::string> checksum_exclusions;
    for (const auto& [field, target] : std::vector<std::pair<const char*, std::set<std::string>*>>{
             {"scientific_artifact_ids", &scientific},
             {"artifact_catalog_row_artifact_ids", &artifact_catalog_rows},
             {"data_lineage_output_artifact_ids", &data_lineage_rows},
             {"semantic_digest_artifact_ids", &semantic_digests},
             {"producer_receipt_artifact_ids", &producer_receipt},
             {"run_receipt_artifact_ids", &run_receipt},
             {"checksums_artifact_ids", &checksums},
             {"checksums_excluded_artifact_ids", &checksum_exclusions},
         }) {
        const auto loaded = load_unique_string_set(membership, field, *target);
        if (!loaded.ok) {
            return loaded;
        }
        for (const auto& id : *target) {
            if (ids.count(id) == 0U) {
                return fail(std::string(field) + " references unknown artifact: " + id);
            }
        }
    }
    const std::set<std::string> expected_scientific = {"site_reads",         "methyl_calls",   "bernoulli_upper",
                                                       "m1_sites",           "m1_assignments", "cooccurrence_pairs",
                                                       "cooccurrence_sites", "topology_units", "summary"};
    auto expected_semantic = expected_scientific;
    expected_semantic.insert("artifact_catalog");
    expected_semantic.insert("data_lineage");
    auto expected_receipt = expected_semantic;
    expected_receipt.insert("semantic_digests");
    auto expected_checksums = expected_receipt;
    expected_checksums.insert("producer_receipt");
    const std::set<std::string> expected_exclusions = {"checksums", "validation_receipt", "run_receipt"};
    if (scientific != expected_scientific || artifact_catalog_rows != expected_scientific ||
        data_lineage_rows != expected_scientific || semantic_digests != expected_semantic ||
        producer_receipt != expected_receipt || run_receipt != expected_receipt || checksums != expected_checksums ||
        checksum_exclusions != expected_exclusions) {
        return fail("run_membership violates the closed acyclic artifact/receipt DAG");
    }

    const auto registry_binding_check = check_contract_registry_bindings(root, catalog.get(), ids);
    if (!registry_binding_check.ok) {
        return registry_binding_check;
    }
    const auto provenance_check = check_source_to_target_lifecycle(root);
    if (!provenance_check.ok) {
        return provenance_check;
    }

    return pass("runtime catalog, " + std::to_string(registered_ids.size()) +
                " offline schema IDs, hash-locked nested/record/registry contracts, semantic groups, index keys, "
                "query/provenance lifecycle bindings, and closed run membership passed (" +
                std::to_string(ids.size()) + " artifacts)");
}

std::vector<std::string> split_tab(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const auto end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return fields;
}

CheckResult check_gate_registry(const std::filesystem::path& root) {
    const auto path = root / "governance" / "gate_registry.tsv";
    std::ifstream input(path);
    if (!input) {
        return fail("cannot open " + path.string());
    }
    std::string line;
    if (!std::getline(input, line)) {
        return fail("gate registry is empty");
    }
    const auto header = split_tab(line);
    std::map<std::string, std::size_t> columns;
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (!columns.emplace(header[index], index).second) {
            return fail("gate registry has duplicate header: " + header[index]);
        }
    }
    for (const auto& required : {"gate_id", "required", "scope", "command", "negative_fixture", "evidence_role"}) {
        if (columns.count(required) == 0U) {
            return fail("gate registry missing column: " + std::string(required));
        }
    }

    std::set<std::string> gate_ids;
    std::set<std::string> negative_bindings;
    std::size_t required_gates = 0;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = split_tab(line);
        if (fields.size() != header.size()) {
            return fail("gate registry row has wrong field count");
        }
        const std::string gate_id = fields[columns.at("gate_id")];
        const std::string required = fields[columns.at("required")];
        const std::string command = fields[columns.at("command")];
        const std::string negative_binding = fields[columns.at("negative_fixture")];
        if (gate_id.empty() || !gate_ids.insert(gate_id).second || (required != "true" && required != "false") ||
            fields[columns.at("scope")].empty() || command.empty() || negative_binding.empty() ||
            fields[columns.at("evidence_role")].empty()) {
            return fail("gate registry row is empty, duplicated or malformed: " + gate_id);
        }
        if (!negative_bindings.insert(negative_binding).second) {
            return fail("gate registry reuses a negative binding: " + negative_binding);
        }
        if (negative_binding.rfind("ctest:", 0) == 0) {
            const auto test_id = negative_binding.substr(std::string("ctest:").size());
            if (!std::regex_match(test_id, std::regex("^[A-Za-z0-9_.-]+$"))) {
                return fail("gate registry has malformed CTest binding: " + negative_binding);
            }
        } else if (negative_binding.rfind("fixture:", 0) == 0) {
            const auto relative = negative_binding.substr(std::string("fixture:").size());
            const auto fixture_path = root / relative;
            std::error_code size_error;
            const auto size = std::filesystem::file_size(fixture_path, size_error);
            if (!path_is_safe_relative(relative) || relative.rfind("tests/fixtures/", 0) != 0 ||
                !std::filesystem::is_regular_file(fixture_path) || std::filesystem::is_symlink(fixture_path) ||
                size_error || size == 0U) {
                return fail("gate registry fixture binding is absent, unsafe or empty: " + negative_binding);
            }
        } else {
            return fail("gate registry negative binding is outside the closed vocabulary: " + negative_binding);
        }
        if (required == "true") {
            ++required_gates;
        }
        if (command.rfind("scripts/", 0) == 0) {
            const auto separator = command.find(' ');
            const auto relative = command.substr(0, separator);
            if (!path_is_safe_relative(relative) || !std::filesystem::is_regular_file(root / relative)) {
                return fail("gate command script is absent or unsafe: " + command);
            }
        } else if (command.rfind("ctest ", 0) != 0 && command.rfind("longlineage-governance ", 0) != 0) {
            return fail("gate command is outside the closed command vocabulary: " + command);
        }
    }
    for (const auto& required_id :
         {"GOV_COLD_START", "GOV_POLICY", "GOV_PROJECT_STATE", "CONTRACT_CATALOG", "GOV_PHASE_LEDGER_SCHEMA",
          "GOV_AGENT_TASK", "GOV_AUDIT_EVIDENCE", "GOV_GATE_REGISTRY", "CONTRACT_SCHEMA_IDS",
          "CONTRACT_SCHEMA_DIGEST_CLOSURE", "CONTRACT_RUN_MEMBERSHIP", "CONTRACT_STATUS",
          "CONTRACT_RELEASE_ATTESTATION", "BOUNDARY_TRUTH_ISOLATION", "HYGIENE_REPOSITORY", "RELEASE_PHASES"}) {
        if (gate_ids.count(required_id) == 0U) {
            return fail("required governance gate is absent: " + std::string(required_id));
        }
    }
    return pass("gate registry passed " + std::to_string(gate_ids.size()) + " rows (" + std::to_string(required_gates) +
                " required)");
}

CheckResult check_status_codes(const std::filesystem::path& root) {
    const auto path = root / "contracts" / "v1" / "status_reason_codes.tsv";
    std::ifstream input(path);
    if (!input) {
        return fail("cannot open " + path.string());
    }
    std::string line;
    if (!std::getline(input, line)) {
        return fail("status registry is empty");
    }
    const auto header = split_tab(line);
    std::map<std::string, std::size_t> columns;
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (!columns.emplace(header[index], index).second) {
            return fail("status registry has duplicate header: " + header[index]);
        }
    }
    for (const auto& required : {"domain", "kind", "code", "terminal", "severity", "allowed_parent_status",
                                 "requires_reason", "forbids_winner", "introduced_in", "description", "test_id"}) {
        if (columns.count(required) == 0U) {
            return fail("status registry missing column: " + std::string(required));
        }
    }

    std::set<std::string> keys;
    std::set<std::string> statuses;
    std::vector<std::string> parents;
    std::size_t rows = 0;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = split_tab(line);
        if (fields.size() != header.size()) {
            return fail("status registry row has wrong field count at logical row " + std::to_string(rows + 2U));
        }
        const auto& domain = fields[columns.at("domain")];
        const auto& kind = fields[columns.at("kind")];
        const auto& code = fields[columns.at("code")];
        if ((kind != "status" && kind != "reason") || domain.empty() || code.empty()) {
            return fail("status registry domain/kind/code is invalid");
        }
        if (!keys.insert(domain + '\t' + kind + '\t' + code).second) {
            return fail("duplicate status registry key: " + domain + "/" + kind + "/" + code);
        }
        for (const auto& boolean_column : {"terminal", "requires_reason", "forbids_winner"}) {
            const auto& value = fields[columns.at(boolean_column)];
            if (value != "true" && value != "false") {
                return fail("invalid boolean in " + std::string(boolean_column));
            }
        }
        if (fields[columns.at("description")].empty() || fields[columns.at("test_id")].empty()) {
            return fail("status registry row lacks description or negative test id");
        }
        if (kind == "status") {
            statuses.insert(code);
        }
        const auto& parent = fields[columns.at("allowed_parent_status")];
        if (parent != ".") {
            parents.push_back(parent);
        }
        ++rows;
    }
    for (const auto& parent : parents) {
        if (statuses.count(parent) == 0U) {
            return fail("allowed_parent_status is not declared: " + parent);
        }
    }
    for (const auto& required :
         {"RUNNING", "FAILED", "VALIDATED", "VALIDATED_FROZEN", "FAMILY_COMPLETE", "FAMILY_INCOMPLETE_CAP",
          "FAMILY_INCOMPLETE_DEADLINE", "ABSTAIN_DEPENDENCY_UNAVAILABLE", "ABSTAIN_NOT_IDENTIFIABLE"}) {
        if (statuses.count(required) == 0U) {
            return fail("required status code is absent: " + std::string(required));
        }
    }
    return pass("status/reason registry passed " + std::to_string(rows) + " closed-vocabulary rows");
}

bool contains_case_insensitive(const std::string& text, const std::string& needle) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return lowered.find(needle) != std::string::npos;
}

bool json_contains_truth_token(const json_t* value, std::string& location, const std::string& current = "$") {
    if (json_is_object(value)) {
        const char* key = nullptr;
        json_t* child = nullptr;
        json_object_foreach(const_cast<json_t*>(value), key, child) {
            const std::string child_location = current + "." + key;
            if (contains_case_insensitive(key, "truth")) {
                location = child_location;
                return true;
            }
            if (json_contains_truth_token(child, location, child_location)) {
                return true;
            }
        }
    } else if (json_is_array(value)) {
        for (std::size_t index = 0; index < json_array_size(value); ++index) {
            if (json_contains_truth_token(json_array_get(value, index), location,
                                          current + "[" + std::to_string(index) + "]")) {
                return true;
            }
        }
    } else if (json_is_string(value) && contains_case_insensitive(json_string_value(value), "truth")) {
        location = current;
        return true;
    }
    return false;
}

CheckResult check_truth_boundary(const std::filesystem::path& root, const std::filesystem::path& optional_manifest) {
    std::string error;
    const auto schema =
        longlineage::cli::load_json_strict(root / "schema" / "core" / "production_manifest.schema.json", error);
    if (!schema) {
        return fail(error);
    }
    std::string location;
    if (json_contains_truth_token(schema.get(), location)) {
        return fail("production manifest schema leaks a truth-aware token at " + location);
    }
    if (!json_is_false(json_object_get(schema.get(), "additionalProperties"))) {
        return fail("production manifest schema must set top-level additionalProperties=false");
    }
    if (!optional_manifest.empty()) {
        const auto manifest = longlineage::cli::load_json_strict(optional_manifest, error);
        if (!manifest) {
            return fail(error);
        }
        if (json_contains_truth_token(manifest.get(), location)) {
            return fail("production manifest contains a truth-aware token at " + location);
        }
        const auto shape = longlineage::cli::validate_production_manifest(manifest.get());
        if (!shape.ok) {
            return shape;
        }
    }
    return pass(optional_manifest.empty() ? "production schema is closed and truth-isolated"
                                          : "production schema and supplied manifest are closed and truth-isolated");
}

CheckResult check_cold_start(const std::filesystem::path& root) {
    const std::vector<std::filesystem::path> required = {
        "README.md",
        "AGENTS.md",
        "docs/claims/CLAIM_BOUNDARY.md",
        "docs/CURRENT_FOCUS.md",
        "ROADMAP.md",
        "docs/development/WORKFLOW.md",
        "docs/release/RELEASE_GATES.md",
        "docs/data/RECORD_AND_QUERY_STANDARD.zh-TW.md",
        "docs/data/DATA_CONTRACTS.md",
        "docs/data/QUERY_GUIDE.md",
        "schema/catalog.json",
        "schema/id_registry.json",
        "schema/core/contract_registry_bindings.schema.json",
        "schema/core/source_to_target_manifest.schema.json",
        "docs/decisions/ADR-0001-trust-boundaries.md",
        "docs/development/implementation-notes.md",
        ".ai/templates/task.json",
        "governance/agent_task.schema.json",
        "governance/audit_evidence.schema.json",
        "governance/phase_ledger.schema.json",
        "governance/project_state.schema.json",
        "contracts/v1/artifact_roles.tsv",
        "contracts/v1/lifecycle_codes.tsv",
        "contracts/v1/query_operators.tsv",
        "contracts/v1/status_reason_codes.tsv",
        "contracts/v1/transform_registry.tsv",
        "contracts/v1/type_registry.tsv",
        "oracle/production_input_authority.json",
        "schema/core/production_input_authority.schema.json",
        "schema/core/release_attestation.schema.json",
        "state/release_attestation.json",
    };
    for (const auto& relative : required) {
        if (!std::filesystem::is_regular_file(root / relative)) {
            return fail("cold-start source of truth is missing: " + relative.string());
        }
    }
    return pass("cold-start Q1-Q5 source-of-truth files are present");
}

void usage() {
    std::cout << "Usage: longlineage-governance COMMAND [--repo DIR] [--manifest FILE]\n"
              << "Commands: check-all, check-cold-start, check-policy, check-state,\n"
              << "          check-catalog, check-status-codes, check-gates,\n"
              << "          check-truth-boundary\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && longlineage::cli::is_help_flag(argv[1])) {
        usage();
        return static_cast<int>(ExitCode::Success);
    }
    if (argc < 2) {
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }
    const std::string command = argv[1];
    std::filesystem::path explicit_root;
    std::filesystem::path manifest;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if ((option != "--repo" && option != "--manifest") || index + 1 >= argc) {
            longlineage::cli::emit_error(command, ExitCode::UsageError, "unknown option or missing value: " + option);
            return static_cast<int>(ExitCode::UsageError);
        }
        auto& target = option == "--repo" ? explicit_root : manifest;
        if (!target.empty()) {
            longlineage::cli::emit_error(command, ExitCode::UsageError, "duplicate option: " + option);
            return static_cast<int>(ExitCode::UsageError);
        }
        target = argv[++index];
    }

    const auto root = longlineage::cli::find_repo_root(explicit_root);
    if (root.empty()) {
        longlineage::cli::emit_error(command, ExitCode::IoError, "cannot locate repository source-of-truth root");
        return static_cast<int>(ExitCode::IoError);
    }

    std::vector<std::pair<std::string, CheckResult>> checks;
    if (command == "check-all" || command == "check-cold-start") {
        checks.emplace_back("check-cold-start", check_cold_start(root));
    }
    if (command == "check-all" || command == "check-policy") {
        checks.emplace_back("check-policy", check_policy(root));
    }
    if (command == "check-all" || command == "check-state") {
        checks.emplace_back("check-state", check_state(root));
    }
    if (command == "check-all" || command == "check-catalog") {
        checks.emplace_back("check-catalog", check_catalog(root));
    }
    if (command == "check-all" || command == "check-status-codes") {
        checks.emplace_back("check-status-codes", check_status_codes(root));
    }
    if (command == "check-all" || command == "check-gates") {
        checks.emplace_back("check-gates", check_gate_registry(root));
    }
    if (command == "check-all" || command == "check-truth-boundary") {
        checks.emplace_back("check-truth-boundary", check_truth_boundary(root, manifest));
    }
    if (checks.empty()) {
        longlineage::cli::emit_error(command, ExitCode::UsageError, "unknown governance command");
        usage();
        return static_cast<int>(ExitCode::UsageError);
    }

    bool all_pass = true;
    for (const auto& [name, result] : checks) {
        longlineage::cli::emit_result(name, result);
        all_pass = all_pass && result.ok;
    }
    return all_pass ? static_cast<int>(ExitCode::Success) : static_cast<int>(ExitCode::SchemaError);
}
