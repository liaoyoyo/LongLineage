// SPDX-License-Identifier: GPL-3.0-only

#include <htslib/bgzf.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/audit/hcc1395_determinism.hpp"

#ifndef LONGLINEAGE_SOURCE_DIR
#error "LONGLINEAGE_SOURCE_DIR must bind the synthetic case manifest"
#endif

namespace {

class TestFailure final : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

void check(bool condition, const std::string& detail) {
    if (!condition) {
        throw TestFailure(detail);
    }
}

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};
using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

struct EvpDeleter {
    void operator()(EVP_MD_CTX* value) const noexcept { EVP_MD_CTX_free(value); }
};
using EvpPtr = std::unique_ptr<EVP_MD_CTX, EvpDeleter>;

std::string sha256_bytes(std::string_view bytes) {
    EvpPtr context(EVP_MD_CTX_new());
    check(context != nullptr && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1 &&
              (bytes.empty() || EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) == 1),
          "cannot initialize/update test SHA-256");
    std::array<unsigned char, 32> raw{};
    unsigned int size = 0;
    check(EVP_DigestFinal_ex(context.get(), raw.data(), &size) == 1 && size == raw.size(),
          "cannot finalize test SHA-256");
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char value : raw) {
        result.push_back(kHex[value >> 4U]);
        result.push_back(kHex[value & 0x0fU]);
    }
    return result;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot read test file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string sha256_file(const std::filesystem::path& path) { return sha256_bytes(read_file(path)); }

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(output.good(), "cannot create test file: " + path.string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(output.good(), "cannot close test file: " + path.string());
}

std::string dump_json(const json_t* value) {
    char* encoded = json_dumps(value, JSON_INDENT(2) | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
    check(encoded != nullptr, "cannot encode test JSON");
    std::string result(encoded);
    std::free(encoded);
    result.push_back('\n');
    return result;
}

void write_json(const std::filesystem::path& path, const json_t* value) { write_file(path, dump_json(value)); }

void write_bgzf(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    BGZF* output = bgzf_open(path.c_str(), "w");
    check(output != nullptr, "cannot create test BGZF");
    check(bgzf_write(output, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()),
          "cannot write test BGZF");
    check(bgzf_close(output) == 0, "cannot close test BGZF");
}

void write_gzip(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    gzFile output = gzopen(path.c_str(), "wb9");
    check(output != nullptr, "cannot create test gzip");
    check(gzwrite(output, bytes.data(), static_cast<unsigned int>(bytes.size())) == static_cast<int>(bytes.size()),
          "cannot write test gzip");
    check(gzclose(output) == Z_OK, "cannot close test gzip");
}

class Scratch {
   public:
    Scratch() {
        std::array<char, 64> pattern{};
        const std::string prefix = "/tmp/longlineage-hcc-audit-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        char* created = ::mkdtemp(pattern.data());
        check(created != nullptr, "cannot create test scratch directory");
        path_ = created;
    }

    ~Scratch() {
        if (preserve_) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    void preserve() { preserve_ = true; }

   private:
    std::filesystem::path path_;
    bool preserve_{false};
};

enum class Fault {
    kNone,
    kFailedValidation,
    kNonFrozen,
    kTruthFields,
    kPathEscape,
    kSymlink,
    kManifestDifference,
    kWrongWorkerCount,
    kSemanticMismatch,
    kSummaryProjection,
    kTopologyRegionPartitionMismatch,
    kM2DoubleCount,
    kM2Duplicate,
    kM2Missing,
    kM2Extra,
    kNativePreambleMissing,
    kNativePreambleWrong,
    kNativePreambleOrder,
    kNativePreambleDuplicateHeader,
    kNativeVersionWrong,
    kNativeRunIdMismatch,
    kNativeHeaderMissingMarker,
    kNativeHeaderMissingColumn,
    kMissingScientificConservation,
    kEmptyExactFamily,
    kPairWorkerMismatch,
    kPairDuplicateKey,
    kPairOutOfOrder,
    kPairCounterOverflow,
    kPairUnknownFamily,
    kPairInternalExactStatus,
    kPairFamilyExactStatusMismatch,
    kPairByOutsideFamily,
    kPairByWithoutBh,
    kPairFormalWithoutBy,
    kPairFamilySizeMismatch,
    kPairSiteAggregateMismatch,
    kJointUnknownStatus,
    kJointTopologyMismatch,
    kHistoricalSha,
    kHistoricalDuplicate,
    kHistoricalMissing,
    kHistoricalStatus,
};

struct PositiveCase {
    std::string_view case_id;
    void (*execute)();
};

struct NegativeCase {
    Fault fault;
    std::string_view case_id;
    std::string_view expected_error_code;
    std::string_view expected_detail_contains{};
};

void check_exact_object_keys(const json_t* value, std::initializer_list<const char*> expected, std::string_view label) {
    check(json_is_object(value), std::string(label) + " must be an object");
    std::set<std::string> expected_keys;
    for (const char* key : expected) {
        expected_keys.emplace(key);
    }
    check(json_object_size(value) == expected_keys.size(),
          std::string(label) + " keys differ from the closed contract");
    for (const std::string& key : expected_keys) {
        check(json_object_get(value, key.c_str()) != nullptr,
              std::string(label) + " lacks required key or contains an unknown key: " + key);
    }
}

std::string required_string(const json_t* object, const char* key, std::string_view label) {
    const json_t* value = json_object_get(object, key);
    check(json_is_string(value), std::string(label) + "." + key + " must be a string");
    return json_string_value(value);
}

json_int_t required_integer(const json_t* object, const char* key, std::string_view label) {
    const json_t* value = json_object_get(object, key);
    check(json_is_integer(value), std::string(label) + "." + key + " must be an integer");
    return json_integer_value(value);
}

JsonPtr load_case_manifest(const std::filesystem::path& path) {
    check(std::filesystem::is_regular_file(path) && !std::filesystem::is_symlink(std::filesystem::symlink_status(path)),
          "case manifest must be a regular non-symlink file: " + path.string());
    check(std::filesystem::file_size(path) <= 1024U * 1024U, "case manifest exceeds the 1 MiB synthetic limit");
    json_error_t error{};
    JsonPtr root(json_load_file(path.c_str(), JSON_REJECT_DUPLICATES, &error));
    check(root != nullptr,
          "cannot strict-parse case manifest: " + path.string() + ":" + std::to_string(error.line) + ": " + error.text);
    return root;
}

std::vector<std::string> bind_case_manifest(const json_t* root, const std::vector<PositiveCase>& positives,
                                            const std::vector<NegativeCase>& negatives) {
    check_exact_object_keys(
        root, {"schema_name", "schema_version", "scope", "production_claim_allowed", "positive", "negative", "notes"},
        "case manifest");
    check(required_string(root, "schema_name", "case manifest") == "longlineage.synthetic_hcc1395_audit_cases" &&
              required_string(root, "schema_version", "case manifest") == "1.1.0" &&
              required_string(root, "scope", "case manifest") == "SYNTHETIC_ONLY",
          "case manifest identity/version/scope mismatch");
    check(json_is_false(json_object_get(root, "production_claim_allowed")),
          "case manifest must forbid production claims");

    const json_t* documented_positives = json_object_get(root, "positive");
    const json_t* documented_negatives = json_object_get(root, "negative");
    const json_t* notes = json_object_get(root, "notes");
    check(json_is_array(documented_positives) && json_is_array(documented_negatives) && json_is_array(notes),
          "case manifest positive/negative/notes must be arrays");
    check(positives.size() == 2U && negatives.size() == 41U,
          "compiled case vector must contain exactly 2 positive and 41 negative cases");
    check(json_array_size(documented_positives) == positives.size() &&
              json_array_size(documented_negatives) == negatives.size(),
          "case manifest count differs from the compiled case vector");
    check(json_array_size(notes) > 0U, "case manifest notes must not be empty");
    for (std::size_t index = 0; index < json_array_size(notes); ++index) {
        check(json_is_string(json_array_get(notes, index)), "case manifest notes must contain only strings");
    }

    std::set<std::string> unique_ids;
    std::vector<std::string> documented_order;
    documented_order.reserve(positives.size() + negatives.size());
    for (std::size_t index = 0; index < positives.size(); ++index) {
        const json_t* row = json_array_get(documented_positives, index);
        const std::string label = "case manifest positive[" + std::to_string(index) + "]";
        check_exact_object_keys(row, {"case_id", "expected_exit", "expected_status"}, label);
        const std::string case_id = required_string(row, "case_id", label);
        check(case_id == positives[index].case_id, label + " ID/order differs from the compiled case vector");
        check(required_integer(row, "expected_exit", label) == 0 &&
                  required_string(row, "expected_status", label) == "PASS",
              label + " expected outcome differs from the compiled positive contract");
        check(unique_ids.emplace(case_id).second, "case manifest contains a duplicate case_id: " + case_id);
        documented_order.push_back(case_id);
    }
    for (std::size_t index = 0; index < negatives.size(); ++index) {
        const json_t* row = json_array_get(documented_negatives, index);
        const std::string label = "case manifest negative[" + std::to_string(index) + "]";
        const json_t* detail = json_object_get(row, "expected_detail_contains");
        if (detail == nullptr) {
            check_exact_object_keys(row, {"case_id", "expected_exit", "expected_error_code"}, label);
        } else {
            check_exact_object_keys(
                row, {"case_id", "expected_exit", "expected_error_code", "expected_detail_contains"}, label);
            check(json_is_string(detail), label + ".expected_detail_contains must be a string");
        }
        const std::string case_id = required_string(row, "case_id", label);
        const std::string expected_error_code = required_string(row, "expected_error_code", label);
        const std::string expected_detail = detail == nullptr ? "" : json_string_value(detail);
        check(case_id == negatives[index].case_id, label + " ID/order differs from the compiled case vector");
        check((detail != nullptr) == !negatives[index].expected_detail_contains.empty(),
              label + " expected_detail_contains presence differs from the compiled negative contract");
        check(required_integer(row, "expected_exit", label) == 1,
              label + " expected_exit differs from the compiled negative contract");
        check(expected_error_code == negatives[index].expected_error_code,
              label + " expected_error_code differs from the compiled negative contract");
        check(expected_detail == negatives[index].expected_detail_contains,
              label + " expected_detail_contains differs from the compiled negative contract");
        check(unique_ids.emplace(case_id).second, "case manifest contains a duplicate case_id: " + case_id);
        documented_order.push_back(case_id);
    }
    return documented_order;
}

template <typename Callback>
void expect_manifest_binding_rejection(Callback&& callback, std::string_view label) {
    bool rejected = false;
    try {
        callback();
    } catch (const TestFailure&) {
        rejected = true;
    }
    check(rejected, "case manifest binding negative passed: " + std::string(label));
}

void test_case_manifest_binding_negatives(const json_t* root, const std::vector<PositiveCase>& positives,
                                          const std::vector<NegativeCase>& negatives) {
    JsonPtr reordered(json_deep_copy(root));
    check(reordered != nullptr, "cannot clone case manifest for order negative");
    json_t* rows = json_object_get(reordered.get(), "negative");
    JsonPtr first(json_deep_copy(json_array_get(rows, 0U)));
    JsonPtr second(json_deep_copy(json_array_get(rows, 1U)));
    check(first != nullptr && second != nullptr, "cannot clone case rows for order negative");
    check(json_array_set_new(rows, 0U, second.release()) == 0 && json_array_set_new(rows, 1U, first.release()) == 0,
          "cannot mutate case order negative");
    expect_manifest_binding_rejection([&] { (void)bind_case_manifest(reordered.get(), positives, negatives); },
                                      "documented ID/order drift");

    JsonPtr wrong_error(json_deep_copy(root));
    check(wrong_error != nullptr, "cannot clone case manifest for error-code negative");
    json_t* first_negative = json_array_get(json_object_get(wrong_error.get(), "negative"), 0U);
    check(json_object_set_new(first_negative, "expected_error_code", json_string("WRONG_ERROR_CODE")) == 0,
          "cannot mutate case error-code negative");
    expect_manifest_binding_rejection([&] { (void)bind_case_manifest(wrong_error.get(), positives, negatives); },
                                      "documented expected_error_code drift");
}

constexpr const char* kProducerSha = "1111111111111111111111111111111111111111111111111111111111111111";
constexpr const char* kValidatorSha = "2222222222222222222222222222222222222222222222222222222222222222";
constexpr const char* kInputSnapshot = "3333333333333333333333333333333333333333333333333333333333333333";
constexpr const char* kCatalogSha = "4444444444444444444444444444444444444444444444444444444444444444";
constexpr const char* kScienceSha = "5555555555555555555555555555555555555555555555555555555555555555";
constexpr const char* kInputMountSha = "6666666666666666666666666666666666666666666666666666666666666666";
constexpr const char* kInputLockSha = "7777777777777777777777777777777777777777777777777777777777777777";
constexpr const char* kPhaseLedgerSha = "8888888888888888888888888888888888888888888888888888888888888888";

struct Artifact {
    std::string id;
    std::string relative;
    std::string schema_name;
    std::string schema_version;
    std::string format;
    std::uint64_t rows{0};
    std::string physical;
    std::string semantic;
    std::uint64_t size{0};
};

struct BuiltRun {
    std::filesystem::path root;
    std::filesystem::path manifest;
};

JsonPtr string_array(const std::vector<std::string>& values) {
    JsonPtr array(json_array());
    for (const std::string& value : values) {
        json_array_append_new(array.get(), json_string(value.c_str()));
    }
    return array;
}

JsonPtr executable_json() {
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "name", json_string("longlineage"));
    json_object_set_new(value.get(), "version", json_string("0.1.0"));
    json_object_set_new(value.get(), "git_commit", json_string("9999999999999999999999999999999999999999"));
    json_object_set_new(value.get(), "executable_sha256", json_string(kProducerSha));
    json_object_set_new(value.get(), "compiler", json_string("GNU-11.4.0"));
    json_object_set_new(value.get(), "htslib_version", json_string("1.18"));
    return value;
}

JsonPtr performance_json() {
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "wall_seconds", json_real(1.0));
    json_object_set_new(value.get(), "user_seconds", json_real(1.0));
    json_object_set_new(value.get(), "system_seconds", json_real(0.1));
    json_object_set_new(value.get(), "memory_peak_bytes", json_integer(1024));
    json_object_set_new(value.get(), "oom_events", json_integer(0));
    json_object_set_new(value.get(), "io_read_bytes", json_integer(1024));
    json_object_set_new(value.get(), "io_write_bytes", json_integer(1024));
    json_object_set_new(value.get(), "major_page_faults", json_integer(0));
    json_object_set_new(value.get(), "minor_page_faults", json_integer(1));
    json_object_set_new(value.get(), "peak_threads", json_integer(4));
    json_object_set_new(value.get(), "queue_wait_seconds", json_real(0.0));
    json_object_set_new(value.get(), "reorder_wait_seconds", json_real(0.0));
    JsonPtr latency(json_object());
    json_object_set_new(latency.get(), "p50", json_real(0.0));
    json_object_set_new(latency.get(), "p95", json_real(0.0));
    json_object_set_new(latency.get(), "p99", json_real(0.0));
    json_object_set_new(latency.get(), "max", json_real(0.0));
    json_object_set_new(value.get(), "task_latency_seconds", latency.release());
    json_object_set_new(value.get(), "logical_records", json_integer(6));
    json_object_set_new(value.get(), "logical_bytes", json_integer(1024));
    json_object_set_new(value.get(), "final_file_count", json_integer(12));
    json_object_set_new(value.get(), "transient_file_count", json_integer(0));
    json_object_set_new(value.get(), "cache_condition", json_string("UNKNOWN"));
    return value;
}

