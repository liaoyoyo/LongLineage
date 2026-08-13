// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/audit/hcc1395_determinism.hpp"

#include <fcntl.h>
#include <htslib/bgzf.h>
#include <htslib/kstring.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace longlineage::audit {
namespace {

constexpr std::string_view kDatasetId = "HCC1395";
constexpr std::string_view kReceiptSchema = "longlineage.hcc1395_determinism_historical_receipt";
constexpr std::string_view kReceiptVersion = "2.0.0";
constexpr std::size_t kMaximumJsonBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumTsvLineBytes = 32U * 1024U * 1024U;
constexpr std::string_view kFormalHistoricalPhysicalSha256 =
    "a8871af3a8c3955bf31aec5eeef0c93aca0683f52cf6d6f1e06fbbb713324f74";
constexpr std::string_view kFormalHistoricalOrderedKeySha256 =
    "ac569c424fd9e1dddf69a8933b6e97cf7570dc9683bb81865853bc5110062468";
constexpr std::string_view kFormalHistoricalSortedSetSha256 =
    "003a5fe2bb2228495e337f1ca098c9fd6b696b01bb4cb13edde8d39f3c946a69";
constexpr std::uint64_t kFormalHistoricalHcc1395Rows = 79687U;

#ifndef LONGLINEAGE_GIT_COMMIT
#define LONGLINEAGE_GIT_COMMIT "0000000000000000000000000000000000000000"
#endif

class AuditError final : public std::runtime_error {
   public:
    AuditError(std::string code, std::string detail)
        : std::runtime_error(detail), code_(std::move(code)), detail_(std::move(detail)) {}

    [[nodiscard]] const std::string& code() const noexcept { return code_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

   private:
    std::string code_;
    std::string detail_;
};

[[noreturn]] void reject(std::string code, std::string detail) { throw AuditError(std::move(code), std::move(detail)); }

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};
using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

struct EvpContextDeleter {
    void operator()(EVP_MD_CTX* value) const noexcept { EVP_MD_CTX_free(value); }
};
using EvpContextPtr = std::unique_ptr<EVP_MD_CTX, EvpContextDeleter>;

struct BgzfDeleter {
    void operator()(BGZF* value) const noexcept {
        if (value != nullptr) {
            bgzf_close(value);
        }
    }
};
using BgzfPtr = std::unique_ptr<BGZF, BgzfDeleter>;

class KStringBuffer final {
   public:
    KStringBuffer() = default;
    ~KStringBuffer() { std::free(value_.s); }

    KStringBuffer(const KStringBuffer&) = delete;
    KStringBuffer& operator=(const KStringBuffer&) = delete;

    [[nodiscard]] kstring_t* get() noexcept { return &value_; }
    [[nodiscard]] const char* data() const noexcept { return value_.s; }

   private:
    kstring_t value_{0U, 0U, nullptr};
};

class Sha256 {
   public:
    Sha256() : context_(EVP_MD_CTX_new()) {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            reject("SHA256", "cannot initialize SHA-256");
        }
    }

    void update(std::string_view bytes) {
        if (!bytes.empty() && EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1) {
            reject("SHA256", "cannot update SHA-256");
        }
    }

    [[nodiscard]] std::string finish() {
        std::array<unsigned char, 32> raw{};
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(context_.get(), raw.data(), &size) != 1 || size != raw.size()) {
            reject("SHA256", "cannot finalize SHA-256");
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(raw.size() * 2U);
        for (const unsigned char value : raw) {
            encoded.push_back(kHex[value >> 4U]);
            encoded.push_back(kHex[value & 0x0fU]);
        }
        return encoded;
    }

   private:
    EvpContextPtr context_;
};

[[nodiscard]] bool is_lower_sha256(std::string_view value) {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] std::string sha256_bytes(std::string_view bytes) {
    Sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

void require_absolute_no_symlink(const std::filesystem::path& path, bool require_directory,
                                 const std::string& check_id) {
    if (path.empty() || !path.is_absolute() || path.lexically_normal() != path) {
        reject(check_id, "path must be absolute and lexically normalized: " + path.string());
    }
    std::filesystem::path current;
    for (const auto& component : path) {
        current /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error) {
            reject(check_id, "cannot inspect path component: " + current.string());
        }
        if (std::filesystem::is_symlink(status)) {
            reject(check_id, "symbolic-link path component is prohibited: " + current.string());
        }
    }
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error ||
        (require_directory ? !std::filesystem::is_directory(status) : !std::filesystem::is_regular_file(status))) {
        reject(check_id, require_directory ? "required directory is absent: " + path.string()
                                           : "required regular file is absent: " + path.string());
    }
}

[[nodiscard]] std::filesystem::path safe_file_under(const std::filesystem::path& root, std::string_view relative,
                                                    const std::string& check_id) {
    const std::filesystem::path parsed(relative);
    if (relative.empty() || parsed.is_absolute() || parsed.lexically_normal() != parsed) {
        reject(check_id, "artifact path is absolute or non-normal: " + std::string(relative));
    }
    for (const auto& component : parsed) {
        if (component == "." || component == "..") {
            reject(check_id, "artifact path escapes the frozen root: " + std::string(relative));
        }
    }
    const std::filesystem::path path = root / parsed;
    require_absolute_no_symlink(path, false, check_id);
    return path;
}

[[nodiscard]] int open_regular_nofollow(const std::filesystem::path& path, const std::string& check_id) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        reject(check_id,
               "cannot open regular file without following links: " + path.string() + ": " + std::strerror(errno));
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        const int saved = errno;
        ::close(descriptor);
        reject(check_id, "opened path is not a regular file: " + path.string() + ": " + std::strerror(saved));
    }
    return descriptor;
}

[[nodiscard]] std::string sha256_file(const std::filesystem::path& path, const std::string& check_id) {
    const int descriptor = open_regular_nofollow(path, check_id);
    Sha256 digest;
    std::array<char, 1U << 20U> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            const int saved = errno;
            ::close(descriptor);
            reject(check_id, "cannot read file for SHA-256: " + path.string() + ": " + std::strerror(saved));
        }
        digest.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
    }
    if (::close(descriptor) != 0) {
        reject(check_id, "cannot close hashed file: " + path.string());
    }
    return digest.finish();
}

[[nodiscard]] std::string read_regular_file(const std::filesystem::path& path, std::size_t maximum_bytes,
                                            const std::string& check_id) {
    const int descriptor = open_regular_nofollow(path, check_id);
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > maximum_bytes) {
        ::close(descriptor);
        reject(check_id, "file exceeds bounded reader size: " + path.string());
    }
    std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            const int saved = errno;
            ::close(descriptor);
            reject(check_id, "short read from file: " + path.string() + ": " + std::strerror(saved));
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) {
        reject(check_id, "cannot close bounded input: " + path.string());
    }
    return bytes;
}

struct LoadedJson {
    JsonPtr value;
    std::string bytes;
    std::string physical_sha256;
};

[[nodiscard]] LoadedJson load_json_file(const std::filesystem::path& path, const std::string& check_id) {
    LoadedJson loaded;
    loaded.bytes = read_regular_file(path, kMaximumJsonBytes, check_id);
    loaded.physical_sha256 = sha256_bytes(loaded.bytes);
    json_error_t error{};
    loaded.value.reset(
        json_loadb(loaded.bytes.data(), loaded.bytes.size(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
    if (!loaded.value || !json_is_object(loaded.value.get())) {
        reject(check_id, "JSON object parse failed at line " + std::to_string(error.line) + ": " + error.text);
    }
    return loaded;
}

void require_only_keys(const json_t* object, std::initializer_list<std::string_view> allowed,
                       std::initializer_list<std::string_view> required, const std::string& path,
                       const std::string& check_id) {
    if (!json_is_object(object)) {
        reject(check_id, path + " must be an object");
    }
    const std::set<std::string_view> allowed_set(allowed);
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(object), key, value) {
        static_cast<void>(value);
        if (allowed_set.count(key) == 0U) {
            reject(check_id, path + " contains unknown field: " + key);
        }
    }
    for (const std::string_view required_key : required) {
        if (json_object_get(object, std::string(required_key).c_str()) == nullptr) {
            reject(check_id, path + " is missing field: " + std::string(required_key));
        }
    }
}

[[nodiscard]] const json_t* object_field(const json_t* object, const char* name, const std::string& check_id) {
    const json_t* value = json_object_get(object, name);
    if (!json_is_object(value)) {
        reject(check_id, std::string(name) + " must be an object");
    }
    return value;
}

[[nodiscard]] const json_t* array_field(const json_t* object, const char* name, const std::string& check_id) {
    const json_t* value = json_object_get(object, name);
    if (!json_is_array(value)) {
        reject(check_id, std::string(name) + " must be an array");
    }
    return value;
}

[[nodiscard]] std::string string_field(const json_t* object, const char* name, const std::string& check_id) {
    const json_t* value = json_object_get(object, name);
    if (!json_is_string(value)) {
        reject(check_id, std::string(name) + " must be a string");
    }
    const char* raw = json_string_value(value);
    const std::size_t size = json_string_length(value);
    if (std::char_traits<char>::length(raw) != size) {
        reject(check_id, std::string(name) + " contains embedded NUL");
    }
    return std::string(raw, size);
}

[[nodiscard]] std::uint64_t uint_field(const json_t* object, const char* name, const std::string& check_id) {
    const json_t* value = json_object_get(object, name);
    if (!json_is_integer(value) || json_integer_value(value) < 0) {
        reject(check_id, std::string(name) + " must be a non-negative integer");
    }
    return static_cast<std::uint64_t>(json_integer_value(value));
}

[[nodiscard]] bool bool_field(const json_t* object, const char* name, const std::string& check_id) {
    const json_t* value = json_object_get(object, name);
    if (!json_is_boolean(value)) {
        reject(check_id, std::string(name) + " must be boolean");
    }
    return json_is_true(value);
}

void require_sha(std::string_view value, const std::string& label, const std::string& check_id) {
    if (!is_lower_sha256(value)) {
        reject(check_id, label + " is not a lowercase SHA-256");
    }
}

[[nodiscard]] std::string canonical_json(const json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
    if (encoded == nullptr) {
        reject("JSON_CANONICAL", "cannot encode canonical JSON");
    }
    std::string result(encoded);
    std::free(encoded);
    return result;
}

[[nodiscard]] std::vector<std::string> split_tsv(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const std::size_t tab = line.find('\t', begin);
        fields.emplace_back(line.substr(begin, tab == std::string_view::npos ? std::string_view::npos : tab - begin));
        if (tab == std::string_view::npos) {
            break;
        }
        begin = tab + 1U;
    }
    return fields;
}

[[nodiscard]] std::map<std::string, std::size_t> header_index(const std::vector<std::string>& header,
                                                              const std::string& check_id) {
    std::map<std::string, std::size_t> index;
    for (std::size_t position = 0; position < header.size(); ++position) {
        if (header[position].empty() || !index.emplace(header[position], position).second) {
            reject(check_id, "TSV header is empty or duplicated");
        }
    }
    return index;
}

[[nodiscard]] std::size_t require_column(const std::map<std::string, std::size_t>& index, const std::string& name,
                                         const std::string& check_id) {
    const auto found = index.find(name);
    if (found == index.end()) {
        reject(check_id, "TSV header lacks required column: " + name);
    }
    return found->second;
}

struct NativeTsvPreamble {
    std::vector<std::string> header;
    std::map<std::string, std::size_t> columns;
    bool complete{false};
};

[[nodiscard]] bool consume_native_tsv_preamble(std::string_view line, std::uint64_t line_number,
                                               std::string_view schema_name, std::string_view schema_version,
                                               std::string_view run_id, NativeTsvPreamble& preamble,
                                               const std::string& check_id) {
    const std::array<std::string, 3> metadata = {
        "##longlineage_schema=" + std::string(schema_name),
        "##schema_version=" + std::string(schema_version),
        "##run_id=" + std::string(run_id),
    };
    if (line_number >= 1U && line_number <= metadata.size()) {
        if (line != metadata.at(static_cast<std::size_t>(line_number - 1U))) {
            reject(check_id, "native TSV metadata preamble differs at line " + std::to_string(line_number));
        }
        return false;
    }
    if (line_number == 4U) {
        if (line.size() < 2U || line.front() != '#' || line[1] == '#') {
            reject(check_id, "native TSV line 4 is not the unique canonical header");
        }
        preamble.header = split_tsv(line.substr(1U));
        preamble.columns = header_index(preamble.header, check_id);
        preamble.complete = true;
        return false;
    }
    if (!preamble.complete) {
        reject(check_id, "native TSV data appeared before the canonical four-line preamble");
    }
    if (!line.empty() && line.front() == '#') {
        reject(check_id, "native TSV contains a duplicate or out-of-order header");
    }
    return true;
}

void require_native_tsv_preamble(const NativeTsvPreamble& preamble, const std::string& check_id) {
    if (!preamble.complete || preamble.header.empty() || preamble.columns.size() != preamble.header.size()) {
        reject(check_id, "native TSV canonical four-line preamble is missing or incomplete");
    }
}

[[nodiscard]] std::uint64_t parse_uint(std::string_view encoded, const std::string& label,
                                       const std::string& check_id) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), value);
    if (encoded.empty() || parsed.ec != std::errc{} || parsed.ptr != encoded.data() + encoded.size()) {
        reject(check_id, label + " is not an unsigned integer");
    }
    return value;
}

void checked_add(std::uint64_t& target, std::uint64_t value, const std::string& label, const std::string& check_id) {
    if (value > std::numeric_limits<std::uint64_t>::max() - target) {
        reject(check_id, label + " counter overflow");
    }
    target += value;
}

[[nodiscard]] bool parse_bool(std::string_view encoded, const std::string& label, const std::string& check_id) {
    if (encoded == "true" || encoded == "True" || encoded == "1") {
        return true;
    }
    if (encoded == "false" || encoded == "False" || encoded == "0") {
        return false;
    }
    reject(check_id, label + " is not a closed boolean token");
}

void for_each_compressed_line(const std::filesystem::path& path, const std::string& check_id,
                              const std::function<void(std::string_view, std::uint64_t)>& callback) {
    BgzfPtr stream(bgzf_open(path.c_str(), "r"));
    if (!stream) {
        reject(check_id, "cannot open gzip/BGZF TSV: " + path.string());
    }
    KStringBuffer line;
    std::uint64_t line_number = 0;
    while (true) {
        const int length = bgzf_getline(stream.get(), '\n', line.get());
        if (length == -1) {
            break;
        }
        if (length < -1) {
            reject(check_id, "compressed TSV read failed");
        }
        ++line_number;
        if (static_cast<std::size_t>(length) > kMaximumTsvLineBytes) {
            reject(check_id, "compressed TSV line exceeds bounded size");
        }
        std::string_view view(line.data(), static_cast<std::size_t>(length));
        if (!view.empty() && view.back() == '\r') {
            view.remove_suffix(1U);
        }
        if (view.find('\0') != std::string_view::npos) {
            reject(check_id, "compressed TSV contains embedded NUL");
        }
        callback(view, line_number);
    }
    if (bgzf_close(stream.release()) != 0) {
        reject(check_id, "compressed TSV close/checksum failed");
    }
}