JsonPtr artifact_json(const Artifact& artifact) {
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "artifact_id", json_string(artifact.id.c_str()));
    json_object_set_new(value.get(), "role", json_string(artifact.id.c_str()));
    json_object_set_new(value.get(), "relative_path", json_string(artifact.relative.c_str()));
    json_object_set_new(value.get(), "schema_name", json_string(artifact.schema_name.c_str()));
    json_object_set_new(value.get(), "schema_version", json_string(artifact.schema_version.c_str()));
    json_object_set_new(value.get(), "format", json_string(artifact.format.c_str()));
    json_object_set_new(value.get(), "size_bytes", json_integer(static_cast<json_int_t>(artifact.size)));
    json_object_set_new(value.get(), "physical_sha256", json_string(artifact.physical.c_str()));
    json_object_set_new(value.get(), "logical_rows", json_integer(static_cast<json_int_t>(artifact.rows)));
    json_object_set_new(value.get(), "semantic_sha256", json_string(artifact.semantic.c_str()));
    json_object_set_new(value.get(), "index", json_null());
    json_object_set_new(value.get(), "sensitivity", json_string("SYNTHETIC_PUBLIC"));
    json_object_set_new(value.get(), "transform_id", json_string("synthetic_transform"));
    json_object_set_new(value.get(), "producer_executable_sha256", json_string(kProducerSha));
    json_object_set_new(value.get(), "inputs", json_array());
    json_object_set_new(value.get(), "primary_key_first", json_null());
    json_object_set_new(value.get(), "primary_key_last", json_null());
    return value;
}

JsonPtr manifest_json(const std::filesystem::path& base, const std::string& run_id, std::uint64_t workers,
                      bool different_buffer) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "schema_name", json_string("longlineage.production_manifest"));
    json_object_set_new(root.get(), "schema_version", json_string("1.1.0"));
    json_object_set_new(root.get(), "authority_profile", json_string("HCC1395_DATASET_GATE"));
    json_object_set_new(root.get(), "run_id", json_string(run_id.c_str()));
    const std::filesystem::path staging = base / ".staging" / run_id;
    json_object_set_new(root.get(), "output_root", json_string(staging.c_str()));

    JsonPtr datasets(json_array());
    JsonPtr dataset(json_object());
    json_object_set_new(dataset.get(), "dataset_id", json_string("HCC1395"));
    json_object_set_new(dataset.get(), "dataset_order", json_integer(0));
    JsonPtr files(json_array());
    const std::vector<std::string> roles = {
        "raw_bam",
        "raw_bam_index",
        "pass_biallelic_ssnv_vcf",
        "pass_biallelic_ssnv_vcf_index",
        "latest_hp_ps_sidecar",
        "latest_hp_ps_sidecar_index",
        "reference_fasta",
        "reference_fai",
    };
    for (const std::string& role : roles) {
        JsonPtr file(json_object());
        json_object_set_new(file.get(), "role", json_string(role.c_str()));
        const std::string input = "/synthetic/HCC1395/" + role;
        json_object_set_new(file.get(), "path", json_string(input.c_str()));
        json_object_set_new(file.get(), "size_bytes", json_integer(1));
        const std::string digest = sha256_bytes(role);
        json_object_set_new(file.get(), "sha256", json_string(digest.c_str()));
        json_array_append_new(files.get(), file.release());
    }
    json_object_set_new(dataset.get(), "files", files.release());
    json_array_append_new(datasets.get(), dataset.release());
    json_object_set_new(root.get(), "datasets", datasets.release());

    JsonPtr runtime(json_object());
    json_object_set_new(runtime.get(), "compute_workers", json_integer(static_cast<json_int_t>(workers)));
    json_object_set_new(runtime.get(), "writer_threads", json_integer(4));
    json_object_set_new(runtime.get(), "coordinator_slots", json_integer(2));
    json_object_set_new(runtime.get(), "buffer_bytes", json_integer(different_buffer ? 2097152 : 1048576));
    json_object_set_new(runtime.get(), "max_focal_sites_per_block", json_integer(64));
    json_object_set_new(runtime.get(), "max_estimated_alignments_per_block", json_integer(1000));
    json_object_set_new(runtime.get(), "halo_bp", json_integer(5000));
    json_object_set_new(root.get(), "runtime", runtime.release());

    JsonPtr bindings(json_object());
    for (const char* key :
         {"science_parameters_sha256", "schema_catalog_sha256", "status_reason_registry_sha256", "type_registry_sha256",
          "transform_registry_sha256", "authority_manifest_sha256", "source_to_target_manifest_sha256",
          "production_input_authority_sha256", "dataset_gate_input_authority_sha256", "schema_id_registry_sha256",
          "release_attestation_sha256"}) {
        const std::string digest = sha256_bytes(key);
        json_object_set_new(bindings.get(), key, json_string(digest.c_str()));
    }
    json_object_set_new(root.get(), "contract_bindings", bindings.release());
    return root;
}

struct SiteRow {
    std::uint64_t order;
    std::string status;
    bool stable;
    std::string groups;
    std::string m2_status;
    std::string m2_reason;
};

std::vector<SiteRow> site_rows() {
    return {
        {0U, "EVALUABLE", true, "2", "PASS", "ALL_MEASURED_AXES_DETERMINATE_NO_ALIGNED_CONFOUND"},
        {1U, "EVALUABLE", true, "2", "FAIL", "HP_AXIS_CONFOUND"},
        {2U, "EVALUABLE", true, "2", "NOT_EVALUABLE", "AXIS_INDETERMINATE"},
        {3U, "EVALUABLE", true, "11", "NOT_EVALUABLE", "GROUP_COUNT_EXCEEDS_PLANNING_MODEL_MAXIMUM"},
        {4U, "EVALUABLE", false, ".", "NOT_RUN", "M1_NOT_FLAGGED"},
        {5U, "INSUFFICIENT_ALT_READS", false, ".", "NOT_RUN", "M1_NOT_FLAGGED"},
    };
}

std::string base_for_order(std::uint64_t order, bool ref) {
    static constexpr std::array<const char*, 4> kBases = {"A", "C", "G", "T"};
    return kBases[(order + (ref ? 0U : 1U)) % kBases.size()];
}

std::string m1_tsv(const std::vector<SiteRow>& rows, const std::string& run_id, Fault fault) {
    const std::string header =
        "dataset_order\tdataset_id\tsite_order\tchrom\tpos1\tref\talt\t"
        "analysis_status\tstable\tnon_germline_groups";
    std::string output;
    if (fault != Fault::kNativePreambleMissing) {
        output += "##longlineage_schema=longlineage.m1_sites\n";
    }
    output += "##schema_version=1.0.0\n##run_id=" + run_id + "\n#" + header + "\n";
    for (const SiteRow& row : rows) {
        output += "0\tHCC1395\t" + std::to_string(row.order) + "\tchrSynthetic\t" + std::to_string(100U + row.order) +
                  "\t" + base_for_order(row.order, true) + "\t" + base_for_order(row.order, false) + "\t" + row.status +
                  "\t" + (row.stable ? "true" : "false") + "\t" + row.groups + "\n";
    }
    return output;
}

std::string cooccurrence_tsv(std::vector<SiteRow> rows, const std::string& run_id, Fault fault) {
    if (fault == Fault::kM2Missing) {
        rows.pop_back();
    } else if (fault == Fault::kM2Extra) {
        rows.push_back({99U, "EVALUABLE", false, ".", "NOT_RUN", "M1_NOT_FLAGGED"});
    } else if (fault == Fault::kM2Duplicate) {
        rows.push_back(rows.front());
        rows.back().m2_status = "FAIL";
        rows.back().m2_reason = "HP_AXIS_CONFOUND";
    }
    const std::string header =
        "dataset_order\tdataset_id\tsite_order\tchrom\tpos1\tref\talt\t"
        "m1_group_count\tm2_status\tm2_reason\tpartner_universe_size\t"
        "exact_testable_pairs\tglobal_bh_discoveries\tglobal_by_discoveries\tjoint_signature_status";
    const std::string preamble_run_id = fault == Fault::kNativeRunIdMismatch ? "synthetic-wrong-run" : run_id;
    std::string output =
        "##longlineage_schema=" +
        std::string(fault == Fault::kNativePreambleWrong ? "longlineage.wrong" : "longlineage.cooccurrence_sites") +
        "\n##schema_version=1.0.0\n##run_id=" + preamble_run_id + "\n#" + header + "\n";
    for (const SiteRow& row : rows) {
        const bool first = row.order == 0U;
        std::string partner_rows = "0";
        if (first) {
            partner_rows = fault == Fault::kPairCounterOverflow
                               ? std::to_string(std::numeric_limits<std::uint64_t>::max())
                               : std::to_string(fault == Fault::kPairSiteAggregateMismatch ? 8U : 9U);
        } else if (row.order == 1U && fault == Fault::kPairCounterOverflow) {
            partner_rows = "1";
        }
        const bool empty_exact_family = fault == Fault::kEmptyExactFamily;
        std::string joint_status =
            first && !empty_exact_family ? "PASS" : "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE";
        if (first && fault == Fault::kJointUnknownStatus) {
            joint_status = "UNKNOWN_JOINT_STATUS";
        } else if (first && fault == Fault::kJointTopologyMismatch) {
            joint_status = "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE";
        }
        output += "0\tHCC1395\t" + std::to_string(row.order) + "\tchrSynthetic\t" + std::to_string(100U + row.order) +
                  "\t" + base_for_order(row.order, true) + "\t" + base_for_order(row.order, false) + "\t" + row.groups +
                  "\t" + row.m2_status + "\t" + row.m2_reason + "\t" + partner_rows + "\t" +
                  std::to_string(first ? 5U : 0U) + "\t" + std::to_string(first && !empty_exact_family ? 3U : 0U) +
                  "\t" + std::to_string(first && !empty_exact_family ? 2U : 0U) + "\t" + joint_status + "\n";
    }
    return output;
}

struct PairRow {
    std::uint64_t partner_order;
    std::string exact_status;
    std::string family_status;
    std::string fdr_family_id;
    std::string fdr_family_size;
    bool exact_bh;
    bool exact_by;
    bool formal;
};