[[nodiscard]] bool contains_truth_token(std::string_view value) {
    constexpr std::string_view kToken = "truth";
    if (value.size() < kToken.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset + kToken.size() <= value.size(); ++offset) {
        bool equal = true;
        for (std::size_t index = 0; index < kToken.size(); ++index) {
            char character = value[offset + index];
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
            if (character != kToken[index]) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return true;
        }
    }
    return false;
}

void reject_manifest_truth_recursive(const json_t* value, std::size_t depth, const std::string& path) {
    if (depth > 64U) {
        reject("MANIFEST_TRUTH_ISOLATION", "manifest nesting exceeds 64 at " + path);
    }
    if (json_is_object(value)) {
        const char* key = nullptr;
        json_t* child = nullptr;
        json_object_foreach(const_cast<json_t*>(value), key, child) {
            if (contains_truth_token(key)) {
                reject("MANIFEST_TRUTH_ISOLATION", "truth-bearing manifest key is prohibited at " + path);
            }
            reject_manifest_truth_recursive(child, depth + 1U, path + "." + key);
        }
    } else if (json_is_array(value)) {
        for (std::size_t index = 0; index < json_array_size(value); ++index) {
            reject_manifest_truth_recursive(json_array_get(value, index), depth + 1U, path + "[]");
        }
    } else if (json_is_string(value)) {
        const std::string encoded(json_string_value(value), json_string_length(value));
        if (contains_truth_token(encoded)) {
            reject("MANIFEST_TRUTH_ISOLATION", "truth-bearing manifest string is prohibited at " + path);
        }
    }
}

struct ArtifactMeta {
    std::string artifact_id;
    std::string relative_path;
    std::string schema_name;
    std::string schema_version;
    std::string format;
    std::uint64_t logical_rows{0};
    std::string physical_sha256;
    std::string semantic_sha256;
    JsonPtr raw;
};

struct SummaryProjection {
    JsonPtr scope;
    JsonPtr counts;
    JsonPtr phase_status;
    std::string canonical_sha256;
};

struct RunData {
    std::filesystem::path root;
    std::filesystem::path manifest_path;
    LoadedJson manifest;
    LoadedJson run_receipt;
    LoadedJson validation_receipt;
    LoadedJson producer_receipt;
    std::string run_id;
    std::uint64_t compute_workers{0};
    std::map<std::string, ArtifactMeta> artifacts;
    std::map<std::string, std::string> checksum_rows;
    std::map<std::string, ArtifactMeta> semantic_rows;
    SummaryProjection summary;
};

struct ExpectedArtifact {
    std::string_view artifact_id;
    std::string_view relative_path;
    std::string_view schema_name;
    std::string_view schema_version;
};

constexpr std::array<ExpectedArtifact, 8> kExpectedArtifacts = {{
    {"site_reads", "site_reads.tsv.bgz", "longlineage.site_reads", "1.0.0"},
    {"methyl_calls", "methyl_calls.tsv.bgz", "longlineage.methyl_calls", "1.0.0"},
    {"bernoulli_upper", "bernoulli_upper.llm.bgz", "longlineage.bernoulli_upper", "1.0.0"},
    {"m1_sites", "m1_sites.tsv.bgz", "longlineage.m1_sites", "1.0.0"},
    {"m1_assignments", "m1_assignments.jsonl.bgz", "longlineage.m1_assignment", "1.0.0"},
    {"cooccurrence_pairs", "cooccurrence_pairs.tsv.bgz", "longlineage.cooccurrence_pairs", "1.0.1"},
    {"cooccurrence_sites", "cooccurrence_sites.tsv.bgz", "longlineage.cooccurrence_sites", "1.0.0"},
    {"topology_units", "topology_units.jsonl.bgz", "longlineage.topology_unit", "2.0.0"},
}};

[[nodiscard]] std::uint64_t validate_manifest(const json_t* manifest, const std::filesystem::path& final_root,
                                              const std::string& expected_run_id, const std::string& check_id) {
    reject_manifest_truth_recursive(manifest, 0U, "$");
    require_only_keys(manifest,
                      {"schema_name", "schema_version", "authority_profile", "run_id", "output_root", "datasets",
                       "runtime", "contract_bindings"},
                      {"schema_name", "schema_version", "authority_profile", "run_id", "output_root", "datasets",
                       "runtime", "contract_bindings"},
                      "$", check_id);
    if (string_field(manifest, "schema_name", check_id) != "longlineage.production_manifest" ||
        string_field(manifest, "schema_version", check_id) != "1.1.0" ||
        string_field(manifest, "authority_profile", check_id) != "HCC1395_DATASET_GATE" ||
        string_field(manifest, "run_id", check_id) != expected_run_id) {
        reject(check_id, "manifest identity/profile differs from HCC1395 dataset gate");
    }

    const std::filesystem::path staging_root(string_field(manifest, "output_root", check_id));
    if (staging_root.empty() || !staging_root.is_absolute() || staging_root.lexically_normal() != staging_root ||
        staging_root.filename() != expected_run_id || staging_root.parent_path().filename() != ".staging") {
        reject(check_id, "manifest output_root is not the closed staging path");
    }
    const std::filesystem::path expected_final = staging_root.parent_path().parent_path() / expected_run_id;
    if (expected_final != final_root) {
        reject(check_id, "manifest staging path does not map to explicit final root");
    }

    const json_t* datasets = array_field(manifest, "datasets", check_id);
    if (json_array_size(datasets) != 1U) {
        reject(check_id, "HCC1395 audit requires exactly one dataset");
    }
    const json_t* dataset = json_array_get(datasets, 0U);
    require_only_keys(dataset, {"dataset_id", "dataset_order", "files"}, {"dataset_id", "dataset_order", "files"},
                      "$.datasets[0]", check_id);
    if (string_field(dataset, "dataset_id", check_id) != kDatasetId ||
        uint_field(dataset, "dataset_order", check_id) != 0U) {
        reject(check_id, "manifest dataset identity/order is not HCC1395/0");
    }
    const json_t* files = array_field(dataset, "files", check_id);
    if (json_array_size(files) != 8U) {
        reject(check_id, "manifest must contain exactly eight locked input roles");
    }
    const std::set<std::string> expected_roles = {
        "raw_bam",
        "raw_bam_index",
        "pass_biallelic_ssnv_vcf",
        "pass_biallelic_ssnv_vcf_index",
        "latest_hp_ps_sidecar",
        "latest_hp_ps_sidecar_index",
        "reference_fasta",
        "reference_fai",
    };
    std::set<std::string> observed_roles;
    for (std::size_t index = 0; index < json_array_size(files); ++index) {
        const json_t* file = json_array_get(files, index);
        require_only_keys(file, {"role", "path", "size_bytes", "sha256"}, {"role", "path", "size_bytes", "sha256"},
                          "$.datasets[0].files[]", check_id);
        const std::string role = string_field(file, "role", check_id);
        const std::filesystem::path input_path(string_field(file, "path", check_id));
        const std::string digest = string_field(file, "sha256", check_id);
        if (expected_roles.count(role) == 0U || !observed_roles.insert(role).second || input_path.empty() ||
            !input_path.is_absolute() || input_path.lexically_normal() != input_path ||
            uint_field(file, "size_bytes", check_id) == 0U) {
            reject(check_id, "manifest locked input role/path/size is invalid");
        }
        require_sha(digest, "manifest input SHA-256", check_id);
    }
    if (observed_roles != expected_roles) {
        reject(check_id, "manifest input roles are incomplete");
    }

    const json_t* runtime = object_field(manifest, "runtime", check_id);
    require_only_keys(runtime,
                      {"compute_workers", "writer_threads", "coordinator_slots", "buffer_bytes",
                       "max_focal_sites_per_block", "max_estimated_alignments_per_block", "halo_bp"},
                      {"compute_workers", "writer_threads", "coordinator_slots", "buffer_bytes",
                       "max_focal_sites_per_block", "max_estimated_alignments_per_block", "halo_bp"},
                      "$.runtime", check_id);
    const std::uint64_t workers = uint_field(runtime, "compute_workers", check_id);
    if (workers == 0U || workers > 40U || uint_field(runtime, "writer_threads", check_id) == 0U ||
        uint_field(runtime, "coordinator_slots", check_id) != 2U ||
        uint_field(runtime, "buffer_bytes", check_id) < 1048576U ||
        uint_field(runtime, "max_focal_sites_per_block", check_id) == 0U ||
        uint_field(runtime, "max_estimated_alignments_per_block", check_id) == 0U ||
        uint_field(runtime, "halo_bp", check_id) != 5000U) {
        reject(check_id, "manifest runtime is outside the closed dataset-gate contract");
    }

    const json_t* bindings = object_field(manifest, "contract_bindings", check_id);
    require_only_keys(
        bindings,
        {"science_parameters_sha256", "schema_catalog_sha256", "status_reason_registry_sha256", "type_registry_sha256",
         "transform_registry_sha256", "authority_manifest_sha256", "source_to_target_manifest_sha256",
         "production_input_authority_sha256", "dataset_gate_input_authority_sha256", "schema_id_registry_sha256",
         "release_attestation_sha256"},
        {"science_parameters_sha256", "schema_catalog_sha256", "status_reason_registry_sha256", "type_registry_sha256",
         "transform_registry_sha256", "authority_manifest_sha256", "source_to_target_manifest_sha256",
         "production_input_authority_sha256", "dataset_gate_input_authority_sha256", "schema_id_registry_sha256",
         "release_attestation_sha256"},
        "$.contract_bindings", check_id);
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(bindings), key, value) {
        static_cast<void>(value);
        require_sha(string_field(bindings, key, check_id), std::string("contract binding ") + key, check_id);
    }
    return workers;
}

[[nodiscard]] JsonPtr normalized_manifest(const json_t* manifest) {
    JsonPtr normalized(json_deep_copy(manifest));
    if (!normalized) {
        reject("MANIFEST_COMPARISON", "cannot copy manifest for normalization");
    }
    json_object_set_new(normalized.get(), "run_id", json_string("<RUN_ID>"));
    json_object_set_new(normalized.get(), "output_root", json_string("/<OUTPUT_BASE>/.staging/<RUN_ID>"));
    json_t* runtime = json_object_get(normalized.get(), "runtime");
    json_object_set_new(runtime, "compute_workers", json_integer(0));
    return normalized;
}

[[nodiscard]] ArtifactMeta parse_artifact_meta(const json_t* value, const std::string& check_id) {
    require_only_keys(value,
                      {"artifact_id", "role", "relative_path", "schema_name", "schema_version", "format", "size_bytes",
                       "physical_sha256", "logical_rows", "semantic_sha256", "index", "sensitivity", "transform_id",
                       "producer_executable_sha256", "inputs", "primary_key_first", "primary_key_last"},
                      {"artifact_id", "role", "relative_path", "schema_name", "schema_version", "format", "size_bytes",
                       "physical_sha256", "logical_rows", "semantic_sha256", "index", "sensitivity", "transform_id",
                       "producer_executable_sha256", "inputs", "primary_key_first", "primary_key_last"},
                      "artifact", check_id);
    ArtifactMeta meta;
    meta.artifact_id = string_field(value, "artifact_id", check_id);
    meta.relative_path = string_field(value, "relative_path", check_id);
    meta.schema_name = string_field(value, "schema_name", check_id);
    meta.schema_version = string_field(value, "schema_version", check_id);
    meta.format = string_field(value, "format", check_id);
    meta.logical_rows = uint_field(value, "logical_rows", check_id);
    meta.physical_sha256 = string_field(value, "physical_sha256", check_id);
    meta.semantic_sha256 = string_field(value, "semantic_sha256", check_id);
    require_sha(meta.physical_sha256, meta.artifact_id + " physical SHA-256", check_id);
    require_sha(meta.semantic_sha256, meta.artifact_id + " semantic SHA-256", check_id);
    if (uint_field(value, "size_bytes", check_id) == 0U) {
        reject(check_id, "artifact size must be positive");
    }
    meta.raw.reset(json_deep_copy(value));
    if (!meta.raw) {
        reject(check_id, "cannot retain artifact metadata");
    }
    return meta;
}

[[nodiscard]] std::map<std::string, ArtifactMeta> parse_artifacts(const json_t* array, const std::string& check_id) {
    if (!json_is_array(array)) {
        reject(check_id, "artifacts must be an array");
    }
    std::map<std::string, ArtifactMeta> artifacts;
    for (std::size_t index = 0; index < json_array_size(array); ++index) {
        ArtifactMeta meta = parse_artifact_meta(json_array_get(array, index), check_id);
        const std::string id = meta.artifact_id;
        if (!artifacts.emplace(id, std::move(meta)).second) {
            reject(check_id, "duplicate artifact metadata: " + id);
        }
    }
    return artifacts;
}

void validate_expected_science_artifacts(const std::map<std::string, ArtifactMeta>& artifacts,
                                         const std::string& check_id) {
    for (const ExpectedArtifact& expected : kExpectedArtifacts) {
        const auto found = artifacts.find(std::string(expected.artifact_id));
        if (found == artifacts.end() || found->second.relative_path != expected.relative_path ||
            found->second.schema_name != expected.schema_name ||
            found->second.schema_version != expected.schema_version) {
            reject(check_id, "science artifact schema/path mismatch: " + std::string(expected.artifact_id));
        }
    }
}

[[nodiscard]] std::map<std::string, std::string> replay_checksums(const std::filesystem::path& run_root,
                                                                  const std::filesystem::path& checksum_path,
                                                                  const std::string& expected_checksum_sha256,
                                                                  const std::string& check_id) {
    require_sha(expected_checksum_sha256, "checksums SHA-256", check_id);
    const std::string bytes = read_regular_file(checksum_path, kMaximumJsonBytes, check_id);
    if (sha256_bytes(bytes) != expected_checksum_sha256) {
        reject(check_id, "checksums.sha256 physical SHA differs from run receipt");
    }
    std::map<std::string, std::string> rows;
    std::string previous;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        const std::size_t end = bytes.find('\n', begin);
        if (end == std::string::npos) {
            reject(check_id, "checksums.sha256 lacks final LF");
        }
        const std::string_view line(bytes.data() + begin, end - begin);
        if (line.size() <= 66U || line[64] != ' ' || line[65] != ' ') {
            reject(check_id, "malformed checksum row");
        }
        const std::string digest(line.substr(0U, 64U));
        const std::string relative(line.substr(66U));
        require_sha(digest, "checksum row digest", check_id);
        if (relative.empty() || (!previous.empty() && relative <= previous) || !rows.emplace(relative, digest).second) {
            reject(check_id, "checksum rows are duplicated or out of order");
        }
        previous = relative;
        const std::filesystem::path artifact = safe_file_under(run_root, relative, check_id);
        if (sha256_file(artifact, check_id) != digest) {
            reject(check_id, "frozen file physical SHA mismatch: " + relative);
        }
        begin = end + 1U;
    }
    if (rows.empty()) {
        reject(check_id, "checksums.sha256 is empty");
    }
    return rows;
}

void require_checksum_binding(const std::map<std::string, std::string>& checksums, const std::string& relative,
                              const std::string& expected, const std::string& check_id) {
    const auto found = checksums.find(relative);
    if (found == checksums.end() || found->second != expected) {
        reject(check_id, "checksum binding differs for " + relative);
    }
}

[[nodiscard]] std::map<std::string, ArtifactMeta> parse_semantic_digests(const std::filesystem::path& path,
                                                                         const ArtifactMeta& meta,
                                                                         const std::string& check_id) {
    const std::string bytes = read_regular_file(path, kMaximumJsonBytes, check_id);
    if (sha256_bytes(bytes) != meta.physical_sha256) {
        reject(check_id, "semantic_digests.tsv physical SHA mismatch");
    }
    const std::string expected_header = "artifact_id\tschema_name\tschema_version\tlogical_rows\tsemantic_sha256\n";
    if (bytes.rfind(expected_header, 0U) != 0U) {
        reject(check_id, "semantic digest header differs from schema 1.0.0");
    }
    std::map<std::string, ArtifactMeta> rows;
    std::size_t begin = expected_header.size();
    std::string previous;
    while (begin < bytes.size()) {
        const std::size_t end = bytes.find('\n', begin);
        if (end == std::string::npos) {
            reject(check_id, "semantic digest TSV lacks final LF");
        }
        const auto fields = split_tsv(std::string_view(bytes.data() + begin, end - begin));
        if (fields.size() != 5U || fields[0].empty() || (!previous.empty() && fields[0] <= previous)) {
            reject(check_id, "semantic digest rows are malformed or unsorted");
        }
        ArtifactMeta row;
        row.artifact_id = fields[0];
        row.schema_name = fields[1];
        row.schema_version = fields[2];
        row.logical_rows = parse_uint(fields[3], "semantic logical_rows", check_id);
        row.semantic_sha256 = fields[4];
        require_sha(row.semantic_sha256, "semantic row digest", check_id);
        previous = row.artifact_id;
        if (!rows.emplace(row.artifact_id, std::move(row)).second) {
            reject(check_id, "duplicate semantic digest artifact");
        }
        begin = end + 1U;
    }
    return rows;
}

void validate_semantic_bindings(const std::map<std::string, ArtifactMeta>& artifacts,
                                const std::map<std::string, ArtifactMeta>& semantic_rows, const std::string& check_id) {
    for (const auto& [id, artifact] : artifacts) {
        if (id == "semantic_digests") {
            continue;
        }
        const auto row = semantic_rows.find(id);
        if (row == semantic_rows.end() || row->second.schema_name != artifact.schema_name ||
            row->second.schema_version != artifact.schema_version ||
            row->second.logical_rows != artifact.logical_rows ||
            row->second.semantic_sha256 != artifact.semantic_sha256) {
            reject(check_id, "semantic digest metadata differs for " + id);
        }
    }
}