std::vector<PairRow> pair_rows() {
    return {
        {0U, "EXACT_IDENTIFIABLE", "INELIGIBLE_M2_SCREEN", ".", ".", false, false, false},
        {1U, "NOT_IDENTIFIABLE_ENDPOINT_A_NOT_TESTABLE", "INELIGIBLE_M2_SCREEN", ".", ".", false, false, false},
        {2U, "EXACT_IDENTIFIABLE", "INELIGIBLE_M2_SCREEN", ".", ".", false, false, false},
        {3U, "NOT_IDENTIFIABLE_ENDPOINT_A_NOT_TESTABLE", "ELIGIBLE_M2_ENDPOINT_A_NOT_TESTABLE", ".", ".", false, false,
         false},
        {4U, "NOT_IDENTIFIABLE_ENDPOINT_A_NOT_TESTABLE", "ELIGIBLE_M2_ENDPOINT_A_NOT_TESTABLE", ".", ".", false, false,
         false},
        {5U, "NOT_IDENTIFIABLE_STATE_SPACE_LIMIT", "ELIGIBLE_M2_EXACT_NOT_IDENTIFIABLE", ".", ".", false, false, false},
        {6U, "EXACT_IDENTIFIABLE", "ELIGIBLE_M2_EXACT_FAMILY", "GLOBAL_M2_ELIGIBLE_ENDPOINT_A_EXACT_V1", "3", true,
         true, true},
        {7U, "EXACT_IDENTIFIABLE", "ELIGIBLE_M2_EXACT_FAMILY", "GLOBAL_M2_ELIGIBLE_ENDPOINT_A_EXACT_V1", "3", true,
         true, false},
        {8U, "EXACT_IDENTIFIABLE", "ELIGIBLE_M2_EXACT_FAMILY", "GLOBAL_M2_ELIGIBLE_ENDPOINT_A_EXACT_V1", "3", true,
         false, false},
    };
}

std::string pair_tsv(const std::string& run_id, Fault fault, bool is_w24) {
    std::vector<PairRow> rows = pair_rows();
    if (fault == Fault::kEmptyExactFamily) {
        for (std::size_t index = 6U; index < rows.size(); ++index) {
            rows[index].family_status = "INELIGIBLE_M2_SCREEN";
            rows[index].fdr_family_id = ".";
            rows[index].fdr_family_size = ".";
            rows[index].exact_bh = false;
            rows[index].exact_by = false;
            rows[index].formal = false;
        }
    }
    if (!is_w24 && fault == Fault::kPairWorkerMismatch) {
        rows[6].formal = false;
    }
    if (is_w24 && fault == Fault::kPairDuplicateKey) {
        rows[1].partner_order = rows[0].partner_order;
    }
    if (is_w24 && fault == Fault::kPairOutOfOrder) {
        std::swap(rows[1], rows[2]);
    }
    if (is_w24 && fault == Fault::kPairUnknownFamily) {
        rows[0].family_status = "UNKNOWN_FAMILY";
    }
    if (is_w24 && fault == Fault::kPairInternalExactStatus) {
        rows[0].exact_status = "EXACT_ENUMERATED";
    }
    if (is_w24 && fault == Fault::kPairFamilyExactStatusMismatch) {
        rows[3].exact_status = "NOT_IDENTIFIABLE_STATE_SPACE_LIMIT";
    }
    if (is_w24 && fault == Fault::kPairByOutsideFamily) {
        rows[0].exact_bh = true;
        rows[0].exact_by = true;
    }
    if (is_w24 && fault == Fault::kPairByWithoutBh) {
        rows[8].exact_bh = false;
        rows[8].exact_by = true;
    }
    if (is_w24 && fault == Fault::kPairFormalWithoutBy) {
        rows[8].formal = true;
    }
    if (is_w24 && fault == Fault::kPairFamilySizeMismatch) {
        rows[8].fdr_family_size = "2";
    }
    std::string header =
        "dataset_order\tdataset_id\tfocal_site_order\tpartner_site_order\texact_status\tfamily_status\t"
        "fdr_family_id\tfdr_family_size\texact_bh_discovery\texact_by_discovery\tformal_pair_by_confirmed";
    if (is_w24 && fault == Fault::kNativeHeaderMissingColumn) {
        header = header.substr(0U, header.rfind('\t'));
    }
    std::string output;
    if (is_w24 && fault == Fault::kNativePreambleOrder) {
        output = "##schema_version=1.0.1\n##longlineage_schema=longlineage.cooccurrence_pairs\n";
    } else {
        output = "##longlineage_schema=longlineage.cooccurrence_pairs\n##schema_version=" +
                 std::string(is_w24 && fault == Fault::kNativeVersionWrong ? "1.0.0" : "1.0.1") + "\n";
    }
    output += "##run_id=" + run_id + "\n" +
              std::string(is_w24 && fault == Fault::kNativeHeaderMissingMarker ? "" : "#") + header + "\n";
    if (is_w24 && fault == Fault::kNativePreambleDuplicateHeader) {
        output += "#" + header + "\n";
    }
    for (const PairRow& row : rows) {
        output += "0\tHCC1395\t0\t" + std::to_string(row.partner_order) + "\t" + row.exact_status + "\t" +
                  row.family_status + "\t" + row.fdr_family_id + "\t" + row.fdr_family_size + "\t" +
                  (row.exact_bh ? "true" : "false") + "\t" + (row.exact_by ? "true" : "false");
        if (!(is_w24 && fault == Fault::kNativeHeaderMissingColumn)) {
            output += "\t" + std::string(row.formal ? "true" : "false");
        }
        output += "\n";
    }
    return output;
}

JsonPtr summary_json(const std::string& run_id, Fault fault, bool fault_applies) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "schema_name", json_string("longlineage.summary"));
    json_object_set_new(root.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(root.get(), "run_id", json_string(run_id.c_str()));
    JsonPtr scope(json_object());
    json_object_set_new(scope.get(), "task_type", json_string("B"));
    json_object_set_new(scope.get(), "completeness", json_string("FULL"));
    json_object_set_new(scope.get(), "dataset_count", json_integer(1));
    json_object_set_new(scope.get(), "dataset_ids", string_array({"HCC1395"}).release());
    json_object_set_new(scope.get(), "site_population", json_string("SYNTHETIC_HCC1395_AUTOSOMES"));
    json_object_set_new(root.get(), "scope", scope.release());

    std::map<std::string, std::uint64_t> counts = {
        {"site_keys", 6U},
        {"site_keys_missing", 0U},
        {"site_keys_extra", 0U},
        {"site_keys_duplicate", 0U},
        {"m1_evaluable", 5U},
        {"m1_insufficient_alt_reads", 1U},
        {"m1_incomplete_distance", 0U},
        {"m1_stable_assignments", 4U},
        {"latest_tag_exact_joins", 12U},
        {"latest_tag_missing", 0U},
        {"latest_tag_conflict", 0U},
        {"latest_tag_multimatch", 0U},
        {"m2_eligible", 1U},
        {"m2_evaluable_ineligible", 1U},
        {"m2_axis_indeterminate", 1U},
        {"m2_group_count_gt10", 1U},
        {"raw_expected", 10U},
        {"raw_matched", 12U},
        {"raw_rg_only_duplicate_occurrences", 2U},
        {"topology_primary_hp_units", 1U},
        {"topology_regions", 2U},
        {"topology_fully_complete_regions", 1U},
        {"topology_incomplete_regions", 1U},
        {"topology_incomplete_units_with_winner", 0U},
    };
    if (fault_applies && fault == Fault::kM2DoubleCount) {
        counts["m2_evaluable_ineligible"] = 2U;
    }
    if (fault_applies && fault == Fault::kSummaryProjection) {
        counts["raw_expected"] = 11U;
        counts["raw_matched"] = 13U;
    }
    if (fault_applies && fault == Fault::kTopologyRegionPartitionMismatch) {
        counts["topology_incomplete_regions"] = 0U;
    }
    if (fault == Fault::kEmptyExactFamily) {
        counts["topology_primary_hp_units"] = 0U;
        counts["topology_regions"] = 0U;
        counts["topology_fully_complete_regions"] = 0U;
        counts["topology_incomplete_regions"] = 0U;
    }
    JsonPtr encoded_counts(json_object());
    for (const auto& [key, value] : counts) {
        json_object_set_new(encoded_counts.get(), key.c_str(), json_integer(static_cast<json_int_t>(value)));
    }
    json_object_set_new(root.get(), "counts", encoded_counts.release());

    JsonPtr phases(json_object());
    for (const char* phase : {"P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"}) {
        json_object_set_new(phases.get(), phase, json_string("IN_PROGRESS"));
    }
    json_object_set_new(root.get(), "phase_status", phases.release());
    return root;
}

Artifact write_artifact(const std::filesystem::path& root, std::string id, std::string relative,
                        std::string schema_name, std::string schema_version, std::string format, std::uint64_t rows,
                        std::string_view bytes, bool bgzf, bool make_symlink) {
    Artifact artifact;
    artifact.id = std::move(id);
    artifact.relative = std::move(relative);
    artifact.schema_name = std::move(schema_name);
    artifact.schema_version = std::move(schema_version);
    artifact.format = std::move(format);
    artifact.rows = rows;
    const std::filesystem::path path = root / artifact.relative;
    if (make_symlink) {
        const std::filesystem::path target = root / "m1_sites.target.bgz";
        write_bgzf(target, bytes);
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::create_symlink(target.filename(), path);
    } else if (bgzf) {
        write_bgzf(path, bytes);
    } else {
        write_file(path, bytes);
    }
    artifact.size = std::filesystem::file_size(path);
    artifact.physical = sha256_file(path);
    artifact.semantic = sha256_bytes("semantic:" + artifact.id);
    return artifact;
}

JsonPtr mount_identity_json() {
    JsonPtr rows(json_array());
    const std::vector<std::string> roles = {
        "raw_bam",
        "raw_bam_index",
        "pass_biallelic_ssnv_vcf",
        "pass_biallelic_ssnv_vcf_index",
        "latest_hp_ps_sidecar",
        "latest_hp_ps_sidecar_index",
        "reference_fasta",
        "reference_fai",
    };
    for (const std::string& role : roles) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "dataset_id", json_string("HCC1395"));
        json_object_set_new(row.get(), "role", json_string(role.c_str()));
        const std::string path = "/synthetic/HCC1395/" + role;
        json_object_set_new(row.get(), "canonical_path", json_string(path.c_str()));
        json_object_set_new(row.get(), "mount_source", json_string("synthetic"));
        json_object_set_new(row.get(), "filesystem_type", json_string("tmpfs"));
        json_object_set_new(row.get(), "readonly", json_true());
        const std::string digest = sha256_bytes("mount:" + role);
        json_object_set_new(row.get(), "mount_options_sha256", json_string(digest.c_str()));
        json_array_append_new(rows.get(), row.release());
    }
    return rows;
}

JsonPtr producer_receipt_json(const std::string& run_id, const std::string& manifest_sha, json_t* artifacts,
                              bool truth_fields) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "schema_name", json_string("longlineage.producer_receipt"));
    json_object_set_new(root.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(root.get(), "run_id", json_string(run_id.c_str()));
    json_object_set_new(root.get(), "state", json_string("RUNNING"));
    json_object_set_new(root.get(), "producer_outcome", json_string("READY_FOR_VALIDATION"));
    json_object_set_new(root.get(), "producer_executable_sha256", json_string(kProducerSha));
    json_object_set_new(root.get(), "producer_hostname", json_string("synthetic-host"));
    json_object_set_new(root.get(), "producer_kernel_release", json_string("synthetic-kernel"));
    json_object_set_new(root.get(), "input_mount_identity", mount_identity_json().release());
    json_object_set_new(root.get(), "manifest_sha256", json_string(manifest_sha.c_str()));
    json_object_set_new(root.get(), "input_snapshot_before_sha256", json_string(kInputSnapshot));
    json_object_set_new(root.get(), "input_snapshot_after_sha256", json_string(kInputSnapshot));
    json_object_set_new(root.get(), "schema_catalog_sha256", json_string(kCatalogSha));
    json_object_set_new(root.get(), "science_parameters_sha256", json_string(kScienceSha));
    JsonPtr draft(json_object());
    json_object_set_new(draft.get(), "production_executable", executable_json().release());
    json_object_set_new(draft.get(), "input_lock_sha256", json_string(kInputLockSha));
    json_object_set_new(draft.get(), "phase_ledger_sha256", json_string(kPhaseLedgerSha));
    json_object_set_new(draft.get(), "performance", performance_json().release());
    json_object_set_new(root.get(), "run_receipt_draft", draft.release());
    json_object_set(root.get(), "artifacts", artifacts);
    json_object_set_new(root.get(), "truth_fields_seen", json_integer(truth_fields ? 1 : 0));
    json_object_set_new(root.get(), "failure_reason", json_null());
    json_object_set_new(root.get(), "finished_at", json_string("2026-07-20T00:00:00+08:00"));
    return root;
}

JsonPtr validation_receipt_json(const std::string& run_id, const std::string& producer_sha, bool failed,
                                bool missing_scientific_conservation) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "schema_name", json_string("longlineage.validation_receipt"));
    json_object_set_new(root.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(root.get(), "run_id", json_string(run_id.c_str()));
    json_object_set_new(root.get(), "validation_profile", json_string("DATASET_GATE"));
    json_object_set_new(root.get(), "production_claim_allowed", json_false());
    json_object_set_new(root.get(), "producer_receipt_sha256", json_string(producer_sha.c_str()));
    json_object_set_new(root.get(), "producer_executable_sha256", json_string(kProducerSha));
    json_object_set_new(root.get(), "validator_executable_sha256", json_string(kValidatorSha));
    json_object_set_new(root.get(), "producer_hostname", json_string("synthetic-host"));
    json_object_set_new(root.get(), "producer_kernel_release", json_string("synthetic-kernel"));
    json_object_set_new(root.get(), "validator_hostname", json_string("synthetic-validator"));
    json_object_set_new(root.get(), "validator_kernel_release", json_string("synthetic-kernel"));
    json_object_set_new(root.get(), "input_mount_identity_sha256", json_string(kInputMountSha));
    json_object_set_new(root.get(), "schema_catalog_sha256", json_string(kCatalogSha));
    json_object_set_new(root.get(), "science_parameters_sha256", json_string(kScienceSha));
    json_object_set_new(root.get(), "all_pass", json_boolean(!failed));
    JsonPtr checks(json_array());
    JsonPtr check_row(json_object());
    json_object_set_new(check_row.get(), "check_id",
                        json_string(missing_scientific_conservation ? "SYNTHETIC_REPLAY" : "SCIENTIFIC_CONSERVATION"));
    json_object_set_new(check_row.get(), "status", json_string(failed ? "FAIL" : "PASS"));
    if (failed) {
        json_object_set_new(check_row.get(), "reason", json_string("synthetic failure"));
    } else {
        json_object_set_new(check_row.get(), "reason", json_null());
    }
    json_object_set_new(check_row.get(), "observed", json_string("synthetic"));
    json_object_set_new(check_row.get(), "expected", json_string("synthetic"));
    const std::string evidence = sha256_bytes("synthetic validation");
    json_object_set_new(check_row.get(), "evidence_sha256", json_string(evidence.c_str()));
    json_array_append_new(checks.get(), check_row.release());
    json_object_set_new(root.get(), "checks", checks.release());
    json_object_set_new(root.get(), "validated_at", json_string("2026-07-20T00:01:00+08:00"));
    json_object_set_new(root.get(), "validator_independent", json_true());
    json_object_set_new(root.get(), "linked_producer_kernels", json_false());
    json_object_set_new(root.get(), "input_snapshot_before_sha256", json_string(kInputSnapshot));
    json_object_set_new(root.get(), "input_snapshot_after_sha256", json_string(kInputSnapshot));
    return root;
}

JsonPtr state_history_json(bool non_frozen) {
    JsonPtr history(json_array());
    const std::array<const char*, 3> states = {"RUNNING", "VALIDATED",
                                               non_frozen ? "VALIDATED_FROZEN" : "VALIDATED_FROZEN_DATASET_GATE"};
    for (std::size_t index = 0; index < states.size(); ++index) {
        JsonPtr event(json_object());
        json_object_set_new(event.get(), "sequence", json_integer(static_cast<json_int_t>(index)));
        json_object_set_new(event.get(), "state", json_string(states[index]));
        json_object_set_new(event.get(), "at", json_string("2026-07-20T00:01:00+08:00"));
        json_object_set_new(event.get(), "actor_executable_sha256",
                            json_string(index == 0U ? kProducerSha : kValidatorSha));
        if (index == 0U) {
            json_object_set_new(event.get(), "previous_event_sha256", json_null());
        } else {
            const std::string prior = sha256_bytes("state" + std::to_string(index));
            json_object_set_new(event.get(), "previous_event_sha256", json_string(prior.c_str()));
        }
        json_object_set_new(event.get(), "reason", json_null());
        json_array_append_new(history.get(), event.release());
    }
    return history;
}

JsonPtr run_receipt_json(const std::string& run_id, const std::string& manifest_sha, const std::string& producer_sha,
                         const std::string& validation_sha, const std::string& checksums_sha, json_t* artifacts,
                         bool non_frozen, bool truth_fields) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "schema_name", json_string("longlineage.run_receipt"));
    json_object_set_new(root.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(root.get(), "run_id", json_string(run_id.c_str()));
    json_object_set_new(root.get(), "state",
                        json_string(non_frozen ? "VALIDATED_FROZEN" : "VALIDATED_FROZEN_DATASET_GATE"));
    json_object_set_new(root.get(), "validation_profile", json_string("DATASET_GATE"));
    json_object_set_new(root.get(), "production_claim_allowed", json_false());
    json_object_set_new(root.get(), "production_executable", executable_json().release());
    json_object_set_new(root.get(), "producer_hostname", json_string("synthetic-host"));
    json_object_set_new(root.get(), "producer_kernel_release", json_string("synthetic-kernel"));
    json_object_set_new(root.get(), "validator_hostname", json_string("synthetic-validator"));
    json_object_set_new(root.get(), "validator_kernel_release", json_string("synthetic-kernel"));
    json_object_set_new(root.get(), "input_mount_identity_sha256", json_string(kInputMountSha));
    json_object_set_new(root.get(), "manifest_sha256", json_string(manifest_sha.c_str()));
    json_object_set_new(root.get(), "input_lock_sha256", json_string(kInputLockSha));
    json_object_set_new(root.get(), "phase_ledger_sha256", json_string(kPhaseLedgerSha));
    json_object_set(root.get(), "artifacts", artifacts);
    json_object_set_new(root.get(), "truth_fields_seen", json_integer(truth_fields ? 1 : 0));
    json_object_set_new(root.get(), "input_snapshot_before_sha256", json_string(kInputSnapshot));
    json_object_set_new(root.get(), "input_snapshot_after_sha256", json_string(kInputSnapshot));
    json_object_set_new(root.get(), "schema_catalog_sha256", json_string(kCatalogSha));
    json_object_set_new(root.get(), "science_parameters_sha256", json_string(kScienceSha));
    json_object_set_new(root.get(), "state_history", state_history_json(non_frozen).release());
    json_object_set_new(root.get(), "performance", performance_json().release());
    json_object_set_new(root.get(), "producer_receipt_sha256", json_string(producer_sha.c_str()));
    json_object_set_new(root.get(), "validation_receipt_sha256", json_string(validation_sha.c_str()));
    json_object_set_new(root.get(), "checksums_sha256", json_string(checksums_sha.c_str()));
    return root;
}