[[nodiscard]] SummaryProjection parse_summary(const std::filesystem::path& path, const ArtifactMeta& meta,
                                              const std::string& run_id, const std::string& check_id) {
    LoadedJson loaded = load_json_file(path, check_id);
    if (loaded.physical_sha256 != meta.physical_sha256) {
        reject(check_id, "summary physical SHA mismatch");
    }
    require_only_keys(loaded.value.get(),
                      {"schema_name", "schema_version", "run_id", "scope", "counts", "phase_status"},
                      {"schema_name", "schema_version", "run_id", "scope", "counts", "phase_status"}, "$", check_id);
    if (string_field(loaded.value.get(), "schema_name", check_id) != "longlineage.summary" ||
        string_field(loaded.value.get(), "schema_version", check_id) != "1.0.0" ||
        string_field(loaded.value.get(), "run_id", check_id) != run_id) {
        reject(check_id, "summary identity differs from frozen run");
    }

    const json_t* scope = object_field(loaded.value.get(), "scope", check_id);
    require_only_keys(scope, {"task_type", "completeness", "dataset_count", "dataset_ids", "site_population"},
                      {"task_type", "completeness", "dataset_count", "dataset_ids", "site_population"}, "$.scope",
                      check_id);
    const json_t* dataset_ids = array_field(scope, "dataset_ids", check_id);
    if (string_field(scope, "task_type", check_id) != "B" || string_field(scope, "completeness", check_id) != "FULL" ||
        uint_field(scope, "dataset_count", check_id) != 1U || json_array_size(dataset_ids) != 1U ||
        !json_is_string(json_array_get(dataset_ids, 0U)) ||
        std::string(json_string_value(json_array_get(dataset_ids, 0U))) != kDatasetId) {
        reject(check_id, "summary scope is not FULL HCC1395 task B");
    }

    static constexpr std::array<std::string_view, 24> kCountFields = {
        "site_keys",
        "site_keys_missing",
        "site_keys_extra",
        "site_keys_duplicate",
        "m1_evaluable",
        "m1_insufficient_alt_reads",
        "m1_incomplete_distance",
        "m1_stable_assignments",
        "latest_tag_exact_joins",
        "latest_tag_missing",
        "latest_tag_conflict",
        "latest_tag_multimatch",
        "m2_eligible",
        "m2_evaluable_ineligible",
        "m2_axis_indeterminate",
        "m2_group_count_gt10",
        "raw_expected",
        "raw_matched",
        "raw_rg_only_duplicate_occurrences",
        "topology_primary_hp_units",
        "topology_regions",
        "topology_fully_complete_regions",
        "topology_incomplete_regions",
        "topology_incomplete_units_with_winner",
    };
    const json_t* counts = object_field(loaded.value.get(), "counts", check_id);
    std::set<std::string_view> count_set(kCountFields.begin(), kCountFields.end());
    const char* count_key = nullptr;
    json_t* count_value = nullptr;
    json_object_foreach(const_cast<json_t*>(counts), count_key, count_value) {
        static_cast<void>(count_value);
        if (count_set.count(count_key) == 0U) {
            reject(check_id, std::string("summary counts unknown field: ") + count_key);
        }
    }
    if (json_object_size(counts) != kCountFields.size()) {
        reject(check_id, "summary counts are incomplete");
    }
    for (const std::string_view field : kCountFields) {
        static_cast<void>(uint_field(counts, std::string(field).c_str(), check_id));
    }
    if (uint_field(counts, "site_keys_missing", check_id) != 0U ||
        uint_field(counts, "site_keys_extra", check_id) != 0U ||
        uint_field(counts, "site_keys_duplicate", check_id) != 0U ||
        uint_field(counts, "latest_tag_missing", check_id) != 0U ||
        uint_field(counts, "latest_tag_conflict", check_id) != 0U ||
        uint_field(counts, "latest_tag_multimatch", check_id) != 0U ||
        uint_field(counts, "topology_incomplete_units_with_winner", check_id) != 0U) {
        reject(check_id, "summary contains a fail-closed nonzero error counter");
    }
    const std::uint64_t raw_expected = uint_field(counts, "raw_expected", check_id);
    const std::uint64_t duplicates = uint_field(counts, "raw_rg_only_duplicate_occurrences", check_id);
    if (raw_expected > std::numeric_limits<std::uint64_t>::max() - duplicates ||
        raw_expected + duplicates != uint_field(counts, "raw_matched", check_id)) {
        reject(check_id, "summary raw occurrence conservation failed");
    }
    const std::uint64_t complete = uint_field(counts, "topology_fully_complete_regions", check_id);
    const std::uint64_t incomplete = uint_field(counts, "topology_incomplete_regions", check_id);
    if (complete > std::numeric_limits<std::uint64_t>::max() - incomplete ||
        complete + incomplete != uint_field(counts, "topology_regions", check_id)) {
        reject(check_id, "summary topology region conservation failed");
    }

    const json_t* phases = object_field(loaded.value.get(), "phase_status", check_id);
    static constexpr std::array<std::string_view, 9> kPhases = {"P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
    if (json_object_size(phases) != kPhases.size()) {
        reject(check_id, "summary phase projection is incomplete");
    }
    const std::set<std::string> allowed_phase = {"NOT_STARTED", "IN_PROGRESS", "BLOCKED", "FAILED", "VERIFIED"};
    for (const std::string_view phase : kPhases) {
        const std::string status = string_field(phases, std::string(phase).c_str(), check_id);
        if (allowed_phase.count(status) == 0U) {
            reject(check_id, "summary contains unknown phase status");
        }
    }

    SummaryProjection projection;
    projection.scope.reset(json_deep_copy(scope));
    projection.counts.reset(json_deep_copy(counts));
    projection.phase_status.reset(json_deep_copy(phases));
    JsonPtr projected(json_object());
    json_object_set(projected.get(), "scope", projection.scope.get());
    json_object_set(projected.get(), "counts", projection.counts.get());
    json_object_set(projected.get(), "phase_status", projection.phase_status.get());
    projection.canonical_sha256 = sha256_bytes(canonical_json(projected.get()));
    return projection;
}

void validate_executable_identity(const json_t* executable, const std::string& check_id) {
    require_only_keys(executable, {"name", "version", "git_commit", "executable_sha256", "compiler", "htslib_version"},
                      {"name", "version", "git_commit", "executable_sha256", "compiler", "htslib_version"},
                      "production_executable", check_id);
    if (string_field(executable, "name", check_id) != "longlineage" ||
        string_field(executable, "version", check_id).empty() ||
        string_field(executable, "compiler", check_id).empty() ||
        string_field(executable, "htslib_version", check_id) != "1.18") {
        reject(check_id, "production executable identity is incomplete");
    }
    const std::string commit = string_field(executable, "git_commit", check_id);
    if (commit.size() != 40U || !std::all_of(commit.begin(), commit.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) {
        reject(check_id, "production git commit is malformed");
    }
    require_sha(string_field(executable, "executable_sha256", check_id), "production executable SHA-256", check_id);
}

void validate_state_history(const json_t* history, const std::string& check_id) {
    if (!json_is_array(history) || json_array_size(history) != 3U) {
        reject(check_id, "dataset-gate state history must contain three events");
    }
    static constexpr std::array<std::string_view, 3> kStates = {"RUNNING", "VALIDATED",
                                                                "VALIDATED_FROZEN_DATASET_GATE"};
    std::optional<std::string> prior_event_sha;
    for (std::size_t index = 0; index < kStates.size(); ++index) {
        const json_t* event = json_array_get(history, index);
        require_only_keys(event,
                          {"sequence", "state", "at", "actor_executable_sha256", "previous_event_sha256", "reason"},
                          {"sequence", "state", "at", "actor_executable_sha256", "previous_event_sha256"},
                          "state_history[]", check_id);
        if (uint_field(event, "sequence", check_id) != index ||
            string_field(event, "state", check_id) != kStates[index] || string_field(event, "at", check_id).empty()) {
            reject(check_id, "dataset-gate state history order differs");
        }
        require_sha(string_field(event, "actor_executable_sha256", check_id), "state actor executable SHA-256",
                    check_id);
        const json_t* previous = json_object_get(event, "previous_event_sha256");
        if (index == 0U) {
            if (!json_is_null(previous)) {
                reject(check_id, "first state event must have null predecessor");
            }
        } else {
            if (!json_is_string(previous)) {
                reject(check_id, "state predecessor digest is absent");
            }
            require_sha(std::string_view(json_string_value(previous), json_string_length(previous)),
                        "state predecessor SHA-256", check_id);
        }
        static_cast<void>(prior_event_sha);
    }
}

void validate_validation_receipt(const json_t* validation, const json_t* run_receipt,
                                 const std::string& expected_run_id, const std::string& producer_receipt_sha256,
                                 const std::string& check_id) {
    require_only_keys(validation,
                      {"schema_name",
                       "schema_version",
                       "run_id",
                       "validation_profile",
                       "production_claim_allowed",
                       "producer_receipt_sha256",
                       "producer_executable_sha256",
                       "validator_executable_sha256",
                       "producer_hostname",
                       "producer_kernel_release",
                       "validator_hostname",
                       "validator_kernel_release",
                       "input_mount_identity_sha256",
                       "schema_catalog_sha256",
                       "science_parameters_sha256",
                       "all_pass",
                       "checks",
                       "validated_at",
                       "validator_independent",
                       "linked_producer_kernels",
                       "input_snapshot_before_sha256",
                       "input_snapshot_after_sha256"},
                      {"schema_name",
                       "schema_version",
                       "run_id",
                       "validation_profile",
                       "production_claim_allowed",
                       "producer_receipt_sha256",
                       "producer_executable_sha256",
                       "validator_executable_sha256",
                       "producer_hostname",
                       "producer_kernel_release",
                       "validator_hostname",
                       "validator_kernel_release",
                       "input_mount_identity_sha256",
                       "schema_catalog_sha256",
                       "science_parameters_sha256",
                       "all_pass",
                       "checks",
                       "validated_at",
                       "validator_independent",
                       "linked_producer_kernels",
                       "input_snapshot_before_sha256",
                       "input_snapshot_after_sha256"},
                      "$", check_id);
    if (string_field(validation, "schema_name", check_id) != "longlineage.validation_receipt" ||
        string_field(validation, "schema_version", check_id) != "1.0.0" ||
        string_field(validation, "run_id", check_id) != expected_run_id ||
        string_field(validation, "validation_profile", check_id) != "DATASET_GATE" ||
        bool_field(validation, "production_claim_allowed", check_id) || !bool_field(validation, "all_pass", check_id) ||
        !bool_field(validation, "validator_independent", check_id) ||
        bool_field(validation, "linked_producer_kernels", check_id) ||
        string_field(validation, "producer_receipt_sha256", check_id) != producer_receipt_sha256) {
        reject(check_id, "validation receipt is failed, non-independent or non-gate");
    }
    require_sha(string_field(validation, "producer_executable_sha256", check_id),
                "validation producer executable SHA-256", check_id);
    require_sha(string_field(validation, "validator_executable_sha256", check_id),
                "validation validator executable SHA-256", check_id);
    const json_t* checks = array_field(validation, "checks", check_id);
    if (json_array_size(checks) == 0U) {
        reject(check_id, "validation receipt has no independent checks");
    }
    std::set<std::string> check_ids;
    for (std::size_t index = 0; index < json_array_size(checks); ++index) {
        const json_t* check = json_array_get(checks, index);
        require_only_keys(check, {"check_id", "status", "reason", "observed", "expected", "evidence_sha256"},
                          {"check_id", "status", "reason", "observed", "expected", "evidence_sha256"}, "checks[]",
                          check_id);
        const std::string id = string_field(check, "check_id", check_id);
        if (id.empty() || !check_ids.insert(id).second || string_field(check, "status", check_id) != "PASS" ||
            !json_is_null(json_object_get(check, "reason"))) {
            reject(check_id, "validation check is failed, duplicated or malformed");
        }
        require_sha(string_field(check, "evidence_sha256", check_id), "validation evidence SHA-256", check_id);
    }
    if (check_ids.count("SCIENTIFIC_CONSERVATION") != 1U) {
        reject(check_id, "validation receipt lacks the independent SCIENTIFIC_CONSERVATION replay");
    }
    const std::string before = string_field(validation, "input_snapshot_before_sha256", check_id);
    const std::string after = string_field(validation, "input_snapshot_after_sha256", check_id);
    require_sha(before, "validation input snapshot", check_id);
    if (before != after || before != string_field(run_receipt, "input_snapshot_before_sha256", check_id) ||
        after != string_field(run_receipt, "input_snapshot_after_sha256", check_id)) {
        reject(check_id, "validation input snapshots differ");
    }
    const json_t* executable = object_field(run_receipt, "production_executable", check_id);
    if (string_field(executable, "executable_sha256", check_id) !=
            string_field(validation, "producer_executable_sha256", check_id) ||
        string_field(run_receipt, "validator_hostname", check_id) !=
            string_field(validation, "validator_hostname", check_id) ||
        string_field(run_receipt, "validator_kernel_release", check_id) !=
            string_field(validation, "validator_kernel_release", check_id) ||
        string_field(run_receipt, "schema_catalog_sha256", check_id) !=
            string_field(validation, "schema_catalog_sha256", check_id) ||
        string_field(run_receipt, "science_parameters_sha256", check_id) !=
            string_field(validation, "science_parameters_sha256", check_id)) {
        reject(check_id, "validation receipt does not bind the final run receipt");
    }
}

void validate_producer_receipt(const json_t* producer, const json_t* run_receipt, const json_t* validation,
                               const std::string& expected_run_id, const std::string& manifest_sha256,
                               const std::string& check_id) {
    require_only_keys(
        producer,
        {"schema_name", "schema_version", "run_id", "state", "producer_outcome", "producer_executable_sha256",
         "producer_hostname", "producer_kernel_release", "input_mount_identity", "manifest_sha256",
         "input_snapshot_before_sha256", "input_snapshot_after_sha256", "schema_catalog_sha256",
         "science_parameters_sha256", "run_receipt_draft", "artifacts", "truth_fields_seen", "failure_reason",
         "finished_at"},
        {"schema_name", "schema_version", "run_id", "state", "producer_outcome", "producer_executable_sha256",
         "producer_hostname", "producer_kernel_release", "input_mount_identity", "manifest_sha256",
         "input_snapshot_before_sha256", "input_snapshot_after_sha256", "schema_catalog_sha256",
         "science_parameters_sha256", "run_receipt_draft", "artifacts", "truth_fields_seen", "finished_at"},
        "$", check_id);
    if (string_field(producer, "schema_name", check_id) != "longlineage.producer_receipt" ||
        string_field(producer, "schema_version", check_id) != "1.0.0" ||
        string_field(producer, "run_id", check_id) != expected_run_id ||
        string_field(producer, "state", check_id) != "RUNNING" ||
        string_field(producer, "producer_outcome", check_id) != "READY_FOR_VALIDATION" ||
        uint_field(producer, "truth_fields_seen", check_id) != 0U ||
        string_field(producer, "manifest_sha256", check_id) != manifest_sha256) {
        reject(check_id, "producer receipt is failed, truth-bearing or mismatched");
    }
    if (json_object_get(producer, "failure_reason") != nullptr &&
        !json_is_null(json_object_get(producer, "failure_reason"))) {
        reject(check_id, "ready producer receipt carries a failure reason");
    }
    const std::string before = string_field(producer, "input_snapshot_before_sha256", check_id);
    const std::string after = string_field(producer, "input_snapshot_after_sha256", check_id);
    if (before != after || before != string_field(run_receipt, "input_snapshot_before_sha256", check_id)) {
        reject(check_id, "producer input snapshots differ");
    }
    const std::string producer_executable = string_field(producer, "producer_executable_sha256", check_id);
    require_sha(producer_executable, "producer executable SHA-256", check_id);
    if (producer_executable != string_field(validation, "producer_executable_sha256", check_id) ||
        producer_executable !=
            string_field(object_field(run_receipt, "production_executable", check_id), "executable_sha256", check_id) ||
        string_field(producer, "schema_catalog_sha256", check_id) !=
            string_field(run_receipt, "schema_catalog_sha256", check_id) ||
        string_field(producer, "science_parameters_sha256", check_id) !=
            string_field(run_receipt, "science_parameters_sha256", check_id) ||
        !json_equal(json_object_get(producer, "artifacts"), json_object_get(run_receipt, "artifacts"))) {
        reject(check_id, "producer receipt does not bind final artifacts/contracts");
    }
    const json_t* draft = object_field(producer, "run_receipt_draft", check_id);
    require_only_keys(draft, {"production_executable", "input_lock_sha256", "phase_ledger_sha256", "performance"},
                      {"production_executable", "input_lock_sha256", "phase_ledger_sha256", "performance"},
                      "run_receipt_draft", check_id);
    if (!json_equal(json_object_get(draft, "production_executable"),
                    json_object_get(run_receipt, "production_executable")) ||
        string_field(draft, "input_lock_sha256", check_id) !=
            string_field(run_receipt, "input_lock_sha256", check_id) ||
        string_field(draft, "phase_ledger_sha256", check_id) !=
            string_field(run_receipt, "phase_ledger_sha256", check_id)) {
        reject(check_id, "producer run-receipt draft binding differs");
    }
}

void verify_artifact_files(RunData& run, const std::string& check_id) {
    for (const auto& [id, artifact] : run.artifacts) {
        static_cast<void>(id);
        const std::filesystem::path path = safe_file_under(run.root, artifact.relative_path, check_id);
        std::error_code error;
        const std::uint64_t size = std::filesystem::file_size(path, error);
        if (error || size != uint_field(artifact.raw.get(), "size_bytes", check_id)) {
            reject(check_id, "artifact physical size differs: " + artifact.relative_path);
        }
        const auto checksum = run.checksum_rows.find(artifact.relative_path);
        if (checksum == run.checksum_rows.end() || checksum->second != artifact.physical_sha256) {
            reject(check_id, "artifact lacks checksum binding: " + artifact.relative_path);
        }
        const json_t* index = json_object_get(artifact.raw.get(), "index");
        if (json_is_object(index)) {
            require_only_keys(index,
                              {"relative_path", "schema_name", "schema_version", "size_bytes", "physical_sha256",
                               "logical_rows", "semantic_sha256"},
                              {"relative_path", "schema_name", "schema_version", "size_bytes", "physical_sha256",
                               "logical_rows", "semantic_sha256"},
                              "artifact.index", check_id);
            const std::string relative = string_field(index, "relative_path", check_id);
            const std::string physical = string_field(index, "physical_sha256", check_id);
            require_sha(physical, "index physical SHA-256", check_id);
            require_sha(string_field(index, "semantic_sha256", check_id), "index semantic SHA-256", check_id);
            const std::filesystem::path index_path = safe_file_under(run.root, relative, check_id);
            if (std::filesystem::file_size(index_path, error) != uint_field(index, "size_bytes", check_id) || error) {
                reject(check_id, "index size differs: " + relative);
            }
            require_checksum_binding(run.checksum_rows, relative, physical, check_id);
        } else if (!json_is_null(index)) {
            reject(check_id, "artifact index must be object or null");
        }
    }
}

[[nodiscard]] RunData load_run(const std::filesystem::path& root, const std::filesystem::path& manifest_path,
                               const std::string& label) {
    const std::string check_id = "FROZEN_ROOT_" + label;
    require_absolute_no_symlink(root, true, check_id);
    require_absolute_no_symlink(manifest_path, false, check_id);
    RunData run;
    run.root = root;
    run.manifest_path = manifest_path;

    run.run_receipt = load_json_file(safe_file_under(root, "run_receipt.json", check_id), check_id);
    const json_t* receipt = run.run_receipt.value.get();
    require_only_keys(receipt,
                      {"schema_name",
                       "schema_version",
                       "run_id",
                       "state",
                       "validation_profile",
                       "production_claim_allowed",
                       "production_executable",
                       "producer_hostname",
                       "producer_kernel_release",
                       "validator_hostname",
                       "validator_kernel_release",
                       "input_mount_identity_sha256",
                       "manifest_sha256",
                       "input_lock_sha256",
                       "phase_ledger_sha256",
                       "artifacts",
                       "truth_fields_seen",
                       "input_snapshot_before_sha256",
                       "input_snapshot_after_sha256",
                       "schema_catalog_sha256",
                       "science_parameters_sha256",
                       "state_history",
                       "performance",
                       "producer_receipt_sha256",
                       "validation_receipt_sha256",
                       "checksums_sha256"},
                      {"schema_name",
                       "schema_version",
                       "run_id",
                       "state",
                       "validation_profile",
                       "production_claim_allowed",
                       "production_executable",
                       "producer_hostname",
                       "producer_kernel_release",
                       "validator_hostname",
                       "validator_kernel_release",
                       "input_mount_identity_sha256",
                       "manifest_sha256",
                       "input_lock_sha256",
                       "phase_ledger_sha256",
                       "artifacts",
                       "truth_fields_seen",
                       "input_snapshot_before_sha256",
                       "input_snapshot_after_sha256",
                       "schema_catalog_sha256",
                       "science_parameters_sha256",
                       "state_history",
                       "performance",
                       "producer_receipt_sha256",
                       "validation_receipt_sha256",
                       "checksums_sha256"},
                      "$", check_id);
    run.run_id = string_field(receipt, "run_id", check_id);
    if (string_field(receipt, "schema_name", check_id) != "longlineage.run_receipt" ||
        string_field(receipt, "schema_version", check_id) != "1.0.0" ||
        string_field(receipt, "state", check_id) != "VALIDATED_FROZEN_DATASET_GATE" ||
        string_field(receipt, "validation_profile", check_id) != "DATASET_GATE" ||
        bool_field(receipt, "production_claim_allowed", check_id) ||
        uint_field(receipt, "truth_fields_seen", check_id) != 0U || root.filename() != run.run_id) {
        reject(check_id, "run is non-frozen, truth-bearing or path-mismatched");
    }
    validate_executable_identity(object_field(receipt, "production_executable", check_id), check_id);
    validate_state_history(array_field(receipt, "state_history", check_id), check_id);
    const std::string input_before = string_field(receipt, "input_snapshot_before_sha256", check_id);
    const std::string input_after = string_field(receipt, "input_snapshot_after_sha256", check_id);
    require_sha(input_before, "input snapshot SHA-256", check_id);
    if (input_before != input_after) {
        reject(check_id, "run input snapshots differ");
    }
    for (const char* field : {"input_mount_identity_sha256", "manifest_sha256", "input_lock_sha256",
                              "phase_ledger_sha256", "schema_catalog_sha256", "science_parameters_sha256",
                              "producer_receipt_sha256", "validation_receipt_sha256", "checksums_sha256"}) {
        require_sha(string_field(receipt, field, check_id), field, check_id);
    }

    run.manifest = load_json_file(manifest_path, check_id);
    if (run.manifest.physical_sha256 != string_field(receipt, "manifest_sha256", check_id)) {
        reject(check_id, "explicit manifest SHA differs from frozen receipt");
    }
    run.compute_workers = validate_manifest(run.manifest.value.get(), root, run.run_id, check_id);

    run.validation_receipt = load_json_file(safe_file_under(root, "validation_receipt.json", check_id), check_id);
    if (run.validation_receipt.physical_sha256 != string_field(receipt, "validation_receipt_sha256", check_id)) {
        reject(check_id, "validation receipt physical SHA differs");
    }
    run.producer_receipt = load_json_file(safe_file_under(root, "receipts/producer_receipt.json", check_id), check_id);
    if (run.producer_receipt.physical_sha256 != string_field(receipt, "producer_receipt_sha256", check_id)) {
        reject(check_id, "producer receipt physical SHA differs");
    }
    validate_validation_receipt(run.validation_receipt.value.get(), receipt, run.run_id,
                                run.producer_receipt.physical_sha256, check_id);
    validate_producer_receipt(run.producer_receipt.value.get(), receipt, run.validation_receipt.value.get(), run.run_id,
                              run.manifest.physical_sha256, check_id);

    run.artifacts = parse_artifacts(array_field(receipt, "artifacts", check_id), check_id);
    validate_expected_science_artifacts(run.artifacts, check_id);
    for (const char* required : {"summary", "semantic_digests"}) {
        if (run.artifacts.count(required) == 0U) {
            reject(check_id, std::string("required closeout artifact absent: ") + required);
        }
    }
    run.checksum_rows = replay_checksums(root, safe_file_under(root, "checksums.sha256", check_id),
                                         string_field(receipt, "checksums_sha256", check_id), check_id);
    require_checksum_binding(run.checksum_rows, "receipts/producer_receipt.json", run.producer_receipt.physical_sha256,
                             check_id);
    verify_artifact_files(run, check_id);
    const ArtifactMeta& semantic = run.artifacts.at("semantic_digests");
    run.semantic_rows =
        parse_semantic_digests(safe_file_under(root, semantic.relative_path, check_id), semantic, check_id);
    validate_semantic_bindings(run.artifacts, run.semantic_rows, check_id);
    const ArtifactMeta& summary = run.artifacts.at("summary");
    run.summary = parse_summary(safe_file_under(root, summary.relative_path, check_id), summary, run.run_id, check_id);
    return run;
}

using SiteOrderKey = std::pair<std::uint64_t, std::uint64_t>;

struct M1Site {
    std::string dataset_id;
    std::uint64_t dataset_order{0};
    std::uint64_t site_order{0};
    std::string chrom;
    std::uint64_t pos1{0};
    std::string ref;
    std::string alt;
    std::string status;
    bool stable{false};
    std::optional<std::uint64_t> group_count;
    std::string canonical_key_line;
};

struct M1Replay {
    std::map<SiteOrderKey, M1Site> by_order;
    std::map<std::string, SiteOrderKey> by_genomic_key;
    std::vector<std::string> ordered_key_lines;
    std::uint64_t evaluable{0};
    std::uint64_t insufficient_alt{0};
    std::uint64_t incomplete_distance{0};
    std::uint64_t stable{0};
    std::string ordered_key_sha256;
    std::string sorted_set_sha256;
};

[[nodiscard]] std::string digest_key_lines(const std::vector<std::string>& lines) {
    Sha256 digest;
    for (const std::string& line : lines) {
        digest.update(line);
    }
    return digest.finish();
}

[[nodiscard]] std::string sorted_key_digest(std::vector<std::string> lines) {
    std::sort(lines.begin(), lines.end());
    return digest_key_lines(lines);
}

[[nodiscard]] bool known_m1_status(std::string_view status) {
    return status == "EVALUABLE" || status == "INSUFFICIENT_ALT_READS" || status == "INCOMPLETE_DISTANCE_BELOW_MINIMUM";
}

[[nodiscard]] M1Replay replay_m1_sites(const RunData& run, const std::string& label) {
    const std::string check_id = "M1_REPLAY_" + label;
    const ArtifactMeta& meta = run.artifacts.at("m1_sites");
    const std::filesystem::path path = safe_file_under(run.root, meta.relative_path, check_id);
    M1Replay replay;
    NativeTsvPreamble preamble;
    std::optional<std::uint64_t> prior_site_order;
    for_each_compressed_line(path, check_id, [&](std::string_view line, std::uint64_t line_number) {
        if (!consume_native_tsv_preamble(line, line_number, meta.schema_name, meta.schema_version, run.run_id, preamble,
                                         check_id)) {
            return;
        }
        const std::vector<std::string> fields = split_tsv(line);
        if (fields.size() != preamble.header.size()) {
            reject(check_id, "M1 TSV row width differs from header");
        }
        const auto field = [&](const char* name) -> const std::string& {
            return fields.at(require_column(preamble.columns, name, check_id));
        };
        M1Site site;
        site.dataset_order = parse_uint(field("dataset_order"), "dataset_order", check_id);
        site.dataset_id = field("dataset_id");
        site.site_order = parse_uint(field("site_order"), "site_order", check_id);
        site.chrom = field("chrom");
        site.pos1 = parse_uint(field("pos1"), "pos1", check_id);
        site.ref = field("ref");
        site.alt = field("alt");
        site.status = field("analysis_status");
        site.stable = parse_bool(field("stable"), "stable", check_id);
        if (field("non_germline_groups") != ".") {
            site.group_count = parse_uint(field("non_germline_groups"), "non_germline_groups", check_id);
        }
        if (site.dataset_order != 0U || site.dataset_id != kDatasetId || site.chrom.empty() || site.pos1 == 0U ||
            site.ref.empty() || site.alt.empty() || !known_m1_status(site.status)) {
            reject(check_id, "M1 site identity/status is outside closed HCC1395 scope");
        }
        if ((!prior_site_order.has_value() && site.site_order != 0U) ||
            (prior_site_order.has_value() && (*prior_site_order == std::numeric_limits<std::uint64_t>::max() ||
                                              site.site_order != *prior_site_order + 1U))) {
            reject(check_id, "M1 site_order is not contiguous from zero");
        }
        prior_site_order = site.site_order;
        if (site.stable) {
            if (site.status != "EVALUABLE" || !site.group_count.has_value() || *site.group_count < 2U) {
                reject(check_id, "stable M1 site lacks evaluable multi-group state");
            }
            ++replay.stable;
        }
        if (site.status == "EVALUABLE") {
            ++replay.evaluable;
        } else if (site.status == "INSUFFICIENT_ALT_READS") {
            ++replay.insufficient_alt;
        } else {
            ++replay.incomplete_distance;
        }
        site.canonical_key_line = site.dataset_id + "\t" + site.chrom + "\t" + std::to_string(site.pos1) + "\t" +
                                  site.ref + "\t" + site.alt + "\n";
        const SiteOrderKey order{site.dataset_order, site.site_order};
        if (replay.by_order.count(order) != 0U ||
            !replay.by_genomic_key.emplace(site.canonical_key_line, order).second) {
            reject(check_id, "M1 site key is duplicated");
        }
        replay.ordered_key_lines.push_back(site.canonical_key_line);
        replay.by_order.emplace(order, std::move(site));
    });
    require_native_tsv_preamble(preamble, check_id);
    for (const char* required : {"dataset_order", "dataset_id", "site_order", "chrom", "pos1", "ref", "alt",
                                 "analysis_status", "stable", "non_germline_groups"}) {
        static_cast<void>(require_column(preamble.columns, required, check_id));
    }
    if (replay.by_order.size() != meta.logical_rows) {
        reject(check_id, "M1 logical row count differs from artifact receipt");
    }
    replay.ordered_key_sha256 = digest_key_lines(replay.ordered_key_lines);
    replay.sorted_set_sha256 = sorted_key_digest(replay.ordered_key_lines);
    return replay;
}

struct M2Replay {
    std::uint64_t cooccurrence_rows{0};
    std::uint64_t unstable_not_run{0};
    std::uint64_t eligible{0};
    std::uint64_t evaluable_ineligible{0};
    std::uint64_t axis_indeterminate{0};
    std::uint64_t group_count_gt10{0};
    std::uint64_t partner_universe_pair_rows{0};
    std::uint64_t exact_identifiable_pairs{0};
    std::uint64_t global_bh_discoveries{0};
    std::uint64_t global_by_discoveries{0};
    std::uint64_t joint_signature_pass_sites{0};
    std::uint64_t joint_signature_not_testable_sites{0};
};

void require_summary_count(const RunData& run, const char* name, std::uint64_t expected, const std::string& check_id) {
    const std::uint64_t observed = uint_field(run.summary.counts.get(), name, check_id);
    if (observed != expected) {
        reject(check_id, std::string("summary count differs: ") + name);
    }
}

[[nodiscard]] M2Replay replay_m2_conservation(const RunData& run, const M1Replay& m1, const std::string& label) {
    const std::string check_id = "M2_CONSERVATION_" + label;
    const ArtifactMeta& meta = run.artifacts.at("cooccurrence_sites");
    const std::filesystem::path path = safe_file_under(run.root, meta.relative_path, check_id);
    M2Replay replay;
    NativeTsvPreamble preamble;
    std::set<SiteOrderKey> observed;
    for_each_compressed_line(path, check_id, [&](std::string_view line, std::uint64_t line_number) {
        if (!consume_native_tsv_preamble(line, line_number, meta.schema_name, meta.schema_version, run.run_id, preamble,
                                         check_id)) {
            return;
        }
        const std::vector<std::string> fields = split_tsv(line);
        if (fields.size() != preamble.header.size()) {
            reject(check_id, "cooccurrence-site TSV row width differs from header");
        }
        const auto field = [&](const char* name) -> const std::string& {
            return fields.at(require_column(preamble.columns, name, check_id));
        };
        const SiteOrderKey key{parse_uint(field("dataset_order"), "dataset_order", check_id),
                               parse_uint(field("site_order"), "site_order", check_id)};
        const auto m1_site = m1.by_order.find(key);
        if (m1_site == m1.by_order.end()) {
            reject(check_id, "cooccurrence site is extra against M1");
        }
        if (!observed.insert(key).second) {
            reject(check_id, "cooccurrence site overlaps an existing terminal category");
        }
        const M1Site& site = m1_site->second;
        if (field("dataset_id") != site.dataset_id || field("chrom") != site.chrom ||
            parse_uint(field("pos1"), "pos1", check_id) != site.pos1 || field("ref") != site.ref ||
            field("alt") != site.alt) {
            reject(check_id, "cooccurrence site identity differs from M1");
        }
        if (site.group_count.has_value()) {
            if (field("m1_group_count") == "." ||
                parse_uint(field("m1_group_count"), "m1_group_count", check_id) != *site.group_count) {
                reject(check_id, "cooccurrence M1 group count differs from M1 site");
            }
        } else if (field("m1_group_count") != ".") {
            reject(check_id, "cooccurrence site invents an M1 group count");
        }
        const std::string& status = field("m2_status");
        const std::string& reason = field("m2_reason");
        if (!site.stable) {
            if (status != "NOT_RUN" || reason != "M1_NOT_FLAGGED") {
                reject(check_id, "unstable M1 site must be NOT_RUN/M1_NOT_FLAGGED");
            }
            ++replay.unstable_not_run;
        } else if (site.group_count.has_value() && *site.group_count > 10U) {
            if (status != "NOT_EVALUABLE" || reason != "GROUP_COUNT_EXCEEDS_PLANNING_MODEL_MAXIMUM") {
                reject(check_id, "M2 group-count ceiling precedence differs");
            }
            ++replay.group_count_gt10;
        } else if (status == "PASS" && reason == "ALL_MEASURED_AXES_DETERMINATE_NO_ALIGNED_CONFOUND") {
            ++replay.eligible;
        } else if (status == "FAIL" && (reason == "HP_AXIS_CONFOUND" || reason == "TECHNICAL_AXIS_CONFOUND" ||
                                        reason == "NOT_PHASE_ANCHORED_ROBUST")) {
            ++replay.evaluable_ineligible;
        } else if (status == "NOT_EVALUABLE" && reason == "AXIS_INDETERMINATE") {
            ++replay.axis_indeterminate;
        } else {
            reject(check_id, "stable M1 site has unsupported M2 terminal category");
        }
        checked_add(replay.partner_universe_pair_rows,
                    parse_uint(field("partner_universe_size"), "partner_universe_size", check_id),
                    "partner_universe_pair_rows", check_id);
        checked_add(replay.exact_identifiable_pairs,
                    parse_uint(field("exact_testable_pairs"), "exact_testable_pairs", check_id),
                    "exact_identifiable_pairs", check_id);
        checked_add(replay.global_bh_discoveries,
                    parse_uint(field("global_bh_discoveries"), "global_bh_discoveries", check_id),
                    "global_bh_discoveries", check_id);
        checked_add(replay.global_by_discoveries,
                    parse_uint(field("global_by_discoveries"), "global_by_discoveries", check_id),
                    "global_by_discoveries", check_id);
        if (field("joint_signature_status") == "PASS") {
            ++replay.joint_signature_pass_sites;
        } else if (field("joint_signature_status") == "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE") {
            ++replay.joint_signature_not_testable_sites;
        } else {
            reject(check_id, "joint_signature_status is outside the closed serialized vocabulary");
        }
        ++replay.cooccurrence_rows;
    });
    require_native_tsv_preamble(preamble, check_id);
    for (const char* required :
         {"dataset_order", "dataset_id", "site_order", "chrom", "pos1", "ref", "alt", "m1_group_count", "m2_status",
          "m2_reason", "partner_universe_size", "exact_testable_pairs", "global_bh_discoveries",
          "global_by_discoveries", "joint_signature_status"}) {
        static_cast<void>(require_column(preamble.columns, required, check_id));
    }
    if (replay.cooccurrence_rows != meta.logical_rows || observed.size() != m1.by_order.size()) {
        reject(check_id, "M1/cooccurrence key set or logical row count differs");
    }
    std::uint64_t remaining = m1.stable;
    for (const std::uint64_t value :
         {replay.eligible, replay.evaluable_ineligible, replay.axis_indeterminate, replay.group_count_gt10}) {
        if (value > remaining) {
            reject(check_id, "M2 terminal bins overlap stable M1 sites");
        }
        remaining -= value;
    }
    if (remaining != 0U) {
        reject(check_id, "M2 terminal bins do not conserve stable M1 sites");
    }
    if (replay.joint_signature_pass_sites + replay.joint_signature_not_testable_sites != replay.cooccurrence_rows) {
        reject(check_id, "joint-signature terminal statuses do not conserve cooccurrence-site rows");
    }
    require_summary_count(run, "site_keys", m1.by_order.size(), check_id);
    require_summary_count(run, "m1_evaluable", m1.evaluable, check_id);
    require_summary_count(run, "m1_insufficient_alt_reads", m1.insufficient_alt, check_id);
    require_summary_count(run, "m1_incomplete_distance", m1.incomplete_distance, check_id);
    require_summary_count(run, "m1_stable_assignments", m1.stable, check_id);
    require_summary_count(run, "m2_eligible", replay.eligible, check_id);
    require_summary_count(run, "m2_evaluable_ineligible", replay.evaluable_ineligible, check_id);
    require_summary_count(run, "m2_axis_indeterminate", replay.axis_indeterminate, check_id);
    require_summary_count(run, "m2_group_count_gt10", replay.group_count_gt10, check_id);
    return replay;
}

struct CooccurrenceAggregateReplay {
    std::uint64_t total_pair_rows{0};
    std::uint64_t exact_identifiable_pairs{0};
    std::uint64_t ineligible_m2_screen_rows{0};
    std::uint64_t eligible_m2_endpoint_a_not_testable_rows{0};
    std::uint64_t eligible_m2_exact_not_identifiable_rows{0};
    std::uint64_t eligible_m2_exact_family_rows{0};
    std::uint64_t global_bh_discoveries{0};
    std::uint64_t exact_by_discoveries{0};
    std::uint64_t formal_pair_by_confirmed{0};
    std::uint64_t family_partition_total{0};
    std::uint64_t fdr_family_size{0};
};

[[nodiscard]] CooccurrenceAggregateReplay replay_cooccurrence_aggregate(const RunData& run, const std::string& label) {
    const std::string check_id = "COOCCURRENCE_AGGREGATE_" + label;
    const ArtifactMeta& meta = run.artifacts.at("cooccurrence_pairs");
    const std::filesystem::path path = safe_file_under(run.root, meta.relative_path, check_id);
    CooccurrenceAggregateReplay replay;
    NativeTsvPreamble preamble;
    std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> prior_key;
    std::optional<std::uint64_t> declared_fdr_family_size;
    for_each_compressed_line(path, check_id, [&](std::string_view line, std::uint64_t line_number) {
        if (!consume_native_tsv_preamble(line, line_number, meta.schema_name, meta.schema_version, run.run_id, preamble,
                                         check_id)) {
            return;
        }
        const std::vector<std::string> fields = split_tsv(line);
        if (fields.size() != preamble.header.size()) {
            reject(check_id, "cooccurrence-pair TSV row width differs from header");
        }
        const auto field = [&](const char* name) -> const std::string& {
            return fields.at(require_column(preamble.columns, name, check_id));
        };
        const std::uint64_t dataset_order = parse_uint(field("dataset_order"), "dataset_order", check_id);
        const std::uint64_t focal_order = parse_uint(field("focal_site_order"), "focal_site_order", check_id);
        const std::uint64_t partner_order = parse_uint(field("partner_site_order"), "partner_site_order", check_id);
        const auto key = std::make_tuple(dataset_order, focal_order, partner_order);
        if (dataset_order != 0U || field("dataset_id") != kDatasetId || (prior_key.has_value() && key <= *prior_key)) {
            reject(check_id, "cooccurrence-pair primary key is outside HCC1395 scope, duplicated or out of order");
        }
        prior_key = key;

        const std::string& family = field("family_status");
        const bool exact_bh = field("exact_bh_discovery") == "true";
        const bool exact_by = field("exact_by_discovery") == "true";
        const bool formal = field("formal_pair_by_confirmed") == "true";
        if ((field("exact_bh_discovery") != "true" && field("exact_bh_discovery") != "false") ||
            (field("exact_by_discovery") != "true" && field("exact_by_discovery") != "false") ||
            (field("formal_pair_by_confirmed") != "true" && field("formal_pair_by_confirmed") != "false")) {
            reject(check_id, "cooccurrence-pair discovery columns are not canonical lowercase booleans");
        }
        const std::string& exact_status = field("exact_status");
        if (exact_status == "EXACT_IDENTIFIABLE") {
            ++replay.exact_identifiable_pairs;
        } else if (exact_status != "NOT_IDENTIFIABLE_STATE_SPACE_LIMIT" &&
                   exact_status != "NOT_IDENTIFIABLE_ENDPOINT_A_NOT_TESTABLE") {
            reject(check_id, "cooccurrence-pair exact_status is outside the closed serialized vocabulary");
        }

        if (family == "INELIGIBLE_M2_SCREEN") {
            ++replay.ineligible_m2_screen_rows;
        } else if (family == "ELIGIBLE_M2_ENDPOINT_A_NOT_TESTABLE") {
            ++replay.eligible_m2_endpoint_a_not_testable_rows;
        } else if (family == "ELIGIBLE_M2_EXACT_NOT_IDENTIFIABLE") {
            ++replay.eligible_m2_exact_not_identifiable_rows;
        } else if (family == "ELIGIBLE_M2_EXACT_FAMILY") {
            ++replay.eligible_m2_exact_family_rows;
            if (field("fdr_family_id") != "GLOBAL_M2_ELIGIBLE_ENDPOINT_A_EXACT_V1" || field("fdr_family_size") == ".") {
                reject(check_id, "eligible exact-family row lacks the frozen global FDR-family binding");
            }
            const std::uint64_t observed_size = parse_uint(field("fdr_family_size"), "fdr_family_size", check_id);
            if (observed_size == 0U ||
                (declared_fdr_family_size.has_value() && *declared_fdr_family_size != observed_size)) {
                reject(check_id, "eligible exact-family rows disagree on the global FDR-family size");
            }
            declared_fdr_family_size = observed_size;
        } else {
            reject(check_id, "cooccurrence-pair family_status is outside the closed schema vocabulary");
        }

        if (family != "ELIGIBLE_M2_EXACT_FAMILY" &&
            (field("fdr_family_id") != "." || field("fdr_family_size") != "." || exact_bh || exact_by || formal)) {
            reject(check_id, "non-family pair carries global FDR or positive-discovery evidence");
        }
        if (family == "ELIGIBLE_M2_ENDPOINT_A_NOT_TESTABLE" &&
            exact_status != "NOT_IDENTIFIABLE_ENDPOINT_A_NOT_TESTABLE") {
            reject(check_id, "endpoint-A-not-testable family row carries an incompatible exact_status");
        }
        if (family == "ELIGIBLE_M2_EXACT_NOT_IDENTIFIABLE" && exact_status != "NOT_IDENTIFIABLE_STATE_SPACE_LIMIT") {
            reject(check_id, "exact-not-identifiable family row carries an incompatible exact_status");
        }
        if (family == "ELIGIBLE_M2_EXACT_FAMILY" && exact_status != "EXACT_IDENTIFIABLE") {
            reject(check_id, "eligible exact-family row is not exact-identifiable");
        }
        if (exact_bh) {
            ++replay.global_bh_discoveries;
        }
        if (exact_by) {
            if (!exact_bh) {
                reject(check_id, "exact BY discovery is not a subset of exact BH discoveries");
            }
            ++replay.exact_by_discoveries;
        }
        if (formal) {
            if (!exact_by) {
                reject(check_id, "formal_pair_by_confirmed is not a subset of exact BY discoveries");
            }
            ++replay.formal_pair_by_confirmed;
        }
        ++replay.total_pair_rows;
    });
    require_native_tsv_preamble(preamble, check_id);
    for (const char* required :
         {"dataset_order", "dataset_id", "focal_site_order", "partner_site_order", "exact_status", "family_status",
          "fdr_family_id", "fdr_family_size", "exact_bh_discovery", "exact_by_discovery", "formal_pair_by_confirmed"}) {
        static_cast<void>(require_column(preamble.columns, required, check_id));
    }
    replay.family_partition_total = replay.ineligible_m2_screen_rows + replay.eligible_m2_endpoint_a_not_testable_rows +
                                    replay.eligible_m2_exact_not_identifiable_rows +
                                    replay.eligible_m2_exact_family_rows;
    replay.fdr_family_size = declared_fdr_family_size.value_or(0U);
    if (replay.total_pair_rows != meta.logical_rows || replay.family_partition_total != replay.total_pair_rows ||
        replay.fdr_family_size != replay.eligible_m2_exact_family_rows ||
        replay.eligible_m2_exact_family_rows > replay.exact_identifiable_pairs ||
        replay.global_bh_discoveries > replay.eligible_m2_exact_family_rows ||
        replay.exact_by_discoveries > replay.global_bh_discoveries ||
        replay.exact_by_discoveries > replay.eligible_m2_exact_family_rows ||
        replay.formal_pair_by_confirmed > replay.exact_by_discoveries) {
        reject(check_id, "cooccurrence-pair aggregate does not conserve rows/family/discovery subsets");
    }
    return replay;
}

void validate_cooccurrence_cross_artifact_conservation(const RunData& run, const M2Replay& sites,
                                                       const CooccurrenceAggregateReplay& pairs,
                                                       const std::string& label) {
    const std::string check_id = "COOCCURRENCE_CROSS_ARTIFACT_" + label;
    const std::uint64_t topology_units = run.artifacts.at("topology_units").logical_rows;
    if (sites.partner_universe_pair_rows != pairs.total_pair_rows ||
        sites.exact_identifiable_pairs != pairs.exact_identifiable_pairs ||
        sites.global_bh_discoveries != pairs.global_bh_discoveries ||
        sites.global_by_discoveries != pairs.exact_by_discoveries ||
        sites.joint_signature_pass_sites != topology_units ||
        uint_field(run.summary.counts.get(), "topology_primary_hp_units", check_id) != topology_units) {
        reject(check_id, "pair/site/joint-signature/topology-unit serialized counters do not conserve");
    }
}

struct HistoricalSite {
    std::string status;
    bool stable{false};
};

struct HistoricalM1 {
    std::map<std::string, HistoricalSite> sites;
    std::vector<std::string> ordered_key_lines;
    std::string ordered_key_sha256;
    std::string sorted_set_sha256;
    std::uint64_t selected_rows{0};
};

[[nodiscard]] std::string map_historical_status(std::string_view status, const std::string& check_id) {
    if (status == "evaluable" || status == "EVALUABLE") {
        return "EVALUABLE";
    }
    if (status == "insufficient_alt_reads" || status == "INSUFFICIENT_ALT_READS") {
        return "INSUFFICIENT_ALT_READS";
    }
    if (status == "incomplete_distance_below_minimum" || status == "INCOMPLETE_DISTANCE_BELOW_MINIMUM") {
        return "INCOMPLETE_DISTANCE_BELOW_MINIMUM";
    }
    reject(check_id, "historical M1 analysis_status is outside mapped vocabulary");
}

[[nodiscard]] HistoricalM1 replay_historical_m1(const std::filesystem::path& path, const std::string& expected_sha256) {
    constexpr const char* kCheckId = "HISTORICAL_M1_SOURCE";
    require_absolute_no_symlink(path, false, kCheckId);
    require_sha(expected_sha256, "historical M1 expected SHA-256", kCheckId);
    if (sha256_file(path, kCheckId) != expected_sha256) {
        reject(kCheckId, "historical M1 physical SHA differs from explicit authority");
    }
    HistoricalM1 replay;
    std::vector<std::string> header;
    std::map<std::string, std::size_t> columns;
    for_each_compressed_line(path, kCheckId, [&](std::string_view line, std::uint64_t line_number) {
        const std::vector<std::string> fields = split_tsv(line);
        if (line_number == 1U) {
            header = fields;
            columns = header_index(header, kCheckId);
            for (const char* required :
                 {"dataset", "chrom", "pos", "ref", "alt", "analysis_status", "stable_null_multigroup"}) {
                static_cast<void>(require_column(columns, required, kCheckId));
            }
            return;
        }
        if (fields.size() != header.size()) {
            reject(kCheckId, "historical TSV row width differs from its header");
        }
        const auto field = [&](const char* name) -> const std::string& {
            return fields.at(require_column(columns, name, kCheckId));
        };
        if (field("dataset") != kDatasetId) {
            return;
        }
        const std::uint64_t pos = parse_uint(field("pos"), "historical pos", kCheckId);
        if (field("chrom").empty() || pos == 0U || field("ref").empty() || field("alt").empty()) {
            reject(kCheckId, "historical HCC1395 site identity is malformed");
        }
        const std::string key = field("dataset") + "\t" + field("chrom") + "\t" + std::to_string(pos) + "\t" +
                                field("ref") + "\t" + field("alt") + "\n";
        HistoricalSite site;
        site.status = map_historical_status(field("analysis_status"), kCheckId);
        site.stable = parse_bool(field("stable_null_multigroup"), "stable_null_multigroup", kCheckId);
        if (!replay.sites.emplace(key, std::move(site)).second) {
            reject(kCheckId, "historical HCC1395 site key is duplicated");
        }
        replay.ordered_key_lines.push_back(key);
        ++replay.selected_rows;
    });
    if (header.empty() || replay.selected_rows == 0U) {
        reject(kCheckId, "historical TSV has no selected HCC1395 rows");
    }
    replay.ordered_key_sha256 = digest_key_lines(replay.ordered_key_lines);
    replay.sorted_set_sha256 = sorted_key_digest(replay.ordered_key_lines);
    if (expected_sha256 == kFormalHistoricalPhysicalSha256 &&
        (replay.selected_rows != kFormalHistoricalHcc1395Rows ||
         replay.ordered_key_sha256 != kFormalHistoricalOrderedKeySha256 ||
         replay.sorted_set_sha256 != kFormalHistoricalSortedSetSha256)) {
        reject(kCheckId, "formal historical authority differs from frozen HCC1395 row/key census");
    }
    return replay;
}

struct HistoricalComparison {
    std::uint64_t missing_keys{0};
    std::uint64_t extra_keys{0};
    std::uint64_t status_mismatches{0};
    std::map<std::pair<std::string, std::string>, std::uint64_t> status_transitions;
    std::uint64_t true_to_true{0};
    std::uint64_t true_to_false{0};
    std::uint64_t false_to_true{0};
    std::uint64_t false_to_false{0};
    std::uint64_t historical_stable{0};
    std::uint64_t current_stable{0};
};

[[nodiscard]] HistoricalComparison compare_historical_m1(const HistoricalM1& historical, const M1Replay& current) {
    constexpr const char* kCheckId = "HISTORICAL_M1_COMPARISON";
    HistoricalComparison comparison;
    for (const auto& [key, old_site] : historical.sites) {
        comparison.historical_stable += old_site.stable ? 1U : 0U;
        const auto current_order = current.by_genomic_key.find(key);
        if (current_order == current.by_genomic_key.end()) {
            ++comparison.missing_keys;
            continue;
        }
        const M1Site& new_site = current.by_order.at(current_order->second);
        comparison.current_stable += new_site.stable ? 1U : 0U;
        ++comparison.status_transitions[{old_site.status, new_site.status}];
        if (old_site.status != new_site.status) {
            ++comparison.status_mismatches;
        }
        if (old_site.stable && new_site.stable) {
            ++comparison.true_to_true;
        } else if (old_site.stable) {
            ++comparison.true_to_false;
        } else if (new_site.stable) {
            ++comparison.false_to_true;
        } else {
            ++comparison.false_to_false;
        }
    }
    for (const auto& [key, order] : current.by_genomic_key) {
        static_cast<void>(order);
        if (historical.sites.count(key) == 0U) {
            ++comparison.extra_keys;
        }
    }
    if (historical.sites.size() != current.by_genomic_key.size() || comparison.missing_keys != 0U ||
        comparison.extra_keys != 0U || historical.ordered_key_lines != current.ordered_key_lines ||
        historical.ordered_key_sha256 != current.ordered_key_sha256 ||
        historical.sorted_set_sha256 != current.sorted_set_sha256) {
        reject(kCheckId, "historical/current HCC1395 ordered site keys are not exact");
    }
    if (comparison.status_mismatches != 0U) {
        reject(kCheckId, "historical/current M1 analysis statuses differ");
    }
    if (comparison.current_stable != current.stable) {
        reject(kCheckId, "historical comparison current stable count is inconsistent");
    }
    return comparison;
}

[[nodiscard]] bool m1_replays_equal(const M1Replay& left, const M1Replay& right) {
    if (left.by_order.size() != right.by_order.size() || left.ordered_key_sha256 != right.ordered_key_sha256 ||
        left.sorted_set_sha256 != right.sorted_set_sha256 || left.evaluable != right.evaluable ||
        left.insufficient_alt != right.insufficient_alt || left.incomplete_distance != right.incomplete_distance ||
        left.stable != right.stable) {
        return false;
    }
    for (const auto& [key, site] : left.by_order) {
        const auto other = right.by_order.find(key);
        if (other == right.by_order.end() || site.canonical_key_line != other->second.canonical_key_line ||
            site.status != other->second.status || site.stable != other->second.stable ||
            site.group_count != other->second.group_count) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool m2_replays_equal(const M2Replay& left, const M2Replay& right) {
    return left.cooccurrence_rows == right.cooccurrence_rows && left.unstable_not_run == right.unstable_not_run &&
           left.eligible == right.eligible && left.evaluable_ineligible == right.evaluable_ineligible &&
           left.axis_indeterminate == right.axis_indeterminate && left.group_count_gt10 == right.group_count_gt10 &&
           left.partner_universe_pair_rows == right.partner_universe_pair_rows &&
           left.exact_identifiable_pairs == right.exact_identifiable_pairs &&
           left.global_bh_discoveries == right.global_bh_discoveries &&
           left.global_by_discoveries == right.global_by_discoveries &&
           left.joint_signature_pass_sites == right.joint_signature_pass_sites &&
           left.joint_signature_not_testable_sites == right.joint_signature_not_testable_sites;
}

[[nodiscard]] bool cooccurrence_replays_equal(const CooccurrenceAggregateReplay& left,
                                              const CooccurrenceAggregateReplay& right) {
    return left.total_pair_rows == right.total_pair_rows &&
           left.exact_identifiable_pairs == right.exact_identifiable_pairs &&
           left.ineligible_m2_screen_rows == right.ineligible_m2_screen_rows &&
           left.eligible_m2_endpoint_a_not_testable_rows == right.eligible_m2_endpoint_a_not_testable_rows &&
           left.eligible_m2_exact_not_identifiable_rows == right.eligible_m2_exact_not_identifiable_rows &&
           left.eligible_m2_exact_family_rows == right.eligible_m2_exact_family_rows &&
           left.global_bh_discoveries == right.global_bh_discoveries &&
           left.exact_by_discoveries == right.exact_by_discoveries &&
           left.formal_pair_by_confirmed == right.formal_pair_by_confirmed &&
           left.family_partition_total == right.family_partition_total && left.fdr_family_size == right.fdr_family_size;
}

[[nodiscard]] JsonPtr run_binding_json(const RunData& run) {
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "run_id", json_string(run.run_id.c_str()));
    json_object_set_new(value.get(), "compute_workers", json_integer(static_cast<json_int_t>(run.compute_workers)));
    json_object_set_new(value.get(), "run_root", json_string(run.root.c_str()));
    json_object_set_new(value.get(), "manifest_path", json_string(run.manifest_path.c_str()));
    json_object_set_new(value.get(), "manifest_sha256", json_string(run.manifest.physical_sha256.c_str()));
    json_object_set_new(value.get(), "run_receipt_sha256", json_string(run.run_receipt.physical_sha256.c_str()));
    json_object_set_new(value.get(), "validation_receipt_sha256",
                        json_string(run.validation_receipt.physical_sha256.c_str()));
    return value;
}

[[nodiscard]] JsonPtr m2_replay_json(const M1Replay& m1, const M2Replay& m2) {
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "m1_site_rows", json_integer(static_cast<json_int_t>(m1.by_order.size())));
    json_object_set_new(value.get(), "m1_stable_assignments", json_integer(static_cast<json_int_t>(m1.stable)));
    json_object_set_new(value.get(), "cooccurrence_site_rows",
                        json_integer(static_cast<json_int_t>(m2.cooccurrence_rows)));
    json_object_set_new(value.get(), "unstable_not_run", json_integer(static_cast<json_int_t>(m2.unstable_not_run)));
    json_object_set_new(value.get(), "m2_eligible", json_integer(static_cast<json_int_t>(m2.eligible)));
    json_object_set_new(value.get(), "m2_evaluable_ineligible",
                        json_integer(static_cast<json_int_t>(m2.evaluable_ineligible)));
    json_object_set_new(value.get(), "m2_axis_indeterminate",
                        json_integer(static_cast<json_int_t>(m2.axis_indeterminate)));
    json_object_set_new(value.get(), "m2_group_count_gt10", json_integer(static_cast<json_int_t>(m2.group_count_gt10)));
    json_object_set_new(value.get(), "partition_total",
                        json_integer(static_cast<json_int_t>(m2.eligible + m2.evaluable_ineligible +
                                                             m2.axis_indeterminate + m2.group_count_gt10)));
    return value;
}

[[nodiscard]] JsonPtr cooccurrence_replay_json(const RunData& run, const M2Replay& sites,
                                               const CooccurrenceAggregateReplay& pairs) {
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "pair_rows", json_integer(static_cast<json_int_t>(pairs.total_pair_rows)));
    json_object_set_new(value.get(), "exact_identifiable_pairs",
                        json_integer(static_cast<json_int_t>(pairs.exact_identifiable_pairs)));
    json_object_set_new(value.get(), "ineligible_m2_screen_pairs",
                        json_integer(static_cast<json_int_t>(pairs.ineligible_m2_screen_rows)));
    json_object_set_new(value.get(), "eligible_endpoint_a_not_testable_pairs",
                        json_integer(static_cast<json_int_t>(pairs.eligible_m2_endpoint_a_not_testable_rows)));
    json_object_set_new(value.get(), "eligible_exact_not_identifiable_pairs",
                        json_integer(static_cast<json_int_t>(pairs.eligible_m2_exact_not_identifiable_rows)));
    json_object_set_new(value.get(), "eligible_exact_family_pairs",
                        json_integer(static_cast<json_int_t>(pairs.eligible_m2_exact_family_rows)));
    json_object_set_new(value.get(), "family_partition_total",
                        json_integer(static_cast<json_int_t>(pairs.family_partition_total)));
    json_object_set_new(value.get(), "fdr_family_size", json_integer(static_cast<json_int_t>(pairs.fdr_family_size)));
    json_object_set_new(value.get(), "global_bh_discoveries",
                        json_integer(static_cast<json_int_t>(pairs.global_bh_discoveries)));
    json_object_set_new(value.get(), "global_by_discoveries",
                        json_integer(static_cast<json_int_t>(pairs.exact_by_discoveries)));
    json_object_set_new(value.get(), "formal_pair_by_confirmed",
                        json_integer(static_cast<json_int_t>(pairs.formal_pair_by_confirmed)));
    json_object_set_new(value.get(), "cooccurrence_site_rows",
                        json_integer(static_cast<json_int_t>(sites.cooccurrence_rows)));
    json_object_set_new(value.get(), "partner_universe_pair_rows",
                        json_integer(static_cast<json_int_t>(sites.partner_universe_pair_rows)));
    json_object_set_new(value.get(), "joint_signature_pass_sites",
                        json_integer(static_cast<json_int_t>(sites.joint_signature_pass_sites)));
    json_object_set_new(value.get(), "joint_signature_not_testable_sites",
                        json_integer(static_cast<json_int_t>(sites.joint_signature_not_testable_sites)));
    json_object_set_new(value.get(), "joint_signature_partition_total",
                        json_integer(static_cast<json_int_t>(sites.joint_signature_pass_sites +
                                                             sites.joint_signature_not_testable_sites)));
    json_object_set_new(value.get(), "topology_units",
                        json_integer(static_cast<json_int_t>(run.artifacts.at("topology_units").logical_rows)));
    return value;
}

[[nodiscard]] JsonPtr status_counts_json(const HistoricalM1& historical) {
    std::uint64_t evaluable = 0;
    std::uint64_t insufficient = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t stable = 0;
    for (const auto& [key, site] : historical.sites) {
        static_cast<void>(key);
        if (site.status == "EVALUABLE") {
            ++evaluable;
        } else if (site.status == "INSUFFICIENT_ALT_READS") {
            ++insufficient;
        } else {
            ++incomplete;
        }
        stable += site.stable ? 1U : 0U;
    }
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "evaluable", json_integer(static_cast<json_int_t>(evaluable)));
    json_object_set_new(value.get(), "insufficient_alt_reads", json_integer(static_cast<json_int_t>(insufficient)));
    json_object_set_new(value.get(), "incomplete_distance_below_minimum",
                        json_integer(static_cast<json_int_t>(incomplete)));
    json_object_set_new(value.get(), "stable", json_integer(static_cast<json_int_t>(stable)));
    return value;
}