bool is_m2_row_fault(Fault fault) {
    return fault == Fault::kM2Duplicate || fault == Fault::kM2Missing || fault == Fault::kM2Extra;
}

BuiltRun build_run(const std::filesystem::path& base, const std::string& run_id, std::uint64_t default_workers,
                   bool is_w24, Fault fault) {
    const bool w24_only = is_w24;
    const bool w40_only = !is_w24;
    std::uint64_t workers = default_workers;
    if (w40_only && fault == Fault::kWrongWorkerCount) {
        workers = 39U;
    }
    JsonPtr manifest = manifest_json(base, run_id, workers, w40_only && fault == Fault::kManifestDifference);
    const std::filesystem::path manifest_path = base / "manifests" / (run_id + ".json");
    write_json(manifest_path, manifest.get());
    const std::string manifest_sha = sha256_file(manifest_path);

    const std::filesystem::path root = base / run_id;
    std::filesystem::create_directories(root / "receipts");
    std::vector<Artifact> artifacts;
    const bool escape = w24_only && fault == Fault::kPathEscape;
    const std::string site_reads_relative = escape ? "../escaped-site-reads.tsv.bgz" : "site_reads.tsv.bgz";
    artifacts.push_back(write_artifact(root, "site_reads", site_reads_relative, "longlineage.site_reads", "1.0.0",
                                       "TSV_BGZF", 6U, "synthetic site reads " + run_id + "\n", true, false));
    artifacts.push_back(write_artifact(root, "methyl_calls", "methyl_calls.tsv.bgz", "longlineage.methyl_calls",
                                       "1.0.0", "TSV_BGZF", 6U, "synthetic methyl calls\n", true, false));
    artifacts.push_back(write_artifact(root, "bernoulli_upper", "bernoulli_upper.llm.bgz",
                                       "longlineage.bernoulli_upper", "1.0.0", "LLM_BGZF", 5U, "synthetic bernoulli\n",
                                       true, false));

    const auto rows = site_rows();
    const Fault w24_fault = is_w24 ? fault : Fault::kNone;
    artifacts.push_back(write_artifact(root, "m1_sites", "m1_sites.tsv.bgz", "longlineage.m1_sites", "1.0.0",
                                       "TSV_BGZF", 6U, m1_tsv(rows, run_id, w24_fault), true,
                                       w24_only && fault == Fault::kSymlink));
    artifacts.push_back(write_artifact(root, "m1_assignments", "m1_assignments.jsonl.bgz", "longlineage.m1_assignment",
                                       "1.0.0", "JSONL_BGZF", 5U, "{\"synthetic\":true}\n", true, false));
    artifacts.push_back(write_artifact(root, "cooccurrence_pairs", "cooccurrence_pairs.tsv.bgz",
                                       "longlineage.cooccurrence_pairs", "1.0.1", "TSV_BGZF", 9U,
                                       pair_tsv(run_id, fault, is_w24), true, false));
    const bool site_fault = is_m2_row_fault(fault) || fault == Fault::kNativePreambleWrong ||
                            fault == Fault::kNativeRunIdMismatch || fault == Fault::kPairSiteAggregateMismatch ||
                            fault == Fault::kJointUnknownStatus || fault == Fault::kJointTopologyMismatch ||
                            fault == Fault::kPairCounterOverflow || fault == Fault::kEmptyExactFamily;
    const bool cross_artifact_fault = fault == Fault::kPairSiteAggregateMismatch ||
                                      fault == Fault::kJointTopologyMismatch || fault == Fault::kEmptyExactFamily;
    const Fault cooccurrence_fault = site_fault && (is_w24 || cross_artifact_fault) ? fault : Fault::kNone;
    artifacts.push_back(write_artifact(root, "cooccurrence_sites", "cooccurrence_sites.tsv.bgz",
                                       "longlineage.cooccurrence_sites", "1.0.0", "TSV_BGZF", 6U,
                                       cooccurrence_tsv(rows, run_id, cooccurrence_fault), true, false));
    const std::uint64_t topology_rows = fault == Fault::kEmptyExactFamily ? 0U : 1U;
    const std::string_view topology_payload =
        topology_rows == 0U ? std::string_view{} : std::string_view{"{\"synthetic\":true}\n"};
    artifacts.push_back(write_artifact(root, "topology_units", "topology_units.jsonl.bgz", "longlineage.topology_unit",
                                       "2.0.0", "JSONL_BGZF", topology_rows, topology_payload, true, false));

    const bool summary_fault = fault == Fault::kM2DoubleCount || (w40_only && fault == Fault::kSummaryProjection) ||
                               (w24_only && fault == Fault::kTopologyRegionPartitionMismatch);
    JsonPtr summary = summary_json(run_id, fault, summary_fault);
    const std::string summary_bytes = dump_json(summary.get());
    artifacts.push_back(write_artifact(root, "summary", "summary.json", "longlineage.summary", "1.0.0", "JSON", 1U,
                                       summary_bytes, false, false));

    if (w40_only && fault == Fault::kSemanticMismatch) {
        artifacts.front().semantic = sha256_bytes("forged semantic mismatch");
    }
    std::sort(artifacts.begin(), artifacts.end(),
              [](const Artifact& left, const Artifact& right) { return left.id < right.id; });
    std::string semantic_tsv = "artifact_id\tschema_name\tschema_version\tlogical_rows\tsemantic_sha256\n";
    for (const Artifact& artifact : artifacts) {
        semantic_tsv += artifact.id + "\t" + artifact.schema_name + "\t" + artifact.schema_version + "\t" +
                        std::to_string(artifact.rows) + "\t" + artifact.semantic + "\n";
    }
    artifacts.push_back(write_artifact(root, "semantic_digests", "semantic_digests.tsv", "longlineage.semantic_digest",
                                       "1.0.0", "TSV", artifacts.size(), semantic_tsv, false, false));
    std::sort(artifacts.begin(), artifacts.end(),
              [](const Artifact& left, const Artifact& right) { return left.id < right.id; });

    JsonPtr artifact_rows(json_array());
    for (const Artifact& artifact : artifacts) {
        json_array_append_new(artifact_rows.get(), artifact_json(artifact).release());
    }
    const bool truth_fields = w24_only && fault == Fault::kTruthFields;
    JsonPtr producer = producer_receipt_json(run_id, manifest_sha, artifact_rows.get(), truth_fields);
    const std::filesystem::path producer_path = root / "receipts" / "producer_receipt.json";
    write_json(producer_path, producer.get());
    const std::string producer_sha = sha256_file(producer_path);

    std::map<std::string, std::string> checksums;
    for (const Artifact& artifact : artifacts) {
        checksums.emplace(artifact.relative, artifact.physical);
    }
    checksums.emplace("receipts/producer_receipt.json", producer_sha);
    std::string checksum_bytes;
    for (const auto& [relative, digest] : checksums) {
        checksum_bytes += digest + "  " + relative + "\n";
    }
    const std::filesystem::path checksum_path = root / "checksums.sha256";
    write_file(checksum_path, checksum_bytes);
    const std::string checksums_sha = sha256_file(checksum_path);

    const bool failed_validation = w24_only && fault == Fault::kFailedValidation;
    const bool missing_scientific_conservation = w24_only && fault == Fault::kMissingScientificConservation;
    JsonPtr validation =
        validation_receipt_json(run_id, producer_sha, failed_validation, missing_scientific_conservation);
    const std::filesystem::path validation_path = root / "validation_receipt.json";
    write_json(validation_path, validation.get());
    const std::string validation_sha = sha256_file(validation_path);

    const bool non_frozen = w24_only && fault == Fault::kNonFrozen;
    JsonPtr run = run_receipt_json(run_id, manifest_sha, producer_sha, validation_sha, checksums_sha,
                                   artifact_rows.get(), non_frozen, truth_fields);
    write_json(root / "run_receipt.json", run.get());
    return BuiltRun{root, manifest_path};
}

struct HistoricalBuilt {
    std::filesystem::path path;
    std::string sha256;
};

HistoricalBuilt build_historical(const std::filesystem::path& base, Fault fault) {
    const auto rows = site_rows();
    std::string tsv =
        "dataset\tsample\ttruth_label\tchrom\tpos\tref\talt\tanalysis_status\t"
        "stable_null_multigroup\n"
        "OTHER\tOTHER\tUNASSESSED\tchrSynthetic\t1\tA\tC\tevaluable\tFalse\n";
    const std::array<bool, 6> old_stable = {true, false, true, false, true, false};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (fault == Fault::kHistoricalMissing && index == rows.size() - 1U) {
            continue;
        }
        std::string status;
        if (rows[index].status == "EVALUABLE") {
            status = "evaluable";
        } else if (rows[index].status == "INSUFFICIENT_ALT_READS") {
            status = "insufficient_alt_reads";
        } else {
            status = "incomplete_distance_below_minimum";
        }
        if (fault == Fault::kHistoricalStatus && index == 0U) {
            status = "insufficient_alt_reads";
        }
        tsv += "HCC1395\tHCC1395\tUNASSESSED\tchrSynthetic\t" + std::to_string(100U + index) + "\t" +
               base_for_order(index, true) + "\t" + base_for_order(index, false) + "\t" + status + "\t" +
               (old_stable[index] ? "True" : "False") + "\n";
    }
    if (fault == Fault::kHistoricalDuplicate) {
        tsv +=
            "HCC1395\tHCC1395\tUNASSESSED\tchrSynthetic\t100\tA\tC\t"
            "evaluable\tTrue\n";
    }
    const std::filesystem::path path = base / "historical_m1.tsv.gz";
    write_gzip(path, tsv);
    return HistoricalBuilt{path, sha256_file(path)};
}

longlineage::audit::Hcc1395DeterminismAuditResult run_case(const std::filesystem::path& base, Fault fault,
                                                           std::filesystem::path& output_path) {
    const BuiltRun w24 = build_run(base, "synthetic-w24", 24U, true, fault);
    const BuiltRun w40 = build_run(base, "synthetic-w40", 40U, false, fault);
    const HistoricalBuilt historical = build_historical(base, fault);
    output_path = base / "audit_receipt.json";
    longlineage::audit::Hcc1395DeterminismAuditOptions options;
    options.w24_run_root = w24.root;
    options.w24_manifest = w24.manifest;
    options.w40_run_root = w40.root;
    options.w40_manifest = w40.manifest;
    options.historical_m1_tsv_gz = historical.path;
    options.historical_m1_sha256 = fault == Fault::kHistoricalSha ? std::string(64U, '0') : historical.sha256;
    options.output_receipt = output_path;
    return longlineage::audit::run_hcc1395_determinism_audit(options);
}

JsonPtr load_json(const std::filesystem::path& path) {
    json_error_t error{};
    JsonPtr value(json_load_file(path.c_str(), JSON_REJECT_DUPLICATES, &error));
    check(value != nullptr, "cannot parse audit receipt: " + std::string(error.text));
    return value;
}

void test_positive() {
    Scratch scratch;
    std::filesystem::path output;
    const auto result = run_case(scratch.path(), Fault::kNone, output);
    check(result.ok, "positive audit failed: " + result.error_code + ": " + result.detail);
    check(std::filesystem::is_regular_file(output), "positive audit did not publish receipt");
    check(read_file(output) == result.receipt_json, "returned receipt differs from atomic output");
    JsonPtr receipt = load_json(output);
    check(std::string(json_string_value(json_object_get(receipt.get(), "schema_name"))) ==
                  "longlineage.hcc1395_determinism_historical_receipt" &&
              std::string(json_string_value(json_object_get(receipt.get(), "schema_version"))) == "2.0.0",
          "positive receipt is not the v2 contract");
    check(std::string(json_string_value(json_object_get(receipt.get(), "overall_status"))) == "PASS",
          "positive receipt lacks PASS");
    const json_t* determinism = json_object_get(receipt.get(), "determinism");
    check(json_array_size(json_object_get(determinism, "artifacts")) == 8U,
          "positive receipt does not compare eight artifacts");
    const json_t* summary_projection = json_object_get(determinism, "summary_projection");
    const json_t* summary_counts = json_object_get(summary_projection, "counts");
    check(json_integer_value(json_object_get(summary_counts, "topology_primary_hp_units")) == 1 &&
              json_integer_value(json_object_get(summary_counts, "topology_regions")) == 2 &&
              json_integer_value(json_object_get(summary_counts, "topology_fully_complete_regions")) == 1 &&
              json_integer_value(json_object_get(summary_counts, "topology_incomplete_regions")) == 1,
          "positive fixture collapsed distinct topology unit/region grains");
    const json_t* history = json_object_get(receipt.get(), "historical");
    const json_t* keys = json_object_get(history, "site_keys");
    check(std::string(json_string_value(json_object_get(keys, "verdict"))) == "EXACT",
          "positive historical site keys are not exact");
    const json_t* m1 = json_object_get(history, "m1");
    check(std::string(json_string_value(json_object_get(m1, "verdict"))) == "COMPARABLE_DIFFERENT",
          "stable churn must be explicit COMPARABLE_DIFFERENT");
    const json_t* stable = json_object_get(m1, "stable_transition");
    check(json_integer_value(json_object_get(stable, "true_to_true")) == 2 &&
              json_integer_value(json_object_get(stable, "true_to_false")) == 1 &&
              json_integer_value(json_object_get(stable, "false_to_true")) == 2 &&
              json_integer_value(json_object_get(stable, "false_to_false")) == 1,
          "stable transition matrix differs from synthetic oracle");
    const json_t* cooccurrence = json_object_get(history, "cooccurrence");
    check(!json_boolean_value(json_object_get(cooccurrence, "old_formal_result_exists")) &&
              json_integer_value(json_object_get(cooccurrence, "new_pair_rows")) == 9,
          "NOT_COMPARABLE co-occurrence boundary is incomplete");
    const json_t* replay = json_object_get(determinism, "cooccurrence_replay");
    check(std::string(json_string_value(json_object_get(replay, "status"))) == "PASS" &&
              std::string(json_string_value(json_object_get(replay, "authority"))) ==
                  "VALIDATED_SERIALIZED_CENSUS_NOT_PQ_RECOMPUTATION" &&
              std::string(json_string_value(json_object_get(replay, "interpretation"))) ==
                  "PAIR_ROWS_ARE_RECORDS_NOT_POSITIVE_DISCOVERIES",
          "co-occurrence replay authority boundary is incomplete");
    const json_t* w24 = json_object_get(replay, "w24");
    const json_t* w40 = json_object_get(replay, "w40");
    for (const json_t* run : {w24, w40}) {
        check(json_integer_value(json_object_get(run, "pair_rows")) == 9 &&
                  json_integer_value(json_object_get(run, "exact_identifiable_pairs")) == 5 &&
                  json_integer_value(json_object_get(run, "eligible_exact_family_pairs")) == 3 &&
                  json_integer_value(json_object_get(run, "global_bh_discoveries")) == 3 &&
                  json_integer_value(json_object_get(run, "global_by_discoveries")) == 2 &&
                  json_integer_value(json_object_get(run, "formal_pair_by_confirmed")) == 1 &&
                  json_integer_value(json_object_get(run, "cooccurrence_site_rows")) == 6 &&
                  json_integer_value(json_object_get(run, "joint_signature_pass_sites")) == 1 &&
                  json_integer_value(json_object_get(run, "topology_units")) == 1,
              "co-occurrence replay counters differ from synthetic oracle");
    }
    check(json_equal(w24, w40) == 1, "co-occurrence replay is not worker invariant");

    if (const char* preserve = std::getenv("LONGLINEAGE_AUDIT_TEST_RECEIPT");
        preserve != nullptr && *preserve != '\0') {
        write_file(preserve, result.receipt_json);
    }
    if (std::getenv("LONGLINEAGE_AUDIT_KEEP_POSITIVE") != nullptr) {
        scratch.preserve();
        std::cout << "POSITIVE_FIXTURE_ROOT=" << scratch.path() << '\n';
    }
}