[[nodiscard]] JsonPtr status_counts_json(const M1Replay& current) {
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "evaluable", json_integer(static_cast<json_int_t>(current.evaluable)));
    json_object_set_new(value.get(), "insufficient_alt_reads",
                        json_integer(static_cast<json_int_t>(current.insufficient_alt)));
    json_object_set_new(value.get(), "incomplete_distance_below_minimum",
                        json_integer(static_cast<json_int_t>(current.incomplete_distance)));
    json_object_set_new(value.get(), "stable", json_integer(static_cast<json_int_t>(current.stable)));
    return value;
}

[[nodiscard]] JsonPtr make_check(std::string_view check_id, std::string_view evidence_payload) {
    JsonPtr check(json_object());
    json_object_set_new(check.get(), "check_id", json_stringn(check_id.data(), check_id.size()));
    json_object_set_new(check.get(), "status", json_string("PASS"));
    const std::string evidence = sha256_bytes(std::string(check_id) + "\n" + std::string(evidence_payload));
    json_object_set_new(check.get(), "evidence_sha256", json_string(evidence.c_str()));
    return check;
}

[[nodiscard]] JsonPtr not_comparable_json(std::string_view verdict, std::string_view reason_code,
                                          std::string_view explanation) {
    JsonPtr value(json_object());
    json_object_set_new(value.get(), "status", json_string("PASS"));
    json_object_set_new(value.get(), "verdict", json_stringn(verdict.data(), verdict.size()));
    json_object_set_new(value.get(), "reason_code", json_stringn(reason_code.data(), reason_code.size()));
    json_object_set_new(value.get(), "explanation", json_stringn(explanation.data(), explanation.size()));
    return value;
}

[[nodiscard]] JsonPtr build_receipt(const RunData& w24, const RunData& w40, const M1Replay& m1_w24,
                                    const M2Replay& m2_w24, const M2Replay& m2_w40,
                                    const CooccurrenceAggregateReplay& pairs_w24,
                                    const CooccurrenceAggregateReplay& pairs_w40, const HistoricalM1& historical,
                                    const HistoricalComparison& historical_comparison,
                                    const std::string& normalized_manifest_sha256,
                                    const std::string& auditor_executable_sha256,
                                    const Hcc1395DeterminismAuditOptions& options) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "schema_name", json_string(kReceiptSchema.data()));
    json_object_set_new(root.get(), "schema_version", json_string(kReceiptVersion.data()));
    json_object_set_new(root.get(), "overall_status", json_string("PASS"));
    json_object_set_new(root.get(), "production_claim_allowed", json_false());

    JsonPtr generator(json_object());
    json_object_set_new(generator.get(), "language", json_string("C++17"));
    json_object_set_new(generator.get(), "executable_sha256", json_string(auditor_executable_sha256.c_str()));
    json_object_set_new(generator.get(), "git_commit", json_string(LONGLINEAGE_GIT_COMMIT));
    json_object_set_new(generator.get(), "independent_of_producer_kernels", json_true());
    json_object_set_new(generator.get(), "reads_alignment_inputs", json_false());
    json_object_set_new(root.get(), "generator", generator.release());

    JsonPtr runs(json_object());
    json_object_set_new(runs.get(), "w24", run_binding_json(w24).release());
    json_object_set_new(runs.get(), "w40", run_binding_json(w40).release());
    json_object_set_new(root.get(), "runs", runs.release());

    JsonPtr determinism(json_object());
    json_object_set_new(determinism.get(), "status", json_string("PASS"));
    JsonPtr manifest_comparison(json_object());
    json_object_set_new(manifest_comparison.get(), "status", json_string("PASS"));
    JsonPtr allowed(json_array());
    for (const char* field : {"run_id", "output_root", "runtime.compute_workers"}) {
        json_array_append_new(allowed.get(), json_string(field));
    }
    json_object_set_new(manifest_comparison.get(), "allowed_differences", allowed.release());
    json_object_set_new(manifest_comparison.get(), "normalized_sha256",
                        json_string(normalized_manifest_sha256.c_str()));
    json_object_set_new(manifest_comparison.get(), "w24_compute_workers", json_integer(24));
    json_object_set_new(manifest_comparison.get(), "w40_compute_workers", json_integer(40));
    json_object_set_new(determinism.get(), "manifest_comparison", manifest_comparison.release());

    JsonPtr artifact_rows(json_array());
    std::string artifact_evidence;
    for (const ExpectedArtifact& expected : kExpectedArtifacts) {
        const ArtifactMeta& left = w24.artifacts.at(std::string(expected.artifact_id));
        const ArtifactMeta& right = w40.artifacts.at(std::string(expected.artifact_id));
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "artifact_id", json_string(left.artifact_id.c_str()));
        json_object_set_new(row.get(), "schema_name", json_string(left.schema_name.c_str()));
        json_object_set_new(row.get(), "schema_version", json_string(left.schema_version.c_str()));
        json_object_set_new(row.get(), "w24_logical_rows", json_integer(static_cast<json_int_t>(left.logical_rows)));
        json_object_set_new(row.get(), "w40_logical_rows", json_integer(static_cast<json_int_t>(right.logical_rows)));
        json_object_set_new(row.get(), "semantic_sha256", json_string(left.semantic_sha256.c_str()));
        json_object_set_new(row.get(), "semantic_equal", json_true());
        json_object_set_new(row.get(), "physical_equal", json_boolean(left.physical_sha256 == right.physical_sha256));
        json_object_set_new(row.get(), "physical_comparison", json_string("DIAGNOSTIC_ONLY"));
        json_array_append_new(artifact_rows.get(), row.release());
        artifact_evidence += left.artifact_id + "\t" + left.schema_name + "\t" + left.schema_version + "\t" +
                             std::to_string(left.logical_rows) + "\t" + left.semantic_sha256 + "\n";
    }
    json_object_set_new(determinism.get(), "artifacts", artifact_rows.release());

    JsonPtr summary(json_object());
    json_object_set_new(summary.get(), "status", json_string("PASS"));
    json_object_set_new(summary.get(), "semantic_equal", json_true());
    json_object_set_new(summary.get(), "semantic_sha256", json_string(w24.summary.canonical_sha256.c_str()));
    json_object_set_new(summary.get(), "canonicalization",
                        json_string("COMPACT_SORTED_JSON_NO_LF over {counts,phase_status,scope}"));
    json_object_set(summary.get(), "scope", w24.summary.scope.get());
    json_object_set(summary.get(), "counts", w24.summary.counts.get());
    json_object_set(summary.get(), "phase_status", w24.summary.phase_status.get());
    json_object_set_new(determinism.get(), "summary_projection", summary.release());

    JsonPtr m2(json_object());
    json_object_set_new(m2.get(), "status", json_string("PASS"));
    json_object_set_new(m2.get(), "w24", m2_replay_json(m1_w24, m2_w24).release());
    json_object_set_new(m2.get(), "w40", m2_replay_json(m1_w24, m2_w40).release());
    json_object_set_new(determinism.get(), "m2_conservation", m2.release());

    JsonPtr cooccurrence_replay(json_object());
    json_object_set_new(cooccurrence_replay.get(), "status", json_string("PASS"));
    json_object_set_new(cooccurrence_replay.get(), "authority",
                        json_string("VALIDATED_SERIALIZED_CENSUS_NOT_PQ_RECOMPUTATION"));
    json_object_set_new(cooccurrence_replay.get(), "interpretation",
                        json_string("PAIR_ROWS_ARE_RECORDS_NOT_POSITIVE_DISCOVERIES"));
    json_object_set_new(cooccurrence_replay.get(), "w24", cooccurrence_replay_json(w24, m2_w24, pairs_w24).release());
    json_object_set_new(cooccurrence_replay.get(), "w40", cooccurrence_replay_json(w40, m2_w40, pairs_w40).release());
    json_object_set_new(determinism.get(), "cooccurrence_replay", cooccurrence_replay.release());
    json_object_set_new(root.get(), "determinism", determinism.release());

    JsonPtr history(json_object());
    JsonPtr source(json_object());
    json_object_set_new(source.get(), "path", json_string(options.historical_m1_tsv_gz.c_str()));
    json_object_set_new(source.get(), "physical_sha256", json_string(options.historical_m1_sha256.c_str()));
    json_object_set_new(
        source.get(), "authority_profile",
        json_string(options.historical_m1_sha256 == kFormalHistoricalPhysicalSha256 ? "FROZEN_HCC1395_AUTHORITY"
                                                                                    : "SYNTHETIC_TEST_ONLY"));
    json_object_set_new(source.get(), "selected_dataset", json_string(kDatasetId.data()));
    JsonPtr columns(json_array());
    for (const char* column : {"dataset", "chrom", "pos", "ref", "alt", "analysis_status", "stable_null_multigroup"}) {
        json_array_append_new(columns.get(), json_string(column));
    }
    json_object_set_new(source.get(), "selected_columns", columns.release());
    json_object_set_new(source.get(), "truth_derived_fields_consumed", json_integer(0));
    json_object_set_new(history.get(), "source", source.release());

    JsonPtr site_keys(json_object());
    json_object_set_new(site_keys.get(), "status", json_string("PASS"));
    json_object_set_new(site_keys.get(), "verdict", json_string("EXACT"));
    json_object_set_new(site_keys.get(), "canonical_rule",
                        json_string("UTF-8 dataset\\tchrom\\tpos\\tref\\talt\\n; no header; LF; original row order"));
    json_object_set_new(site_keys.get(), "old_count", json_integer(static_cast<json_int_t>(historical.selected_rows)));
    json_object_set_new(site_keys.get(), "new_count", json_integer(static_cast<json_int_t>(m1_w24.by_order.size())));
    json_object_set_new(site_keys.get(), "old_ordered_sha256", json_string(historical.ordered_key_sha256.c_str()));
    json_object_set_new(site_keys.get(), "new_ordered_sha256", json_string(m1_w24.ordered_key_sha256.c_str()));
    json_object_set_new(site_keys.get(), "old_sorted_set_sha256", json_string(historical.sorted_set_sha256.c_str()));
    json_object_set_new(site_keys.get(), "new_sorted_set_sha256", json_string(m1_w24.sorted_set_sha256.c_str()));
    json_object_set_new(site_keys.get(), "missing",
                        json_integer(static_cast<json_int_t>(historical_comparison.missing_keys)));
    json_object_set_new(site_keys.get(), "extra",
                        json_integer(static_cast<json_int_t>(historical_comparison.extra_keys)));
    json_object_set_new(site_keys.get(), "old_duplicates", json_integer(0));
    json_object_set_new(site_keys.get(), "new_duplicates", json_integer(0));
    json_object_set_new(history.get(), "site_keys", site_keys.release());

    JsonPtr m1_history(json_object());
    json_object_set_new(m1_history.get(), "status", json_string("PASS"));
    const std::uint64_t symmetric_difference =
        historical_comparison.true_to_false + historical_comparison.false_to_true;
    json_object_set_new(m1_history.get(), "verdict",
                        json_string(symmetric_difference == 0U ? "EXACT" : "COMPARABLE_DIFFERENT"));
    json_object_set_new(m1_history.get(), "old_counts", status_counts_json(historical).release());
    json_object_set_new(m1_history.get(), "new_counts", status_counts_json(m1_w24).release());
    json_object_set_new(m1_history.get(), "status_mismatches",
                        json_integer(static_cast<json_int_t>(historical_comparison.status_mismatches)));
    JsonPtr transitions(json_array());
    static constexpr std::array<std::string_view, 3> kStatuses = {"EVALUABLE", "INSUFFICIENT_ALT_READS",
                                                                  "INCOMPLETE_DISTANCE_BELOW_MINIMUM"};
    for (const std::string_view from : kStatuses) {
        for (const std::string_view to : kStatuses) {
            JsonPtr transition(json_object());
            json_object_set_new(transition.get(), "from", json_stringn(from.data(), from.size()));
            json_object_set_new(transition.get(), "to", json_stringn(to.data(), to.size()));
            const auto found = historical_comparison.status_transitions.find({std::string(from), std::string(to)});
            const std::uint64_t count = found == historical_comparison.status_transitions.end() ? 0U : found->second;
            json_object_set_new(transition.get(), "count", json_integer(static_cast<json_int_t>(count)));
            json_array_append_new(transitions.get(), transition.release());
        }
    }
    json_object_set_new(m1_history.get(), "status_transitions", transitions.release());
    JsonPtr stable_transition(json_object());
    json_object_set_new(stable_transition.get(), "true_to_true",
                        json_integer(static_cast<json_int_t>(historical_comparison.true_to_true)));
    json_object_set_new(stable_transition.get(), "true_to_false",
                        json_integer(static_cast<json_int_t>(historical_comparison.true_to_false)));
    json_object_set_new(stable_transition.get(), "false_to_true",
                        json_integer(static_cast<json_int_t>(historical_comparison.false_to_true)));
    json_object_set_new(stable_transition.get(), "false_to_false",
                        json_integer(static_cast<json_int_t>(historical_comparison.false_to_false)));
    json_object_set_new(stable_transition.get(), "symmetric_difference",
                        json_integer(static_cast<json_int_t>(symmetric_difference)));
    JsonPtr jaccard(json_object());
    json_object_set_new(jaccard.get(), "intersection",
                        json_integer(static_cast<json_int_t>(historical_comparison.true_to_true)));
    json_object_set_new(
        jaccard.get(), "union",
        json_integer(static_cast<json_int_t>(historical_comparison.true_to_true + symmetric_difference)));
    json_object_set_new(stable_transition.get(), "jaccard", jaccard.release());
    json_object_set_new(m1_history.get(), "stable_transition", stable_transition.release());
    json_object_set_new(history.get(), "m1", m1_history.release());

    json_object_set_new(history.get(), "m2",
                        not_comparable_json("NOT_COMPARABLE_METHOD_CHANGED", "HISTORICAL_SCREENING_AGGREGATE_ONLY",
                                            "Historical M2 is a screening census with a different method boundary; it "
                                            "is not current co-occurrence parity.")
                            .release());
    JsonPtr cooccurrence = not_comparable_json(
        "NOT_COMPARABLE_NO_FORMAL_OLD_RESULT", "NO_SUCCESSFUL_FORMAL_HISTORICAL_AUTHORITY",
        "The historical full co-occurrence attempt failed before publication; no formal old result exists.");
    json_object_set_new(cooccurrence.get(), "old_formal_result_exists", json_false());
    json_object_set_new(cooccurrence.get(), "new_pair_rows",
                        json_integer(static_cast<json_int_t>(pairs_w24.total_pair_rows)));
    json_object_set_new(history.get(), "cooccurrence", cooccurrence.release());
    json_object_set_new(history.get(), "regional_topology",
                        not_comparable_json("NOT_COMPARABLE_METHOD_AND_GATE_CHANGED",
                                            "HISTORICAL_TOPOLOGY_USED_DIFFERENT_METHOD_CN_LOH_BOUNDARY",
                                            "Historical regional reconstruction and current truth-isolated topology "
                                            "use different grains, methods and CN/LOH gates.")
                            .release());
    json_object_set_new(
        history.get(), "runtime",
        not_comparable_json("NOT_COMPARABLE_PROGRAM_SCOPE_THREAD_OUTPUT_CHANGED", "NO_MATCHED_RUNTIME_DENOMINATOR",
                            "Historical runtimes use different programs, scopes, thread models and output contracts; "
                            "no speed ratio is published.")
            .release());
    json_object_set_new(root.get(), "historical", history.release());

    JsonPtr checks(json_array());
    json_array_append_new(checks.get(), make_check("FROZEN_W24", w24.run_receipt.physical_sha256).release());
    json_array_append_new(checks.get(), make_check("FROZEN_W40", w40.run_receipt.physical_sha256).release());
    json_array_append_new(checks.get(), make_check("MANIFEST_WHITELIST", normalized_manifest_sha256).release());
    json_array_append_new(checks.get(), make_check("SCIENCE_ARTIFACT_DETERMINISM", artifact_evidence).release());
    json_array_append_new(checks.get(), make_check("SUMMARY_PROJECTION", w24.summary.canonical_sha256).release());
    json_array_append_new(
        checks.get(),
        make_check("M2_MUTUALLY_EXCLUSIVE_CONSERVATION",
                   std::to_string(m1_w24.stable) + "\t" + std::to_string(m2_w24.eligible) + "\t" +
                       std::to_string(m2_w24.evaluable_ineligible) + "\t" + std::to_string(m2_w24.axis_indeterminate) +
                       "\t" + std::to_string(m2_w24.group_count_gt10))
            .release());
    json_array_append_new(checks.get(), make_check("COOCCURRENCE_SERIALIZED_CENSUS_CONSERVATION",
                                                   std::to_string(pairs_w24.total_pair_rows) + "\t" +
                                                       std::to_string(pairs_w24.exact_identifiable_pairs) + "\t" +
                                                       std::to_string(pairs_w24.eligible_m2_exact_family_rows) + "\t" +
                                                       std::to_string(pairs_w24.global_bh_discoveries) + "\t" +
                                                       std::to_string(pairs_w24.exact_by_discoveries) + "\t" +
                                                       std::to_string(pairs_w24.formal_pair_by_confirmed) + "\t" +
                                                       std::to_string(m2_w24.cooccurrence_rows) + "\t" +
                                                       std::to_string(m2_w24.joint_signature_pass_sites) + "\t" +
                                                       std::to_string(w24.artifacts.at("topology_units").logical_rows))
                                            .release());
    json_array_append_new(checks.get(), make_check("HISTORICAL_SITE_KEY_M1", historical.ordered_key_sha256 + "\t" +
                                                                                 std::to_string(symmetric_difference))
                                            .release());
    json_object_set_new(root.get(), "checks", checks.release());
    return root;
}