void test_empty_exact_family_positive() {
    Scratch scratch;
    std::filesystem::path output;
    const auto result = run_case(scratch.path(), Fault::kEmptyExactFamily, output);
    check(result.ok, "empty exact-family audit failed: " + result.error_code + ": " + result.detail);
    check(std::filesystem::is_regular_file(output), "empty exact-family audit did not publish receipt");
    JsonPtr receipt = load_json(output);
    const json_t* replay = json_object_get(json_object_get(receipt.get(), "determinism"), "cooccurrence_replay");
    const json_t* w24 = json_object_get(replay, "w24");
    const json_t* w40 = json_object_get(replay, "w40");
    for (const json_t* run : {w24, w40}) {
        check(json_integer_value(json_object_get(run, "pair_rows")) == 9 &&
                  json_integer_value(json_object_get(run, "exact_identifiable_pairs")) == 5 &&
                  json_integer_value(json_object_get(run, "eligible_exact_family_pairs")) == 0 &&
                  json_integer_value(json_object_get(run, "fdr_family_size")) == 0 &&
                  json_integer_value(json_object_get(run, "global_bh_discoveries")) == 0 &&
                  json_integer_value(json_object_get(run, "global_by_discoveries")) == 0 &&
                  json_integer_value(json_object_get(run, "formal_pair_by_confirmed")) == 0 &&
                  json_integer_value(json_object_get(run, "joint_signature_pass_sites")) == 0 &&
                  json_integer_value(json_object_get(run, "topology_units")) == 0,
              "empty exact-family legal census differs from oracle");
    }
    check(json_equal(w24, w40) == 1, "empty exact-family census is not worker invariant");
}

void test_negative(Fault fault, std::string_view case_id, std::string_view expected_error_code,
                   std::string_view expected_detail_contains) {
    Scratch scratch;
    std::filesystem::path output;
    const auto result = run_case(scratch.path(), fault, output);
    check(!result.ok, "negative fixture passed: " + std::string(case_id));
    check(!result.error_code.empty() && !result.detail.empty(),
          "negative fixture lacks fail-closed diagnostic: " + std::string(case_id));
    check(result.error_code == expected_error_code,
          "negative fixture hit the wrong fail-closed gate: " + std::string(case_id) + "; expected=" +
              std::string(expected_error_code) + "; observed=" + result.error_code + "; detail=" + result.detail);
    if (!expected_detail_contains.empty()) {
        check(result.detail.find(expected_detail_contains) != std::string::npos,
              "negative fixture hit the expected check_id but wrong "
              "failure branch: " +
                  std::string(case_id) + "; expected detail marker=" + std::string(expected_detail_contains) +
                  "; observed=" + result.detail);
    }
    check(!std::filesystem::exists(output), "negative fixture published a receipt: " + std::string(case_id));
}

}  // namespace

int main() {
    try {
        const std::vector<PositiveCase> positives = {
            {"frozen_w24_w40_equal_science_with_stable_m1_churn", &test_positive},
            {"empty_exact_fdr_family_is_valid_and_worker_invariant", &test_empty_exact_family_positive},
        };
        const std::vector<NegativeCase> negatives = {
            {Fault::kFailedValidation, "failed_validation_receipt", "FROZEN_ROOT_W24"},
            {Fault::kNonFrozen, "non_frozen_run_receipt", "FROZEN_ROOT_W24"},
            {Fault::kTruthFields, "truth_fields_seen_nonzero", "FROZEN_ROOT_W24"},
            {Fault::kPathEscape, "artifact_path_escape", "FROZEN_ROOT_W24"},
            {Fault::kSymlink, "artifact_symlink", "FROZEN_ROOT_W24"},
            {Fault::kManifestDifference, "manifest_non_whitelisted_difference", "RUN_CONTRACT_DETERMINISM"},
            {Fault::kWrongWorkerCount, "wrong_worker_count", "RUN_CONTRACT_DETERMINISM"},
            {Fault::kSemanticMismatch, "science_semantic_digest_mismatch", "RUN_CONTRACT_DETERMINISM"},
            {Fault::kSummaryProjection, "summary_projection_mismatch", "RUN_CONTRACT_DETERMINISM"},
            {Fault::kTopologyRegionPartitionMismatch, "topology_region_partition_mismatch", "FROZEN_ROOT_W24"},
            {Fault::kM2DoubleCount, "m2_summary_double_count", "M2_CONSERVATION_W24"},
            {Fault::kM2Duplicate, "m2_overlapping_duplicate_key", "M2_CONSERVATION_W24"},
            {Fault::kM2Missing, "m2_missing_key", "M2_CONSERVATION_W24"},
            {Fault::kM2Extra, "m2_extra_key", "M2_CONSERVATION_W24"},
            {Fault::kNativePreambleMissing, "native_tsv_preamble_missing", "M1_REPLAY_W24"},
            {Fault::kNativePreambleWrong, "native_tsv_preamble_wrong_schema", "M2_CONSERVATION_W24"},
            {Fault::kNativePreambleOrder, "native_tsv_preamble_wrong_order", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kNativePreambleDuplicateHeader, "native_tsv_duplicate_header", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kNativeVersionWrong, "native_tsv_preamble_wrong_version", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kNativeRunIdMismatch, "native_tsv_preamble_wrong_run_id", "M2_CONSERVATION_W24"},
            {Fault::kNativeHeaderMissingMarker, "native_tsv_header_missing_marker", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kNativeHeaderMissingColumn, "native_tsv_header_missing_required_column",
             "COOCCURRENCE_AGGREGATE_W24", "TSV header lacks required column: formal_pair_by_confirmed"},
            {Fault::kMissingScientificConservation, "validation_receipt_missing_scientific_conservation",
             "FROZEN_ROOT_W24"},
            {Fault::kPairWorkerMismatch, "cooccurrence_worker_counter_mismatch", "COOCCURRENCE_WORKER_DETERMINISM"},
            {Fault::kPairDuplicateKey, "cooccurrence_duplicate_pair_key", "COOCCURRENCE_AGGREGATE_W24",
             "cooccurrence-pair primary key is outside HCC1395 scope, duplicated or out of order"},
            {Fault::kPairOutOfOrder, "cooccurrence_out_of_order_pair_key", "COOCCURRENCE_AGGREGATE_W24",
             "cooccurrence-pair primary key is outside HCC1395 scope, duplicated or out of order"},
            {Fault::kPairCounterOverflow, "cooccurrence_site_aggregate_overflow", "M2_CONSERVATION_W24",
             "partner_universe_pair_rows counter overflow"},
            {Fault::kPairUnknownFamily, "cooccurrence_unknown_family_status", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kPairInternalExactStatus, "cooccurrence_internal_exact_status_leak", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kPairFamilyExactStatusMismatch, "cooccurrence_family_exact_status_mismatch",
             "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kPairByOutsideFamily, "cooccurrence_by_outside_exact_family", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kPairByWithoutBh, "cooccurrence_by_true_bh_false", "COOCCURRENCE_AGGREGATE_W24",
             "exact BY discovery is not a subset of exact BH discoveries"},
            {Fault::kPairFormalWithoutBy, "cooccurrence_formal_without_by", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kPairFamilySizeMismatch, "cooccurrence_fdr_family_size_mismatch", "COOCCURRENCE_AGGREGATE_W24"},
            {Fault::kPairSiteAggregateMismatch, "cooccurrence_pair_site_aggregate_mismatch",
             "COOCCURRENCE_CROSS_ARTIFACT_W24"},
            {Fault::kJointUnknownStatus, "cooccurrence_unknown_joint_signature_status", "M2_CONSERVATION_W24"},
            {Fault::kJointTopologyMismatch, "cooccurrence_joint_signature_topology_mismatch",
             "COOCCURRENCE_CROSS_ARTIFACT_W24"},
            {Fault::kHistoricalSha, "historical_physical_sha_mismatch", "HISTORICAL_M1_SOURCE"},
            {Fault::kHistoricalDuplicate, "historical_duplicate_key", "HISTORICAL_M1_SOURCE"},
            {Fault::kHistoricalMissing, "historical_missing_key", "HISTORICAL_M1_COMPARISON"},
            {Fault::kHistoricalStatus, "historical_status_mismatch", "HISTORICAL_M1_COMPARISON"},
        };

        const std::filesystem::path repository_root = std::filesystem::canonical(LONGLINEAGE_SOURCE_DIR);
        const std::filesystem::path manifest_path = repository_root / "tests/fixtures/hcc1395_audit/cases.json";
        JsonPtr manifest = load_case_manifest(manifest_path);
        const std::vector<std::string> documented_order = bind_case_manifest(manifest.get(), positives, negatives);
        test_case_manifest_binding_negatives(manifest.get(), positives, negatives);

        std::vector<std::string> executed_order;
        executed_order.reserve(documented_order.size());
        for (const PositiveCase& positive : positives) {
            positive.execute();
            executed_order.emplace_back(positive.case_id);
        }
        for (const NegativeCase& negative : negatives) {
            test_negative(negative.fault, negative.case_id, negative.expected_error_code,
                          negative.expected_detail_contains);
            executed_order.emplace_back(negative.case_id);
        }
        check(executed_order == documented_order,
              "executed case vector differs from the strictly bound case manifest order");
        std::cout << "PASS hcc1395_audit manifest_bound="
                  << manifest_path.lexically_relative(repository_root).generic_string()
                  << " manifest_sha256=" << sha256_file(manifest_path)
                  << " binding_negatives=2 executed_order_exact=true synthetic_cases=" << executed_order.size()
                  << " positive=" << positives.size() << " negative=" << negatives.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL test_hcc1395_audit: " << error.what() << '\n';
        return 1;
    }
}