void write_atomic(const std::filesystem::path& output, std::string_view bytes) {
    constexpr const char* kCheckId = "AUDIT_RECEIPT_WRITE";
    if (output.empty() || !output.is_absolute() || output.lexically_normal() != output || output.filename().empty()) {
        reject(kCheckId, "output receipt path must be absolute and normalized");
    }
    require_absolute_no_symlink(output.parent_path(), true, kCheckId);
    std::error_code error;
    const auto existing = std::filesystem::symlink_status(output, error);
    if (!error || error != std::errc::no_such_file_or_directory) {
        if (!error || std::filesystem::exists(existing) || std::filesystem::is_symlink(existing)) {
            reject(kCheckId, "output receipt already exists");
        }
    }
    const std::filesystem::path temporary = output.parent_path() / ("." + output.filename().string() + ".tmp." +
                                                                    std::to_string(static_cast<long long>(::getpid())));
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (descriptor < 0) {
        reject(kCheckId, "cannot create temporary audit receipt: " + std::string(std::strerror(errno)));
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            const int saved = errno;
            ::close(descriptor);
            reject(kCheckId, "cannot write audit receipt: " + std::string(std::strerror(saved)));
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
        reject(kCheckId, "cannot fsync/close audit receipt");
    }
    if (::rename(temporary.c_str(), output.c_str()) != 0) {
        reject(kCheckId, "cannot atomically publish audit receipt: " + std::string(std::strerror(errno)));
    }
    const int directory = ::open(output.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0 || ::fsync(directory) != 0 || ::close(directory) != 0) {
        reject(kCheckId, "cannot fsync audit receipt directory");
    }
}

void compare_run_contracts(const RunData& w24, const RunData& w40) {
    constexpr const char* kCheckId = "RUN_CONTRACT_DETERMINISM";
    if (w24.run_id == w40.run_id || w24.compute_workers != 24U || w40.compute_workers != 40U) {
        reject(kCheckId, "explicit runs are not distinct w24/w40 dataset gates");
    }
    JsonPtr normalized_w24 = normalized_manifest(w24.manifest.value.get());
    JsonPtr normalized_w40 = normalized_manifest(w40.manifest.value.get());
    if (!json_equal(normalized_w24.get(), normalized_w40.get())) {
        reject(kCheckId, "manifests differ outside the three-field whitelist");
    }
    const json_t* left = w24.run_receipt.value.get();
    const json_t* right = w40.run_receipt.value.get();
    for (const char* field : {"input_lock_sha256", "phase_ledger_sha256", "input_snapshot_before_sha256",
                              "input_snapshot_after_sha256", "schema_catalog_sha256", "science_parameters_sha256"}) {
        if (string_field(left, field, kCheckId) != string_field(right, field, kCheckId)) {
            reject(kCheckId, std::string("run contract differs: ") + field);
        }
    }
    if (!json_equal(json_object_get(left, "production_executable"), json_object_get(right, "production_executable")) ||
        string_field(w24.validation_receipt.value.get(), "validator_executable_sha256", kCheckId) !=
            string_field(w40.validation_receipt.value.get(), "validator_executable_sha256", kCheckId)) {
        reject(kCheckId, "producer or independent validator binary differs");
    }
    for (const ExpectedArtifact& expected : kExpectedArtifacts) {
        const ArtifactMeta& left_artifact = w24.artifacts.at(std::string(expected.artifact_id));
        const ArtifactMeta& right_artifact = w40.artifacts.at(std::string(expected.artifact_id));
        if (left_artifact.schema_name != right_artifact.schema_name ||
            left_artifact.schema_version != right_artifact.schema_version ||
            left_artifact.logical_rows != right_artifact.logical_rows ||
            left_artifact.semantic_sha256 != right_artifact.semantic_sha256) {
            reject(kCheckId, "science artifact semantic mismatch: " + std::string(expected.artifact_id));
        }
    }
    if (!json_equal(w24.summary.scope.get(), w40.summary.scope.get()) ||
        !json_equal(w24.summary.counts.get(), w40.summary.counts.get()) ||
        !json_equal(w24.summary.phase_status.get(), w40.summary.phase_status.get()) ||
        w24.summary.canonical_sha256 != w40.summary.canonical_sha256) {
        reject(kCheckId, "summary closed projection differs");
    }
}

}  // namespace

Hcc1395DeterminismAuditResult run_hcc1395_determinism_audit(const Hcc1395DeterminismAuditOptions& options) noexcept {
    Hcc1395DeterminismAuditResult result;
    try {
        require_sha(options.historical_m1_sha256, "historical M1 explicit SHA-256", "AUDIT_OPTIONS");
        constexpr std::string_view kGeneratorCommit = LONGLINEAGE_GIT_COMMIT;
        if (kGeneratorCommit.size() != 40U || kGeneratorCommit == "0000000000000000000000000000000000000000") {
            reject("AUDITOR_IDENTITY", "audit executable lacks a non-zero Git commit binding");
        }
        const RunData w24 = load_run(options.w24_run_root, options.w24_manifest, "W24");
        const RunData w40 = load_run(options.w40_run_root, options.w40_manifest, "W40");
        compare_run_contracts(w24, w40);

        const M1Replay m1_w24 = replay_m1_sites(w24, "W24");
        const M1Replay m1_w40 = replay_m1_sites(w40, "W40");
        if (!m1_replays_equal(m1_w24, m1_w40)) {
            reject("M1_WORKER_DETERMINISM", "w24/w40 M1 site keys/status/stable decisions differ");
        }
        const M2Replay m2_w24 = replay_m2_conservation(w24, m1_w24, "W24");
        const M2Replay m2_w40 = replay_m2_conservation(w40, m1_w40, "W40");
        if (!m2_replays_equal(m2_w24, m2_w40)) {
            reject("M2_WORKER_DETERMINISM", "w24/w40 replayed M2 partitions differ");
        }
        const CooccurrenceAggregateReplay pairs_w24 = replay_cooccurrence_aggregate(w24, "W24");
        const CooccurrenceAggregateReplay pairs_w40 = replay_cooccurrence_aggregate(w40, "W40");
        validate_cooccurrence_cross_artifact_conservation(w24, m2_w24, pairs_w24, "W24");
        validate_cooccurrence_cross_artifact_conservation(w40, m2_w40, pairs_w40, "W40");
        if (!cooccurrence_replays_equal(pairs_w24, pairs_w40)) {
            reject("COOCCURRENCE_WORKER_DETERMINISM", "w24/w40 serialized pair-family/discovery aggregates differ");
        }
        const HistoricalM1 historical =
            replay_historical_m1(options.historical_m1_tsv_gz, options.historical_m1_sha256);
        const HistoricalComparison historical_comparison = compare_historical_m1(historical, m1_w24);

        JsonPtr normalized = normalized_manifest(w24.manifest.value.get());
        const std::string normalized_manifest_sha256 = sha256_bytes(canonical_json(normalized.get()));
        std::error_code error;
        const std::filesystem::path executable = std::filesystem::canonical("/proc/self/exe", error);
        if (error) {
            reject("AUDITOR_IDENTITY", "cannot resolve current audit executable identity");
        }
        require_absolute_no_symlink(executable, false, "AUDITOR_IDENTITY");
        const std::string executable_sha256 = sha256_file(executable, "AUDITOR_IDENTITY");
        JsonPtr receipt = build_receipt(w24, w40, m1_w24, m2_w24, m2_w40, pairs_w24, pairs_w40, historical,
                                        historical_comparison, normalized_manifest_sha256, executable_sha256, options);
        char* encoded = json_dumps(receipt.get(), JSON_INDENT(2) | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
        if (encoded == nullptr) {
            reject("AUDIT_RECEIPT", "cannot encode audit receipt");
        }
        result.receipt_json.assign(encoded);
        std::free(encoded);
        result.receipt_json.push_back('\n');
        write_atomic(options.output_receipt, result.receipt_json);
        result.ok = true;
        return result;
    } catch (const AuditError& error) {
        result.error_code = error.code();
        result.detail = error.detail();
    } catch (const std::exception& error) {
        result.error_code = "UNEXPECTED";
        result.detail = error.what();
    } catch (...) {
        result.error_code = "UNEXPECTED";
        result.detail = "unknown non-standard exception";
    }
    return result;
}

}  // namespace longlineage::audit
