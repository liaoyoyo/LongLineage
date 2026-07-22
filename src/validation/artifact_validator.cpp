// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/validation/artifact_validator.hpp"

#include <fcntl.h>
#include <htslib/bgzf.h>
#include <htslib/kstring.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"
#include "longlineage/manifest/production_manifest.hpp"

namespace longlineage::validation {
namespace {

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};

using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;
using SharedJson = std::shared_ptr<json_t>;

class ValidationError final : public std::runtime_error {
   public:
    ValidationError(std::string check_id, std::string detail)
        : std::runtime_error(detail), check_id_(std::move(check_id)) {}

    [[nodiscard]] const std::string& check_id() const noexcept { return check_id_; }

   private:
    std::string check_id_;
};

[[noreturn]] void reject(const std::string& check_id, const std::string& detail) {
    throw ValidationError(check_id, detail);
}

bool is_lower_sha256(std::string_view value) noexcept {
    if (value.size() != 64U) {
        return false;
    }
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool safe_relative_path(const std::string& value) {
    const std::filesystem::path path(value);
    if (value.empty() || path.is_absolute() || path.lexically_normal() != path) {
        return false;
    }
    return std::none_of(path.begin(), path.end(),
                        [](const std::filesystem::path& component) { return component == ".." || component == "."; });
}

std::string string_field(const json_t* object, const char* key) {
    const json_t* value = json_object_get(object, key);
    return json_is_string(value) ? std::string(json_string_value(value)) : std::string{};
}

std::uint64_t uint_field(const json_t* object, const char* key, const std::string& check_id) {
    const json_t* value = json_object_get(object, key);
    if (!json_is_integer(value) || json_integer_value(value) < 0) {
        reject(check_id, std::string("expected nonnegative integer field: ") + key);
    }
    return static_cast<std::uint64_t>(json_integer_value(value));
}

void require_exact_keys(const json_t* object, const std::set<std::string>& required,
                        const std::set<std::string>& optional, const std::string& check_id, const std::string& role) {
    if (!json_is_object(object)) {
        reject(check_id, role + " must be a JSON object");
    }
    for (const std::string& key : required) {
        if (json_object_get(object, key.c_str()) == nullptr) {
            reject(check_id, role + " is missing required field: " + key);
        }
    }
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(const_cast<json_t*>(object), key, value) {
        static_cast<void>(value);
        if (required.count(key) == 0U && optional.count(key) == 0U) {
            reject(check_id, role + " contains unknown field: " + std::string(key));
        }
    }
}

JsonPtr load_json_strict(const std::filesystem::path& path, const std::string& check_id) {
    json_error_t error{};
    json_t* value = json_load_file(path.c_str(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error);
    if (value == nullptr) {
        std::ostringstream detail;
        detail << path.string() << ':' << error.line << ':' << error.column << ": " << error.text;
        reject(check_id, detail.str());
    }
    return JsonPtr(value);
}

std::string sha256_bytes(std::string_view bytes);

struct FileBytesSnapshot {
    std::string bytes;
    std::string sha256;
};

FileBytesSnapshot read_regular_file_snapshot(const std::filesystem::path& path, const std::string& check_id,
                                             std::uint64_t maximum_bytes = 128U * 1024U * 1024U) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        reject(check_id, "cannot open immutable byte snapshot: " + path.string());
    }
    struct stat before {};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > maximum_bytes) {
        ::close(descriptor);
        reject(check_id, "byte snapshot is not a bounded regular file: " + path.string());
    }
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ::close(descriptor);
            reject(check_id, "short read while capturing immutable byte snapshot: " + path.string());
        }
        offset += static_cast<std::size_t>(count);
    }
    char trailing = '\0';
    ssize_t trailing_count = 0;
    do {
        trailing_count = ::read(descriptor, &trailing, 1U);
    } while (trailing_count < 0 && errno == EINTR);
    struct stat after {};
    const bool stable =
        trailing_count == 0 && ::fstat(descriptor, &after) == 0 && before.st_dev == after.st_dev &&
        before.st_ino == after.st_ino && before.st_mode == after.st_mode && before.st_size == after.st_size &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    ::close(descriptor);
    if (!stable) {
        reject(check_id, "file identity changed while capturing immutable byte snapshot: " + path.string());
    }
    return {bytes, sha256_bytes(bytes)};
}

JsonPtr parse_json_snapshot(const FileBytesSnapshot& snapshot, const std::filesystem::path& path,
                            const std::string& check_id) {
    json_error_t error{};
    json_t* value =
        json_loadb(snapshot.bytes.data(), snapshot.bytes.size(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error);
    if (value == nullptr) {
        std::ostringstream detail;
        detail << path.string() << ':' << error.line << ':' << error.column << ": " << error.text;
        reject(check_id, detail.str());
    }
    return JsonPtr(value);
}

SharedJson load_json_shared(const std::filesystem::path& path, const std::string& check_id) {
    JsonPtr loaded = load_json_strict(path, check_id);
    return SharedJson(loaded.release(), JsonDeleter{});
}

class Sha256 final {
   public:
    Sha256() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            reject("INTERNAL_DIGEST", "cannot initialize SHA-256");
        }
    }

    void update(std::string_view bytes) {
        if (!bytes.empty() && EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1) {
            reject("INTERNAL_DIGEST", "cannot update SHA-256");
        }
    }

    [[nodiscard]] std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> raw{};
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(context_.get(), raw.data(), &size) != 1 || size != 32U) {
            reject("INTERNAL_DIGEST", "cannot finalize SHA-256");
        }
        constexpr char kHex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64U);
        for (unsigned int index = 0; index < size; ++index) {
            const unsigned char byte = raw[index];
            result.push_back(kHex[(byte >> 4U) & 0x0fU]);
            result.push_back(kHex[byte & 0x0fU]);
        }
        return result;
    }

   private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

std::string sha256_bytes(std::string_view bytes) {
    Sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

std::string sha256_file(const std::filesystem::path& path, const std::string& check_id) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reject(check_id, "cannot open file for SHA-256: " + path.string());
    }
    Sha256 digest;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            digest.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if (!input.eof()) {
        reject(check_id, "I/O error while hashing: " + path.string());
    }
    return digest.finish();
}

std::filesystem::path require_regular_under(const std::filesystem::path& root, const std::string& relative,
                                            const std::string& check_id) {
    if (!safe_relative_path(relative)) {
        reject(check_id, "unsafe relative path: " + relative);
    }
    std::filesystem::path current = root;
    for (const std::filesystem::path& component : std::filesystem::path(relative)) {
        current /= component;
        struct stat status {};
        if (::lstat(current.c_str(), &status) != 0) {
            reject(check_id, "missing required path: " + current.string());
        }
        if (S_ISLNK(status.st_mode)) {
            reject(check_id, "symlink is forbidden in run root: " + current.string());
        }
        const bool final = current == root / relative;
        if (final ? !S_ISREG(status.st_mode) : !S_ISDIR(status.st_mode)) {
            reject(check_id, "unexpected file type: " + current.string());
        }
    }
    return root / relative;
}

bool canonical_bgzf_eof(const std::filesystem::path& path) {
    static constexpr std::array<unsigned char, 28> kCanonicalEof = {
        0x1f, 0x8b, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x06, 0x00, 0x42, 0x43,
        0x02, 0x00, 0x1b, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    if (input.tellg() < static_cast<std::streamoff>(kCanonicalEof.size())) {
        return false;
    }
    input.seekg(-static_cast<std::streamoff>(kCanonicalEof.size()), std::ios::end);
    std::array<unsigned char, kCanonicalEof.size()> observed{};
    input.read(reinterpret_cast<char*>(observed.data()), static_cast<std::streamsize>(observed.size()));
    return input.gcount() == static_cast<std::streamsize>(observed.size()) && observed == kCanonicalEof;
}

std::vector<std::string> split_tab(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t tab = line.find('\t', begin);
        fields.push_back(line.substr(begin, tab == std::string::npos ? std::string::npos : tab - begin));
        if (tab == std::string::npos) {
            break;
        }
        begin = tab + 1U;
    }
    return fields;
}

std::string join_tab(const std::vector<std::string>& fields) {
    std::string line;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0U) {
            line.push_back('\t');
        }
        line.append(fields[index]);
    }
    return line;
}

std::vector<std::string> string_array(const json_t* value, const std::string& check_id, const std::string& role) {
    if (!json_is_array(value)) {
        reject(check_id, role + " must be an array");
    }
    std::vector<std::string> result;
    std::set<std::string> unique;
    for (std::size_t index = 0; index < json_array_size(value); ++index) {
        const json_t* item = json_array_get(value, index);
        if (!json_is_string(item) || !unique.insert(json_string_value(item)).second) {
            reject(check_id, role + " contains a non-string or duplicate value");
        }
        result.emplace_back(json_string_value(item));
    }
    return result;
}

std::string format_sci17(double value) {
    if (!std::isfinite(value) || (value == 0.0 && std::signbit(value))) {
        reject("JSON_CANONICAL", "non-finite or negative-zero JSON number");
    }
    std::ostringstream raw;
    raw.imbue(std::locale::classic());
    raw << std::scientific << std::setprecision(16) << value;
    const std::string encoded = raw.str();
    const std::size_t exponent = encoded.find('e');
    if (exponent == std::string::npos || exponent + 2U >= encoded.size()) {
        reject("JSON_CANONICAL", "cannot encode JSON real as LONGLINEAGE_SCI17");
    }
    const char sign = encoded[exponent + 1U];
    std::string digits = encoded.substr(exponent + 2U);
    while (digits.size() < 3U) {
        digits.insert(digits.begin(), '0');
    }
    return encoded.substr(0, exponent + 1U) + sign + digits;
}

std::string quote_json_string(const std::string& value) {
    JsonPtr string(json_stringn(value.data(), value.size()));
    if (!string) {
        reject("JSON_CANONICAL", "invalid UTF-8 JSON string");
    }
    char* encoded = json_dumps(string.get(), JSON_ENCODE_ANY | JSON_COMPACT | JSON_ENSURE_ASCII);
    if (encoded == nullptr) {
        reject("JSON_CANONICAL", "cannot encode JSON string");
    }
    std::string result(encoded);
    std::free(encoded);
    return result;
}

class SchemaStore final {
   public:
    explicit SchemaStore(std::filesystem::path repo_root) : repo_root_(std::move(repo_root)) {
        const std::filesystem::path registry_path = repo_root_ / "schema" / "id_registry.json";
        std::error_code error;
        if (!std::filesystem::is_regular_file(registry_path, error)) {
            return;
        }
        const JsonPtr registry = load_json_strict(registry_path, "SCHEMA_REGISTRY");
        const json_t* schemas = json_object_get(registry.get(), "schemas");
        if (!json_is_array(schemas)) {
            return;
        }
        for (std::size_t index = 0; index < json_array_size(schemas); ++index) {
            const json_t* row = json_array_get(schemas, index);
            const std::string id = string_field(row, "id");
            const std::string path = string_field(row, "path");
            if (!id.empty() && safe_relative_path(path)) {
                id_paths_.emplace(id, path);
            }
        }
    }

    SharedJson load_relative(const std::string& relative, const std::string& check_id) {
        if (!safe_relative_path(relative)) {
            reject(check_id, "unsafe schema path: " + relative);
        }
        const auto found = documents_.find(relative);
        if (found != documents_.end()) {
            return found->second;
        }
        SharedJson loaded = load_json_shared(repo_root_ / relative, check_id);
        documents_.emplace(relative, loaded);
        return loaded;
    }

    std::pair<const json_t*, SharedJson> resolve(const json_t* schema, const SharedJson& document,
                                                 const std::string& check_id) {
        const json_t* reference = json_object_get(schema, "$ref");
        if (!json_is_string(reference)) {
            return {schema, document};
        }
        const std::string value = json_string_value(reference);
        if (!value.empty() && value.front() == '#') {
            const json_t* resolved = document.get();
            std::size_t begin = value.size() > 1U && value[1] == '/' ? 2U : 1U;
            while (begin < value.size()) {
                const std::size_t slash = value.find('/', begin);
                std::string token = value.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
                std::size_t escaped = 0;
                while ((escaped = token.find('~', escaped)) != std::string::npos) {
                    if (escaped + 1U >= token.size()) {
                        reject(check_id, "malformed local JSON schema reference: " + value);
                    }
                    const char code = token[escaped + 1U];
                    token.replace(escaped, 2U, code == '1' ? "/" : code == '0' ? "~" : "?");
                    if (code != '0' && code != '1') {
                        reject(check_id, "malformed local JSON schema escape: " + value);
                    }
                    ++escaped;
                }
                resolved = json_object_get(resolved, token.c_str());
                if (resolved == nullptr) {
                    reject(check_id, "unresolved local JSON schema reference: " + value);
                }
                if (slash == std::string::npos) {
                    break;
                }
                begin = slash + 1U;
            }
            return {resolved, document};
        }
        const auto found = id_paths_.find(value);
        if (found == id_paths_.end()) {
            reject(check_id, "external JSON schema reference is not offline registered: " + value);
        }
        SharedJson external = load_relative(found->second, check_id);
        return {external.get(), external};
    }

   private:
    std::filesystem::path repo_root_;
    std::map<std::string, std::string> id_paths_;
    std::map<std::string, SharedJson> documents_;
};

bool basic_schema_type_matches(const json_t* value, const json_t* schema) {
    const json_t* type = json_object_get(schema, "type");
    if (json_is_string(type)) {
        const std::string name = json_string_value(type);
        return (name == "object" && json_is_object(value)) || (name == "array" && json_is_array(value)) ||
               (name == "string" && json_is_string(value)) || (name == "integer" && json_is_integer(value)) ||
               (name == "number" && json_is_number(value)) || (name == "boolean" && json_is_boolean(value)) ||
               (name == "null" && json_is_null(value));
    }
    if (json_is_array(type)) {
        for (std::size_t index = 0; index < json_array_size(type); ++index) {
            json_t* wrapper = json_object();
            json_object_set(wrapper, "type", json_array_get(type, index));
            const bool matches = basic_schema_type_matches(value, wrapper);
            json_decref(wrapper);
            if (matches) {
                return true;
            }
        }
    }
    return true;
}

void validate_json_schema_value(const json_t* value, const json_t* raw_schema, const SharedJson& document,
                                SchemaStore& store);

bool json_schema_accepts(const json_t* value, const json_t* schema, const SharedJson& document, SchemaStore& store) {
    try {
        validate_json_schema_value(value, schema, document, store);
        return true;
    } catch (const ValidationError&) {
        return false;
    }
}

void validate_json_schema_value(const json_t* value, const json_t* raw_schema, const SharedJson& document,
                                SchemaStore& store) {
    const auto resolved = store.resolve(raw_schema, document, "JSON_SCHEMA");
    const json_t* schema = resolved.first;
    const SharedJson& schema_document = resolved.second;
    if (!basic_schema_type_matches(value, schema)) {
        reject("JSON_SCHEMA", "JSON value type differs from record schema");
    }
    const json_t* constant = json_object_get(schema, "const");
    if (constant != nullptr && !json_equal(value, constant)) {
        reject("JSON_SCHEMA", "JSON value differs from schema const");
    }
    const json_t* enumeration = json_object_get(schema, "enum");
    if (json_is_array(enumeration)) {
        bool found = false;
        for (std::size_t index = 0; index < json_array_size(enumeration); ++index) {
            found = found || json_equal(value, json_array_get(enumeration, index));
        }
        if (!found) {
            reject("JSON_SCHEMA", "JSON value is outside the schema enum");
        }
    }

    for (const char* keyword : {"oneOf", "anyOf"}) {
        const json_t* alternatives = json_object_get(schema, keyword);
        if (!json_is_array(alternatives)) {
            continue;
        }
        std::size_t matches = 0;
        const json_t* selected = nullptr;
        for (std::size_t index = 0; index < json_array_size(alternatives); ++index) {
            const json_t* candidate = json_array_get(alternatives, index);
            if (json_schema_accepts(value, candidate, schema_document, store)) {
                ++matches;
                selected = candidate;
            }
        }
        if ((std::string_view(keyword) == "oneOf" && matches != 1U) ||
            (std::string_view(keyword) == "anyOf" && matches == 0U)) {
            reject("JSON_SCHEMA", std::string("JSON value does not satisfy ") + keyword);
        }
        if (selected != nullptr) {
            validate_json_schema_value(value, selected, schema_document, store);
        }
    }
    const json_t* conjunction = json_object_get(schema, "allOf");
    if (json_is_array(conjunction)) {
        for (std::size_t index = 0; index < json_array_size(conjunction); ++index) {
            validate_json_schema_value(value, json_array_get(conjunction, index), schema_document, store);
        }
    }
    const json_t* condition = json_object_get(schema, "if");
    if (json_is_object(condition)) {
        const bool matches = json_schema_accepts(value, condition, schema_document, store);
        const json_t* branch = json_object_get(schema, matches ? "then" : "else");
        if (json_is_object(branch)) {
            validate_json_schema_value(value, branch, schema_document, store);
        }
    }

    if (json_is_integer(value) || json_is_real(value)) {
        const double number = json_number_value(value);
        const json_t* minimum = json_object_get(schema, "minimum");
        const json_t* maximum = json_object_get(schema, "maximum");
        if ((json_is_number(minimum) && number < json_number_value(minimum)) ||
            (json_is_number(maximum) && number > json_number_value(maximum))) {
            reject("JSON_SCHEMA", "JSON number is outside schema bounds");
        }
        if (!std::isfinite(number) || (number == 0.0 && json_is_real(value) && std::signbit(number))) {
            reject("JSON_SCHEMA", "JSON number is non-finite or negative zero");
        }
    }
    if (json_is_string(value)) {
        const std::string text = json_string_value(value);
        const json_t* minimum = json_object_get(schema, "minLength");
        const json_t* maximum = json_object_get(schema, "maxLength");
        if ((json_is_integer(minimum) && text.size() < static_cast<std::size_t>(json_integer_value(minimum))) ||
            (json_is_integer(maximum) && text.size() > static_cast<std::size_t>(json_integer_value(maximum)))) {
            reject("JSON_SCHEMA", "JSON string length is outside schema bounds");
        }
        const json_t* pattern = json_object_get(schema, "pattern");
        if (json_is_string(pattern)) {
            try {
                const std::regex expression(json_string_value(pattern), std::regex::ECMAScript);
                if (!std::regex_search(text, expression)) {
                    reject("JSON_SCHEMA", "JSON string differs from schema pattern");
                }
            } catch (const std::regex_error&) {
                reject("JSON_SCHEMA", "record schema contains an unsupported regular expression");
            }
        }
    }
    if (json_is_array(value)) {
        const std::size_t size = json_array_size(value);
        const json_t* minimum = json_object_get(schema, "minItems");
        const json_t* maximum = json_object_get(schema, "maxItems");
        if ((json_is_integer(minimum) && size < static_cast<std::size_t>(json_integer_value(minimum))) ||
            (json_is_integer(maximum) && size > static_cast<std::size_t>(json_integer_value(maximum)))) {
            std::ostringstream detail;
            detail << "JSON array length observed=" << size;
            if (json_is_integer(minimum)) {
                detail << " minItems=" << json_integer_value(minimum);
            }
            if (json_is_integer(maximum)) {
                detail << " maxItems=" << json_integer_value(maximum);
            }
            reject("JSON_SCHEMA", detail.str());
        }
        if (json_is_true(json_object_get(schema, "uniqueItems"))) {
            for (std::size_t left = 0; left < size; ++left) {
                for (std::size_t right = left + 1U; right < size; ++right) {
                    if (json_equal(json_array_get(value, left), json_array_get(value, right))) {
                        reject("JSON_SCHEMA", "JSON array violates uniqueItems");
                    }
                }
            }
        }
        const json_t* items = json_object_get(schema, "items");
        if (json_is_object(items)) {
            for (std::size_t index = 0; index < size; ++index) {
                validate_json_schema_value(json_array_get(value, index), items, schema_document, store);
            }
        } else if (json_is_array(items)) {
            const std::size_t declared = json_array_size(items);
            for (std::size_t index = 0; index < std::min(size, declared); ++index) {
                validate_json_schema_value(json_array_get(value, index), json_array_get(items, index), schema_document,
                                           store);
            }
            if (size > declared && json_is_false(json_object_get(schema, "additionalItems"))) {
                reject("JSON_SCHEMA", "JSON tuple contains additional items");
            }
        }
    }
    if (json_is_object(value)) {
        const json_t* required = json_object_get(schema, "required");
        if (json_is_array(required)) {
            for (std::size_t index = 0; index < json_array_size(required); ++index) {
                const json_t* key = json_array_get(required, index);
                if (!json_is_string(key) || json_object_get(value, json_string_value(key)) == nullptr) {
                    reject("JSON_SCHEMA", "JSON object is missing a required field");
                }
            }
        }
        const json_t* properties = json_object_get(schema, "properties");
        const json_t* additional = json_object_get(schema, "additionalProperties");
        const char* key = nullptr;
        json_t* child = nullptr;
        json_object_foreach(const_cast<json_t*>(value), key, child) {
            const json_t* child_schema = json_is_object(properties) ? json_object_get(properties, key) : nullptr;
            if (child_schema != nullptr) {
                validate_json_schema_value(child, child_schema, schema_document, store);
            } else if (json_is_false(additional)) {
                reject("JSON_SCHEMA", "JSON object contains an undeclared field: " + std::string(key));
            } else if (json_is_object(additional)) {
                validate_json_schema_value(child, additional, schema_document, store);
            }
        }
    }
}

std::string canonical_json(const json_t* value, const json_t* raw_schema, const SharedJson& document,
                           SchemaStore& store);

std::pair<const json_t*, SharedJson> choose_schema(const json_t* value, const json_t* raw_schema,
                                                   const SharedJson& document, SchemaStore& store) {
    auto resolved = store.resolve(raw_schema, document, "JSON_SCHEMA");
    const json_t* alternatives = json_object_get(resolved.first, "oneOf");
    if (!json_is_array(alternatives)) {
        return resolved;
    }
    std::size_t matches = 0;
    std::pair<const json_t*, SharedJson> chosen{nullptr, document};
    for (std::size_t index = 0; index < json_array_size(alternatives); ++index) {
        const json_t* candidate = json_array_get(alternatives, index);
        auto selected = store.resolve(candidate, resolved.second, "JSON_SCHEMA");
        if (json_schema_accepts(value, selected.first, selected.second, store)) {
            ++matches;
            chosen = selected;
        }
    }
    if (matches != 1U || chosen.first == nullptr) {
        reject("JSON_SCHEMA", "JSON value does not match exactly one oneOf branch");
    }
    return chosen;
}

std::string canonical_json_object(const json_t* value, const json_t* schema, const SharedJson& document,
                                  SchemaStore& store) {
    const json_t* required = json_object_get(schema, "required");
    const json_t* properties = json_object_get(schema, "properties");
    const json_t* additional = json_object_get(schema, "additionalProperties");
    std::vector<std::string> order;
    std::set<std::string> emitted;
    if (json_is_array(required)) {
        for (std::size_t index = 0; index < json_array_size(required); ++index) {
            const json_t* key = json_array_get(required, index);
            if (!json_is_string(key)) {
                reject("JSON_SCHEMA", "required field name is not a string");
            }
            const std::string name = json_string_value(key);
            if (json_object_get(value, name.c_str()) == nullptr) {
                reject("JSON_SCHEMA", "JSON record is missing required field: " + name);
            }
            if (emitted.insert(name).second) {
                order.push_back(name);
            }
        }
    }
    if (json_is_object(properties)) {
        const char* key = nullptr;
        json_t* child_schema = nullptr;
        json_object_foreach(const_cast<json_t*>(properties), key, child_schema) {
            static_cast<void>(child_schema);
            if (json_object_get(value, key) != nullptr && emitted.insert(key).second) {
                order.emplace_back(key);
            }
        }
    }
    std::vector<std::string> extra;
    const char* key = nullptr;
    json_t* child = nullptr;
    json_object_foreach(const_cast<json_t*>(value), key, child) {
        static_cast<void>(child);
        if (emitted.count(key) == 0U) {
            if (json_is_false(additional)) {
                reject("JSON_SCHEMA", "JSON record contains undeclared field: " + std::string(key));
            }
            extra.emplace_back(key);
        }
    }
    std::sort(extra.begin(), extra.end());
    order.insert(order.end(), extra.begin(), extra.end());

    std::string result = "{";
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        const std::string& name = order[index];
        const json_t* child_value = json_object_get(value, name.c_str());
        const json_t* child_schema = json_is_object(properties) ? json_object_get(properties, name.c_str()) : nullptr;
        if (child_schema == nullptr && json_is_object(additional)) {
            child_schema = additional;
        }
        result.append(quote_json_string(name));
        result.push_back(':');
        result.append(child_schema == nullptr ? canonical_json(child_value, json_object(), document, store)
                                              : canonical_json(child_value, child_schema, document, store));
    }
    result.push_back('}');
    return result;
}

std::string canonical_json(const json_t* value, const json_t* raw_schema, const SharedJson& document,
                           SchemaStore& store) {
    const auto selected = choose_schema(value, raw_schema, document, store);
    const json_t* schema = selected.first;
    validate_json_schema_value(value, schema, selected.second, store);
    if (json_is_object(value)) {
        return canonical_json_object(value, schema, selected.second, store);
    }
    if (json_is_array(value)) {
        const json_t* items = json_object_get(schema, "items");
        const json_t* additional_items = json_object_get(schema, "additionalItems");
        JsonPtr unconstrained_schema(json_object());
        if (!unconstrained_schema) {
            reject("INTERNAL_VALIDATOR_ERROR", "cannot allocate unconstrained array item schema");
        }
        std::string result = "[";
        for (std::size_t index = 0; index < json_array_size(value); ++index) {
            if (index != 0U) {
                result.push_back(',');
            }
            const json_t* item = json_array_get(value, index);
            const json_t* item_schema = nullptr;
            if (json_is_object(items)) {
                item_schema = items;
            } else if (json_is_array(items) && index < json_array_size(items)) {
                item_schema = json_array_get(items, index);
            } else if (json_is_object(additional_items)) {
                item_schema = additional_items;
            }
            result.append(item_schema == nullptr
                              ? canonical_json(item, unconstrained_schema.get(), selected.second, store)
                              : canonical_json(item, item_schema, selected.second, store));
        }
        result.push_back(']');
        return result;
    }
    if (json_is_string(value)) {
        return quote_json_string(json_string_value(value));
    }
    if (json_is_integer(value)) {
        return std::to_string(json_integer_value(value));
    }
    if (json_is_real(value)) {
        return format_sci17(json_real_value(value));
    }
    if (json_is_true(value)) {
        return "true";
    }
    if (json_is_false(value)) {
        return "false";
    }
    if (json_is_null(value)) {
        return "null";
    }
    reject("JSON_SCHEMA", "unsupported JSON value");
}

struct ArtifactSpec {
    std::string artifact_id;
    std::string relative_path;
    std::string format;
    std::string record_schema;
    std::string record_schema_sha256;
    std::vector<std::string> primary_key;
    std::vector<std::string> sort_key;
    std::optional<std::string> index_path;
    std::vector<std::string> index_key;
};

struct Catalog {
    std::map<std::string, ArtifactSpec> artifacts;
    std::vector<std::string> scientific_ids;
    std::vector<std::string> artifact_catalog_ids;
    std::vector<std::string> data_lineage_ids;
    std::vector<std::string> semantic_digest_ids;
    std::vector<std::string> producer_receipt_ids;
    std::vector<std::string> checksum_ids;
    bool checksums_include_indexes = false;
};

ArtifactSpec parse_artifact_spec(const json_t* row) {
    ArtifactSpec spec;
    spec.artifact_id = string_field(row, "artifact_id");
    spec.relative_path = string_field(row, "relative_path");
    spec.format = string_field(row, "format");
    spec.record_schema = string_field(row, "record_schema");
    spec.record_schema_sha256 = string_field(row, "record_schema_sha256");
    if (spec.artifact_id.empty() || !safe_relative_path(spec.relative_path) || spec.format.empty() ||
        !safe_relative_path(spec.record_schema) || !is_lower_sha256(spec.record_schema_sha256)) {
        reject("CATALOG_CONTRACT", "catalog artifact row is malformed");
    }
    spec.primary_key = string_array(json_object_get(row, "primary_key"), "CATALOG_CONTRACT", "primary_key");
    spec.sort_key = string_array(json_object_get(row, "sort_key"), "CATALOG_CONTRACT", "sort_key");
    const json_t* index = json_object_get(row, "index");
    const json_t* index_key = json_object_get(row, "index_key");
    if (json_is_null(index) && json_is_null(index_key)) {
        return spec;
    }
    if (!json_is_string(index) || !safe_relative_path(json_string_value(index))) {
        reject("CATALOG_CONTRACT", "catalog index path is malformed");
    }
    spec.index_path = std::string(json_string_value(index));
    spec.index_key = string_array(index_key, "CATALOG_CONTRACT", "index_key");
    if (spec.index_key.empty()) {
        reject("CATALOG_CONTRACT", "indexed artifact lacks index_key");
    }
    return spec;
}

Catalog load_catalog(const std::filesystem::path& repo_root) {
    const JsonPtr catalog_json = load_json_strict(repo_root / "schema" / "catalog.json", "CATALOG_CONTRACT");
    if (!json_is_object(catalog_json.get()) ||
        string_field(catalog_json.get(), "schema_name") != "longlineage.artifact_schema_catalog" ||
        string_field(catalog_json.get(), "schema_version") != "1.0.0") {
        reject("CATALOG_CONTRACT", "unexpected artifact schema catalog identity");
    }
    Catalog catalog;
    const json_t* membership = json_object_get(catalog_json.get(), "run_membership");
    if (!json_is_object(membership)) {
        reject("CATALOG_CONTRACT", "catalog run_membership is absent");
    }
    catalog.scientific_ids =
        string_array(json_object_get(membership, "scientific_artifact_ids"), "CATALOG_CONTRACT", "scientific IDs");
    catalog.artifact_catalog_ids = string_array(json_object_get(membership, "artifact_catalog_row_artifact_ids"),
                                                "CATALOG_CONTRACT", "artifact catalog IDs");
    catalog.data_lineage_ids = string_array(json_object_get(membership, "data_lineage_output_artifact_ids"),
                                            "CATALOG_CONTRACT", "data lineage IDs");
    catalog.semantic_digest_ids = string_array(json_object_get(membership, "semantic_digest_artifact_ids"),
                                               "CATALOG_CONTRACT", "semantic digest IDs");
    catalog.producer_receipt_ids = string_array(json_object_get(membership, "producer_receipt_artifact_ids"),
                                                "CATALOG_CONTRACT", "producer receipt IDs");
    catalog.checksum_ids =
        string_array(json_object_get(membership, "checksums_artifact_ids"), "CATALOG_CONTRACT", "checksum IDs");
    catalog.checksums_include_indexes = json_is_true(json_object_get(membership, "checksums_include_declared_indexes"));

    const json_t* artifacts = json_object_get(catalog_json.get(), "artifacts");
    if (!json_is_array(artifacts)) {
        reject("CATALOG_CONTRACT", "catalog artifacts is not an array");
    }
    for (std::size_t index = 0; index < json_array_size(artifacts); ++index) {
        ArtifactSpec spec = parse_artifact_spec(json_array_get(artifacts, index));
        const std::string artifact_id = spec.artifact_id;
        if (!catalog.artifacts.emplace(artifact_id, std::move(spec)).second) {
            reject("CATALOG_CONTRACT", "duplicate catalog artifact_id: " + artifact_id);
        }
    }
    for (const auto* required : {"producer_receipt", "artifact_catalog", "data_lineage", "semantic_digests",
                                 "checksums", "validation_receipt", "run_receipt"}) {
        if (catalog.artifacts.count(required) == 0U) {
            reject("CATALOG_CONTRACT", std::string("catalog lacks required closeout artifact: ") + required);
        }
    }
    return catalog;
}

struct IndexBinding {
    std::string relative_path;
    std::string schema_name;
    std::string schema_version;
    std::uint64_t size_bytes = 0;
    std::string physical_sha256;
    std::uint64_t logical_rows = 0;
    std::string semantic_sha256;
};

struct ArtifactRecord {
    std::string artifact_id;
    std::string relative_path;
    std::string schema_name;
    std::string schema_version;
    std::string format;
    std::uint64_t size_bytes = 0;
    std::string physical_sha256;
    std::uint64_t logical_rows = 0;
    std::string semantic_sha256;
    std::optional<IndexBinding> index;
    std::string transform_id;
    std::string producer_executable_sha256;
    std::vector<std::string> primary_key_first;
    std::vector<std::string> primary_key_last;
    JsonPtr raw;
};

std::vector<std::string> key_array(const json_t* value, const std::string& field) {
    if (json_is_null(value)) {
        return {};
    }
    if (!json_is_array(value)) {
        reject("PRODUCER_RECEIPT", field + " must be an array or null");
    }
    std::vector<std::string> result;
    for (std::size_t index = 0; index < json_array_size(value); ++index) {
        const json_t* item = json_array_get(value, index);
        if (json_is_string(item)) {
            result.emplace_back(json_string_value(item));
        } else if (json_is_integer(item)) {
            result.push_back(std::to_string(json_integer_value(item)));
        } else {
            reject("PRODUCER_RECEIPT", field + " contains unsupported primary-key type");
        }
    }
    return result;
}

ArtifactRecord parse_artifact_record(const json_t* value) {
    static const std::set<std::string> kRequired = {
        "artifact_id",
        "role",
        "relative_path",
        "schema_name",
        "schema_version",
        "format",
        "size_bytes",
        "physical_sha256",
        "logical_rows",
        "semantic_sha256",
        "index",
        "sensitivity",
        "transform_id",
        "producer_executable_sha256",
        "inputs",
        "primary_key_first",
        "primary_key_last",
    };
    require_exact_keys(value, kRequired, {}, "PRODUCER_RECEIPT", "artifact record");
    ArtifactRecord record;
    record.artifact_id = string_field(value, "artifact_id");
    record.relative_path = string_field(value, "relative_path");
    record.schema_name = string_field(value, "schema_name");
    record.schema_version = string_field(value, "schema_version");
    record.format = string_field(value, "format");
    record.size_bytes = uint_field(value, "size_bytes", "PRODUCER_RECEIPT");
    record.physical_sha256 = string_field(value, "physical_sha256");
    record.logical_rows = uint_field(value, "logical_rows", "PRODUCER_RECEIPT");
    record.semantic_sha256 = string_field(value, "semantic_sha256");
    record.transform_id = string_field(value, "transform_id");
    record.producer_executable_sha256 = string_field(value, "producer_executable_sha256");
    record.primary_key_first = key_array(json_object_get(value, "primary_key_first"), "primary_key_first");
    record.primary_key_last = key_array(json_object_get(value, "primary_key_last"), "primary_key_last");
    if (record.artifact_id.empty() || !safe_relative_path(record.relative_path) || record.schema_name.empty() ||
        record.schema_version.empty() || record.format.empty() || !is_lower_sha256(record.physical_sha256) ||
        !is_lower_sha256(record.semantic_sha256) || record.transform_id.empty() ||
        !is_lower_sha256(record.producer_executable_sha256)) {
        reject("PRODUCER_RECEIPT", "artifact record contains malformed identity or digest fields");
    }
    const json_t* index = json_object_get(value, "index");
    if (!json_is_null(index)) {
        static const std::set<std::string> kIndexKeys = {
            "relative_path",   "schema_name",  "schema_version",  "size_bytes",
            "physical_sha256", "logical_rows", "semantic_sha256",
        };
        require_exact_keys(index, kIndexKeys, {}, "PRODUCER_RECEIPT", "index binding");
        IndexBinding binding;
        binding.relative_path = string_field(index, "relative_path");
        binding.schema_name = string_field(index, "schema_name");
        binding.schema_version = string_field(index, "schema_version");
        binding.size_bytes = uint_field(index, "size_bytes", "PRODUCER_RECEIPT");
        binding.physical_sha256 = string_field(index, "physical_sha256");
        binding.logical_rows = uint_field(index, "logical_rows", "PRODUCER_RECEIPT");
        binding.semantic_sha256 = string_field(index, "semantic_sha256");
        if (!safe_relative_path(binding.relative_path) || binding.schema_name != "longlineage.site_index" ||
            binding.schema_version != "1.0.0" || !is_lower_sha256(binding.physical_sha256) ||
            !is_lower_sha256(binding.semantic_sha256)) {
            reject("PRODUCER_RECEIPT", "index binding is malformed");
        }
        record.index = std::move(binding);
    }
    record.raw.reset(json_deep_copy(value));
    if (!record.raw) {
        reject("PRODUCER_RECEIPT", "cannot retain artifact record");
    }
    return record;
}

struct ProducerReceipt {
    std::string run_id;
    std::string producer_executable_sha256;
    std::string producer_hostname;
    std::string producer_kernel_release;
    std::string input_mount_identity_sha256;
    std::string manifest_sha256;
    std::string input_before;
    std::string input_after;
    std::string catalog_sha256;
    std::string science_sha256;
    std::string finished_at;
    std::string physical_sha256;
    std::map<std::string, ArtifactRecord> artifacts;
    JsonPtr artifacts_raw;
    JsonPtr run_receipt_draft;
    struct MountIdentity {
        std::string dataset_id;
        std::string role;
        std::filesystem::path canonical_path;
    };
    std::vector<MountIdentity> mounts;
};

bool all_lower_hex(std::string_view value);

std::string canonical_compact_sorted_json(const json_t* value, const std::string& check_id) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    if (encoded == nullptr) {
        reject(check_id, "cannot encode canonical compact JSON");
    }
    std::string result(encoded);
    std::free(encoded);
    return result;
}

std::pair<std::string, std::vector<ProducerReceipt::MountIdentity>> validate_input_mount_identity(const json_t* rows) {
    static const std::set<std::string> kRoles = {
        "raw_bam",
        "raw_bam_index",
        "pass_biallelic_ssnv_vcf",
        "pass_biallelic_ssnv_vcf_index",
        "latest_hp_ps_sidecar",
        "latest_hp_ps_sidecar_index",
        "reference_fasta",
        "reference_fai",
    };
    if (!json_is_array(rows) || json_array_size(rows) < kRoles.size()) {
        reject("PRODUCER_PROVENANCE", "input_mount_identity must contain at least one complete eight-role dataset");
    }
    std::map<std::string, std::set<std::string>> observed;
    std::vector<ProducerReceipt::MountIdentity> ordered;
    ordered.reserve(json_array_size(rows));
    for (std::size_t index = 0; index < json_array_size(rows); ++index) {
        const json_t* row = json_array_get(rows, index);
        static const std::set<std::string> kRequired = {
            "dataset_id",      "role",     "canonical_path",       "mount_source",
            "filesystem_type", "readonly", "mount_options_sha256",
        };
        require_exact_keys(row, kRequired, {}, "PRODUCER_PROVENANCE", "input mount identity");
        const std::string dataset = string_field(row, "dataset_id");
        const std::string role = string_field(row, "role");
        const std::string canonical_path = string_field(row, "canonical_path");
        const std::string mount_source = string_field(row, "mount_source");
        const std::string filesystem = string_field(row, "filesystem_type");
        const std::string options_sha256 = string_field(row, "mount_options_sha256");
        const json_t* readonly = json_object_get(row, "readonly");
        const std::filesystem::path path(canonical_path);
        if (dataset.empty() || kRoles.count(role) == 0U || !path.is_absolute() || path.lexically_normal() != path ||
            mount_source.empty() || filesystem.empty() || !json_is_boolean(readonly) ||
            !is_lower_sha256(options_sha256) || !observed[dataset].insert(role).second) {
            reject("PRODUCER_PROVENANCE", "input mount identity row is malformed or duplicated");
        }
        ordered.push_back({dataset, role, path});
    }
    for (const auto& [dataset, roles] : observed) {
        if (dataset.empty() || roles != kRoles) {
            reject("PRODUCER_PROVENANCE", "input mount identity does not contain exactly eight roles per dataset");
        }
    }
    return {sha256_bytes(canonical_compact_sorted_json(rows, "PRODUCER_PROVENANCE") + "\n"), std::move(ordered)};
}

void validate_run_receipt_draft(const json_t* draft, const std::string& producer_executable_sha256,
                                const std::filesystem::path& repo_root, bool bind_current_phase_ledger) {
    static const std::set<std::string> kDraftKeys = {"production_executable", "input_lock_sha256",
                                                     "phase_ledger_sha256", "performance"};
    require_exact_keys(draft, kDraftKeys, {}, "RUN_RECEIPT_DRAFT", "run receipt draft");
    const json_t* executable = json_object_get(draft, "production_executable");
    static const std::set<std::string> kExecutableKeys = {
        "name", "version", "git_commit", "executable_sha256", "compiler", "htslib_version"};
    require_exact_keys(executable, kExecutableKeys, {}, "RUN_RECEIPT_DRAFT", "production executable");
    const std::string commit = string_field(executable, "git_commit");
    if (string_field(executable, "name") != "longlineage" || string_field(executable, "version").empty() ||
        commit.size() != 40U || !all_lower_hex(commit) ||
        string_field(executable, "executable_sha256") != producer_executable_sha256 ||
        string_field(executable, "compiler").empty() || string_field(executable, "htslib_version") != "1.18") {
        reject("RUN_RECEIPT_DRAFT", "production executable identity differs from producer receipt");
    }
    for (const char* digest : {"input_lock_sha256", "phase_ledger_sha256"}) {
        if (!is_lower_sha256(string_field(draft, digest))) {
            reject("RUN_RECEIPT_DRAFT", std::string(digest) + " is not a canonical SHA-256");
        }
    }
    if (bind_current_phase_ledger) {
        const std::filesystem::path phase_ledger =
            require_regular_under(repo_root, "state/phase_ledger.json", "RUN_RECEIPT_DRAFT");
        if (string_field(draft, "phase_ledger_sha256") != sha256_file(phase_ledger, "RUN_RECEIPT_DRAFT")) {
            reject("RUN_RECEIPT_DRAFT", "phase_ledger_sha256 differs from current repository phase ledger");
        }
    }
    const json_t* performance = json_object_get(draft, "performance");
    static const std::set<std::string> kPerformanceKeys = {
        "wall_seconds",       "user_seconds",         "system_seconds",       "memory_peak_bytes", "oom_events",
        "io_read_bytes",      "io_write_bytes",       "major_page_faults",    "minor_page_faults", "peak_threads",
        "queue_wait_seconds", "reorder_wait_seconds", "task_latency_seconds", "logical_records",   "logical_bytes",
        "final_file_count",   "transient_file_count", "cache_condition"};
    require_exact_keys(performance, kPerformanceKeys, {}, "RUN_RECEIPT_DRAFT", "performance");
    for (const char* field :
         {"wall_seconds", "user_seconds", "system_seconds", "queue_wait_seconds", "reorder_wait_seconds"}) {
        const json_t* value = json_object_get(performance, field);
        if (!json_is_number(value) || !std::isfinite(json_number_value(value)) || json_number_value(value) < 0.0) {
            reject("RUN_RECEIPT_DRAFT", std::string(field) + " is not a nonnegative finite number");
        }
    }
    for (const char* field :
         {"memory_peak_bytes", "oom_events", "io_read_bytes", "io_write_bytes", "major_page_faults",
          "minor_page_faults", "logical_records", "logical_bytes", "final_file_count", "transient_file_count"}) {
        static_cast<void>(uint_field(performance, field, "RUN_RECEIPT_DRAFT"));
    }
    const std::uint64_t peak_threads = uint_field(performance, "peak_threads", "RUN_RECEIPT_DRAFT");
    if (peak_threads == 0U || peak_threads > 46U) {
        reject("RUN_RECEIPT_DRAFT", "peak_threads is outside [1,46]");
    }
    const json_t* latency = json_object_get(performance, "task_latency_seconds");
    static const std::set<std::string> kLatencyKeys = {"p50", "p95", "p99", "max"};
    require_exact_keys(latency, kLatencyKeys, {}, "RUN_RECEIPT_DRAFT", "task latency");
    for (const char* field : {"p50", "p95", "p99", "max"}) {
        const json_t* value = json_object_get(latency, field);
        if (!json_is_number(value) || !std::isfinite(json_number_value(value)) || json_number_value(value) < 0.0) {
            reject("RUN_RECEIPT_DRAFT", std::string("task latency ") + field + " is not a nonnegative finite number");
        }
    }
    static const std::set<std::string> kCache = {"COLD", "WARM", "MIXED", "UNKNOWN"};
    if (kCache.count(string_field(performance, "cache_condition")) == 0U) {
        reject("RUN_RECEIPT_DRAFT", "cache condition is outside the contract");
    }
}

bool is_canonical_science_catalog(const Catalog& catalog);

ProducerReceipt load_producer_receipt(const std::filesystem::path& repo_root, const std::filesystem::path& run_root,
                                      const Catalog& catalog) {
    const ArtifactSpec& receipt_spec = catalog.artifacts.at("producer_receipt");
    const std::filesystem::path path = require_regular_under(run_root, receipt_spec.relative_path, "PRODUCER_RECEIPT");
    const FileBytesSnapshot receipt_snapshot = read_regular_file_snapshot(path, "PRODUCER_RECEIPT");
    const JsonPtr receipt = parse_json_snapshot(receipt_snapshot, path, "PRODUCER_RECEIPT");
    static const std::set<std::string> kRequired = {
        "schema_name",
        "schema_version",
        "run_id",
        "state",
        "producer_outcome",
        "producer_executable_sha256",
        "producer_hostname",
        "producer_kernel_release",
        "input_mount_identity",
        "manifest_sha256",
        "input_snapshot_before_sha256",
        "input_snapshot_after_sha256",
        "schema_catalog_sha256",
        "science_parameters_sha256",
        "artifacts",
        "run_receipt_draft",
        "truth_fields_seen",
        "finished_at",
    };
    require_exact_keys(receipt.get(), kRequired, {"failure_reason"}, "PRODUCER_RECEIPT", "producer receipt");
    if (string_field(receipt.get(), "schema_name") != "longlineage.producer_receipt" ||
        string_field(receipt.get(), "schema_version") != "1.0.0" || string_field(receipt.get(), "state") != "RUNNING" ||
        string_field(receipt.get(), "producer_outcome") != "READY_FOR_VALIDATION" ||
        !json_is_integer(json_object_get(receipt.get(), "truth_fields_seen")) ||
        json_integer_value(json_object_get(receipt.get(), "truth_fields_seen")) != 0) {
        reject("PRODUCER_RECEIPT", "producer receipt is not truth-isolated READY_FOR_VALIDATION/RUNNING");
    }
    const json_t* failure_reason = json_object_get(receipt.get(), "failure_reason");
    if (failure_reason != nullptr && !json_is_null(failure_reason)) {
        reject("PRODUCER_RECEIPT", "READY producer receipt carries a failure reason");
    }

    ProducerReceipt parsed;
    parsed.run_id = string_field(receipt.get(), "run_id");
    parsed.producer_executable_sha256 = string_field(receipt.get(), "producer_executable_sha256");
    parsed.producer_hostname = string_field(receipt.get(), "producer_hostname");
    parsed.producer_kernel_release = string_field(receipt.get(), "producer_kernel_release");
    std::tie(parsed.input_mount_identity_sha256, parsed.mounts) =
        validate_input_mount_identity(json_object_get(receipt.get(), "input_mount_identity"));
    parsed.manifest_sha256 = string_field(receipt.get(), "manifest_sha256");
    parsed.input_before = string_field(receipt.get(), "input_snapshot_before_sha256");
    parsed.input_after = string_field(receipt.get(), "input_snapshot_after_sha256");
    parsed.catalog_sha256 = string_field(receipt.get(), "schema_catalog_sha256");
    parsed.science_sha256 = string_field(receipt.get(), "science_parameters_sha256");
    parsed.finished_at = string_field(receipt.get(), "finished_at");
    for (const auto* digest : {&parsed.producer_executable_sha256, &parsed.manifest_sha256, &parsed.input_before,
                               &parsed.input_after, &parsed.catalog_sha256, &parsed.science_sha256}) {
        if (!is_lower_sha256(*digest)) {
            reject("PRODUCER_RECEIPT", "producer receipt contains malformed SHA-256");
        }
    }
    static const std::regex kHostname("^[A-Za-z0-9][A-Za-z0-9._-]{0,252}$");
    if (parsed.run_id.empty() || parsed.input_before != parsed.input_after ||
        !std::regex_match(parsed.producer_hostname, kHostname) || parsed.producer_kernel_release.empty()) {
        reject("PRODUCER_RECEIPT", "producer receipt run_id/environment is malformed or input snapshots differ");
    }
    const std::string observed_catalog = sha256_file(repo_root / "schema" / "catalog.json", "PRODUCER_RECEIPT");
    if (observed_catalog != parsed.catalog_sha256) {
        reject("PRODUCER_RECEIPT", "producer receipt schema_catalog_sha256 mismatch");
    }
    const std::filesystem::path science_path = repo_root / "contracts" / "v1" / "science_parameters.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(science_path, error) ||
        sha256_file(science_path, "PRODUCER_RECEIPT") != parsed.science_sha256) {
        reject("PRODUCER_RECEIPT", "producer receipt science_parameters_sha256 mismatch");
    }
    const json_t* artifacts = json_object_get(receipt.get(), "artifacts");
    if (!json_is_array(artifacts)) {
        reject("PRODUCER_RECEIPT", "producer receipt artifacts must be an array");
    }
    for (std::size_t index = 0; index < json_array_size(artifacts); ++index) {
        ArtifactRecord record = parse_artifact_record(json_array_get(artifacts, index));
        const std::string artifact_id = record.artifact_id;
        if (record.producer_executable_sha256 != parsed.producer_executable_sha256) {
            reject("PRODUCER_PROVENANCE", "artifact producer executable differs from producer receipt");
        }
        if (!parsed.artifacts.emplace(artifact_id, std::move(record)).second) {
            reject("PRODUCER_RECEIPT", "duplicate artifact record: " + artifact_id);
        }
    }
    parsed.artifacts_raw.reset(json_deep_copy(artifacts));
    parsed.run_receipt_draft.reset(json_deep_copy(json_object_get(receipt.get(), "run_receipt_draft")));
    if (!parsed.artifacts_raw || !parsed.run_receipt_draft) {
        reject("PRODUCER_RECEIPT", "cannot retain producer artifacts or run receipt draft");
    }
    validate_run_receipt_draft(parsed.run_receipt_draft.get(), parsed.producer_executable_sha256, repo_root,
                               is_canonical_science_catalog(catalog));
    parsed.physical_sha256 = receipt_snapshot.sha256;
    return parsed;
}

bool is_canonical_science_catalog(const Catalog& catalog) {
    static const std::set<std::string> kCanonicalScientific = {
        "site_reads",         "methyl_calls",       "bernoulli_upper", "m1_sites", "m1_assignments",
        "cooccurrence_pairs", "cooccurrence_sites", "topology_units",  "summary"};
    return std::set<std::string>(catalog.scientific_ids.begin(), catalog.scientific_ids.end()) == kCanonicalScientific;
}

struct ExpectedInputBinding {
    std::string source_kind;
    std::string source_id;
    std::string digest_kind;
    std::string sha256;
};

void validate_hcc1395_authority_contract(const std::filesystem::path& repo_root, const ProductionManifest& manifest) {
    const std::filesystem::path relative = "oracle/hcc1395_dataset_gate_input_authority.json";
    const std::filesystem::path path =
        require_regular_under(repo_root, relative.generic_string(), "HCC1395_AUTHORITY_REPLAY");
    const FileBytesSnapshot snapshot = read_regular_file_snapshot(path, "HCC1395_AUTHORITY_REPLAY");
    if (snapshot.sha256 != manifest.contract_bindings.dataset_gate_input_authority_sha256) {
        reject("HCC1395_AUTHORITY_REPLAY", "HCC1395 authority SHA-256 differs from manifest binding");
    }
    const JsonPtr authority = parse_json_snapshot(snapshot, path, "HCC1395_AUTHORITY_REPLAY");
    static const std::set<std::string> kAuthorityKeys = {
        "schema_name",     "schema_version", "authority_id", "authority_profile",           "allowed_terminal_state",
        "dataset_id",      "dataset_order",  "truth_fields", "private_source_paths_stored", "tagged_bam_persisted",
        "latest_tag_join", "variant_scope",  "files",        "full_content_freeze",         "claim",
    };
    require_exact_keys(authority.get(), kAuthorityKeys, {}, "HCC1395_AUTHORITY_REPLAY", "HCC1395 authority");
    if (string_field(authority.get(), "schema_name") != "longlineage.dataset_gate_input_authority" ||
        string_field(authority.get(), "schema_version") != "1.0.0" ||
        string_field(authority.get(), "authority_id") != "HCC1395_RAW_BAM_LPS_SIDECAR_V2_PASS_SSNV_GRCH38_20260720" ||
        string_field(authority.get(), "authority_profile") != "HCC1395_DATASET_GATE" ||
        string_field(authority.get(), "allowed_terminal_state") != "VALIDATED_FROZEN_DATASET_GATE" ||
        string_field(authority.get(), "dataset_id") != "HCC1395" ||
        uint_field(authority.get(), "dataset_order", "HCC1395_AUTHORITY_REPLAY") != 0U ||
        uint_field(authority.get(), "truth_fields", "HCC1395_AUTHORITY_REPLAY") != 0U ||
        !json_is_false(json_object_get(authority.get(), "private_source_paths_stored")) ||
        !json_is_false(json_object_get(authority.get(), "tagged_bam_persisted")) ||
        string_field(authority.get(), "latest_tag_join") != "EXACT_PROJECTION_NO_FALLBACK") {
        reject("HCC1395_AUTHORITY_REPLAY", "HCC1395 authority identity or truth-isolation claim differs");
    }
    const json_t* variant_scope = json_object_get(authority.get(), "variant_scope");
    static const std::set<std::string> kVariantKeys = {"all_pass_biallelic_ssnv", "autosomal_chr1_to_chr22",
                                                       "outside_autosomal_scope"};
    require_exact_keys(variant_scope, kVariantKeys, {}, "HCC1395_AUTHORITY_REPLAY", "HCC1395 variant census");
    const std::uint64_t all = uint_field(variant_scope, "all_pass_biallelic_ssnv", "HCC1395_AUTHORITY_REPLAY");
    const std::uint64_t autosomal = uint_field(variant_scope, "autosomal_chr1_to_chr22", "HCC1395_AUTHORITY_REPLAY");
    const std::uint64_t outside = uint_field(variant_scope, "outside_autosomal_scope", "HCC1395_AUTHORITY_REPLAY");
    if (all != 113061U || autosomal != 79687U || outside != 33374U || autosomal + outside != all) {
        reject("HCC1395_AUTHORITY_REPLAY", "HCC1395 frozen variant census differs or does not conserve");
    }
    const json_t* claim = json_object_get(authority.get(), "claim");
    static const std::set<std::string> kClaimKeys = {"dataset_gate_allowed", "production_seven_dataset_claim_allowed",
                                                     "cross_dataset_generalization_allowed"};
    require_exact_keys(claim, kClaimKeys, {}, "HCC1395_AUTHORITY_REPLAY", "HCC1395 claim boundary");
    if (!json_is_true(json_object_get(claim, "dataset_gate_allowed")) ||
        !json_is_false(json_object_get(claim, "production_seven_dataset_claim_allowed")) ||
        !json_is_false(json_object_get(claim, "cross_dataset_generalization_allowed"))) {
        reject("HCC1395_AUTHORITY_REPLAY", "HCC1395 claim boundary differs from dataset-gate-only authority");
    }
    const json_t* freeze = json_object_get(authority.get(), "full_content_freeze");
    static const std::set<std::string> kFreezeKeys = {
        "command",          "started_at", "observed_complete_no_later_than", "observed_wall_upper_bound_seconds",
        "timing_precision", "cost_domain"};
    require_exact_keys(freeze, kFreezeKeys, {}, "HCC1395_AUTHORITY_REPLAY", "HCC1395 full-content freeze");
    if (string_field(freeze, "command") != "sha256sum <raw_bam> <reference_fasta>" ||
        string_field(freeze, "started_at").empty() || string_field(freeze, "observed_complete_no_later_than").empty() ||
        uint_field(freeze, "observed_wall_upper_bound_seconds", "HCC1395_AUTHORITY_REPLAY") != 3148U ||
        string_field(freeze, "timing_precision") != "POLL_BOUNDED_NOT_EXACT" ||
        string_field(freeze, "cost_domain") != "INPUT_FREEZE_NOT_SCIENCE_RUNTIME") {
        reject("HCC1395_AUTHORITY_REPLAY", "HCC1395 full-content freeze metadata differs");
    }
    if (manifest.datasets.size() != 1U || manifest.datasets.front().dataset_id != "HCC1395" ||
        manifest.datasets.front().dataset_order != 0U) {
        reject("HCC1395_AUTHORITY_REPLAY", "HCC1395 manifest dataset identity differs from authority");
    }
    std::map<std::string, const LockedFile*> manifest_files;
    for (const LockedFile& file : manifest.datasets.front().files) {
        manifest_files.emplace(std::string(to_string(file.role)), &file);
    }
    const std::map<std::string, std::string> expected_tokens = {
        {"raw_bam", "LOCAL_HCC1395_RAW_BAM"},
        {"raw_bam_index", "LOCAL_HCC1395_RAW_BAM_BAI"},
        {"pass_biallelic_ssnv_vcf", "LOCAL_HCC1395_LPS_PASS_VCF"},
        {"pass_biallelic_ssnv_vcf_index", "LOCAL_HCC1395_LPS_PASS_VCF_CSI"},
        {"latest_hp_ps_sidecar", "LOCAL_HCC1395_LPS_SIDECAR_V2"},
        {"latest_hp_ps_sidecar_index", "LOCAL_HCC1395_LPS_SIDECAR_V2_TBI"},
        {"reference_fasta", "LOCAL_GRCH38_NO_ALT_FASTA"},
        {"reference_fai", "LOCAL_GRCH38_NO_ALT_FASTA_FAI"},
    };
    const json_t* files = json_object_get(authority.get(), "files");
    if (!json_is_array(files) || json_array_size(files) != expected_tokens.size() ||
        manifest_files.size() != expected_tokens.size()) {
        reject("HCC1395_AUTHORITY_REPLAY", "HCC1395 authority/manifest must contain exactly eight locked roles");
    }
    std::set<std::string> observed_roles;
    for (std::size_t index = 0; index < json_array_size(files); ++index) {
        const json_t* row = json_array_get(files, index);
        static const std::set<std::string> kFileKeys = {"role", "path_token", "size_bytes", "sha256"};
        require_exact_keys(row, kFileKeys, {}, "HCC1395_AUTHORITY_REPLAY", "HCC1395 authority file");
        const std::string role = string_field(row, "role");
        const auto manifest_file = manifest_files.find(role);
        const auto token = expected_tokens.find(role);
        const std::string digest = string_field(row, "sha256");
        if (manifest_file == manifest_files.end() || token == expected_tokens.end() ||
            !observed_roles.insert(role).second || string_field(row, "path_token") != token->second ||
            !is_lower_sha256(digest) ||
            uint_field(row, "size_bytes", "HCC1395_AUTHORITY_REPLAY") != manifest_file->second->size_bytes ||
            digest != manifest_file->second->sha256) {
            reject("HCC1395_AUTHORITY_REPLAY", "HCC1395 authority file size/SHA or role differs from manifest");
        }
    }
}

std::string verify_manifest_contract_bindings(const std::filesystem::path& repo_root,
                                              const ProductionManifest& manifest) {
    const std::vector<std::pair<std::filesystem::path, std::string>> bindings = {
        {"schema/catalog.json", manifest.contract_bindings.schema_catalog_sha256},
        {"contracts/v1/status_reason_codes.tsv", manifest.contract_bindings.status_reason_registry_sha256},
        {"contracts/v1/type_registry.tsv", manifest.contract_bindings.type_registry_sha256},
        {"contracts/v1/transform_registry.tsv", manifest.contract_bindings.transform_registry_sha256},
        {"oracle/authority_manifest.json", manifest.contract_bindings.authority_manifest_sha256},
        {"provenance/source_to_target_manifest.json", manifest.contract_bindings.source_to_target_manifest_sha256},
        {"oracle/production_input_authority.json", manifest.contract_bindings.production_input_authority_sha256},
        {"schema/id_registry.json", manifest.contract_bindings.schema_id_registry_sha256},
        {"state/release_attestation.json", manifest.contract_bindings.release_attestation_sha256},
    };
    for (const auto& [relative, expected] : bindings) {
        if (sha256_file(require_regular_under(repo_root, relative.generic_string(), "INPUT_CONTENT_REPLAY"),
                        "INPUT_CONTENT_REPLAY") != expected) {
            reject("INPUT_CONTENT_REPLAY",
                   "manifest contract binding differs from current repository: " + relative.generic_string());
        }
    }
    const std::filesystem::path science_path =
        require_regular_under(repo_root, "contracts/v1/science_parameters.json", "INPUT_CONTENT_REPLAY");
    const FileBytesSnapshot science = read_regular_file_snapshot(science_path, "INPUT_CONTENT_REPLAY");
    if (science.sha256 != manifest.contract_bindings.science_parameters_sha256) {
        reject("INPUT_CONTENT_REPLAY", "manifest science-parameter binding differs from current repository");
    }
    const JsonPtr science_json = parse_json_snapshot(science, science_path, "INPUT_CONTENT_REPLAY");
    if (string_field(science_json.get(), "schema_name") != "longlineage.science_parameters" ||
        string_field(science_json.get(), "schema_version") != "1.0.0") {
        reject("INPUT_CONTENT_REPLAY", "science-parameter contract identity differs");
    }
    const std::string m1_representation = string_field(science_json.get(), "m1_runtime_representation");
    if (m1_representation != "HISTORICAL_OBSERVED_ROUND6_NULL_ROUND4") {
        reject("INPUT_CONTENT_REPLAY", "science-parameter M1 runtime representation differs");
    }
    if (manifest.authority_profile == AuthorityProfile::kHcc1395DatasetGate) {
        validate_hcc1395_authority_contract(repo_root, manifest);
    }
    return m1_representation;
}

void validate_site_read_input_bindings(const ProducerReceipt& producer,
                                       const std::vector<ExpectedInputBinding>& expected) {
    const auto found = producer.artifacts.find("site_reads");
    if (found == producer.artifacts.end()) {
        reject("INPUT_CONTENT_REPLAY", "site_reads artifact is absent while replaying manifest inputs");
    }
    const json_t* inputs = json_object_get(found->second.raw.get(), "inputs");
    if (!json_is_array(inputs) || json_array_size(inputs) != expected.size()) {
        reject("INPUT_CONTENT_REPLAY", "site_reads manifest input binding count differs");
    }
    static const std::set<std::string> kKeys = {"source_kind", "source_id", "digest_kind", "sha256"};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const json_t* row = json_array_get(inputs, index);
        require_exact_keys(row, kKeys, {}, "INPUT_CONTENT_REPLAY", "site_reads input binding");
        const auto& wanted = expected[index];
        if (string_field(row, "source_kind") != wanted.source_kind ||
            string_field(row, "source_id") != wanted.source_id ||
            string_field(row, "digest_kind") != wanted.digest_kind || string_field(row, "sha256") != wanted.sha256) {
            reject("INPUT_CONTENT_REPLAY", "site_reads input binding differs at frozen order " + std::to_string(index));
        }
    }
}

struct ExpectedDatasetIdentity {
    std::uint32_t dataset_order{0};
    std::string dataset_id;
};

struct InputContentReplay {
    bool complete{false};
    std::vector<ExpectedDatasetIdentity> datasets;
    std::string m1_runtime_representation;
};

void validate_run_artifact_dependencies(const ProducerReceipt& producer, const std::string& artifact_id,
                                        const std::string& transform_id,
                                        const std::vector<std::string>& dependency_ids) {
    const auto artifact = producer.artifacts.find(artifact_id);
    if (artifact == producer.artifacts.end() || artifact->second.transform_id != transform_id) {
        reject("STATIC_PROVENANCE_GRAPH", artifact_id + ": transform differs from independent static graph");
    }
    const json_t* inputs = json_object_get(artifact->second.raw.get(), "inputs");
    if (!json_is_array(inputs) || json_array_size(inputs) != dependency_ids.size()) {
        reject("STATIC_PROVENANCE_GRAPH", artifact_id + ": dependency count differs from independent static graph");
    }
    static const std::set<std::string> kKeys = {"source_kind", "source_id", "digest_kind", "sha256"};
    for (std::size_t index = 0; index < dependency_ids.size(); ++index) {
        const json_t* row = json_array_get(inputs, index);
        require_exact_keys(row, kKeys, {}, "STATIC_PROVENANCE_GRAPH", artifact_id + " dependency");
        const std::string& dependency_id = dependency_ids[index];
        const auto dependency = producer.artifacts.find(dependency_id);
        if (dependency == producer.artifacts.end() || string_field(row, "source_kind") != "RUN_ARTIFACT" ||
            string_field(row, "source_id") != dependency_id || string_field(row, "digest_kind") != "SEMANTIC_SHA256" ||
            string_field(row, "sha256") != dependency->second.semantic_sha256) {
            reject("STATIC_PROVENANCE_GRAPH",
                   artifact_id + ": dependency differs at frozen order " + std::to_string(index));
        }
    }
}

void validate_static_provenance_graph(const Catalog& catalog, const ProducerReceipt& producer) {
    const auto site_reads = producer.artifacts.find("site_reads");
    if (site_reads == producer.artifacts.end() || site_reads->second.transform_id != "raw_alignment_to_site_reads") {
        reject("STATIC_PROVENANCE_GRAPH", "site_reads transform differs from independent static graph");
    }
    validate_run_artifact_dependencies(producer, "methyl_calls", "site_reads_to_methyl_calls", {"site_reads"});
    for (const char* id : {"bernoulli_upper", "m1_sites", "m1_assignments"}) {
        validate_run_artifact_dependencies(producer, id, "methyl_calls_to_m1", {"methyl_calls"});
    }
    validate_run_artifact_dependencies(producer, "cooccurrence_pairs", "m1_to_cooccurrence",
                                       {"site_reads", "m1_sites", "m1_assignments"});
    validate_run_artifact_dependencies(producer, "cooccurrence_sites", "m1_to_cooccurrence",
                                       {"cooccurrence_pairs", "m1_sites"});
    validate_run_artifact_dependencies(producer, "topology_units", "cooccurrence_to_topology",
                                       {"site_reads", "cooccurrence_pairs", "cooccurrence_sites"});
    validate_run_artifact_dependencies(producer, "summary", "run_artifacts_to_summary",
                                       {"site_reads", "methyl_calls", "m1_sites", "m1_assignments",
                                        "cooccurrence_pairs", "cooccurrence_sites", "topology_units"});

    std::vector<std::string> scientific = catalog.scientific_ids;
    std::sort(scientific.begin(), scientific.end());
    validate_run_artifact_dependencies(producer, "artifact_catalog", "scientific_artifacts_to_catalog", scientific);
    validate_run_artifact_dependencies(producer, "data_lineage", "scientific_artifacts_to_lineage", scientific);
    std::vector<std::string> digest_sources = scientific;
    digest_sources.push_back("artifact_catalog");
    digest_sources.push_back("data_lineage");
    std::sort(digest_sources.begin(), digest_sources.end());
    validate_run_artifact_dependencies(producer, "semantic_digests", "artifacts_to_semantic_digests", digest_sources);
}

InputContentReplay validate_input_content(const ArtifactValidationOptions& options, const Catalog& catalog,
                                          const ProducerReceipt& producer) {
    if (!is_canonical_science_catalog(catalog)) {
        return {};
    }
    if (options.manifest_path.empty() || !options.manifest_path.is_absolute() ||
        options.manifest_path.lexically_normal() != options.manifest_path) {
        reject("INPUT_CONTENT_REPLAY", "canonical DATASET_GATE validation requires an absolute manifest path");
    }
    struct stat manifest_status {};
    if (::lstat(options.manifest_path.c_str(), &manifest_status) != 0 || !S_ISREG(manifest_status.st_mode) ||
        S_ISLNK(manifest_status.st_mode)) {
        reject("INPUT_CONTENT_REPLAY", "production manifest must be a real regular file");
    }
    std::error_code error;
    const std::filesystem::path canonical_manifest = std::filesystem::canonical(options.manifest_path, error);
    if (error || canonical_manifest != options.manifest_path) {
        reject("INPUT_CONTENT_REPLAY", "production manifest path must be canonical and must not traverse aliases");
    }
    const FileBytesSnapshot manifest_snapshot = read_regular_file_snapshot(canonical_manifest, "INPUT_CONTENT_REPLAY");
    const std::string& manifest_sha256 = manifest_snapshot.sha256;
    if (manifest_sha256 != producer.manifest_sha256) {
        reject("INPUT_CONTENT_REPLAY", "production manifest SHA-256 differs from producer receipt");
    }
    const auto loaded = parse_production_manifest_json(manifest_snapshot.bytes);
    if (!loaded.ok() || !loaded.value.has_value()) {
        reject("INPUT_CONTENT_REPLAY", "production manifest parse failed: " + loaded.detail);
    }
    const ProductionManifest& manifest = *loaded.value;
    if (manifest.run_id != producer.run_id || manifest.output_root != options.run_root ||
        (manifest.authority_profile != AuthorityProfile::kHcc1395DatasetGate &&
         manifest.authority_profile != AuthorityProfile::kSynthetic)) {
        reject("INPUT_CONTENT_REPLAY", "manifest run/output/profile differs from DATASET_GATE validation scope");
    }
    const std::string m1_runtime_representation = verify_manifest_contract_bindings(options.repo_root, manifest);

    std::ostringstream snapshot;
    std::ostringstream locks;
    snapshot << "longlineage.input_snapshot\t1.1.0\n";
    locks << "longlineage.input_lock\t1.0.0\n";
    std::vector<ExpectedInputBinding> expected_bindings;
    std::size_t mount_index = 0;
    for (const DatasetManifest& dataset : manifest.datasets) {
        for (const LockedFile& file : dataset.files) {
            if (mount_index >= producer.mounts.size()) {
                reject("INPUT_CONTENT_REPLAY", "producer mount identity has fewer rows than the manifest");
            }
            const auto locked = verify_locked_file(file);
            if (!locked.ok() || !locked.value.has_value()) {
                reject("INPUT_CONTENT_REPLAY", dataset.dataset_id + ": " + locked.detail);
            }
            const auto& mount = producer.mounts[mount_index++];
            const std::string role(to_string(file.role));
            if (mount.dataset_id != dataset.dataset_id || mount.role != role ||
                mount.canonical_path != locked.value->canonical_path) {
                reject("INPUT_CONTENT_REPLAY", "producer mount identity differs from manifest/observed input");
            }
            snapshot << dataset.dataset_order << '\t' << dataset.dataset_id << '\t' << role << '\t'
                     << locked.value->canonical_path.string() << '\t' << locked.value->device << '\t'
                     << locked.value->inode << '\t' << locked.value->observed_size_bytes << '\t'
                     << locked.value->mtime_seconds << '\t' << locked.value->mtime_nanoseconds << '\t'
                     << locked.value->ctime_seconds << '\t' << locked.value->ctime_nanoseconds << '\t' << file.sha256
                     << '\n';
            locks << dataset.dataset_order << '\t' << dataset.dataset_id << '\t' << role << '\t'
                  << locked.value->canonical_path.string() << '\t' << file.size_bytes << '\t' << file.sha256 << '\n';
            expected_bindings.push_back(
                {"MANIFEST_INPUT", dataset.dataset_id + ":" + role, "PHYSICAL_SHA256", file.sha256});
        }
    }
    if (mount_index != producer.mounts.size()) {
        reject("INPUT_CONTENT_REPLAY", "producer mount identity has extra rows beyond the manifest");
    }
    const std::string snapshot_sha256 = sha256_bytes(snapshot.str());
    const std::string input_lock_sha256 = sha256_bytes(locks.str());
    if (snapshot_sha256 != producer.input_before || snapshot_sha256 != producer.input_after ||
        input_lock_sha256 != string_field(producer.run_receipt_draft.get(), "input_lock_sha256")) {
        reject("INPUT_CONTENT_REPLAY", "recomputed input snapshot or lock differs from producer receipt");
    }
    expected_bindings.push_back({"MANIFEST_INPUT", "run_manifest", "PHYSICAL_SHA256", manifest_sha256});
    if (manifest.authority_profile == AuthorityProfile::kHcc1395DatasetGate) {
        expected_bindings.push_back({"CONTRACT", "hcc1395_dataset_gate_input_authority", "PHYSICAL_SHA256",
                                     manifest.contract_bindings.dataset_gate_input_authority_sha256});
    }
    validate_site_read_input_bindings(producer, expected_bindings);
    InputContentReplay replay;
    replay.complete = true;
    replay.m1_runtime_representation = m1_runtime_representation;
    for (const DatasetManifest& dataset : manifest.datasets) {
        replay.datasets.push_back({dataset.dataset_order, dataset.dataset_id});
    }
    return replay;
}

struct KeyAtom {
    bool numeric = false;
    std::uint64_t number = 0;
    std::string text;

    friend bool operator==(const KeyAtom& left, const KeyAtom& right) {
        return std::tie(left.numeric, left.number, left.text) == std::tie(right.numeric, right.number, right.text);
    }

    friend bool operator<(const KeyAtom& left, const KeyAtom& right) {
        return std::tie(left.numeric, left.number, left.text) < std::tie(right.numeric, right.number, right.text);
    }
};

using RecordKey = std::vector<KeyAtom>;

std::vector<std::string> display_key(const RecordKey& key) {
    std::vector<std::string> result;
    result.reserve(key.size());
    for (const KeyAtom& atom : key) {
        result.push_back(atom.numeric ? std::to_string(atom.number) : atom.text);
    }
    return result;
}

KeyAtom key_atom_from_text(const std::string& value, const std::string& type) {
    const bool numeric = type.rfind("uint", 0) == 0U || type == "Position1";
    if (!numeric) {
        return {false, 0, value};
    }
    if (value.empty() || (value.size() > 1U && value.front() == '0') || value.front() == '+') {
        reject("ARTIFACT_ORDER", "non-canonical unsigned primary-key scalar: " + value);
    }
    std::uint64_t parsed = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
        reject("ARTIFACT_ORDER", "invalid unsigned primary-key scalar: " + value);
    }
    return {true, parsed, {}};
}

KeyAtom key_atom_from_json(const json_t* value) {
    if (json_is_integer(value) && json_integer_value(value) >= 0) {
        return {true, static_cast<std::uint64_t>(json_integer_value(value)), {}};
    }
    if (json_is_string(value)) {
        return {false, 0, json_string_value(value)};
    }
    reject("ARTIFACT_ORDER", "JSON primary key must be a nonnegative integer or string");
}

const json_t* json_path(const json_t* value, const std::string& dotted) {
    const json_t* current = value;
    std::size_t begin = 0;
    while (begin < dotted.size()) {
        const std::size_t dot = dotted.find('.', begin);
        const std::string part = dotted.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin);
        current = json_is_object(current) ? json_object_get(current, part.c_str()) : nullptr;
        if (current == nullptr || dot == std::string::npos) {
            return current;
        }
        begin = dot + 1U;
    }
    return current;
}

struct IndexGroup {
    RecordKey key;
    std::uint64_t first_virtual_offset = 0;
    std::uint64_t past_end_virtual_offset = 0;
    std::uint64_t logical_rows = 0;
    std::string range_semantic_sha256;
    std::unique_ptr<Sha256> range_digest;
};

struct ParsedArtifact {
    struct LlmFrameSummary {
        std::uint64_t dataset_order = 0;
        std::uint64_t site_order = 0;
        std::uint64_t dimension = 0;
    };

    std::uint64_t logical_rows = 0;
    std::string semantic_sha256;
    std::vector<std::string> primary_first;
    std::vector<std::string> primary_last;
    std::vector<IndexGroup> index_groups;
    std::vector<JsonPtr> json_records;
    std::vector<std::vector<std::string>> table_rows;
    std::map<std::string, std::size_t> columns;
    std::vector<LlmFrameSummary> llm_frames;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::set<std::string>> methyl_read_presence;
};

struct SchemaIdentity {
    struct FieldRule {
        std::string name;
        std::string type;
        bool required = true;
        std::optional<std::string> null_token;
        std::optional<std::string> const_value;
    };

    std::string name;
    std::string version;
    std::string format;
    std::vector<std::string> header;
    std::map<std::string, std::string> field_types;
    std::map<std::string, std::string> field_domains;
    std::vector<FieldRule> field_rules;
};

SchemaIdentity schema_identity(const json_t* schema, const std::string& check_id) {
    SchemaIdentity identity;
    identity.name = string_field(schema, "schema_name");
    identity.version = string_field(schema, "schema_version");
    identity.format = string_field(schema, "format");
    if (identity.name.empty()) {
        const json_t* properties = json_object_get(schema, "properties");
        const json_t* name = json_is_object(properties) ? json_object_get(properties, "schema_name") : nullptr;
        const json_t* version = json_is_object(properties) ? json_object_get(properties, "schema_version") : nullptr;
        identity.name = string_field(name, "const");
        identity.version = string_field(version, "const");
    }
    const json_t* header = json_object_get(schema, "header");
    if (json_is_array(header)) {
        identity.header = string_array(header, check_id, "schema header");
    }
    const json_t* fields = json_object_get(schema, "fields");
    if (json_is_array(fields)) {
        for (std::size_t index = 0; index < json_array_size(fields); ++index) {
            const json_t* field = json_array_get(fields, index);
            const std::string name = string_field(field, "name");
            const std::string type = string_field(field, "type");
            if (name.empty() || type.empty() || !identity.field_types.emplace(name, type).second) {
                reject(check_id, "record schema contains malformed or duplicate field");
            }
            const json_t* required = json_object_get(field, "required");
            if (!json_is_boolean(required)) {
                reject(check_id, "record schema field required flag is absent or non-boolean");
            }
            SchemaIdentity::FieldRule rule;
            rule.name = name;
            rule.type = type;
            rule.required = json_is_true(required);
            const json_t* null_token = json_object_get(field, "null");
            if (null_token != nullptr) {
                if (!json_is_string(null_token)) {
                    reject(check_id, "record schema null token must be a string");
                }
                rule.null_token = std::string(json_string_value(null_token));
            }
            if (rule.required && rule.null_token.has_value()) {
                reject(check_id, "required record field must not declare a null token");
            }
            const json_t* constant = json_object_get(field, "const");
            if (constant != nullptr) {
                if (json_is_string(constant)) {
                    rule.const_value = std::string(json_string_value(constant));
                } else if (json_is_integer(constant)) {
                    rule.const_value = std::to_string(json_integer_value(constant));
                } else if (json_is_real(constant)) {
                    rule.const_value = format_sci17(json_real_value(constant));
                } else if (json_is_true(constant)) {
                    rule.const_value = "true";
                } else if (json_is_false(constant)) {
                    rule.const_value = "false";
                } else {
                    reject(check_id, "record schema field const has an unsupported scalar type");
                }
            }
            identity.field_rules.push_back(std::move(rule));
        }
    }
    if (!identity.header.empty()) {
        if (identity.field_rules.size() != identity.header.size()) {
            reject(check_id, "record schema header and fields differ in length");
        }
        for (std::size_t index = 0; index < identity.header.size(); ++index) {
            if (identity.header[index] != identity.field_rules[index].name) {
                reject(check_id, "record schema header and field order differ");
            }
        }
    }
    const json_t* status_bindings = json_object_get(schema, "status_domain_bindings");
    if (json_is_object(status_bindings)) {
        const char* field_name = nullptr;
        json_t* domain = nullptr;
        json_object_foreach(const_cast<json_t*>(status_bindings), field_name, domain) {
            if (!json_is_string(domain) || identity.field_types.count(field_name) == 0U) {
                reject(check_id, "record schema status-domain binding is malformed");
            }
            identity.field_domains.emplace(field_name, json_string_value(domain));
        }
    }
    if (identity.name.empty() || identity.version.empty()) {
        reject(check_id, "cannot resolve record schema identity");
    }
    return identity;
}

class BgzfHandle final {
   public:
    explicit BgzfHandle(const std::filesystem::path& path) : value_(bgzf_open(path.c_str(), "r")) {
        if (value_ == nullptr) {
            reject("TRUNCATED_ARTIFACT", "cannot open BGZF artifact: " + path.string());
        }
    }
    ~BgzfHandle() {
        if (value_ != nullptr) {
            bgzf_close(value_);
        }
    }
    BgzfHandle(const BgzfHandle&) = delete;
    BgzfHandle& operator=(const BgzfHandle&) = delete;
    [[nodiscard]] BGZF* get() const noexcept { return value_; }

   private:
    BGZF* value_;
};

bool next_bgzf_line(BGZF* input, std::string& line, std::uint64_t& begin, std::uint64_t& end) {
    const int64_t before = bgzf_tell(input);
    kstring_t buffer{0, 0, nullptr};
    const int length = bgzf_getline(input, '\n', &buffer);
    if (length < 0) {
        std::free(buffer.s);
        if (length == -1) {
            return false;
        }
        reject("TRUNCATED_ARTIFACT", "BGZF line decompression failed");
    }
    const int64_t after = bgzf_tell(input);
    if (before < 0 || after < 0) {
        std::free(buffer.s);
        reject("INDEX_REPLAY", "cannot observe BGZF virtual offsets");
    }
    line.assign(buffer.s == nullptr ? "" : buffer.s, static_cast<std::size_t>(length));
    std::free(buffer.s);
    if (line.find('\r') != std::string::npos || line.find('\0') != std::string::npos) {
        reject("TRUNCATED_ARTIFACT", "BGZF logical line contains forbidden control bytes");
    }
    begin = static_cast<std::uint64_t>(before);
    end = static_cast<std::uint64_t>(after);
    return true;
}

std::string hex_bytes(const unsigned char* bytes, std::size_t size) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(size * 2U);
    for (std::size_t index = 0; index < size; ++index) {
        encoded.push_back(kHex[(bytes[index] >> 4U) & 0x0fU]);
        encoded.push_back(kHex[bytes[index] & 0x0fU]);
    }
    return encoded;
}

bool read_bgzf_exact(BGZF* input, unsigned char* destination, std::size_t size, bool allow_clean_eof) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = bgzf_read(input, destination + offset, size - offset);
        if (count < 0) {
            reject("TRUNCATED_ARTIFACT", "BGZF binary decompression failed");
        }
        if (count == 0) {
            if (allow_clean_eof && offset == 0U) {
                return false;
            }
            reject("TRUNCATED_ARTIFACT", "BGZF binary frame ended before its declared size");
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

std::uint16_t little_u16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t little_u32(const unsigned char* bytes) {
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (8U * index);
    }
    return value;
}

std::uint64_t little_u64(const unsigned char* bytes) {
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
    }
    return value;
}

std::uint64_t parse_canonical_uint(const std::string& value, std::uint64_t maximum, const std::string& field_name) {
    if (value.empty() || value.front() == '+' || (value.size() > 1U && value.front() == '0')) {
        reject("TSV_SCHEMA", field_name + ": unsigned integer is not canonical");
    }
    std::uint64_t parsed = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || parsed > maximum) {
        reject("TSV_SCHEMA", field_name + ": unsigned integer is out of range");
    }
    return parsed;
}

void parse_canonical_int64(const std::string& value, const std::string& field_name) {
    if (value.empty() || value.front() == '+' || value == "-0" || (value.size() > 1U && value.front() == '0') ||
        (value.size() > 2U && value[0] == '-' && value[1] == '0')) {
        reject("TSV_SCHEMA", field_name + ": signed integer is not canonical");
    }
    std::int64_t parsed = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
        reject("TSV_SCHEMA", field_name + ": signed integer is out of range");
    }
}

bool all_lower_hex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](const char raw_character) {
        const auto character = static_cast<unsigned char>(raw_character);
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

void validate_canonical_json_field(const std::string& value, const std::string& field_name) {
    json_error_t error{};
    JsonPtr parsed(json_loadb(value.data(), value.size(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
    if (!parsed) {
        reject("TSV_SCHEMA", field_name + ": embedded canonical JSON is malformed");
    }
    char* encoded = json_dumps(parsed.get(), JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    if (encoded == nullptr) {
        reject("TSV_SCHEMA", field_name + ": embedded canonical JSON cannot be encoded");
    }
    const std::string canonical(encoded);
    std::free(encoded);
    if (canonical != value) {
        reject("TSV_SCHEMA", field_name + ": embedded JSON has non-canonical bytes");
    }
}

void validate_tsv_scalar(const std::string& value, const SchemaIdentity::FieldRule& rule) {
    if (rule.null_token.has_value() && value == *rule.null_token) {
        if (rule.required) {
            reject("TSV_SCHEMA", rule.name + ": required field uses its null token");
        }
        return;
    }
    if (!rule.required && value == ".") {
        reject("TSV_SCHEMA", rule.name + ": undeclared dot null token");
    }
    if (rule.const_value.has_value() && value != *rule.const_value) {
        reject("TSV_SCHEMA", rule.name + ": value differs from frozen const");
    }
    const std::string& type = rule.type;
    if (type == "uint8") {
        static_cast<void>(parse_canonical_uint(value, 255U, rule.name));
    } else if (type == "uint16") {
        static_cast<void>(parse_canonical_uint(value, 65535U, rule.name));
    } else if (type == "uint32") {
        static_cast<void>(parse_canonical_uint(value, 4294967295ULL, rule.name));
    } else if (type == "uint64") {
        static_cast<void>(parse_canonical_uint(value, std::numeric_limits<std::uint64_t>::max(), rule.name));
    } else if (type == "Position1") {
        if (parse_canonical_uint(value, std::numeric_limits<std::uint64_t>::max(), rule.name) == 0U) {
            reject("TSV_SCHEMA", rule.name + ": Position1 must be at least one");
        }
    } else if (type == "int64") {
        parse_canonical_int64(value, rule.name);
    } else if (type == "float64") {
        std::istringstream input(value);
        input.imbue(std::locale::classic());
        double parsed = 0.0;
        input >> parsed;
        if (!input || input.peek() != std::char_traits<char>::eof() || format_sci17(parsed) != value) {
            reject("TSV_SCHEMA", rule.name + ": float64 is not canonical LONGLINEAGE_SCI17");
        }
    } else if (type == "bool") {
        if (value != "true" && value != "false") {
            reject("TSV_SCHEMA", rule.name + ": boolean must be true or false");
        }
    } else if (type == "base") {
        if (value != "A" && value != "C" && value != "G" && value != "T") {
            reject("TSV_SCHEMA", rule.name + ": base is outside A/C/G/T");
        }
    } else if (type == "sha256" || type == "opaque_sha256") {
        if (!is_lower_sha256(value)) {
            reject("TSV_SCHEMA", rule.name + ": SHA-256 token is malformed");
        }
    } else if (type == "hex16") {
        if (value.size() != 16U || !all_lower_hex(value)) {
            reject("TSV_SCHEMA", rule.name + ": hex16 token is malformed");
        }
    } else if (type == "canonical_json") {
        validate_canonical_json_field(value, rule.name);
    } else if (type == "axis_status") {
        static const std::set<std::string> kAxis = {
            "NOT_RUN_UNSTABLE",
            "PASS",
            "FAIL",
            "INDETERMINATE",
        };
        if (kAxis.count(value) == 0U) {
            reject("TSV_SCHEMA", rule.name + ": axis status is outside the closed vocabulary");
        }
    } else if (type == "hp_state") {
        static const std::set<std::string> kHp = {
            "0", "1", "2", "3", "4", "1-1", "2-1", "1-2", "2-2",
        };
        if (kHp.count(value) == 0U) {
            reject("TSV_SCHEMA", rule.name + ": HP state is outside the closed vocabulary");
        }
    } else if (type.rfind("enum:", 0U) == 0U) {
        const std::string choices = type.substr(5U);
        bool matched = false;
        std::size_t begin = 0;
        while (begin <= choices.size()) {
            const std::size_t separator = choices.find('|', begin);
            const std::string choice =
                choices.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin);
            matched = matched || value == choice;
            if (separator == std::string::npos) {
                break;
            }
            begin = separator + 1U;
        }
        if (!matched) {
            reject("TSV_SCHEMA", rule.name + ": value is outside the schema enum");
        }
    } else if (type == "status_code" || type == "reason_code") {
        if (value.empty() || !std::all_of(value.begin(), value.end(), [](const char raw_character) {
                const auto character = static_cast<unsigned char>(raw_character);
                return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
                       character == '_';
            })) {
            reject("TSV_SCHEMA", rule.name + ": status/reason token is malformed");
        }
    } else if (type != "string") {
        reject("TSV_SCHEMA", rule.name + ": validator does not recognize TSV type " + type);
    }
}

void validate_tsv_row(const std::vector<std::string>& fields, const SchemaIdentity& schema,
                      bool methyl_interval_fast_path = false) {
    if (fields.size() != schema.field_rules.size()) {
        reject("TSV_SCHEMA", "TSV row field count differs from schema fields");
    }
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (methyl_interval_fast_path && (schema.field_rules[index].name == "probability_lower" ||
                                          schema.field_rules[index].name == "probability_upper")) {
            continue;
        }
        validate_tsv_scalar(fields[index], schema.field_rules[index]);
    }
}

const std::string& required_column_value(const std::vector<std::string>& fields,
                                         const std::map<std::string, std::size_t>& columns, const std::string& name,
                                         const std::string& check_id) {
    const auto column = columns.find(name);
    if (column == columns.end() || column->second >= fields.size()) {
        reject(check_id, "required table field is absent: " + name);
    }
    return fields[column->second];
}

const std::pair<std::string, std::string>& independent_ml_probability_interval(std::uint64_t ml_raw) {
    static const auto kIntervals = [] {
        std::array<std::pair<std::string, std::string>, 256> intervals;
        for (std::size_t raw = 0; raw < intervals.size(); ++raw) {
            const double lower = static_cast<double>(raw) / 256.0;
            const double upper = raw == 255U ? 1.0 : static_cast<double>(raw + 1U) / 256.0;
            intervals[raw] = {
                format_sci17(lower),
                format_sci17(upper),
            };
        }
        return intervals;
    }();
    if (ml_raw >= kIntervals.size()) {
        reject("METHYL_CONSERVATION", "ml_raw is outside uint8");
    }
    return kIntervals[static_cast<std::size_t>(ml_raw)];
}

using MethylPresenceKey = std::tuple<std::uint64_t, std::uint64_t, std::string>;

void validate_and_record_methyl_row(const std::vector<std::string>& fields,
                                    const std::map<std::string, std::size_t>& columns, ParsedArtifact& parsed,
                                    std::optional<MethylPresenceKey>& previous_presence) {
    constexpr std::string_view kCheck = "METHYL_CONSERVATION";
    const auto& dataset_text = required_column_value(fields, columns, "dataset_order", std::string(kCheck));
    const auto& site_text = required_column_value(fields, columns, "site_order", std::string(kCheck));
    const auto& read_id = required_column_value(fields, columns, "read_id", std::string(kCheck));
    const auto& ml_text = required_column_value(fields, columns, "ml_raw", std::string(kCheck));
    const auto& lower_text = required_column_value(fields, columns, "probability_lower", std::string(kCheck));
    const auto& upper_text = required_column_value(fields, columns, "probability_upper", std::string(kCheck));
    const std::uint64_t dataset_order =
        parse_canonical_uint(dataset_text, std::numeric_limits<std::uint32_t>::max(), "dataset_order");
    const std::uint64_t site_order =
        parse_canonical_uint(site_text, std::numeric_limits<std::uint64_t>::max(), "site_order");
    const std::uint64_t ml_raw = parse_canonical_uint(ml_text, 255U, "ml_raw");
    const auto& expected = independent_ml_probability_interval(ml_raw);
    if (lower_text != expected.first || upper_text != expected.second) {
        reject(std::string(kCheck), "ML raw value and frozen half-open probability interval differ");
    }

    MethylPresenceKey presence{dataset_order, site_order, read_id};
    if (!previous_presence.has_value() || *previous_presence != presence) {
        const auto inserted = parsed.methyl_read_presence[{dataset_order, site_order}].insert(read_id);
        if (!inserted.second) {
            reject(std::string(kCheck), "site/read methylation presence is non-contiguous");
        }
        previous_presence = std::move(presence);
    }
}

RecordKey table_key(const std::vector<std::string>& row, const std::vector<std::string>& key_fields,
                    const std::map<std::string, std::size_t>& columns, const SchemaIdentity& schema) {
    RecordKey key;
    for (const std::string& field : key_fields) {
        const auto column = columns.find(field);
        const auto type = schema.field_types.find(field);
        if (column == columns.end() || type == schema.field_types.end()) {
            reject("ARTIFACT_ORDER", "primary/index key field is absent from table schema: " + field);
        }
        key.push_back(key_atom_from_text(row.at(column->second), type->second));
    }
    return key;
}

void append_index_group(std::vector<IndexGroup>& groups, const RecordKey& key, std::uint64_t begin, std::uint64_t end,
                        const std::string& canonical_row) {
    if (groups.empty() || groups.back().key != key) {
        if (!groups.empty() && groups.back().range_digest) {
            groups.back().range_semantic_sha256 = groups.back().range_digest->finish();
            groups.back().range_digest.reset();
        }
        IndexGroup group;
        group.key = key;
        group.first_virtual_offset = begin;
        group.past_end_virtual_offset = end;
        group.logical_rows = 1;
        group.range_digest = std::make_unique<Sha256>();
        group.range_digest->update(canonical_row);
        groups.push_back(std::move(group));
        return;
    }
    IndexGroup& group = groups.back();
    group.past_end_virtual_offset = end;
    ++group.logical_rows;
    if (!group.range_digest) {
        reject("INDEX_REPLAY", "artifact index group digest was finalized before its range ended");
    }
    group.range_digest->update(canonical_row);
}

void finalize_range_digests(std::vector<IndexGroup>& groups) {
    for (IndexGroup& group : groups) {
        if (group.range_digest) {
            group.range_semantic_sha256 = group.range_digest->finish();
            group.range_digest.reset();
        }
        if (!is_lower_sha256(group.range_semantic_sha256)) {
            reject("INDEX_REPLAY", "artifact index range digest is absent or malformed");
        }
    }
}

ParsedArtifact parse_tsv_bgzf(const std::filesystem::path& path, const ArtifactSpec& spec, const SchemaIdentity& schema,
                              const std::string& run_id) {
    if (!canonical_bgzf_eof(path)) {
        reject("TRUNCATED_ARTIFACT", "BGZF artifact lacks canonical EOF: " + path.string());
    }
    BgzfHandle input(path);
    std::string line;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    const std::vector<std::string> preamble = {
        "##longlineage_schema=" + schema.name,
        "##schema_version=" + schema.version,
        "##run_id=" + run_id,
        "#" + join_tab(schema.header),
    };
    for (const std::string& expected : preamble) {
        if (!next_bgzf_line(input.get(), line, begin, end) || line != expected) {
            reject("TRUNCATED_ARTIFACT", "BGZF TSV preamble/header mismatch: " + path.string());
        }
    }
    Sha256 semantic;
    semantic.update(schema.name + "\t" + schema.version + "\n");
    semantic.update(join_tab(schema.header) + "\n");

    std::map<std::string, std::size_t> columns;
    for (std::size_t index = 0; index < schema.header.size(); ++index) {
        columns.emplace(schema.header[index], index);
    }
    ParsedArtifact parsed;
    parsed.columns = columns;
    std::optional<RecordKey> previous;
    std::optional<MethylPresenceKey> previous_methyl_presence;
    while (next_bgzf_line(input.get(), line, begin, end)) {
        const std::vector<std::string> fields = split_tab(line);
        if (fields.size() != schema.header.size()) {
            reject("TRUNCATED_ARTIFACT", "TSV row field count differs from schema header");
        }
        validate_tsv_row(fields, schema, spec.artifact_id == "methyl_calls");
        const RecordKey key = table_key(fields, spec.sort_key, columns, schema);
        if (previous.has_value() && !(*previous < key)) {
            reject(*previous == key ? "DUPLICATE_KEY" : "ARTIFACT_ORDER",
                   *previous == key ? "duplicate primary/sort key" : "out-of-order primary/sort key");
        }
        previous = key;
        const std::string canonical_row = line + "\n";
        semantic.update(canonical_row);
        if (!spec.index_key.empty()) {
            append_index_group(parsed.index_groups, table_key(fields, spec.index_key, columns, schema), begin, end,
                               canonical_row);
        }
        if (spec.artifact_id == "methyl_calls") {
            validate_and_record_methyl_row(fields, columns, parsed, previous_methyl_presence);
        } else {
            parsed.table_rows.push_back(fields);
        }
        if (parsed.logical_rows == 0U) {
            parsed.primary_first = display_key(table_key(fields, spec.primary_key, columns, schema));
        }
        parsed.primary_last = display_key(table_key(fields, spec.primary_key, columns, schema));
        ++parsed.logical_rows;
    }
    finalize_range_digests(parsed.index_groups);
    parsed.semantic_sha256 = semantic.finish();
    return parsed;
}

ParsedArtifact parse_plain_tsv(const std::filesystem::path& path, const ArtifactSpec& spec,
                               const SchemaIdentity& schema) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reject("TRUNCATED_ARTIFACT", "cannot open TSV artifact: " + path.string());
    }
    std::string line;
    if (!std::getline(input, line) || line != join_tab(schema.header)) {
        reject("TRUNCATED_ARTIFACT", "plain TSV header mismatch");
    }
    Sha256 semantic;
    semantic.update(schema.name + "\t" + schema.version + "\n");
    semantic.update(line + "\n");
    std::map<std::string, std::size_t> columns;
    for (std::size_t index = 0; index < schema.header.size(); ++index) {
        columns.emplace(schema.header[index], index);
    }
    ParsedArtifact parsed;
    parsed.columns = columns;
    std::optional<RecordKey> previous;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            reject("TRUNCATED_ARTIFACT", "plain TSV uses CRLF");
        }
        const std::vector<std::string> fields = split_tab(line);
        if (fields.size() != schema.header.size()) {
            reject("TRUNCATED_ARTIFACT", "plain TSV row field count mismatch");
        }
        validate_tsv_row(fields, schema);
        const RecordKey key = table_key(fields, spec.sort_key, columns, schema);
        if (previous.has_value() && !(*previous < key)) {
            reject(*previous == key ? "DUPLICATE_KEY" : "ARTIFACT_ORDER",
                   *previous == key ? "duplicate TSV key" : "out-of-order TSV key");
        }
        previous = key;
        semantic.update(line + "\n");
        parsed.table_rows.push_back(fields);
        if (parsed.logical_rows == 0U) {
            parsed.primary_first = display_key(table_key(fields, spec.primary_key, columns, schema));
        }
        parsed.primary_last = display_key(table_key(fields, spec.primary_key, columns, schema));
        ++parsed.logical_rows;
    }
    if (!input.eof()) {
        reject("TRUNCATED_ARTIFACT", "plain TSV read failed");
    }
    parsed.semantic_sha256 = semantic.finish();
    return parsed;
}

RecordKey json_key(const json_t* record, const std::vector<std::string>& fields) {
    RecordKey key;
    for (const std::string& field : fields) {
        const json_t* value = json_path(record, field);
        if (value == nullptr) {
            reject("ARTIFACT_ORDER", "JSON primary/index key field is absent: " + field);
        }
        key.push_back(key_atom_from_json(value));
    }
    return key;
}

void validate_topology_winner(const json_t* record) {
    const std::string family_state = string_field(record, "family_state");
    const std::string ranking_state = string_field(record, "ranking_state");
    const json_t* winner = json_object_get(record, "winner");
    const bool incomplete =
        family_state.find("INCOMPLETE") != std::string::npos || family_state.find("ABSTAIN") != std::string::npos;
    const bool ranking_unresolved = !ranking_state.empty() && ranking_state != "RANKING_RESOLVED_UNIQUE";
    if ((incomplete || ranking_unresolved) && winner != nullptr && !json_is_null(winner)) {
        reject("TOPOLOGY_INCOMPLETE_WINNER", "incomplete, abstained or unresolved topology record publishes a winner");
    }
    const json_t* candidates = json_object_get(record, "candidates");
    const json_t* count = json_object_get(record, "candidate_count");
    if (json_is_array(candidates) && json_is_integer(count) &&
        json_integer_value(count) != static_cast<json_int_t>(json_array_size(candidates))) {
        reject("TOPOLOGY_INCOMPLETE_WINNER", "topology candidate_count differs from candidates length");
    }
}

ParsedArtifact parse_jsonl_bgzf(const std::filesystem::path& path, const ArtifactSpec& spec,
                                const SchemaIdentity& schema_identity_value, const SharedJson& schema,
                                SchemaStore& store) {
    if (!canonical_bgzf_eof(path)) {
        reject("TRUNCATED_ARTIFACT", "JSONL BGZF artifact lacks canonical EOF");
    }
    BgzfHandle input(path);
    Sha256 semantic;
    semantic.update(schema_identity_value.name + "\t" + schema_identity_value.version + "\n");
    ParsedArtifact parsed;
    std::optional<RecordKey> previous;
    std::string line;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    while (next_bgzf_line(input.get(), line, begin, end)) {
        json_error_t error{};
        JsonPtr record(json_loadb(line.data(), line.size(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
        if (!record || !json_is_object(record.get())) {
            reject("TRUNCATED_ARTIFACT", spec.artifact_id + ": malformed JSONL data row (1-based) " +
                                             std::to_string(parsed.logical_rows + 1U));
        }
        std::string canonical;
        try {
            canonical = canonical_json(record.get(), schema.get(), schema, store) + "\n";
        } catch (const ValidationError& validation_error) {
            reject(validation_error.check_id(), spec.artifact_id + ": JSONL data row (1-based) " +
                                                    std::to_string(parsed.logical_rows + 1U) + ": " +
                                                    validation_error.what());
        }
        semantic.update(canonical);
        const RecordKey key = json_key(record.get(), spec.sort_key);
        if (previous.has_value() && !(*previous < key)) {
            reject(*previous == key ? "DUPLICATE_KEY" : "ARTIFACT_ORDER",
                   *previous == key ? "duplicate JSONL key" : "out-of-order JSONL key");
        }
        previous = key;
        if (!spec.index_key.empty()) {
            append_index_group(parsed.index_groups, json_key(record.get(), spec.index_key), begin, end, canonical);
        }
        if (spec.artifact_id == "topology_units") {
            validate_topology_winner(record.get());
        }
        if (parsed.logical_rows == 0U) {
            parsed.primary_first = display_key(json_key(record.get(), spec.primary_key));
        }
        parsed.primary_last = display_key(json_key(record.get(), spec.primary_key));
        parsed.json_records.push_back(std::move(record));
        ++parsed.logical_rows;
    }
    finalize_range_digests(parsed.index_groups);
    parsed.semantic_sha256 = semantic.finish();
    return parsed;
}

ParsedArtifact parse_json_file(const std::filesystem::path& path, const ArtifactSpec& spec,
                               const SchemaIdentity& identity, const SharedJson& schema, SchemaStore& store) {
    JsonPtr record = load_json_strict(path, "TRUNCATED_ARTIFACT");
    std::string canonical;
    try {
        canonical = canonical_json(record.get(), schema.get(), schema, store) + "\n";
    } catch (const ValidationError& validation_error) {
        reject(validation_error.check_id(),
               spec.artifact_id + ": JSON data row (1-based) 1: " + validation_error.what());
    }
    Sha256 semantic;
    semantic.update(identity.name + "\t" + identity.version + "\n");
    semantic.update(canonical);
    ParsedArtifact parsed;
    parsed.logical_rows = 1;
    parsed.semantic_sha256 = semantic.finish();
    parsed.primary_first = display_key(json_key(record.get(), spec.primary_key));
    parsed.primary_last = parsed.primary_first;
    parsed.json_records.push_back(std::move(record));
    return parsed;
}

ParsedArtifact parse_llm_bgzf(const std::filesystem::path& path, const ArtifactSpec& spec,
                              const SchemaIdentity& identity) {
    if (identity.name != "longlineage.bernoulli_upper" || identity.version != "1.0.0" ||
        identity.format != "LLM_BGZF") {
        reject("LLM_SCHEMA", "LLM artifact schema identity/format differs from the frozen v1 contract");
    }
    if (spec.primary_key != std::vector<std::string>{"dataset_order", "site_order"} ||
        spec.sort_key != spec.primary_key || spec.index_key != spec.primary_key) {
        reject("LLM_SCHEMA", "LLM catalog keys differ from dataset_order/site_order");
    }
    if (!canonical_bgzf_eof(path)) {
        reject("TRUNCATED_ARTIFACT", "LLM BGZF artifact lacks canonical EOF");
    }
    static_assert(sizeof(double) == sizeof(std::uint64_t), "LLM v1 requires binary64");
    static_assert(std::numeric_limits<double>::is_iec559, "LLM v1 requires IEEE-754 binary64");

    BgzfHandle input(path);
    Sha256 semantic;
    semantic.update(identity.name + "\t" + identity.version + "\n");
    ParsedArtifact parsed;
    std::optional<RecordKey> previous;
    constexpr std::uint64_t kMaximumReplayValues = 100000000ULL;

    while (true) {
        const int64_t raw_begin = bgzf_tell(input.get());
        if (raw_begin < 0) {
            reject("INDEX_REPLAY", "cannot observe LLM frame begin virtual offset");
        }
        std::array<unsigned char, 4> magic{};
        if (!read_bgzf_exact(input.get(), magic.data(), magic.size(), true)) {
            break;
        }
        if (magic != std::array<unsigned char, 4>{'L', 'L', 'M', '1'}) {
            reject("LLM_SCHEMA", "LLM frame magic differs from LLM1");
        }

        std::array<unsigned char, 36> fixed{};
        read_bgzf_exact(input.get(), fixed.data(), fixed.size(), false);
        Sha256 frame;
        frame.update(std::string_view(reinterpret_cast<const char*>(magic.data()), magic.size()));
        frame.update(std::string_view(reinterpret_cast<const char*>(fixed.data()), fixed.size()));
        semantic.update(std::string_view(reinterpret_cast<const char*>(magic.data()), magic.size()));
        semantic.update(std::string_view(reinterpret_cast<const char*>(fixed.data()), fixed.size()));

        const std::uint16_t major = little_u16(fixed.data());
        const std::uint16_t minor = little_u16(fixed.data() + 2U);
        const std::uint32_t dataset_order = little_u32(fixed.data() + 4U);
        const std::uint64_t site_order = little_u64(fixed.data() + 8U);
        const std::uint32_t dimension = little_u32(fixed.data() + 16U);
        const std::uint64_t value_count = little_u64(fixed.data() + 20U);
        const std::uint64_t mask_bytes = little_u64(fixed.data() + 28U);
        if (major != 1U || minor != 0U) {
            reject("LLM_SCHEMA", "LLM frame schema version differs from 1.0");
        }
        const std::uint64_t expected_values = static_cast<std::uint64_t>(dimension) *
                                              static_cast<std::uint64_t>(dimension - (dimension == 0U ? 0U : 1U)) / 2U;
        if (value_count != expected_values || mask_bytes != (value_count + 7U) / 8U) {
            reject("LLM_COUNT", "LLM dimension/value/mask count conservation failed");
        }
        if (value_count > kMaximumReplayValues ||
            value_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            mask_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            reject("LLM_RESOURCE_LIMIT", "LLM frame exceeds the independent validator replay bound");
        }

        std::vector<unsigned char> value_is_positive_zero(static_cast<std::size_t>(value_count), 0U);
        std::array<unsigned char, 8> value_bytes{};
        for (std::uint64_t index = 0; index < value_count; ++index) {
            read_bgzf_exact(input.get(), value_bytes.data(), value_bytes.size(), false);
            frame.update(std::string_view(reinterpret_cast<const char*>(value_bytes.data()), value_bytes.size()));
            semantic.update(std::string_view(reinterpret_cast<const char*>(value_bytes.data()), value_bytes.size()));
            const std::uint64_t bits = little_u64(value_bytes.data());
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value)) {
                reject("LLM_VALUE", "LLM frame contains NaN or infinity");
            }
            value_is_positive_zero[static_cast<std::size_t>(index)] = bits == 0U ? 1U : 0U;
        }
        std::vector<unsigned char> mask(static_cast<std::size_t>(mask_bytes), 0U);
        if (!mask.empty()) {
            read_bgzf_exact(input.get(), mask.data(), mask.size(), false);
            frame.update(std::string_view(reinterpret_cast<const char*>(mask.data()), mask.size()));
            semantic.update(std::string_view(reinterpret_cast<const char*>(mask.data()), mask.size()));
        }
        const unsigned int remainder = static_cast<unsigned int>(value_count % 8U);
        if (remainder != 0U && !mask.empty()) {
            const unsigned char allowed = static_cast<unsigned char>((1U << remainder) - 1U);
            if ((mask.back() & static_cast<unsigned char>(~allowed)) != 0U) {
                reject("LLM_MASK", "LLM invalid-mask padding bits are nonzero");
            }
        }
        for (std::uint64_t index = 0; index < value_count; ++index) {
            const std::size_t byte_index = static_cast<std::size_t>(index / 8U);
            const unsigned int bit_index = static_cast<unsigned int>(index % 8U);
            const bool invalid = (mask[byte_index] & static_cast<unsigned char>(1U << bit_index)) != 0U;
            if (invalid && value_is_positive_zero[static_cast<std::size_t>(index)] == 0U) {
                reject("LLM_MASK", "masked LLM value does not use the canonical positive-zero payload");
            }
        }

        std::array<unsigned char, 32> checksum{};
        read_bgzf_exact(input.get(), checksum.data(), checksum.size(), false);
        const std::string frame_sha256 = frame.finish();
        if (frame_sha256 != hex_bytes(checksum.data(), checksum.size())) {
            reject("LLM_FRAME_CHECKSUM", "LLM frame SHA-256 differs from fields 1-9");
        }
        const int64_t raw_end = bgzf_tell(input.get());
        if (raw_end < 0) {
            reject("INDEX_REPLAY", "cannot observe LLM frame past-end virtual offset");
        }
        RecordKey key = {
            {true, static_cast<std::uint64_t>(dataset_order), {}},
            {true, site_order, {}},
        };
        if (previous.has_value() && !(*previous < key)) {
            reject(*previous == key ? "DUPLICATE_KEY" : "ARTIFACT_ORDER",
                   *previous == key ? "duplicate LLM frame key" : "out-of-order LLM frame key");
        }
        previous = key;
        IndexGroup group;
        group.key = key;
        group.first_virtual_offset = static_cast<std::uint64_t>(raw_begin);
        group.past_end_virtual_offset = static_cast<std::uint64_t>(raw_end);
        group.logical_rows = 1U;
        group.range_semantic_sha256 = frame_sha256;
        parsed.index_groups.push_back(std::move(group));
        parsed.llm_frames.push_back(
            {static_cast<std::uint64_t>(dataset_order), site_order, static_cast<std::uint64_t>(dimension)});
        if (parsed.logical_rows == 0U) {
            parsed.primary_first = display_key(key);
        }
        parsed.primary_last = display_key(key);
        ++parsed.logical_rows;
    }
    parsed.semantic_sha256 = semantic.finish();
    return parsed;
}

struct StatusRegistry {
    std::map<std::string, std::set<std::string>> statuses;
    std::map<std::string, std::set<std::string>> reasons;
    std::map<std::pair<std::string, std::string>, std::string> reason_parent;
};

StatusRegistry load_status_registry(const std::filesystem::path& repo_root) {
    const std::filesystem::path path = repo_root / "contracts" / "v1" / "status_reason_codes.tsv";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reject("STATUS_VOCABULARY", "cannot open status/reason registry");
    }
    std::string line;
    const std::string expected_header =
        "domain\tkind\tcode\tterminal\tseverity\tallowed_parent_status\trequires_reason\tforbids_winner\t"
        "introduced_in\tdescription\ttest_id";
    if (!std::getline(input, line) || line != expected_header) {
        reject("STATUS_VOCABULARY", "status/reason registry header differs from the frozen contract");
    }
    StatusRegistry registry;
    std::set<std::tuple<std::string, std::string, std::string>> unique;
    while (std::getline(input, line)) {
        const std::vector<std::string> fields = split_tab(line);
        if (fields.size() != 11U || fields[0].empty() || fields[2].empty() ||
            !unique.emplace(fields[0], fields[1], fields[2]).second) {
            reject("STATUS_VOCABULARY", "status/reason registry contains malformed or duplicate rows");
        }
        if (fields[1] == "status") {
            registry.statuses[fields[0]].insert(fields[2]);
        } else if (fields[1] == "reason") {
            registry.reasons[fields[0]].insert(fields[2]);
            registry.reason_parent.emplace(std::make_pair(fields[0], fields[2]), fields[5]);
        } else {
            reject("STATUS_VOCABULARY", "status/reason registry kind is outside status/reason");
        }
    }
    if (!input.eof()) {
        reject("STATUS_VOCABULARY", "status/reason registry read failed");
    }
    return registry;
}

void validate_status_fields(const ParsedArtifact& parsed, const SchemaIdentity& identity,
                            const StatusRegistry& registry) {
    for (std::size_t column = 0; column < identity.field_rules.size(); ++column) {
        const auto& rule = identity.field_rules[column];
        if (rule.type != "status_code" && rule.type != "reason_code") {
            continue;
        }
        const auto binding = identity.field_domains.find(rule.name);
        if (binding == identity.field_domains.end()) {
            reject("STATUS_VOCABULARY", rule.name + ": status/reason field lacks a domain binding");
        }
        const auto& domains = rule.type == "status_code" ? registry.statuses : registry.reasons;
        const auto domain = domains.find(binding->second);
        if (domain == domains.end()) {
            reject("STATUS_VOCABULARY", rule.name + ": bound status/reason domain is absent");
        }
        for (const auto& row : parsed.table_rows) {
            const std::string& value = row[column];
            if (rule.null_token.has_value() && value == *rule.null_token) {
                continue;
            }
            if (domain->second.count(value) == 0U) {
                reject("STATUS_VOCABULARY", rule.name + ": token is outside bound domain " + binding->second);
            }
        }
    }
    for (std::size_t reason_column = 0; reason_column < identity.field_rules.size(); ++reason_column) {
        const auto& reason_rule = identity.field_rules[reason_column];
        if (reason_rule.type != "reason_code") {
            continue;
        }
        const auto reason_domain = identity.field_domains.find(reason_rule.name);
        if (reason_domain == identity.field_domains.end()) {
            reject("STATUS_VOCABULARY", reason_rule.name + ": reason field lacks domain binding");
        }
        std::optional<std::size_t> status_column;
        for (std::size_t index = 0; index < identity.field_rules.size(); ++index) {
            const auto& candidate = identity.field_rules[index];
            const auto candidate_domain = identity.field_domains.find(candidate.name);
            if (candidate.type == "status_code" && candidate_domain != identity.field_domains.end() &&
                candidate_domain->second == reason_domain->second) {
                if (status_column.has_value()) {
                    reject("STATUS_VOCABULARY", reason_rule.name + ": reason domain maps to multiple status columns");
                }
                status_column = index;
            }
        }
        if (!status_column.has_value()) {
            reject("STATUS_VOCABULARY", reason_rule.name + ": no status column shares its domain");
        }
        for (const auto& row : parsed.table_rows) {
            const std::string& reason = row[reason_column];
            if (reason_rule.null_token.has_value() && reason == *reason_rule.null_token) {
                continue;
            }
            const auto parent = registry.reason_parent.find({reason_domain->second, reason});
            if (parent == registry.reason_parent.end() ||
                (parent->second != "." && parent->second != row[*status_column])) {
                reject("STATUS_PARENT_REPLAY", reason_rule.name + ": reason is incompatible with serialized status");
            }
        }
    }
}

const std::string& table_field(const ParsedArtifact& parsed, const std::vector<std::string>& row,
                               const std::string& name, const std::string& check_id) {
    const auto column = parsed.columns.find(name);
    if (column == parsed.columns.end() || column->second >= row.size()) {
        reject(check_id, "required table field is absent: " + name);
    }
    return row[column->second];
}

std::uint64_t table_uint(const ParsedArtifact& parsed, const std::vector<std::string>& row, const std::string& name,
                         const std::string& check_id) {
    const std::string& value = table_field(parsed, row, name, check_id);
    std::uint64_t parsed_value = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
        reject(check_id, name + ": cannot replay unsigned integer");
    }
    return parsed_value;
}

JsonPtr parse_embedded_json(const std::string& value, const std::string& check_id) {
    json_error_t error{};
    JsonPtr parsed(json_loadb(value.data(), value.size(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
    if (!parsed) {
        reject(check_id, "embedded JSON cannot be parsed");
    }
    return parsed;
}

void validate_tabular_semantic_groups(const ParsedArtifact& parsed, const json_t* schema) {
    const json_t* groups = json_object_get(schema, "semantic_groups");
    if (groups == nullptr) {
        return;
    }
    if (!json_is_array(groups)) {
        reject("TSV_SEMANTIC_GROUP", "semantic_groups must be an array");
    }
    for (std::size_t group_index = 0; group_index < json_array_size(groups); ++group_index) {
        const json_t* group = json_array_get(groups, group_index);
        const json_t* constraints = json_object_get(group, "constraints");
        if (!json_is_array(constraints)) {
            reject("TSV_SEMANTIC_GROUP", "semantic group constraints must be an array");
        }
        for (std::size_t constraint_index = 0; constraint_index < json_array_size(constraints); ++constraint_index) {
            const json_t* constraint = json_array_get(constraints, constraint_index);
            const std::string operation = string_field(constraint, "operator");
            if (operation != "LT") {
                reject("TSV_SEMANTIC_GROUP", "unsupported semantic group operator: " + operation);
            }
            const std::string left = string_field(constraint, "left_field");
            const std::string right = string_field(constraint, "right_field");
            for (const auto& row : parsed.table_rows) {
                if (table_uint(parsed, row, left, "TSV_SEMANTIC_GROUP") >=
                    table_uint(parsed, row, right, "TSV_SEMANTIC_GROUP")) {
                    reject("TSV_SEMANTIC_GROUP", left + " must be strictly less than " + right);
                }
            }
        }
    }
}

void validate_embedded_json_bindings(const std::filesystem::path& repo_root, const std::string& artifact_id,
                                     const ParsedArtifact& parsed, const json_t* schema, SchemaStore& store) {
    const json_t* bindings = json_object_get(schema, "embedded_json_bindings");
    if (bindings == nullptr) {
        return;
    }
    if (!json_is_object(bindings)) {
        reject("EMBEDDED_JSON_CONTRACT", "embedded_json_bindings must be an object");
    }
    const char* field_name = nullptr;
    json_t* binding = nullptr;
    json_object_foreach(const_cast<json_t*>(bindings), field_name, binding) {
        const std::string schema_path = string_field(binding, "schema_path");
        const std::string schema_sha256 = string_field(binding, "schema_sha256");
        if (!safe_relative_path(schema_path) || !is_lower_sha256(schema_sha256) ||
            sha256_file(repo_root / schema_path, "EMBEDDED_JSON_CONTRACT") != schema_sha256) {
            reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": embedded schema path or SHA-256 differs");
        }
        const SharedJson nested_schema = store.load_relative(schema_path, "EMBEDDED_JSON_CONTRACT");
        const std::string declared_schema_id = string_field(binding, "schema_id");
        const std::string observed_schema_id = string_field(nested_schema.get(), "$id");
        if (declared_schema_id.empty() || declared_schema_id != observed_schema_id) {
            reject("EMBEDDED_JSON_CONTRACT",
                   std::string(field_name) + ": embedded schema_id differs from nested schema $id");
        }
        const json_t* constraints = json_object_get(binding, "constraints");
        if (!json_is_array(constraints)) {
            reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": embedded constraints must be an array");
        }
        for (std::size_t row_index = 0; row_index < parsed.table_rows.size(); ++row_index) {
            const auto& row = parsed.table_rows[row_index];
            const std::string& encoded = table_field(parsed, row, field_name, "EMBEDDED_JSON_CONTRACT");
            const bool absent = encoded == ".";
            const std::string null_policy = string_field(binding, "null_policy");
            if ((null_policy == "FORBIDDEN" && absent) ||
                (null_policy != "FORBIDDEN" && null_policy != "DOT_TOKEN_ONLY")) {
                reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": value violates embedded null policy");
            }
            JsonPtr value;
            if (!absent) {
                value = parse_embedded_json(encoded, "EMBEDDED_JSON_CONTRACT");
                std::string canonical;
                try {
                    validate_json_schema_value(value.get(), nested_schema.get(), nested_schema, store);
                    canonical = canonical_json(value.get(), nested_schema.get(), nested_schema, store);
                } catch (const ValidationError& validation_error) {
                    reject(validation_error.check_id(), artifact_id + ": TSV data row (1-based) " +
                                                            std::to_string(row_index + 1U) + " field " + field_name +
                                                            ": " + validation_error.what());
                }
                if (canonical != encoded) {
                    reject("EMBEDDED_JSON_CONTRACT",
                           std::string(field_name) + ": bytes differ from nested-schema canonical JSON");
                }
            }
            for (std::size_t index = 0; index < json_array_size(constraints); ++index) {
                const json_t* constraint = json_array_get(constraints, index);
                const std::string operation = string_field(constraint, "operator");
                const std::string companion = string_field(constraint, "field");
                if (operation == "VALUE_PRESENT_IFF_FIELD_EQUALS") {
                    const std::string expected = string_field(constraint, "value");
                    const bool should_exist = table_field(parsed, row, companion, "EMBEDDED_JSON_CONTRACT") == expected;
                    if (should_exist == absent) {
                        reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": presence predicate differs");
                    }
                    continue;
                }
                if (operation == "NULLABILITY_EQUALS_FIELD") {
                    const bool companion_absent = table_field(parsed, row, companion, "EMBEDDED_JSON_CONTRACT") == ".";
                    if (companion_absent != absent) {
                        reject("EMBEDDED_JSON_CONTRACT",
                               std::string(field_name) + ": nullability differs from " + companion);
                    }
                    continue;
                }
                if (operation == "PARTNER_SITE_ORDER_SPACING_AT_LEAST") {
                    continue;
                }
                if (absent || !value) {
                    reject("EMBEDDED_JSON_CONTRACT",
                           std::string(field_name) + ": absent value cannot satisfy " + operation);
                }
                if (operation == "ARRAY_LENGTH_EQUALS_FIELD") {
                    if (!json_is_array(value.get()) ||
                        json_array_size(value.get()) != table_uint(parsed, row, companion, "EMBEDDED_JSON_CONTRACT")) {
                        reject("EMBEDDED_JSON_CONTRACT",
                               std::string(field_name) + ": array length differs from " + companion);
                    }
                } else if (operation == "CANONICAL_JSON_SHA256_EQUALS_FIELD") {
                    if (sha256_bytes(encoded) != table_field(parsed, row, companion, "EMBEDDED_JSON_CONTRACT")) {
                        reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": canonical JSON SHA-256 differs");
                    }
                } else if (operation == "MATRIX_TOTAL_EQUALS_FIELD" || operation == "MATRIX_COLUMN_SUM_EQUALS_FIELD" ||
                           operation == "MINIMUM_ROW_SUM_EQUALS_FIELD") {
                    if (!json_is_array(value.get())) {
                        reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": matrix value is not an array");
                    }
                    std::uint64_t total = 0;
                    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
                    std::array<std::uint64_t, 2> columns{};
                    for (std::size_t matrix_row_index = 0; matrix_row_index < json_array_size(value.get());
                         ++matrix_row_index) {
                        const json_t* matrix_row = json_array_get(value.get(), matrix_row_index);
                        if (!json_is_array(matrix_row) || json_array_size(matrix_row) != 2U) {
                            reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": matrix row is not Kx2");
                        }
                        std::uint64_t row_total = 0;
                        for (std::size_t column = 0; column < 2U; ++column) {
                            const json_t* cell = json_array_get(matrix_row, column);
                            if (!json_is_integer(cell) || json_integer_value(cell) < 0) {
                                reject("EMBEDDED_JSON_CONTRACT",
                                       std::string(field_name) + ": matrix cell is not nonnegative");
                            }
                            const std::uint64_t count = static_cast<std::uint64_t>(json_integer_value(cell));
                            if (count > std::numeric_limits<std::uint64_t>::max() - row_total ||
                                count > std::numeric_limits<std::uint64_t>::max() - columns[column] ||
                                count > std::numeric_limits<std::uint64_t>::max() - total) {
                                reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": matrix count overflows");
                            }
                            row_total += count;
                            columns[column] += count;
                            total += count;
                        }
                        minimum = std::min(minimum, row_total);
                    }
                    const std::uint64_t expected = table_uint(parsed, row, companion, "EMBEDDED_JSON_CONTRACT");
                    if ((operation == "MATRIX_TOTAL_EQUALS_FIELD" && total != expected) ||
                        (operation == "MINIMUM_ROW_SUM_EQUALS_FIELD" &&
                         (json_array_size(value.get()) == 0U ? 0U : minimum) != expected)) {
                        reject("EMBEDDED_JSON_CONTRACT",
                               std::string(field_name) + ": matrix aggregate differs from " + companion);
                    }
                    if (operation == "MATRIX_COLUMN_SUM_EQUALS_FIELD") {
                        const std::uint64_t column = uint_field(constraint, "column", "EMBEDDED_JSON_CONTRACT");
                        if (column >= columns.size() || columns[static_cast<std::size_t>(column)] != expected) {
                            reject("EMBEDDED_JSON_CONTRACT",
                                   std::string(field_name) + ": matrix column sum differs from " + companion);
                        }
                    }
                } else {
                    reject("EMBEDDED_JSON_CONTRACT", std::string(field_name) + ": unsupported constraint " + operation);
                }
            }
        }
    }
}

void compare_record_receipt(const ArtifactRecord& record, const ParsedArtifact& parsed) {
    if (record.logical_rows != parsed.logical_rows) {
        reject("SEMANTIC_DIGEST", record.artifact_id + ": logical row count mismatch");
    }
    if (record.semantic_sha256 != parsed.semantic_sha256) {
        reject("SEMANTIC_DIGEST", record.artifact_id + ": semantic SHA-256 mismatch");
    }
    const std::vector<std::string> expected_first =
        parsed.logical_rows == 0U ? std::vector<std::string>{} : parsed.primary_first;
    const std::vector<std::string> expected_last =
        parsed.logical_rows == 0U ? std::vector<std::string>{} : parsed.primary_last;
    if (record.primary_key_first != expected_first || record.primary_key_last != expected_last) {
        reject("ARTIFACT_ORDER", record.artifact_id + ": receipted primary-key range mismatch");
    }
}

void validate_index_file(const std::filesystem::path& run_root, const ArtifactSpec& spec, const ArtifactRecord& record,
                         const ParsedArtifact& parsed, SchemaStore& store, const Catalog& catalog,
                         const std::string& run_id) {
    if (!spec.index_path.has_value()) {
        if (record.index.has_value()) {
            reject("INDEX_REPLAY", spec.artifact_id + ": unexpected nested index binding");
        }
        return;
    }
    if (!record.index.has_value() || record.index->relative_path != *spec.index_path) {
        reject("INDEX_REPLAY", spec.artifact_id + ": missing or wrong nested index binding");
    }
    const ArtifactSpec& index_catalog = catalog.artifacts.at("checksums");
    static_cast<void>(index_catalog);
    const std::string index_schema_path = "schema/records/site_index.record.json";
    const SharedJson index_schema = store.load_relative(index_schema_path, "INDEX_REPLAY");
    const SchemaIdentity identity = schema_identity(index_schema.get(), "INDEX_REPLAY");
    ArtifactSpec index_spec;
    index_spec.artifact_id = spec.artifact_id + ".index";
    index_spec.primary_key = {"artifact_id", "dataset_order", "record_order"};
    index_spec.sort_key = index_spec.primary_key;
    const std::filesystem::path index_path =
        require_regular_under(run_root, record.index->relative_path, "INDEX_REPLAY");
    if (std::filesystem::file_size(index_path) != record.index->size_bytes ||
        sha256_file(index_path, "INDEX_REPLAY") != record.index->physical_sha256) {
        reject("INDEX_REPLAY", spec.artifact_id + ": index physical size/SHA mismatch");
    }
    ParsedArtifact observed = parse_tsv_bgzf(index_path, index_spec, identity, run_id);
    if (observed.logical_rows != record.index->logical_rows ||
        observed.semantic_sha256 != record.index->semantic_sha256 ||
        observed.table_rows.size() != parsed.index_groups.size()) {
        reject("INDEX_REPLAY", spec.artifact_id + ": index row count/semantic digest mismatch");
    }
    for (std::size_t index = 0; index < parsed.index_groups.size(); ++index) {
        const auto& fields = observed.table_rows[index];
        const IndexGroup& expected = parsed.index_groups[index];
        if (fields.size() != 7U || fields[0] != spec.artifact_id || expected.key.size() != 2U ||
            fields[1] != (expected.key[0].numeric ? std::to_string(expected.key[0].number) : expected.key[0].text) ||
            fields[2] != (expected.key[1].numeric ? std::to_string(expected.key[1].number) : expected.key[1].text) ||
            fields[3] != std::to_string(expected.first_virtual_offset) ||
            fields[4] != std::to_string(expected.past_end_virtual_offset) ||
            fields[5] != std::to_string(expected.logical_rows) || fields[6] != expected.range_semantic_sha256) {
            reject("INDEX_REPLAY",
                   spec.artifact_id + ": index row " + std::to_string(index) +
                       " differs from reconstructed artifact range; observed=[" + join_tab(fields) + "], expected=[" +
                       spec.artifact_id + "\t" +
                       (expected.key.size() > 0U
                            ? (expected.key[0].numeric ? std::to_string(expected.key[0].number) : expected.key[0].text)
                            : "<missing>") +
                       "\t" +
                       (expected.key.size() > 1U
                            ? (expected.key[1].numeric ? std::to_string(expected.key[1].number) : expected.key[1].text)
                            : "<missing>") +
                       "\t" + std::to_string(expected.first_virtual_offset) + "\t" +
                       std::to_string(expected.past_end_virtual_offset) + "\t" + std::to_string(expected.logical_rows) +
                       "\t" + expected.range_semantic_sha256 + "]");
        }
    }
}

std::map<std::string, ParsedArtifact> validate_artifacts(const std::filesystem::path& repo_root,
                                                         const std::filesystem::path& run_root, const Catalog& catalog,
                                                         const ProducerReceipt& receipt) {
    SchemaStore store(repo_root);
    const StatusRegistry status_registry = load_status_registry(repo_root);
    std::map<std::string, ParsedArtifact> parsed_artifacts;
    std::vector<std::string> replay_ids = catalog.producer_receipt_ids;
    std::sort(replay_ids.begin(), replay_ids.end(), [&](const std::string& left, const std::string& right) {
        const auto left_record = receipt.artifacts.find(left);
        const auto right_record = receipt.artifacts.find(right);
        const std::uint64_t left_size = left_record == receipt.artifacts.end()
                                            ? std::numeric_limits<std::uint64_t>::max()
                                            : left_record->second.size_bytes;
        const std::uint64_t right_size = right_record == receipt.artifacts.end()
                                             ? std::numeric_limits<std::uint64_t>::max()
                                             : right_record->second.size_bytes;
        return std::tie(left_size, left) < std::tie(right_size, right);
    });
    for (const std::string& artifact_id : replay_ids) {
        const auto record_found = receipt.artifacts.find(artifact_id);
        const auto spec_found = catalog.artifacts.find(artifact_id);
        if (record_found == receipt.artifacts.end() || spec_found == catalog.artifacts.end()) {
            reject("ARTIFACT_MEMBERSHIP", "required producer artifact is absent: " + artifact_id);
        }
        const ArtifactRecord& record = record_found->second;
        const ArtifactSpec& spec = spec_found->second;
        if (record.relative_path != spec.relative_path || record.format != spec.format) {
            reject("ARTIFACT_MEMBERSHIP", artifact_id + ": artifact path/format differs from catalog");
        }
        const SharedJson schema = store.load_relative(spec.record_schema, "SCHEMA_BINDING");
        if (sha256_file(repo_root / spec.record_schema, "SCHEMA_BINDING") != spec.record_schema_sha256) {
            reject("SCHEMA_BINDING", artifact_id + ": record schema SHA-256 mismatch");
        }
        const SchemaIdentity identity = schema_identity(schema.get(), "SCHEMA_BINDING");
        if (record.schema_name != identity.name || record.schema_version != identity.version) {
            reject("SCHEMA_BINDING", artifact_id + ": receipted schema identity mismatch");
        }
        const std::filesystem::path artifact_path =
            require_regular_under(run_root, record.relative_path, "ARTIFACT_MEMBERSHIP");
        if (std::filesystem::file_size(artifact_path) != record.size_bytes ||
            sha256_file(artifact_path, "ARTIFACT_HASH") != record.physical_sha256) {
            reject("ARTIFACT_HASH", artifact_id + ": physical size/SHA-256 mismatch");
        }

        ParsedArtifact parsed;
        if (spec.format == "TSV_BGZF") {
            parsed = parse_tsv_bgzf(artifact_path, spec, identity, receipt.run_id);
        } else if (spec.format == "TSV") {
            parsed = parse_plain_tsv(artifact_path, spec, identity);
        } else if (spec.format == "JSONL_BGZF") {
            parsed = parse_jsonl_bgzf(artifact_path, spec, identity, schema, store);
        } else if (spec.format == "JSON") {
            parsed = parse_json_file(artifact_path, spec, identity, schema, store);
        } else if (spec.format == "LLM_BGZF") {
            parsed = parse_llm_bgzf(artifact_path, spec, identity);
        } else {
            reject("UNSUPPORTED_ARTIFACT_FORMAT", "unsupported producer artifact format: " + spec.format);
        }
        if (artifact_id == "methyl_calls") {
            for (const auto& rule : identity.field_rules) {
                if (rule.type == "status_code" || rule.type == "reason_code") {
                    reject("METHYL_CONSERVATION",
                           "streamed methyl schema cannot contain "
                           "deferred status/reason validation");
                }
            }
            if (json_object_get(schema.get(), "semantic_groups") != nullptr ||
                json_object_get(schema.get(), "embedded_json_bindings") != nullptr) {
                reject("METHYL_CONSERVATION",
                       "streamed methyl schema cannot contain deferred "
                       "row constraints");
            }
        } else if (spec.format == "TSV_BGZF" || spec.format == "TSV") {
            validate_status_fields(parsed, identity, status_registry);
            validate_tabular_semantic_groups(parsed, schema.get());
            validate_embedded_json_bindings(repo_root, artifact_id, parsed, schema.get(), store);
        }
        compare_record_receipt(record, parsed);
        validate_index_file(run_root, spec, record, parsed, store, catalog, receipt.run_id);
        parsed_artifacts.emplace(artifact_id, std::move(parsed));
    }
    return parsed_artifacts;
}

std::set<std::string> as_set(const std::vector<std::string>& values) { return {values.begin(), values.end()}; }

void validate_membership(const Catalog& catalog, const ProducerReceipt& receipt) {
    const std::set<std::string> expected = as_set(catalog.producer_receipt_ids);
    std::set<std::string> observed;
    for (const auto& [artifact_id, record] : receipt.artifacts) {
        static_cast<void>(record);
        observed.insert(artifact_id);
    }
    if (expected != observed) {
        reject("ARTIFACT_MEMBERSHIP", "producer receipt artifact set differs from catalog membership");
    }
}

void validate_file_census(const std::filesystem::path& run_root, const Catalog& catalog,
                          const ProducerReceipt& receipt) {
    std::set<std::string> expected_files;
    std::set<std::string> expected_directories;
    auto add_file = [&](const std::string& relative) {
        expected_files.insert(relative);
        std::filesystem::path parent = std::filesystem::path(relative).parent_path();
        while (!parent.empty()) {
            expected_directories.insert(parent.generic_string());
            parent = parent.parent_path();
        }
    };
    for (const auto& [artifact_id, record] : receipt.artifacts) {
        static_cast<void>(artifact_id);
        add_file(record.relative_path);
        if (record.index.has_value()) {
            add_file(record.index->relative_path);
        }
    }
    add_file(catalog.artifacts.at("producer_receipt").relative_path);
    add_file(catalog.artifacts.at("checksums").relative_path);

    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(run_root, error), end; iterator != end;
         iterator.increment(error)) {
        if (error) {
            reject("FILE_CENSUS", "cannot enumerate run root: " + error.message());
        }
        const std::filesystem::path relative = std::filesystem::relative(iterator->path(), run_root, error);
        if (error) {
            reject("FILE_CENSUS", "cannot derive run-relative path");
        }
        const auto status = iterator->symlink_status(error);
        if (error || std::filesystem::is_symlink(status)) {
            reject("FILE_CENSUS", "symlink or unreadable path in run root: " + relative.generic_string());
        }
        if (std::filesystem::is_regular_file(status)) {
            if (expected_files.count(relative.generic_string()) == 0U) {
                reject("FILE_CENSUS", "extra regular file in staging root: " + relative.generic_string());
            }
        } else if (std::filesystem::is_directory(status)) {
            if (expected_directories.count(relative.generic_string()) == 0U) {
                reject("FILE_CENSUS", "extra directory in staging root: " + relative.generic_string());
            }
        } else {
            reject("FILE_CENSUS", "unsupported file type in staging root: " + relative.generic_string());
        }
    }
    for (const std::string& relative : expected_files) {
        require_regular_under(run_root, relative, "FILE_CENSUS");
    }
}

void validate_semantic_digest_table(const Catalog& catalog, const ProducerReceipt& receipt,
                                    const std::map<std::string, ParsedArtifact>& parsed) {
    const auto digest_artifact = parsed.find("semantic_digests");
    if (digest_artifact == parsed.end()) {
        reject("SEMANTIC_DIGEST", "semantic_digests artifact was not parsed");
    }
    std::map<std::string, std::tuple<std::string, std::string, std::uint64_t, std::string>> rows;
    for (const auto& fields : digest_artifact->second.table_rows) {
        if (fields.size() != 5U) {
            reject("SEMANTIC_DIGEST", "semantic digest row has wrong field count");
        }
        std::uint64_t logical_rows = 0;
        const auto parsed_count = std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), logical_rows);
        if (parsed_count.ec != std::errc{} || parsed_count.ptr != fields[3].data() + fields[3].size() ||
            !is_lower_sha256(fields[4]) ||
            !rows.emplace(fields[0], std::make_tuple(fields[1], fields[2], logical_rows, fields[4])).second) {
            reject("SEMANTIC_DIGEST", "malformed or duplicate semantic digest row");
        }
    }
    if (as_set(catalog.semantic_digest_ids) != [&]() {
            std::set<std::string> keys;
            for (const auto& [id, row] : rows) {
                static_cast<void>(row);
                keys.insert(id);
            }
            return keys;
        }()) {
        reject("SEMANTIC_DIGEST", "semantic digest artifact set differs from catalog membership");
    }
    for (const std::string& artifact_id : catalog.semantic_digest_ids) {
        const auto record = receipt.artifacts.find(artifact_id);
        if (record == receipt.artifacts.end()) {
            reject("SEMANTIC_DIGEST", "semantic digest references absent artifact: " + artifact_id);
        }
        const auto& [schema_name, schema_version, logical_rows, digest] = rows.at(artifact_id);
        if (schema_name != record->second.schema_name || schema_version != record->second.schema_version ||
            logical_rows != record->second.logical_rows || digest != record->second.semantic_sha256) {
            reject("SEMANTIC_DIGEST", artifact_id + ": semantic digest row differs from artifact receipt");
        }
    }
}

void validate_artifact_catalog_rows(const Catalog& catalog, const ProducerReceipt& receipt,
                                    const std::map<std::string, ParsedArtifact>& parsed) {
    const auto artifact = parsed.find("artifact_catalog");
    if (artifact == parsed.end()) {
        reject("ARTIFACT_CATALOG_REPLAY", "artifact_catalog was not parsed");
    }
    std::set<std::string> observed;
    for (const JsonPtr& row : artifact->second.json_records) {
        if (string_field(row.get(), "run_id") != receipt.run_id) {
            reject("ARTIFACT_CATALOG_REPLAY", "artifact catalog run_id mismatch");
        }
        const json_t* nested = json_object_get(row.get(), "artifact");
        const std::string artifact_id = string_field(nested, "artifact_id");
        const auto expected = receipt.artifacts.find(artifact_id);
        if (expected == receipt.artifacts.end() || !observed.insert(artifact_id).second) {
            reject("ARTIFACT_CATALOG_REPLAY", "artifact catalog row is extra or duplicated");
        }
        if (!json_equal(nested, expected->second.raw.get())) {
            reject("ARTIFACT_CATALOG_REPLAY", artifact_id + ": artifact catalog row differs from producer receipt");
        }
    }
    if (observed != as_set(catalog.artifact_catalog_ids)) {
        reject("ARTIFACT_CATALOG_REPLAY", "artifact catalog row set differs from catalog membership");
    }
}

void validate_data_lineage_rows(const Catalog& catalog, const ProducerReceipt& receipt,
                                const std::map<std::string, ParsedArtifact>& parsed) {
    const auto artifact = parsed.find("data_lineage");
    if (artifact == parsed.end()) {
        reject("DATA_LINEAGE_REPLAY", "data_lineage was not parsed");
    }
    std::set<std::string> observed;
    for (const JsonPtr& row : artifact->second.json_records) {
        const std::string artifact_id = string_field(row.get(), "output_artifact_id");
        const auto expected = receipt.artifacts.find(artifact_id);
        if (string_field(row.get(), "run_id") != receipt.run_id || expected == receipt.artifacts.end() ||
            !observed.insert(artifact_id).second ||
            string_field(row.get(), "transform_id") != expected->second.transform_id ||
            string_field(row.get(), "output_semantic_sha256") != expected->second.semantic_sha256 ||
            string_field(row.get(), "producer_executable_sha256") != expected->second.producer_executable_sha256 ||
            !json_equal(json_object_get(row.get(), "inputs"), json_object_get(expected->second.raw.get(), "inputs"))) {
            reject("DATA_LINEAGE_REPLAY", artifact_id + ": lineage row differs from artifact receipt");
        }
    }
    if (observed != as_set(catalog.data_lineage_ids)) {
        reject("DATA_LINEAGE_REPLAY", "data lineage row set differs from catalog membership");
    }
}

void validate_checksums(const std::filesystem::path& run_root, const Catalog& catalog, const ProducerReceipt& receipt) {
    const std::filesystem::path checksum_path =
        require_regular_under(run_root, catalog.artifacts.at("checksums").relative_path, "CHECKSUM_REPLAY");
    std::ifstream input(checksum_path, std::ios::binary);
    if (!input) {
        reject("CHECKSUM_REPLAY", "cannot open checksum manifest");
    }
    std::map<std::string, std::string> rows;
    std::string line;
    std::string previous_path;
    while (std::getline(input, line)) {
        if (line.size() < 67U || line[64] != ' ' || line[65] != ' ') {
            reject("CHECKSUM_REPLAY", "malformed checksum line");
        }
        const std::string digest = line.substr(0, 64U);
        const std::string relative = line.substr(66U);
        if (!is_lower_sha256(digest) || !safe_relative_path(relative) ||
            (!previous_path.empty() && !(previous_path < relative)) || !rows.emplace(relative, digest).second) {
            reject("CHECKSUM_REPLAY", "checksum path/digest/order is malformed or duplicated");
        }
        previous_path = relative;
    }
    if (!input.eof()) {
        reject("CHECKSUM_REPLAY", "checksum manifest read failed");
    }
    std::set<std::string> expected;
    for (const std::string& artifact_id : catalog.checksum_ids) {
        if (artifact_id == "producer_receipt") {
            expected.insert(catalog.artifacts.at("producer_receipt").relative_path);
            continue;
        }
        const auto artifact = receipt.artifacts.find(artifact_id);
        if (artifact == receipt.artifacts.end()) {
            reject("CHECKSUM_REPLAY", "checksum membership references absent artifact: " + artifact_id);
        }
        expected.insert(artifact->second.relative_path);
        if (catalog.checksums_include_indexes && artifact->second.index.has_value()) {
            expected.insert(artifact->second.index->relative_path);
        }
    }
    std::set<std::string> observed;
    for (const auto& [relative, digest] : rows) {
        observed.insert(relative);
        if (sha256_file(require_regular_under(run_root, relative, "CHECKSUM_REPLAY"), "CHECKSUM_REPLAY") != digest) {
            reject("CHECKSUM_REPLAY", "checksum mismatch: " + relative);
        }
    }
    if (expected != observed) {
        reject("CHECKSUM_REPLAY", "checksum manifest file set differs from catalog membership");
    }
}

std::vector<PublicationFileSnapshot> capture_publication_snapshot(const std::filesystem::path& run_root,
                                                                  const Catalog& catalog,
                                                                  const ProducerReceipt& producer) {
    std::set<std::string> relative_paths;
    for (const auto& [artifact_id, artifact] : producer.artifacts) {
        static_cast<void>(artifact_id);
        relative_paths.insert(artifact.relative_path);
        if (artifact.index.has_value()) {
            relative_paths.insert(artifact.index->relative_path);
        }
    }
    relative_paths.insert(catalog.artifacts.at("producer_receipt").relative_path);
    relative_paths.insert(catalog.artifacts.at("checksums").relative_path);
    std::vector<PublicationFileSnapshot> snapshot;
    snapshot.reserve(relative_paths.size() + 1U);
    for (const std::string& relative : relative_paths) {
        const std::filesystem::path path = require_regular_under(run_root, relative, "PUBLICATION_BASELINE");
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error) {
            reject("PUBLICATION_BASELINE", "cannot observe publication file size: " + relative);
        }
        snapshot.push_back({relative, size, sha256_file(path, "PUBLICATION_BASELINE")});
    }
    return snapshot;
}

void append_validation_receipt_snapshot(const std::filesystem::path& run_root, const Catalog& catalog,
                                        ArtifactValidationReport& report) {
    const std::string relative = catalog.artifacts.at("validation_receipt").relative_path;
    const std::filesystem::path path = require_regular_under(run_root, relative, "PUBLICATION_BASELINE");
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || sha256_file(path, "PUBLICATION_BASELINE") != report.validation_receipt_sha256) {
        reject("PUBLICATION_BASELINE", "validation receipt differs while completing publication snapshot");
    }
    report.publication_snapshot.push_back({relative, size, report.validation_receipt_sha256});
    std::sort(report.publication_snapshot.begin(), report.publication_snapshot.end(),
              [](const PublicationFileSnapshot& left, const PublicationFileSnapshot& right) {
                  return left.relative_path < right.relative_path;
              });
    report.publication_snapshot_captured = true;
}

void verify_publication_snapshot(const std::filesystem::path& root,
                                 const std::vector<PublicationFileSnapshot>& snapshot,
                                 const std::optional<std::string>& pending_receipt_sha256) {
    constexpr std::string_view kPendingReceipt = ".run_receipt.json.pending";
    std::set<std::string> expected_files;
    std::set<std::string> expected_directories;
    for (const PublicationFileSnapshot& entry : snapshot) {
        if (!safe_relative_path(entry.relative_path) || !is_lower_sha256(entry.physical_sha256) ||
            !expected_files.insert(entry.relative_path).second) {
            reject("PRE_FREEZE_REPLAY", "publication snapshot contains malformed or duplicate identity");
        }
        std::filesystem::path parent = std::filesystem::path(entry.relative_path).parent_path();
        while (!parent.empty()) {
            expected_directories.insert(parent.generic_string());
            parent = parent.parent_path();
        }
        const std::filesystem::path path = require_regular_under(root, entry.relative_path, "PRE_FREEZE_REPLAY");
        std::error_code error;
        if (std::filesystem::file_size(path, error) != entry.size_bytes || error ||
            sha256_file(path, "PRE_FREEZE_REPLAY") != entry.physical_sha256) {
            reject("PRE_FREEZE_REPLAY", "publication file changed after validation: " + entry.relative_path);
        }
    }
    if (pending_receipt_sha256.has_value()) {
        if (!is_lower_sha256(*pending_receipt_sha256)) {
            reject("PRE_FREEZE_REPLAY", "pending run receipt digest is malformed");
        }
        expected_files.insert(std::string(kPendingReceipt));
        const std::filesystem::path pending =
            require_regular_under(root, std::string(kPendingReceipt), "PRE_FREEZE_REPLAY");
        if (sha256_file(pending, "PRE_FREEZE_REPLAY") != *pending_receipt_sha256) {
            reject("PRE_FREEZE_REPLAY", "pending run receipt changed before publication");
        }
    }
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end; iterator != end;
         iterator.increment(error)) {
        if (error) {
            reject("PRE_FREEZE_REPLAY", "cannot enumerate publication root: " + error.message());
        }
        const std::filesystem::path relative = std::filesystem::relative(iterator->path(), root, error);
        const auto status = iterator->symlink_status(error);
        if (error || std::filesystem::is_symlink(status)) {
            reject("PRE_FREEZE_REPLAY", "unsafe path appeared after validation");
        }
        const std::string key = relative.generic_string();
        if ((std::filesystem::is_regular_file(status) && expected_files.count(key) == 0U) ||
            (std::filesystem::is_directory(status) && expected_directories.count(key) == 0U) ||
            (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status))) {
            reject("PRE_FREEZE_REPLAY", "unexpected path appeared after validation: " + key);
        }
    }
}

class PublicationLock final {
   public:
    explicit PublicationLock(const std::filesystem::path& output_base) {
        const std::filesystem::path path = output_base / ".longlineage-publication.lock";
        descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
                descriptor_ = -1;
            }
            reject("PRE_FREEZE_REPLAY", "cannot acquire exclusive output-base publication lock");
        }
    }
    ~PublicationLock() {
        if (descriptor_ >= 0) {
            static_cast<void>(::flock(descriptor_, LOCK_UN));
            ::close(descriptor_);
        }
    }
    PublicationLock(const PublicationLock&) = delete;
    PublicationLock& operator=(const PublicationLock&) = delete;

   private:
    int descriptor_{-1};
};

using SiteKey = std::pair<std::uint64_t, std::uint64_t>;

double table_float(const ParsedArtifact& parsed, const std::vector<std::string>& row, const std::string& name,
                   const std::string& check_id) {
    const std::string& encoded = table_field(parsed, row, name, check_id);
    if (encoded == ".") {
        reject(check_id, name + ": required replay value is null");
    }
    std::istringstream input(encoded);
    input.imbue(std::locale::classic());
    double value = 0.0;
    input >> value;
    if (!input || input.peek() != std::char_traits<char>::eof() || !std::isfinite(value)) {
        reject(check_id, name + ": cannot replay finite float");
    }
    return value;
}

bool near_science(double observed, double expected, double tolerance = 2e-12) noexcept {
    return std::isfinite(observed) && std::isfinite(expected) &&
           std::abs(observed - expected) <= tolerance * std::max({1.0, std::abs(observed), std::abs(expected)});
}

double independent_log_choose(std::uint64_t n, std::uint64_t k) {
    if (k > n) {
        return -std::numeric_limits<double>::infinity();
    }
    return std::lgamma(static_cast<double>(n) + 1.0) - std::lgamma(static_cast<double>(k) + 1.0) -
           std::lgamma(static_cast<double>(n - k) + 1.0);
}

double independent_log_add(double left, double right) noexcept {
    if (std::isinf(left) && left < 0.0) {
        return right;
    }
    if (std::isinf(right) && right < 0.0) {
        return left;
    }
    const double maximum = std::max(left, right);
    const double minimum = std::min(left, right);
    return maximum + std::log1p(std::exp(minimum - maximum));
}

struct IndependentExactResult {
    enum class State {
        kDegenerate,
        kEnumerated,
        kCeiling,
    };
    State state = State::kDegenerate;
    std::uint64_t state_count = 0;
    double p_value = 0.0;
};

IndependentExactResult independent_exact_kx2(const std::vector<std::array<std::uint64_t, 2>>& input,
                                             std::uint64_t ceiling = 250000U) {
    std::vector<std::array<std::uint64_t, 2>> rows;
    std::uint64_t ref_total = 0;
    std::uint64_t alt_total = 0;
    for (const auto& row : input) {
        if (row[0] > std::numeric_limits<std::uint64_t>::max() - row[1] ||
            ref_total > std::numeric_limits<std::uint64_t>::max() - row[0] ||
            alt_total > std::numeric_limits<std::uint64_t>::max() - row[1]) {
            reject("EXACT_STATISTIC_REPLAY", "Kx2 table overflows uint64");
        }
        if (row[0] + row[1] != 0U) {
            rows.push_back(row);
            ref_total += row[0];
            alt_total += row[1];
        }
    }
    if (rows.size() < 2U || ref_total == 0U || alt_total == 0U) {
        return {};
    }

    std::vector<std::uint64_t> totals;
    std::vector<std::vector<double>> log_weights;
    double observed = 0.0;
    double compensation = 0.0;
    for (const auto& row : rows) {
        const std::uint64_t total = row[0] + row[1];
        if (total > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() - 1U)) {
            reject("EXACT_STATISTIC_REPLAY", "Kx2 row exceeds addressable replay storage");
        }
        totals.push_back(total);
        std::vector<double> choices(static_cast<std::size_t>(total + 1U));
        for (std::uint64_t alt = 0; alt <= total; ++alt) {
            choices[static_cast<std::size_t>(alt)] = independent_log_choose(total, alt);
        }
        const double adjusted = choices[static_cast<std::size_t>(row[1])] - compensation;
        const double next = observed + adjusted;
        compensation = (next - observed) - adjusted;
        observed = next;
        log_weights.push_back(std::move(choices));
    }
    std::vector<std::uint64_t> suffix(totals.size() + 1U, 0U);
    for (std::size_t index = totals.size(); index-- > 0U;) {
        if (suffix[index + 1U] > std::numeric_limits<std::uint64_t>::max() - totals[index]) {
            reject("EXACT_STATISTIC_REPLAY", "Kx2 suffix capacity overflows");
        }
        suffix[index] = suffix[index + 1U] + totals[index];
    }

    std::uint64_t states = 0;
    double total_log = -std::numeric_limits<double>::infinity();
    double tail_log = -std::numeric_limits<double>::infinity();
    bool exceeded = false;
    const auto visit = [&](double weight) {
        ++states;
        if (states > ceiling) {
            exceeded = true;
            return;
        }
        total_log = independent_log_add(total_log, weight);
        if (weight <= observed + 1e-12) {
            tail_log = independent_log_add(tail_log, weight);
        }
    };
    std::function<void(std::size_t, std::uint64_t, double)> enumerate;
    enumerate = [&](std::size_t index, std::uint64_t remaining_alt, double weight) {
        if (exceeded) {
            return;
        }
        const std::uint64_t row_total = totals[index];
        if (index + 1U == totals.size()) {
            if (remaining_alt <= row_total) {
                visit(weight + log_weights[index][static_cast<std::size_t>(remaining_alt)]);
            }
            return;
        }
        const std::uint64_t remaining_capacity = suffix[index + 1U];
        const std::uint64_t lower = remaining_alt > remaining_capacity ? remaining_alt - remaining_capacity : 0U;
        const std::uint64_t upper = std::min(row_total, remaining_alt);
        for (std::uint64_t alt = lower; alt <= upper; ++alt) {
            enumerate(index + 1U, remaining_alt - alt, weight + log_weights[index][static_cast<std::size_t>(alt)]);
            if (exceeded || alt == std::numeric_limits<std::uint64_t>::max()) {
                break;
            }
        }
    };
    enumerate(0U, alt_total, 0.0);
    if (exceeded) {
        return {IndependentExactResult::State::kCeiling, ceiling + 1U, 0.0};
    }
    if (states == 0U || !std::isfinite(total_log)) {
        return {};
    }
    return {IndependentExactResult::State::kEnumerated, states, std::min(1.0, std::exp(tail_log - total_log))};
}

std::vector<std::optional<double>> independent_fdr(const std::vector<std::optional<double>>& p_values, bool by) {
    std::vector<std::pair<std::size_t, double>> valid;
    for (std::size_t index = 0; index < p_values.size(); ++index) {
        if (p_values[index].has_value()) {
            if (!std::isfinite(*p_values[index]) || *p_values[index] < 0.0 || *p_values[index] > 1.0) {
                reject("GLOBAL_FDR_REPLAY", "p-value is outside [0,1]");
            }
            valid.emplace_back(index, *p_values[index]);
        }
    }
    std::stable_sort(valid.begin(), valid.end(),
                     [](const auto& left, const auto& right) { return left.second < right.second; });
    std::vector<std::optional<double>> adjusted(p_values.size());
    double running = 1.0;
    for (std::size_t reverse = valid.size(); reverse-- > 0U;) {
        const double rank = static_cast<double>(reverse + 1U);
        running = std::min(running, valid[reverse].second * static_cast<double>(valid.size()) / rank);
        adjusted[valid[reverse].first] = std::min(1.0, running);
    }
    if (by && !valid.empty()) {
        double harmonic = 0.0;
        for (std::size_t rank = 1U; rank <= valid.size(); ++rank) {
            harmonic += 1.0 / static_cast<double>(rank);
        }
        for (auto& value : adjusted) {
            if (value.has_value()) {
                *value = std::min(1.0, *value * harmonic);
            }
        }
    }
    return adjusted;
}

struct SiteReadReplay {
    struct SiteIdentity {
        std::string dataset_id;
        std::string chrom;
        std::uint64_t pos1 = 0;
        std::string ref;
        std::string alt;
    };
    std::map<SiteKey, SiteIdentity> sites;
    std::map<SiteKey, std::map<std::string, char>> calls;
    std::uint64_t exact_joins = 0;
    std::uint64_t raw_expected = 0;
    std::uint64_t raw_matched = 0;
    std::uint64_t rg_only_duplicates = 0;
};

SiteReadReplay replay_site_reads(const ParsedArtifact& artifact) {
    SiteReadReplay replay;
    for (const auto& row : artifact.table_rows) {
        const SiteKey key{table_uint(artifact, row, "dataset_order", "SITE_READ_CONSERVATION"),
                          table_uint(artifact, row, "site_order", "SITE_READ_CONSERVATION")};
        SiteReadReplay::SiteIdentity identity{table_field(artifact, row, "dataset_id", "SITE_READ_CONSERVATION"),
                                              table_field(artifact, row, "chrom", "SITE_READ_CONSERVATION"),
                                              table_uint(artifact, row, "pos1", "SITE_READ_CONSERVATION"),
                                              table_field(artifact, row, "ref", "SITE_READ_CONSERVATION"),
                                              table_field(artifact, row, "alt", "SITE_READ_CONSERVATION")};
        const auto existing = replay.sites.find(key);
        if (existing != replay.sites.end() &&
            std::tie(existing->second.dataset_id, existing->second.chrom, existing->second.pos1, existing->second.ref,
                     existing->second.alt) !=
                std::tie(identity.dataset_id, identity.chrom, identity.pos1, identity.ref, identity.alt)) {
            reject("SITE_READ_CONSERVATION", "site identity changes across serialized read rows");
        }
        replay.sites.emplace(key, std::move(identity));
        const std::string& read_id = table_field(artifact, row, "read_id", "SITE_READ_CONSERVATION");
        const std::string& allele = table_field(artifact, row, "allele_call", "SITE_READ_CONSERVATION");
        if (allele.size() != 1U || !replay.calls[key].emplace(read_id, allele.front()).second) {
            reject("SITE_READ_CONSERVATION", "duplicate or malformed site/read call");
        }
        const std::uint64_t identity_count = table_uint(artifact, row, "full_identity_count", "SITE_READ_CONSERVATION");
        const std::string& join = table_field(artifact, row, "projection_join_status", "SITE_READ_CONSERVATION");
        if ((join == "EXACT_UNIQUE" && identity_count != 1U) || (join == "RG_ONLY_COLLAPSED" && identity_count < 2U)) {
            reject("SITE_READ_CONSERVATION", "projection join status disagrees with full identity count");
        }
        if (replay.raw_expected == std::numeric_limits<std::uint64_t>::max() ||
            replay.raw_matched > std::numeric_limits<std::uint64_t>::max() - identity_count ||
            replay.rg_only_duplicates > std::numeric_limits<std::uint64_t>::max() - (identity_count - 1U)) {
            reject("SITE_READ_CONSERVATION", "raw occurrence conservation counters overflow uint64");
        }
        ++replay.raw_expected;
        replay.raw_matched += identity_count;
        replay.rg_only_duplicates += identity_count - 1U;
        if (join == "EXACT_UNIQUE") {
            ++replay.exact_joins;
        }
    }
    if (replay.raw_expected + replay.rg_only_duplicates != replay.raw_matched) {
        reject("SITE_READ_CONSERVATION", "raw occurrence conservation failed");
    }
    return replay;
}

struct MethylReplay {
    const std::map<SiteKey, std::set<std::string>>* reads = nullptr;
};

MethylReplay replay_methyl_calls(const ParsedArtifact& artifact, const SiteReadReplay& site_reads) {
    for (const auto& [key, read_ids] : artifact.methyl_read_presence) {
        const auto site = site_reads.calls.find(key);
        if (site == site_reads.calls.end()) {
            reject("METHYL_CONSERVATION",
                   "methylation site does not reference serialized "
                   "site-read evidence");
        }
        for (const auto& read_id : read_ids) {
            const auto call = site->second.find(read_id);
            if (call == site->second.end() || call->second != 'A') {
                reject("METHYL_CONSERVATION",
                       "methylation call does not reference a "
                       "focal-ALT site/read row");
            }
        }
    }
    return MethylReplay{&artifact.methyl_read_presence};
}

struct M1Replay {
    struct Site {
        std::string dataset_id;
        std::string status;
        std::string reason;
        std::uint64_t joined = 0;
        std::uint64_t after_peel = 0;
        std::optional<std::uint64_t> group_count;
        bool stable = false;
    };
    std::map<SiteKey, Site> sites;
    std::uint64_t evaluable = 0;
    std::uint64_t insufficient_alt = 0;
    std::uint64_t incomplete_distance = 0;
    std::uint64_t stable = 0;
};

M1Replay replay_m1_sites(const ParsedArtifact& artifact, const SiteReadReplay& site_reads) {
    M1Replay replay;
    static const std::array<const char*, 8> kAxes = {"hp_exact_axis", "hp_family_axis", "strand_axis",
                                                     "start_axis",    "end_axis",       "length_axis",
                                                     "mapq_axis",     "cpg_called_axis"};
    for (const auto& row : artifact.table_rows) {
        const SiteKey key{table_uint(artifact, row, "dataset_order", "M1_CONSERVATION"),
                          table_uint(artifact, row, "site_order", "M1_CONSERVATION")};
        M1Replay::Site site;
        site.dataset_id = table_field(artifact, row, "dataset_id", "M1_CONSERVATION");
        site.status = table_field(artifact, row, "analysis_status", "M1_CONSERVATION");
        site.reason = table_field(artifact, row, "reason", "M1_CONSERVATION");
        site.joined = table_uint(artifact, row, "n_alt_joined", "M1_CONSERVATION");
        site.after_peel = table_uint(artifact, row, "n_alt_after_peel", "M1_CONSERVATION");
        site.stable = table_field(artifact, row, "stable", "M1_CONSERVATION") == "true";
        const std::string& group_count = table_field(artifact, row, "non_germline_groups", "M1_CONSERVATION");
        if (group_count != ".") {
            site.group_count = table_uint(artifact, row, "non_germline_groups", "M1_CONSERVATION");
        }
        std::uint64_t joined_from_site_reads = 0U;
        const auto serialized_calls = site_reads.calls.find(key);
        if (serialized_calls != site_reads.calls.end()) {
            joined_from_site_reads = static_cast<std::uint64_t>(
                std::count_if(serialized_calls->second.begin(), serialized_calls->second.end(),
                              [](const auto& read_call) { return read_call.second == 'A'; }));
        }
        if (site.joined != joined_from_site_reads || site.after_peel > site.joined) {
            reject("M1_CONSERVATION",
                   "M1 joined/peeled counts differ from focal-ALT "
                   "site-read replay");
        }
        const auto serialized_site = site_reads.sites.find(key);
        if (serialized_site != site_reads.sites.end()) {
            const auto& identity = serialized_site->second;
            if (site.dataset_id != identity.dataset_id ||
                table_field(artifact, row, "chrom", "M1_CONSERVATION") != identity.chrom ||
                table_uint(artifact, row, "pos1", "M1_CONSERVATION") != identity.pos1 ||
                table_field(artifact, row, "ref", "M1_CONSERVATION") != identity.ref ||
                table_field(artifact, row, "alt", "M1_CONSERVATION") != identity.alt) {
                reject("M1_CONSERVATION", "M1 site identity differs from site-read identity");
            }
        }
        if (site.joined < 6U) {
            if (site.status != "INSUFFICIENT_ALT_READS" ||
                (site.reason != "INSUFFICIENT_FOCAL_ALT_READS" &&
                 site.reason != "INSUFFICIENT_MATRIX_JOINED_FOCAL_ALT_READS")) {
                reject("M1_CONSERVATION", "M1 insufficient-ALT threshold/status precedence differs");
            }
            ++replay.insufficient_alt;
        } else if (site.after_peel < 6U) {
            if (site.status != "INCOMPLETE_DISTANCE_BELOW_MINIMUM" ||
                site.reason != "INCOMPLETE_DISTANCE_BELOW_MINIMUM") {
                reject("M1_CONSERVATION", "M1 complete-distance threshold/status precedence differs");
            }
            ++replay.incomplete_distance;
        } else {
            if (site.status != "EVALUABLE") {
                reject("M1_CONSERVATION", "M1 evaluable threshold/status precedence differs");
            }
            ++replay.evaluable;
        }
        if (site.stable) {
            if (site.status != "EVALUABLE" || !site.group_count.has_value() || *site.group_count < 2U) {
                reject("M1_CONSERVATION", "stable M1 site lacks an evaluable multi-group assignment");
            }
            ++replay.stable;
        } else {
            for (const char* axis : kAxes) {
                if (table_field(artifact, row, axis, "M1_CONSERVATION") != "NOT_RUN_UNSTABLE") {
                    reject("M1_CONSERVATION", "unstable M1 site contains a measured axis result");
                }
            }
        }
        if (!replay.sites.emplace(key, std::move(site)).second) {
            reject("M1_CONSERVATION", "duplicate M1 site key");
        }
    }
    return replay;
}

struct AssignmentReplay {
    std::map<SiteKey, std::map<std::string, std::string>> labels;
};

std::vector<std::string> json_string_vector(const json_t* value, const std::string& check_id) {
    if (!json_is_array(value)) {
        reject(check_id, "expected JSON string array");
    }
    std::vector<std::string> result;
    result.reserve(json_array_size(value));
    for (std::size_t index = 0; index < json_array_size(value); ++index) {
        const json_t* element = json_array_get(value, index);
        if (!json_is_string(element)) {
            reject(check_id, "JSON array contains a non-string element");
        }
        result.emplace_back(json_string_value(element));
    }
    return result;
}

AssignmentReplay replay_m1_assignments(const ParsedArtifact& artifact, const M1Replay& m1,
                                       const SiteReadReplay& site_reads, const MethylReplay& methyl) {
    if (methyl.reads == nullptr) {
        reject("M1_ASSIGNMENT_CONSERVATION", "methyl read-presence replay is absent");
    }
    const auto& methyl_reads = *methyl.reads;
    AssignmentReplay replay;
    for (const JsonPtr& record : artifact.json_records) {
        const SiteKey key{uint_field(record.get(), "dataset_order", "M1_ASSIGNMENT_CONSERVATION"),
                          uint_field(record.get(), "site_order", "M1_ASSIGNMENT_CONSERVATION")};
        const auto site = m1.sites.find(key);
        if (site == m1.sites.end() || site->second.status != "EVALUABLE") {
            reject("M1_ASSIGNMENT_CONSERVATION", "assignment row does not reference an evaluable M1 site");
        }
        const std::vector<std::string> read_ids =
            json_string_vector(json_object_get(record.get(), "read_ids"), "M1_ASSIGNMENT_CONSERVATION");
        const std::vector<std::string> labels =
            json_string_vector(json_object_get(record.get(), "labels"), "M1_ASSIGNMENT_CONSERVATION");
        if (read_ids.size() != labels.size() || read_ids.size() != site->second.after_peel) {
            reject("M1_ASSIGNMENT_CONSERVATION", "assignment read/label cardinality differs from M1 peeled count");
        }
        std::map<std::string, std::string> by_read;
        for (std::size_t index = 0; index < read_ids.size(); ++index) {
            const auto calls = site_reads.calls.find(key);
            if (labels[index].empty() || calls == site_reads.calls.end() ||
                calls->second.count(read_ids[index]) == 0U || calls->second.at(read_ids[index]) != 'A' ||
                methyl_reads.count(key) == 0U || methyl_reads.at(key).count(read_ids[index]) == 0U ||
                !by_read.emplace(read_ids[index], labels[index]).second) {
                reject("M1_ASSIGNMENT_CONSERVATION", "assignment contains an invalid, duplicate or non-ALT read");
            }
        }
        const json_t* coarse = json_object_get(record.get(), "coarse_runs");
        if (!json_is_array(coarse) || json_array_size(coarse) != 10U) {
            reject("M1_ASSIGNMENT_CONSERVATION", "coarse assignment replay requires exactly ten seeds");
        }
        for (std::size_t seed = 0; seed < 10U; ++seed) {
            const json_t* run = json_array_get(coarse, seed);
            if (uint_field(run, "seed_order", "M1_ASSIGNMENT_CONSERVATION") != seed ||
                json_array_size(json_object_get(run, "labels")) != read_ids.size()) {
                reject("M1_ASSIGNMENT_CONSERVATION", "coarse seed order or label cardinality differs");
            }
        }
        const json_t* fine = json_object_get(record.get(), "fine_run");
        if (!json_is_object(fine) || json_array_size(json_object_get(fine, "labels")) != read_ids.size()) {
            reject("M1_ASSIGNMENT_CONSERVATION", "fine assignment label cardinality differs");
        }
        if (!replay.labels.emplace(key, std::move(by_read)).second) {
            reject("M1_ASSIGNMENT_CONSERVATION", "duplicate M1 assignment key");
        }
    }
    for (const auto& [key, site] : m1.sites) {
        const bool has_assignment = replay.labels.count(key) != 0U;
        if ((site.status == "EVALUABLE") != has_assignment) {
            reject("M1_ASSIGNMENT_CONSERVATION", "evaluable M1 site and assignment membership differ");
        }
    }
    return replay;
}

void validate_llm_conservation(const ParsedArtifact& llm, const M1Replay& m1) {
    std::set<SiteKey> observed;
    for (const auto& frame : llm.llm_frames) {
        const SiteKey key{frame.dataset_order, frame.site_order};
        const auto site = m1.sites.find(key);
        if (site == m1.sites.end() || site->second.joined < 6U || frame.dimension != site->second.joined ||
            !observed.insert(key).second) {
            reject("LLM_M1_CONSERVATION", "LLM frame membership/dimension differs from M1 joined reads");
        }
    }
    for (const auto& [key, site] : m1.sites) {
        if ((site.joined >= 6U) != (observed.count(key) != 0U)) {
            reject("LLM_M1_CONSERVATION", "M1 joined threshold and LLM frame membership differ");
        }
    }
}

std::int64_t table_int64(const ParsedArtifact& parsed, const std::vector<std::string>& row, const std::string& name,
                         const std::string& check_id) {
    const std::string& value = table_field(parsed, row, name, check_id);
    std::int64_t parsed_value = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
        reject(check_id, name + ": cannot replay signed integer");
    }
    return parsed_value;
}

struct PairReplay {
    std::vector<std::optional<double>> p_values;
    std::map<SiteKey, std::uint64_t> partner_counts;
    std::map<SiteKey, std::uint64_t> exact_counts;
    std::map<SiteKey, std::uint64_t> bh_discoveries;
    std::map<SiteKey, std::uint64_t> by_discoveries;
    std::map<SiteKey, std::map<std::uint64_t, std::uint64_t>> partner_positions;
};

std::vector<std::array<std::uint64_t, 2>> embedded_kx2(const std::string& encoded, const std::string& check_id) {
    JsonPtr value = parse_embedded_json(encoded, check_id);
    if (!json_is_array(value.get())) {
        reject(check_id, "group-allele JSON is not an array");
    }
    std::vector<std::array<std::uint64_t, 2>> table;
    table.reserve(json_array_size(value.get()));
    for (std::size_t index = 0; index < json_array_size(value.get()); ++index) {
        const json_t* row = json_array_get(value.get(), index);
        if (!json_is_array(row) || json_array_size(row) != 2U) {
            reject(check_id, "group-allele JSON row is not Kx2");
        }
        std::array<std::uint64_t, 2> parsed{};
        for (std::size_t column = 0; column < 2U; ++column) {
            const json_t* cell = json_array_get(row, column);
            if (!json_is_integer(cell) || json_integer_value(cell) < 0) {
                reject(check_id, "group-allele JSON cell is invalid");
            }
            parsed[column] = static_cast<std::uint64_t>(json_integer_value(cell));
        }
        table.push_back(parsed);
    }
    return table;
}

double cramers_v_kx2(const std::vector<std::array<std::uint64_t, 2>>& table) {
    std::array<std::uint64_t, 2> columns{};
    std::uint64_t total = 0;
    for (const auto& row : table) {
        for (std::size_t column = 0; column < 2U; ++column) {
            if (columns[column] > std::numeric_limits<std::uint64_t>::max() - row[column] ||
                total > std::numeric_limits<std::uint64_t>::max() - row[column]) {
                reject("COOCCURRENCE_CONSERVATION", "group-allele count overflows");
            }
            columns[column] += row[column];
            total += row[column];
        }
    }
    if (total == 0U) {
        return 0.0;
    }
    double chi_square = 0.0;
    for (const auto& row : table) {
        const std::uint64_t row_total = row[0] + row[1];
        for (std::size_t column = 0; column < 2U; ++column) {
            const double expected =
                static_cast<double>(row_total) * static_cast<double>(columns[column]) / static_cast<double>(total);
            if (expected > 0.0) {
                const double delta = static_cast<double>(row[column]) - expected;
                chi_square += delta * delta / expected;
            }
        }
    }
    return std::sqrt(chi_square / static_cast<double>(total));
}

double delta_alt_fraction(const std::vector<std::array<std::uint64_t, 2>>& table) {
    std::vector<double> fractions;
    for (const auto& row : table) {
        const std::uint64_t total = row[0] + row[1];
        if (total != 0U) {
            fractions.push_back(static_cast<double>(row[1]) / static_cast<double>(total));
        }
    }
    if (fractions.empty()) {
        return 0.0;
    }
    const auto bounds = std::minmax_element(fractions.begin(), fractions.end());
    return *bounds.second - *bounds.first;
}

PairReplay replay_cooccurrence_pairs(const ParsedArtifact& artifact, const SiteReadReplay& site_reads,
                                     const AssignmentReplay& assignments, const ParsedArtifact& cooccurrence_sites) {
    static const std::array<const char*, 16> kCellNames = {"rr", "ra", "ro", "rx", "ar", "aa", "ao", "ax",
                                                           "or", "oa", "oo", "ox", "xr", "xa", "xo", "xx"};
    const auto allele_index = [](char allele) -> std::size_t {
        switch (allele) {
            case 'R':
                return 0U;
            case 'A':
                return 1U;
            case 'O':
                return 2U;
            case 'X':
                return 3U;
        }
        reject("COOCCURRENCE_CONSERVATION", "unknown allele call");
    };

    PairReplay replay;
    std::map<SiteKey, bool> m2_family_eligible;
    for (const auto& row : cooccurrence_sites.table_rows) {
        const SiteKey key{table_uint(cooccurrence_sites, row, "dataset_order", "PAIR_FAMILY_REPLAY"),
                          table_uint(cooccurrence_sites, row, "site_order", "PAIR_FAMILY_REPLAY")};
        const bool eligible = table_field(cooccurrence_sites, row, "m2_status", "PAIR_FAMILY_REPLAY") == "PASS";
        if (!m2_family_eligible.emplace(key, eligible).second) {
            reject("PAIR_FAMILY_REPLAY", "duplicate focal M2 status while reconstructing pair family");
        }
    }
    replay.p_values.resize(artifact.table_rows.size());
    for (std::size_t row_index = 0; row_index < artifact.table_rows.size(); ++row_index) {
        const auto& row = artifact.table_rows[row_index];
        const std::uint64_t dataset = table_uint(artifact, row, "dataset_order", "COOCCURRENCE_CONSERVATION");
        const SiteKey focal{dataset, table_uint(artifact, row, "focal_site_order", "COOCCURRENCE_CONSERVATION")};
        const SiteKey partner{dataset, table_uint(artifact, row, "partner_site_order", "COOCCURRENCE_CONSERVATION")};
        const auto focal_identity = site_reads.sites.find(focal);
        const auto partner_identity = site_reads.sites.find(partner);
        const auto focal_calls = site_reads.calls.find(focal);
        const auto partner_calls = site_reads.calls.find(partner);
        const auto assignment = assignments.labels.find(focal);
        if (focal_identity == site_reads.sites.end() || partner_identity == site_reads.sites.end() ||
            focal_calls == site_reads.calls.end() || partner_calls == site_reads.calls.end() ||
            assignment == assignments.labels.end()) {
            reject("COOCCURRENCE_CONSERVATION", "pair references an absent site/read/assignment key");
        }
        const auto family_gate = m2_family_eligible.find(focal);
        if (family_gate == m2_family_eligible.end()) {
            reject("PAIR_FAMILY_REPLAY", "pair focal site lacks a serialized M2 family gate");
        }
        if (focal_identity->second.chrom != partner_identity->second.chrom ||
            table_field(artifact, row, "dataset_id", "COOCCURRENCE_CONSERVATION") !=
                focal_identity->second.dataset_id ||
            table_field(artifact, row, "chrom", "COOCCURRENCE_CONSERVATION") != focal_identity->second.chrom ||
            table_uint(artifact, row, "focal_pos1", "COOCCURRENCE_CONSERVATION") != focal_identity->second.pos1 ||
            table_uint(artifact, row, "partner_pos1", "COOCCURRENCE_CONSERVATION") != partner_identity->second.pos1) {
            reject("COOCCURRENCE_CONSERVATION", "pair locus identity differs from serialized site rows");
        }
        if (focal_identity->second.pos1 > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            partner_identity->second.pos1 > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            reject("COOCCURRENCE_CONSERVATION", "pair position exceeds signed distance domain");
        }
        const std::int64_t focal_position = static_cast<std::int64_t>(focal_identity->second.pos1);
        const std::int64_t partner_position = static_cast<std::int64_t>(partner_identity->second.pos1);
        const std::int64_t signed_distance = partner_position - focal_position;
        const std::uint64_t absolute_distance = signed_distance < 0
                                                    ? static_cast<std::uint64_t>(-(signed_distance + 1)) + 1U
                                                    : static_cast<std::uint64_t>(signed_distance);
        if (table_int64(artifact, row, "distance_bp", "COOCCURRENCE_CONSERVATION") != signed_distance ||
            absolute_distance > 5000U) {
            reject("COOCCURRENCE_CONSERVATION", "pair distance differs from the inclusive 5 kb contract");
        }

        std::array<std::uint64_t, 16> cells{};
        std::map<std::string, std::array<std::uint64_t, 2>> groups;
        for (const auto& [read_id, focal_call] : focal_calls->second) {
            const auto partner_call = partner_calls->second.find(read_id);
            if (partner_call == partner_calls->second.end()) {
                continue;
            }
            const std::size_t cell = allele_index(focal_call) * 4U + allele_index(partner_call->second);
            ++cells[cell];
            const auto label = assignment->second.find(read_id);
            if (label != assignment->second.end() && label->second != "other" && label->second != "outlier" &&
                (partner_call->second == 'R' || partner_call->second == 'A')) {
                ++groups[label->second][partner_call->second == 'R' ? 0U : 1U];
            }
        }
        std::uint64_t pair_count = 0;
        for (std::size_t index = 0; index < cells.size(); ++index) {
            pair_count += cells[index];
            if (table_uint(artifact, row, kCellNames[index], "COOCCURRENCE_CONSERVATION") != cells[index]) {
                reject("COOCCURRENCE_CONSERVATION", "serialized R/A/O/X cell differs from read-level join");
            }
        }
        const std::uint64_t ra_count = cells[0] + cells[1] + cells[4] + cells[5];
        if (table_uint(artifact, row, "pair_read_count", "COOCCURRENCE_CONSERVATION") != pair_count ||
            table_uint(artifact, row, "ra_complete_read_count", "COOCCURRENCE_CONSERVATION") != ra_count) {
            reject("COOCCURRENCE_CONSERVATION", "pair or R/A-complete read count does not conserve 16 cells");
        }

        std::vector<std::array<std::uint64_t, 2>> expected_table;
        for (const auto& [label, counts] : groups) {
            static_cast<void>(label);
            expected_table.push_back(counts);
        }
        const std::vector<std::array<std::uint64_t, 2>> observed_table =
            embedded_kx2(table_field(artifact, row, "group_allele_counts_json", "COOCCURRENCE_CONSERVATION"),
                         "COOCCURRENCE_CONSERVATION");
        if (observed_table != expected_table ||
            table_uint(artifact, row, "group_count", "COOCCURRENCE_CONSERVATION") != expected_table.size()) {
            reject("COOCCURRENCE_CONSERVATION", "group-allele matrix differs from M1/read join");
        }

        const std::uint64_t n_informative = table_uint(artifact, row, "n_informative", "COOCCURRENCE_CONSERVATION");
        const std::uint64_t minimum_group = table_uint(artifact, row, "min_group_n", "COOCCURRENCE_CONSERVATION");
        const std::uint64_t ref_n = table_uint(artifact, row, "ref_n", "COOCCURRENCE_CONSERVATION");
        const std::uint64_t alt_n = table_uint(artifact, row, "alt_n", "COOCCURRENCE_CONSERVATION");
        const bool testable =
            observed_table.size() >= 2U && n_informative >= 10U && minimum_group >= 3U && ref_n >= 3U && alt_n >= 3U;
        IndependentExactResult exact;
        if (testable) {
            exact = independent_exact_kx2(observed_table);
        }
        const std::string& status = table_field(artifact, row, "exact_status", "COOCCURRENCE_CONSERVATION");
        const std::string& p_encoded = table_field(artifact, row, "p_exact", "COOCCURRENCE_CONSERVATION");
        const std::uint64_t serialized_states =
            table_uint(artifact, row, "exact_state_count", "COOCCURRENCE_CONSERVATION");
        if (!testable) {
            if (status != "NOT_IDENTIFIABLE_ENDPOINT_A_NOT_TESTABLE" || p_encoded != "." || serialized_states != 0U) {
                reject("EXACT_STATISTIC_REPLAY", "non-testable endpoint-A row carries an exact result");
            }
        } else if (exact.state == IndependentExactResult::State::kCeiling) {
            if (status != "NOT_IDENTIFIABLE_STATE_SPACE_LIMIT" || p_encoded != "." ||
                serialized_states != exact.state_count) {
                reject("EXACT_STATISTIC_REPLAY", "state-space ceiling status/count differs");
            }
        } else if (exact.state == IndependentExactResult::State::kEnumerated) {
            if (status != "EXACT_IDENTIFIABLE" || serialized_states != exact.state_count || p_encoded == "." ||
                !near_science(table_float(artifact, row, "p_exact", "EXACT_STATISTIC_REPLAY"), exact.p_value)) {
                reject("EXACT_STATISTIC_REPLAY", "independent fixed-margin exact result differs");
            }
            ++replay.exact_counts[focal];
            const double observed_v = table_float(artifact, row, "cramers_v", "COOCCURRENCE_CONSERVATION");
            const double observed_delta = table_float(artifact, row, "delta_af", "COOCCURRENCE_CONSERVATION");
            const double expected_v = cramers_v_kx2(observed_table);
            const double expected_delta = delta_alt_fraction(observed_table);
            if (!near_science(observed_v, expected_v) || !near_science(observed_delta, expected_delta) ||
                (table_field(artifact, row, "effect_gate_pass", "COOCCURRENCE_CONSERVATION") == "true") !=
                    (expected_v >= 0.3 && expected_delta >= 0.5)) {
                reject("COOCCURRENCE_CONSERVATION", "effect size or frozen effect gate differs");
            }
        } else {
            reject("EXACT_STATISTIC_REPLAY", "testable table independently classified degenerate");
        }
        std::string expected_family_status;
        if (!family_gate->second) {
            expected_family_status = "INELIGIBLE_M2_SCREEN";
        } else if (!testable) {
            expected_family_status = "ELIGIBLE_M2_ENDPOINT_A_NOT_TESTABLE";
        } else if (exact.state != IndependentExactResult::State::kEnumerated) {
            expected_family_status = "ELIGIBLE_M2_EXACT_NOT_IDENTIFIABLE";
        } else {
            expected_family_status = "ELIGIBLE_M2_EXACT_FAMILY";
            replay.p_values[row_index] = exact.p_value;
        }
        if (table_field(artifact, row, "family_status", "PAIR_FAMILY_REPLAY") != expected_family_status) {
            reject("PAIR_FAMILY_REPLAY", "serialized pair family differs from M2/exact replay");
        }
        ++replay.partner_counts[focal];
        replay.partner_positions[focal][partner.second] = partner_identity->second.pos1;
    }

    const auto bh = independent_fdr(replay.p_values, false);
    const auto by = independent_fdr(replay.p_values, true);
    const std::uint64_t family_size = static_cast<std::uint64_t>(std::count_if(
        replay.p_values.begin(), replay.p_values.end(), [](const auto& value) { return value.has_value(); }));
    std::optional<std::string> family_id;
    for (std::size_t index = 0; index < artifact.table_rows.size(); ++index) {
        const auto& row = artifact.table_rows[index];
        const SiteKey focal{table_uint(artifact, row, "dataset_order", "GLOBAL_FDR_REPLAY"),
                            table_uint(artifact, row, "focal_site_order", "GLOBAL_FDR_REPLAY")};
        const std::string& id = table_field(artifact, row, "fdr_family_id", "GLOBAL_FDR_REPLAY");
        const std::string& size = table_field(artifact, row, "fdr_family_size", "GLOBAL_FDR_REPLAY");
        const std::string& q_bh = table_field(artifact, row, "q_global_bh", "GLOBAL_FDR_REPLAY");
        const std::string& q_by = table_field(artifact, row, "q_global_by", "GLOBAL_FDR_REPLAY");
        if (!replay.p_values[index].has_value()) {
            if (id != "." || size != "." || q_bh != "." || q_by != "." ||
                table_field(artifact, row, "exact_bh_discovery", "GLOBAL_FDR_REPLAY") != "false" ||
                table_field(artifact, row, "exact_by_discovery", "GLOBAL_FDR_REPLAY") != "false") {
                reject("GLOBAL_FDR_REPLAY", "row outside the M2-screened exact family carries global FDR output");
            }
            if (table_uint(artifact, row, "conditional_valid_permutations", "GLOBAL_FDR_REPLAY") != 0U ||
                table_field(artifact, row, "conditional_exceedance", "GLOBAL_FDR_REPLAY") != "." ||
                table_field(artifact, row, "conditional_p", "GLOBAL_FDR_REPLAY") != "." ||
                table_field(artifact, row, "conditional_status", "GLOBAL_FDR_REPLAY") !=
                    "NOT_RUN_NOT_EXACT_BY_DISCOVERY" ||
                table_field(artifact, row, "formal_pair_by_confirmed", "GLOBAL_FDR_REPLAY") != "false") {
                reject("GLOBAL_FDR_REPLAY", "row outside the formal exact family carries conditional or formal output");
            }
            continue;
        }
        if (id == "." || table_uint(artifact, row, "fdr_family_size", "GLOBAL_FDR_REPLAY") != family_size ||
            !bh[index].has_value() || !by[index].has_value() ||
            !near_science(table_float(artifact, row, "q_global_bh", "GLOBAL_FDR_REPLAY"), *bh[index]) ||
            !near_science(table_float(artifact, row, "q_global_by", "GLOBAL_FDR_REPLAY"), *by[index])) {
            reject("GLOBAL_FDR_REPLAY", "global BH/BY family, size or adjusted value differs");
        }
        if (!family_id.has_value()) {
            family_id = id;
        } else if (*family_id != id) {
            reject("GLOBAL_FDR_REPLAY", "global exact rows use different FDR family IDs");
        }
        const bool effect = table_field(artifact, row, "effect_gate_pass", "GLOBAL_FDR_REPLAY") == "true";
        const bool bh_discovery = *bh[index] <= 0.05 && effect;
        const bool by_discovery = *by[index] <= 0.05 && effect;
        if ((table_field(artifact, row, "exact_bh_discovery", "GLOBAL_FDR_REPLAY") == "true") != bh_discovery ||
            (table_field(artifact, row, "exact_by_discovery", "GLOBAL_FDR_REPLAY") == "true") != by_discovery) {
            reject("GLOBAL_FDR_REPLAY", "exact discovery decision differs from q/effect gate");
        }
        replay.bh_discoveries[focal] += bh_discovery ? 1U : 0U;
        replay.by_discoveries[focal] += by_discovery ? 1U : 0U;
        const std::string& conditional_status = table_field(artifact, row, "conditional_status", "GLOBAL_FDR_REPLAY");
        const std::uint64_t conditional_permutations =
            table_uint(artifact, row, "conditional_valid_permutations", "GLOBAL_FDR_REPLAY");
        const std::string& conditional_exceedance =
            table_field(artifact, row, "conditional_exceedance", "GLOBAL_FDR_REPLAY");
        const std::string& conditional_p = table_field(artifact, row, "conditional_p", "GLOBAL_FDR_REPLAY");
        if (!by_discovery) {
            if (conditional_status != "NOT_RUN_NOT_EXACT_BY_DISCOVERY" || conditional_permutations != 0U ||
                conditional_exceedance != "." || conditional_p != ".") {
                reject("GLOBAL_FDR_REPLAY", "non-BY-discovery row carries conditional permutation output");
            }
        } else if (conditional_status == "PERMUTABLE") {
            if (conditional_permutations != 999U || conditional_exceedance == "." || conditional_p == ".") {
                reject("GLOBAL_FDR_REPLAY", "permutable conditional row lacks the frozen 999-replicate result");
            }
            const std::uint64_t exceedance = table_uint(artifact, row, "conditional_exceedance", "GLOBAL_FDR_REPLAY");
            if (exceedance > 999U) {
                reject("GLOBAL_FDR_REPLAY", "conditional exceedance exceeds the frozen replicate count");
            }
            const double expected_conditional = static_cast<double>(exceedance + 1U) / 1000.0;
            if (!near_science(table_float(artifact, row, "conditional_p", "GLOBAL_FDR_REPLAY"), expected_conditional)) {
                reject("GLOBAL_FDR_REPLAY", "conditional exceedance and plus-one p-value differ");
            }
        } else if (conditional_status == "NOT_IDENTIFIABLE_DEGENERATE_TABLE" ||
                   conditional_status == "NOT_IDENTIFIABLE_NONEXCHANGEABLE") {
            if (conditional_permutations != 0U || conditional_exceedance != "." || conditional_p != ".") {
                reject("GLOBAL_FDR_REPLAY", "non-identifiable conditional row carries permutation output");
            }
        } else {
            reject("GLOBAL_FDR_REPLAY", "BY discovery has an unsupported conditional status");
        }
        const bool conditional_confirmed = conditional_status == "PERMUTABLE" && conditional_p != "." &&
                                           table_float(artifact, row, "conditional_p", "GLOBAL_FDR_REPLAY") <= 0.05;
        const std::string& callability_status = table_field(artifact, row, "callability_status", "GLOBAL_FDR_REPLAY");
        const bool callability_pass = callability_status == "PASS_ALL_CORE_READS_CALLABLE" ||
                                      callability_status == "PASS_NO_STRONG_DIFFERENTIAL_CALLABILITY_DETECTED";
        const bool formal = by_discovery && conditional_confirmed && callability_pass;
        if ((table_field(artifact, row, "formal_pair_by_confirmed", "GLOBAL_FDR_REPLAY") == "true") != formal) {
            reject("GLOBAL_FDR_REPLAY", "formal BY+conditional pair decision differs");
        }
    }
    return replay;
}

struct CooccurrenceSiteReplay {
    std::uint64_t m2_eligible = 0;
    std::uint64_t m2_evaluable_ineligible = 0;
    std::uint64_t m2_axis_indeterminate = 0;
    std::uint64_t m2_group_count_gt10 = 0;
};

CooccurrenceSiteReplay replay_cooccurrence_sites(const ParsedArtifact& artifact, const M1Replay& m1,
                                                 const SiteReadReplay& site_reads, const PairReplay& pairs) {
    CooccurrenceSiteReplay replay;
    std::set<SiteKey> observed;
    for (const auto& row : artifact.table_rows) {
        const SiteKey key{table_uint(artifact, row, "dataset_order", "COOCCURRENCE_SITE_CONSERVATION"),
                          table_uint(artifact, row, "site_order", "COOCCURRENCE_SITE_CONSERVATION")};
        const auto m1_site = m1.sites.find(key);
        if (m1_site == m1.sites.end() || !observed.insert(key).second) {
            reject("COOCCURRENCE_SITE_CONSERVATION", "co-occurrence site is absent from or duplicated against M1");
        }
        const auto serialized_site = site_reads.sites.find(key);
        if (serialized_site != site_reads.sites.end()) {
            const auto& identity = serialized_site->second;
            if (table_field(artifact, row, "dataset_id", "COOCCURRENCE_SITE_CONSERVATION") != identity.dataset_id ||
                table_field(artifact, row, "chrom", "COOCCURRENCE_SITE_CONSERVATION") != identity.chrom ||
                table_uint(artifact, row, "pos1", "COOCCURRENCE_SITE_CONSERVATION") != identity.pos1) {
                reject("COOCCURRENCE_SITE_CONSERVATION", "co-occurrence site identity differs from site-read rows");
            }
        }
        const std::string& serialized_groups =
            table_field(artifact, row, "m1_group_count", "COOCCURRENCE_SITE_CONSERVATION");
        if (m1_site->second.group_count.has_value()) {
            if (serialized_groups == "." ||
                table_uint(artifact, row, "m1_group_count", "COOCCURRENCE_SITE_CONSERVATION") !=
                    *m1_site->second.group_count) {
                reject("COOCCURRENCE_SITE_CONSERVATION", "M1 group count differs between site artifacts");
            }
        } else if (serialized_groups != ".") {
            reject("COOCCURRENCE_SITE_CONSERVATION", "co-occurrence site invents an absent M1 group count");
        }
        const std::uint64_t expected_partners =
            pairs.partner_counts.count(key) == 0U ? 0U : pairs.partner_counts.at(key);
        const std::uint64_t expected_exact = pairs.exact_counts.count(key) == 0U ? 0U : pairs.exact_counts.at(key);
        const std::uint64_t expected_bh = pairs.bh_discoveries.count(key) == 0U ? 0U : pairs.bh_discoveries.at(key);
        const std::uint64_t expected_by = pairs.by_discoveries.count(key) == 0U ? 0U : pairs.by_discoveries.at(key);
        if (table_uint(artifact, row, "partner_universe_size", "COOCCURRENCE_SITE_CONSERVATION") != expected_partners ||
            table_uint(artifact, row, "exact_testable_pairs", "COOCCURRENCE_SITE_CONSERVATION") != expected_exact ||
            table_uint(artifact, row, "global_bh_discoveries", "COOCCURRENCE_SITE_CONSERVATION") != expected_bh ||
            table_uint(artifact, row, "global_by_discoveries", "COOCCURRENCE_SITE_CONSERVATION") != expected_by) {
            reject("COOCCURRENCE_SITE_CONSERVATION", "site-level pair/FDR aggregates differ from pair rows");
        }

        const std::string& m2_status = table_field(artifact, row, "m2_status", "COOCCURRENCE_SITE_CONSERVATION");
        const std::string& m2_reason = table_field(artifact, row, "m2_reason", "COOCCURRENCE_SITE_CONSERVATION");
        const std::uint64_t serialized_rank = table_uint(artifact, row, "m2_precedence_rank", "M2_PRECEDENCE_REPLAY");
        static const std::map<std::string, std::uint64_t> kReasonRank = {
            {"M1_NOT_FLAGGED", 0U},
            {"GROUP_COUNT_EXCEEDS_PLANNING_MODEL_MAXIMUM", 1U},
            {"AXIS_INDETERMINATE", 2U},
            {"HP_AXIS_CONFOUND", 3U},
            {"TECHNICAL_AXIS_CONFOUND", 4U},
            {"NOT_PHASE_ANCHORED_ROBUST", 5U},
            {"ALL_MEASURED_AXES_DETERMINATE_NO_ALIGNED_CONFOUND", 6U}};
        const auto expected_rank = kReasonRank.find(m2_reason);
        if (expected_rank == kReasonRank.end() || serialized_rank != expected_rank->second) {
            reject("M2_PRECEDENCE_REPLAY", "M2 reason and frozen precedence rank differ");
        }
        if (!m1_site->second.stable) {
            if (m2_status != "NOT_RUN" || m2_reason != "M1_NOT_FLAGGED") {
                reject("M2_PRECEDENCE_REPLAY", "M1-unstable site must stop M2 at precedence rank zero");
            }
        } else if (m1_site->second.group_count.has_value() && *m1_site->second.group_count > 10U) {
            if (m2_status != "NOT_EVALUABLE" || m2_reason != "GROUP_COUNT_EXCEEDS_PLANNING_MODEL_MAXIMUM") {
                reject("M2_PRECEDENCE_REPLAY", "M2 group-count ceiling precedence differs");
            }
            ++replay.m2_group_count_gt10;
        } else if (m2_status == "NOT_EVALUABLE" && m2_reason == "AXIS_INDETERMINATE") {
            ++replay.m2_axis_indeterminate;
        } else if (m2_status == "PASS" && m2_reason == "ALL_MEASURED_AXES_DETERMINATE_NO_ALIGNED_CONFOUND") {
            ++replay.m2_eligible;
        } else if (m2_status == "FAIL" && (m2_reason == "HP_AXIS_CONFOUND" || m2_reason == "TECHNICAL_AXIS_CONFOUND" ||
                                           m2_reason == "NOT_PHASE_ANCHORED_ROBUST")) {
            ++replay.m2_evaluable_ineligible;
        } else {
            reject("M2_PRECEDENCE_REPLAY", "stable M1 site has an unsupported M2 terminal decision");
        }

        const std::string& joint_status =
            table_field(artifact, row, "joint_signature_status", "JOINT_SIGNATURE_REPLAY");
        const std::string& encoded = table_field(artifact, row, "joint_partner_orders_json", "JOINT_SIGNATURE_REPLAY");
        if (joint_status == "PASS") {
            JsonPtr orders = parse_embedded_json(encoded, "JOINT_SIGNATURE_REPLAY");
            if (!json_is_array(orders.get()) || json_array_size(orders.get()) == 0U ||
                json_array_size(orders.get()) > 3U) {
                reject("JOINT_SIGNATURE_REPLAY", "passing joint signature must contain one to three partners");
            }
            std::vector<std::uint64_t> positions;
            const auto site_partners = pairs.partner_positions.find(key);
            for (std::size_t index = 0; index < json_array_size(orders.get()); ++index) {
                const json_t* order = json_array_get(orders.get(), index);
                if (!json_is_integer(order) || json_integer_value(order) < 0 ||
                    site_partners == pairs.partner_positions.end()) {
                    reject("JOINT_SIGNATURE_REPLAY", "joint partner order is malformed or absent");
                }
                const std::uint64_t partner_order = static_cast<std::uint64_t>(json_integer_value(order));
                const auto position = site_partners->second.find(partner_order);
                if (position == site_partners->second.end()) {
                    reject("JOINT_SIGNATURE_REPLAY", "joint partner order is not in focal pair universe");
                }
                positions.push_back(position->second);
            }
            std::sort(positions.begin(), positions.end());
            for (std::size_t index = 1U; index < positions.size(); ++index) {
                if (positions[index] - positions[index - 1U] < 20U) {
                    reject("JOINT_SIGNATURE_REPLAY", "joint partner spacing is below 20 bp");
                }
            }
        } else if (joint_status != "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE") {
            reject("JOINT_SIGNATURE_REPLAY", "joint signature status is outside its closed contract");
        }
    }
    if (observed.size() != m1.sites.size()) {
        reject("COOCCURRENCE_SITE_CONSERVATION", "M1 and co-occurrence site key sets differ");
    }
    std::uint64_t remaining = m1.stable;
    for (const std::uint64_t value : {replay.m2_eligible, replay.m2_evaluable_ineligible, replay.m2_axis_indeterminate,
                                      replay.m2_group_count_gt10}) {
        if (value > remaining) {
            reject("M2_PRECEDENCE_REPLAY", "M2 status partition exceeds stable M1 site census");
        }
        remaining -= value;
    }
    if (remaining != 0U) {
        reject("M2_PRECEDENCE_REPLAY", "M2 status partition does not conserve stable M1 sites");
    }
    return replay;
}

std::string decimal_add(std::string left, std::string right) {
    std::string result;
    std::size_t left_index = left.size();
    std::size_t right_index = right.size();
    unsigned int carry = 0U;
    while (left_index != 0U || right_index != 0U || carry != 0U) {
        const unsigned int lhs = left_index == 0U ? 0U : static_cast<unsigned int>(left[--left_index] - '0');
        const unsigned int rhs = right_index == 0U ? 0U : static_cast<unsigned int>(right[--right_index] - '0');
        const unsigned int sum = lhs + rhs + carry;
        result.push_back(static_cast<char>('0' + (sum % 10U)));
        carry = sum / 10U;
    }
    std::reverse(result.begin(), result.end());
    const std::size_t nonzero = result.find_first_not_of('0');
    return nonzero == std::string::npos ? "0" : result.substr(nonzero);
}

std::string decimal_multiply_small(std::string value, std::uint64_t factor) {
    if (factor == 0U || value == "0") {
        return "0";
    }
    const std::string multiplier = std::to_string(factor);
    std::vector<std::uint64_t> digits(value.size() + multiplier.size(), 0U);
    for (std::size_t left_index = value.size(); left_index-- > 0U;) {
        for (std::size_t right_index = multiplier.size(); right_index-- > 0U;) {
            digits[left_index + right_index + 1U] += static_cast<std::uint64_t>(value[left_index] - '0') *
                                                     static_cast<std::uint64_t>(multiplier[right_index] - '0');
        }
    }
    for (std::size_t index = digits.size(); index-- > 1U;) {
        digits[index - 1U] += digits[index] / 10U;
        digits[index] %= 10U;
    }
    std::string result;
    result.reserve(digits.size());
    bool started = false;
    for (const std::uint64_t digit : digits) {
        if (digit != 0U || started) {
            started = true;
            result.push_back(static_cast<char>('0' + static_cast<unsigned int>(digit)));
        }
    }
    if (!started) {
        return "0";
    }
    return result;
}

bool sorted_unique_strings(const json_t* values) {
    if (!json_is_array(values)) {
        return false;
    }
    std::string previous;
    for (std::size_t index = 0; index < json_array_size(values); ++index) {
        const json_t* value = json_array_get(values, index);
        if (!json_is_string(value)) {
            return false;
        }
        const std::string current = json_string_value(value);
        if (index != 0U && !(previous < current)) {
            return false;
        }
        previous = current;
    }
    return true;
}

std::uint64_t binary_state_value(const std::string& state, std::size_t dimensions) {
    if (dimensions == 0U || dimensions > 63U || state.size() != dimensions) {
        reject("TOPOLOGY_CONSERVATION", "topology binary state exceeds the independent replay domain");
    }
    std::uint64_t value = 0U;
    for (const char bit : state) {
        if (bit != '0' && bit != '1') {
            reject("TOPOLOGY_CONSERVATION", "topology state is not a binary string");
        }
        value = (value << 1U) | (bit == '1' ? 1U : 0U);
    }
    return value;
}

void validate_topology_conservation(const ParsedArtifact& artifact) {
    for (const JsonPtr& record : artifact.json_records) {
        const json_t* active_bits = json_object_get(record.get(), "active_bits");
        const std::size_t dimensions = json_array_size(active_bits);
        std::uint64_t previous_bit = 0U;
        for (std::size_t index = 0; index < dimensions; ++index) {
            const json_t* bit = json_array_get(active_bits, index);
            if (!json_is_integer(bit) || json_integer_value(bit) < 0 ||
                (index != 0U && previous_bit >= static_cast<std::uint64_t>(json_integer_value(bit)))) {
                reject("TOPOLOGY_CONSERVATION", "active_bits must be strictly increasing");
            }
            previous_bit = static_cast<std::uint64_t>(json_integer_value(bit));
        }

        const json_t* patterns = json_object_get(record.get(), "input_patterns");
        std::set<std::string> expected_observed;
        std::pair<std::string, std::string> previous_pattern;
        for (std::size_t index = 0; index < json_array_size(patterns); ++index) {
            const json_t* pattern = json_array_get(patterns, index);
            const std::string kind = string_field(pattern, "kind");
            const std::string bits = string_field(pattern, "pattern");
            const std::pair<std::string, std::string> key{kind, bits};
            if (bits.size() != dimensions || (index != 0U && !(previous_pattern < key))) {
                reject("TOPOLOGY_CONSERVATION", "input patterns have wrong dimension, order or duplicate");
            }
            previous_pattern = key;
            if (kind == "FULL_STATE") {
                expected_observed.insert(bits);
            }
        }
        const std::vector<std::string> observed =
            json_string_vector(json_object_get(record.get(), "observed_states"), "TOPOLOGY_CONSERVATION");
        if (!std::is_sorted(observed.begin(), observed.end()) ||
            std::set<std::string>(observed.begin(), observed.end()) != expected_observed) {
            reject("TOPOLOGY_CONSERVATION", "observed_states differs from FULL_STATE input patterns");
        }

        const std::string objective_state = string_field(record.get(), "objective_state");
        const json_t* objective_h = json_object_get(record.get(), "objective_h");
        const json_t* bounds = json_object_get(record.get(), "objective_bounds");
        const json_t* objective_evidence = json_object_get(record.get(), "objective_evidence_sha256");
        const std::string objective_reason = string_field(record.get(), "objective_reason");
        if (objective_state == "OBJECTIVE_CERTIFIED") {
            if (!json_is_integer(objective_h) || json_integer_value(objective_h) < 0 || !json_is_object(bounds) ||
                uint_field(bounds, "lower_bound", "TOPOLOGY_CONSERVATION") !=
                    static_cast<std::uint64_t>(json_integer_value(objective_h)) ||
                uint_field(bounds, "upper_bound", "TOPOLOGY_CONSERVATION") !=
                    static_cast<std::uint64_t>(json_integer_value(objective_h)) ||
                uint_field(bounds, "gap", "TOPOLOGY_CONSERVATION") != 0U || !json_is_string(objective_evidence) ||
                objective_reason != "NONE") {
                reject("TOPOLOGY_CONSERVATION", "certified objective lacks exact zero-gap evidence");
            }
        } else if (!json_is_null(objective_h) || !json_is_null(bounds) || !json_is_null(objective_evidence) ||
                   objective_reason == "NONE") {
            reject("TOPOLOGY_CONSERVATION", "abstaining objective carries certified fields");
        }

        const std::string family_state = string_field(record.get(), "family_state");
        const json_t* candidates = json_object_get(record.get(), "candidates");
        const std::uint64_t candidate_count = uint_field(record.get(), "candidate_count", "TOPOLOGY_CONSERVATION");
        if (!json_is_array(candidates) || candidate_count != json_array_size(candidates)) {
            reject("TOPOLOGY_CONSERVATION", "candidate_count differs from candidates length");
        }
        std::set<std::string> candidate_digests;
        std::map<std::string, std::set<std::pair<std::string, std::string>>> legal_edges;
        std::map<std::string, std::set<std::string>> nonroot_vertices;
        std::vector<std::tuple<std::string, std::string, std::string>> family_rows;
        std::optional<std::vector<std::string>> previous_vertex_set;
        std::string expected_tree_count = "0";
        for (std::size_t index = 0; index < json_array_size(candidates); ++index) {
            const json_t* candidate = json_array_get(candidates, index);
            const std::string digest = string_field(candidate, "vertex_set_sha256");
            const json_t* vertices = json_object_get(candidate, "vertex_set");
            if (!sorted_unique_strings(vertices)) {
                reject("TOPOLOGY_CONSERVATION", "candidate vertex set is duplicate or unsorted");
            }
            const std::vector<std::string> vertex_states = json_string_vector(vertices, "TOPOLOGY_CONSERVATION");
            const std::string root(dimensions, '0');
            if (vertex_states.empty() || vertex_states.front() != root ||
                (previous_vertex_set.has_value() && !(*previous_vertex_set < vertex_states))) {
                reject("TOPOLOGY_CONSERVATION", "candidate family is not root-containing and canonically ordered");
            }
            previous_vertex_set = vertex_states;
            std::ostringstream vertex_stream;
            vertex_stream.imbue(std::locale::classic());
            vertex_stream << "schema=longlineage.exact_vertex_set.v1\n"
                          << "q=" << dimensions << "\nvertices=";
            for (std::size_t vertex = 0; vertex < json_array_size(vertices); ++vertex) {
                if (vertex != 0U) {
                    vertex_stream << ',';
                }
                vertex_stream << binary_state_value(vertex_states[vertex], dimensions);
            }
            vertex_stream << '\n';
            if (sha256_bytes(vertex_stream.str()) != digest || !candidate_digests.insert(digest).second) {
                reject("TOPOLOGY_CONSERVATION", "candidate vertex-set digest is wrong or duplicated");
            }
            const json_t* choices = json_object_get(candidate, "legal_parent_choices");
            if (!json_is_array(choices)) {
                reject("TOPOLOGY_CONSERVATION", "candidate legal parent choices are absent");
            }
            std::uint64_t legal_parent_count = 0U;
            std::string tree_count = "1";
            std::set<std::string> choice_vertices;
            std::string previous_child;
            for (std::size_t choice_index = 0; choice_index < json_array_size(choices); ++choice_index) {
                const json_t* choice = json_array_get(choices, choice_index);
                const std::string child = string_field(choice, "vertex");
                const json_t* parents = json_object_get(choice, "parents");
                if ((choice_index != 0U && !(previous_child < child)) ||
                    std::find(vertex_states.begin(), vertex_states.end(), child) == vertex_states.end() ||
                    child == root || !choice_vertices.insert(child).second || !sorted_unique_strings(parents) ||
                    json_array_size(parents) == 0U) {
                    reject("TOPOLOGY_CONSERVATION",
                           "legal parent choices are absent, non-member, duplicated or unsorted");
                }
                previous_child = child;
                if (json_array_size(parents) > std::numeric_limits<std::uint64_t>::max() - legal_parent_count) {
                    reject("TOPOLOGY_CONSERVATION", "legal parent count overflows");
                }
                legal_parent_count += json_array_size(parents);
                tree_count = decimal_multiply_small(tree_count, static_cast<std::uint64_t>(json_array_size(parents)));
                for (std::size_t parent_index = 0; parent_index < json_array_size(parents); ++parent_index) {
                    const std::string parent = json_string_value(json_array_get(parents, parent_index));
                    if (std::find(vertex_states.begin(), vertex_states.end(), parent) == vertex_states.end()) {
                        reject("TOPOLOGY_CONSERVATION", "legal parent is outside the fixed vertex set");
                    }
                    std::size_t changes = 0U;
                    if (child.size() != parent.size() || child.size() != dimensions) {
                        reject("TOPOLOGY_CONSERVATION", "parent edge dimension differs");
                    }
                    for (std::size_t bit = 0; bit < child.size(); ++bit) {
                        if (child[bit] != parent[bit]) {
                            if (child[bit] != '1' || parent[bit] != '0') {
                                reject("TOPOLOGY_CONSERVATION", "parent edge is not mutation-monotone");
                            }
                            ++changes;
                        }
                    }
                    if (changes != 1U) {
                        reject("TOPOLOGY_CONSERVATION", "legal parent edge is not a hypercube edge");
                    }
                    legal_edges[digest].insert({child, parent});
                }
            }
            std::set<std::string> expected_nonroot(vertex_states.begin() + 1, vertex_states.end());
            if (choice_vertices != expected_nonroot) {
                reject("TOPOLOGY_CONSERVATION", "legal parent choices do not cover every non-root vertex exactly once");
            }
            nonroot_vertices.emplace(digest, std::move(expected_nonroot));
            if (string_field(candidate, "legal_parent_count") != std::to_string(legal_parent_count) ||
                string_field(candidate, "tree_count") != tree_count) {
                reject("TOPOLOGY_CONSERVATION", "candidate legal-parent/tree count differs");
            }
            family_rows.emplace_back(digest, std::to_string(legal_parent_count), tree_count);
            expected_tree_count = decimal_add(expected_tree_count, tree_count);
        }
        if (string_field(record.get(), "tree_count") != expected_tree_count) {
            reject("TOPOLOGY_CONSERVATION", "unit tree_count differs from candidate sum");
        }

        const json_t* published_rank = json_object_get(record.get(), "published_rank");
        const std::string ranking_state = string_field(record.get(), "ranking_state");
        if (family_state != "FAMILY_COMPLETE") {
            if (candidate_count != 0U || !json_is_null(published_rank) || ranking_state == "RANKING_COMPLETE") {
                reject("TOPOLOGY_INCOMPLETE_WINNER", "incomplete family exposes candidates or a published rank");
            }
        } else if (objective_state != "OBJECTIVE_CERTIFIED" || candidate_count == 0U) {
            reject("TOPOLOGY_CONSERVATION", "complete family lacks a certified non-empty candidate family");
        } else {
            std::ostringstream family_stream;
            family_stream.imbue(std::locale::classic());
            family_stream << "schema=longlineage.candidate_family.v1\n"
                          << "q=" << dimensions << '\n';
            for (const auto& [digest, parent_count, tree_count] : family_rows) {
                family_stream << digest << '\t' << parent_count << '\t' << tree_count << '\n';
            }
            const std::string family_digest = sha256_bytes(family_stream.str());
            if (!json_is_string(json_object_get(record.get(), "candidate_family_digest")) ||
                string_field(record.get(), "candidate_family_digest") != family_digest) {
                reject("TOPOLOGY_CONSERVATION", "complete candidate-family digest differs from replay");
            }
            std::ostringstream evidence;
            evidence.imbue(std::locale::classic());
            evidence << "schema=longlineage.complete_family_evidence.v1\n"
                     << "input_evidence_sha256=" << string_field(record.get(), "input_evidence_sha256")
                     << "\nobjective_evidence_sha256=" << string_field(record.get(), "objective_evidence_sha256")
                     << "\ncandidate_family_digest=" << family_digest << "\ncandidate_count=" << candidate_count
                     << "\ntree_count=" << expected_tree_count << "\nfamily_enumeration_exhausted=1\n";
            if (!json_is_string(json_object_get(record.get(), "family_evidence_sha256")) ||
                string_field(record.get(), "family_evidence_sha256") != sha256_bytes(evidence.str())) {
                reject("TOPOLOGY_CONSERVATION", "complete-family evidence digest differs from replay");
            }
        }
        if (ranking_state == "RANKING_COMPLETE") {
            if (!json_is_object(published_rank)) {
                reject("TOPOLOGY_RANKING_REPLAY", "complete ranking lacks published rank");
            }
            const std::vector<std::string> ties = json_string_vector(
                json_object_get(published_rank, "best_vertex_set_tie_class"), "TOPOLOGY_RANKING_REPLAY");
            if (!std::is_sorted(ties.begin(), ties.end())) {
                reject("TOPOLOGY_RANKING_REPLAY", "published ranking tie class is not sorted");
            }
            for (const std::string& digest : ties) {
                if (candidate_digests.count(digest) == 0U) {
                    reject("TOPOLOGY_RANKING_REPLAY", "published rank references a non-candidate");
                }
            }
            const json_t* certificate = json_object_get(record.get(), "ranking_certificate");
            if (!json_is_object(certificate) ||
                uint_field(certificate, "evaluated_vertex_set_count", "TOPOLOGY_RANKING_REPLAY") +
                        uint_field(certificate, "excluded_by_interval_count", "TOPOLOGY_RANKING_REPLAY") !=
                    candidate_count) {
                reject("TOPOLOGY_RANKING_REPLAY", "ranking certificate does not conserve candidate family");
            }
        } else if (!json_is_null(published_rank)) {
            reject("TOPOLOGY_RANKING_REPLAY", "non-complete ranking exposes a published rank");
        }

        const json_t* edge = json_object_get(record.get(), "edge_endpoint");
        const std::string edge_status = string_field(edge, "status");
        const json_t* edge_results = json_object_get(edge, "candidate_results");
        if (edge_status == "EDGE_COMPLETE") {
            if (!json_is_array(edge_results) || json_array_size(edge_results) != candidate_count) {
                reject("TOPOLOGY_EDGE_REPLAY", "complete edge endpoint does not cover every candidate");
            }
            std::set<std::string> edge_digests;
            for (std::size_t index = 0; index < json_array_size(edge_results); ++index) {
                const json_t* result = json_array_get(edge_results, index);
                const std::string digest = string_field(result, "candidate_vertex_set_sha256");
                const std::string tie_count = string_field(result, "best_parent_tie_count");
                const json_t* mapping = json_object_get(result, "published_parent_mapping");
                if (candidate_digests.count(digest) == 0U || !edge_digests.insert(digest).second ||
                    ((tie_count == "1") != json_is_array(mapping))) {
                    reject("TOPOLOGY_EDGE_REPLAY", "edge result digest/tie/mapping contract differs");
                }
                if (json_is_array(mapping)) {
                    std::set<std::pair<std::string, std::string>> published_edges;
                    std::set<std::string> published_children;
                    for (std::size_t edge_index = 0; edge_index < json_array_size(mapping); ++edge_index) {
                        const json_t* mapping_row = json_array_get(mapping, edge_index);
                        const std::string child = string_field(mapping_row, "child_state");
                        const std::string parent = string_field(mapping_row, "parent_state");
                        if (!published_children.insert(child).second ||
                            !published_edges.insert({child, parent}).second) {
                            reject("TOPOLOGY_EDGE_REPLAY", "published parent mapping duplicates a child or edge");
                        }
                    }
                    if (published_children != nonroot_vertices[digest] ||
                        !std::includes(legal_edges[digest].begin(), legal_edges[digest].end(), published_edges.begin(),
                                       published_edges.end())) {
                        reject("TOPOLOGY_EDGE_REPLAY",
                               "published parent mapping is incomplete or contains an illegal edge");
                    }
                }
            }
            if (edge_digests != candidate_digests) {
                reject("TOPOLOGY_EDGE_REPLAY", "complete edge endpoint omits a candidate digest");
            }
        } else if (json_is_array(edge_results) && json_array_size(edge_results) != 0U) {
            reject("TOPOLOGY_EDGE_REPLAY", "non-complete edge endpoint carries candidate results");
        }
    }
}

std::uint64_t json_count_field(const json_t* object, const char* name, const std::string& check_id) {
    return uint_field(object, name, check_id);
}

void validate_dataset_scope(const std::vector<ExpectedDatasetIdentity>& expected, const SiteReadReplay& site_reads,
                            const M1Replay& m1) {
    std::map<std::uint64_t, std::string> contract;
    for (const ExpectedDatasetIdentity& dataset : expected) {
        if (!contract.emplace(dataset.dataset_order, dataset.dataset_id).second) {
            reject("DATASET_SCOPE_REPLAY", "manifest contains duplicate dataset order");
        }
    }
    std::map<std::uint64_t, std::string> site_scope;
    for (const auto& [key, identity] : site_reads.sites) {
        const auto wanted = contract.find(key.first);
        if (wanted == contract.end() || wanted->second != identity.dataset_id) {
            reject("DATASET_SCOPE_REPLAY", "site_reads dataset identity/order differs from manifest");
        }
        site_scope.emplace(key.first, identity.dataset_id);
    }
    std::map<std::uint64_t, std::string> m1_scope;
    for (const auto& [key, site] : m1.sites) {
        const auto wanted = contract.find(key.first);
        if (wanted == contract.end() || wanted->second != site.dataset_id) {
            reject("DATASET_SCOPE_REPLAY", "m1_sites dataset identity/order differs from manifest");
        }
        m1_scope.emplace(key.first, site.dataset_id);
    }
    if (site_scope != contract || m1_scope != contract) {
        reject("DATASET_SCOPE_REPLAY", "manifest dataset scope is not represented exactly by site_reads and M1");
    }
}

void validate_summary_conservation(const ParsedArtifact& summary, const ProducerReceipt& producer,
                                   const SiteReadReplay& site_reads, const M1Replay& m1,
                                   const CooccurrenceSiteReplay& m2, const ParsedArtifact& topology,
                                   const InputContentReplay& input_contract) {
    if (summary.json_records.size() != 1U) {
        reject("SUMMARY_CONSERVATION", "summary artifact must contain exactly one JSON record");
    }
    const json_t* root = summary.json_records.front().get();
    if (string_field(root, "run_id") != producer.run_id) {
        reject("SUMMARY_CONSERVATION", "summary run_id differs from receipt");
    }
    if (string_field(root, "phase_status_scope") != "RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER") {
        reject("SUMMARY_CONSERVATION", "summary phase status lacks its required run-local scope");
    }
    const json_t* scope = json_object_get(root, "scope");
    const json_t* datasets = json_object_get(scope, "dataset_ids");
    if (!json_is_array(datasets) ||
        json_count_field(scope, "dataset_count", "SUMMARY_CONSERVATION") != json_array_size(datasets)) {
        reject("SUMMARY_CONSERVATION", "summary dataset_count differs from dataset_ids");
    }
    if (json_array_size(datasets) != input_contract.datasets.size()) {
        reject("SUMMARY_CONSERVATION", "summary dataset count differs from manifest dataset scope");
    }
    for (std::size_t index = 0; index < input_contract.datasets.size(); ++index) {
        const json_t* dataset_id = json_array_get(datasets, index);
        if (!json_is_string(dataset_id) || json_string_value(dataset_id) != input_contract.datasets[index].dataset_id ||
            input_contract.datasets[index].dataset_order != index) {
            reject("SUMMARY_CONSERVATION", "summary ordered dataset IDs differ from manifest dataset scope");
        }
    }
    if (string_field(scope, "m1_representation") != input_contract.m1_runtime_representation) {
        reject("SUMMARY_CONSERVATION", "summary M1 representation differs from versioned science metadata");
    }
    const json_t* counts = json_object_get(root, "counts");
    const auto require_count = [&](const char* name, std::uint64_t expected) {
        if (json_count_field(counts, name, "SUMMARY_CONSERVATION") != expected) {
            reject("SUMMARY_CONSERVATION", std::string("summary count differs: ") + name);
        }
    };
    require_count("site_keys", static_cast<std::uint64_t>(m1.sites.size()));
    if (string_field(scope, "completeness") == "FULL") {
        require_count("site_keys_missing", 0U);
        require_count("site_keys_extra", 0U);
        require_count("site_keys_duplicate", 0U);
    }
    require_count("m1_evaluable", m1.evaluable);
    require_count("m1_insufficient_alt_reads", m1.insufficient_alt);
    require_count("m1_incomplete_distance", m1.incomplete_distance);
    require_count("m1_stable_assignments", m1.stable);
    require_count("latest_tag_exact_joins", site_reads.exact_joins);
    require_count("latest_tag_missing", 0U);
    require_count("latest_tag_conflict", 0U);
    require_count("latest_tag_multimatch", 0U);
    require_count("m2_eligible", m2.m2_eligible);
    require_count("m2_evaluable_ineligible", m2.m2_evaluable_ineligible);
    require_count("m2_axis_indeterminate", m2.m2_axis_indeterminate);
    require_count("m2_group_count_gt10", m2.m2_group_count_gt10);
    std::uint64_t m2_remaining = m1.stable;
    for (const std::uint64_t value :
         {m2.m2_eligible, m2.m2_evaluable_ineligible, m2.m2_axis_indeterminate, m2.m2_group_count_gt10}) {
        if (value > m2_remaining) {
            reject("SUMMARY_CONSERVATION", "M2 status partition exceeds stable M1 site census");
        }
        m2_remaining -= value;
    }
    if (m2_remaining != 0U) {
        reject("SUMMARY_CONSERVATION", "M2 status partition does not conserve stable M1 sites");
    }
    require_count("raw_expected", site_reads.raw_expected);
    require_count("raw_matched", site_reads.raw_matched);
    require_count("raw_rg_only_duplicate_occurrences", site_reads.rg_only_duplicates);

    std::uint64_t incomplete_with_rank = 0U;
    for (const JsonPtr& unit : topology.json_records) {
        if (string_field(unit.get(), "family_state") != "FAMILY_COMPLETE" &&
            !json_is_null(json_object_get(unit.get(), "published_rank"))) {
            ++incomplete_with_rank;
        }
    }
    require_count("topology_incomplete_units_with_winner", incomplete_with_rank);
    const std::uint64_t regions = json_count_field(counts, "topology_regions", "SUMMARY_CONSERVATION");
    const std::uint64_t complete = json_count_field(counts, "topology_fully_complete_regions", "SUMMARY_CONSERVATION");
    const std::uint64_t incomplete = json_count_field(counts, "topology_incomplete_regions", "SUMMARY_CONSERVATION");
    if (complete > std::numeric_limits<std::uint64_t>::max() - incomplete || complete + incomplete != regions) {
        reject("SUMMARY_CONSERVATION", "topology complete/incomplete region partition differs");
    }
    if (m1.evaluable + m1.insufficient_alt + m1.incomplete_distance != m1.sites.size()) {
        reject("SUMMARY_CONSERVATION", "M1 status partition does not conserve site keys");
    }
}

bool validate_scientific_conservation(const Catalog& catalog, const ProducerReceipt& producer,
                                      const std::map<std::string, ParsedArtifact>& parsed,
                                      const InputContentReplay& input_contract) {
    static const std::set<std::string> kCanonicalScientific = {
        "site_reads",         "methyl_calls",       "bernoulli_upper", "m1_sites", "m1_assignments",
        "cooccurrence_pairs", "cooccurrence_sites", "topology_units",  "summary"};
    if (as_set(catalog.scientific_ids) != kCanonicalScientific) {
        // The small validator-fault harness uses an intentionally reduced
        // synthetic catalog to exercise physical/receipt faults. It is not a
        // DATASET_GATE science fixture and cannot satisfy this full replay.
        return false;
    }
    for (const std::string& id : kCanonicalScientific) {
        if (parsed.count(id) == 0U) {
            reject("SCIENTIFIC_CONSERVATION", "canonical scientific artifact was not parsed: " + id);
        }
    }
    const SiteReadReplay site_reads = replay_site_reads(parsed.at("site_reads"));
    const MethylReplay methyl = replay_methyl_calls(parsed.at("methyl_calls"), site_reads);
    const M1Replay m1 = replay_m1_sites(parsed.at("m1_sites"), site_reads);
    validate_dataset_scope(input_contract.datasets, site_reads, m1);
    validate_llm_conservation(parsed.at("bernoulli_upper"), m1);
    const AssignmentReplay assignments = replay_m1_assignments(parsed.at("m1_assignments"), m1, site_reads, methyl);
    const PairReplay pairs = replay_cooccurrence_pairs(parsed.at("cooccurrence_pairs"), site_reads, assignments,
                                                       parsed.at("cooccurrence_sites"));
    const CooccurrenceSiteReplay m2 = replay_cooccurrence_sites(parsed.at("cooccurrence_sites"), m1, site_reads, pairs);
    validate_topology_conservation(parsed.at("topology_units"));
    validate_summary_conservation(parsed.at("summary"), producer, site_reads, m1, m2, parsed.at("topology_units"),
                                  input_contract);
    return true;
}

std::string rfc3339_now() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream encoded;
    encoded << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return encoded.str();
}

std::pair<std::string, std::string> local_host_kernel() {
    struct utsname identity {};
    if (::uname(&identity) != 0) {
        reject("VALIDATOR_ENVIRONMENT", "uname failed while recording validator environment");
    }
    const std::string hostname(identity.nodename);
    const std::string kernel(identity.release);
    static const std::regex kHostname("^[A-Za-z0-9][A-Za-z0-9._-]{0,252}$");
    if (!std::regex_match(hostname, kHostname) || kernel.empty() || kernel.size() > 255U) {
        reject("VALIDATOR_ENVIRONMENT", "validator hostname or kernel release is outside the receipt contract");
    }
    return {hostname, kernel};
}

JsonPtr report_receipt_json(const ArtifactValidationReport& report) {
    JsonPtr root(json_object());
    if (!root) {
        reject("VALIDATION_RECEIPT_WRITE", "cannot allocate validation receipt");
    }
    json_object_set_new(root.get(), "schema_name", json_string("longlineage.validation_receipt"));
    json_object_set_new(root.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(root.get(), "run_id", json_string(report.run_id.c_str()));
    json_object_set_new(root.get(), "validation_profile", json_string(report.validation_profile.c_str()));
    json_object_set_new(root.get(), "production_claim_allowed", json_boolean(report.production_claim_allowed));
    json_object_set_new(root.get(), "producer_receipt_sha256", json_string(report.producer_receipt_sha256.c_str()));
    json_object_set_new(root.get(), "producer_executable_sha256",
                        json_string(report.producer_executable_sha256.c_str()));
    json_object_set_new(root.get(), "validator_executable_sha256",
                        json_string(report.validator_executable_sha256.c_str()));
    json_object_set_new(root.get(), "producer_hostname", json_string(report.producer_hostname.c_str()));
    json_object_set_new(root.get(), "producer_kernel_release", json_string(report.producer_kernel_release.c_str()));
    json_object_set_new(root.get(), "validator_hostname", json_string(report.validator_hostname.c_str()));
    json_object_set_new(root.get(), "validator_kernel_release", json_string(report.validator_kernel_release.c_str()));
    json_object_set_new(root.get(), "input_mount_identity_sha256",
                        json_string(report.input_mount_identity_sha256.c_str()));
    json_object_set_new(root.get(), "schema_catalog_sha256", json_string(report.schema_catalog_sha256.c_str()));
    json_object_set_new(root.get(), "science_parameters_sha256", json_string(report.science_parameters_sha256.c_str()));
    json_object_set_new(root.get(), "all_pass", json_boolean(report.all_pass));
    JsonPtr checks(json_array());
    for (const ValidationCheck& check : report.checks) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "check_id", json_string(check.check_id.c_str()));
        json_object_set_new(row.get(), "status", json_string(check.passed ? "PASS" : "FAIL"));
        json_object_set_new(row.get(), "reason", check.passed ? json_null() : json_string(check.detail.c_str()));
        json_object_set_new(row.get(), "observed", json_string(check.detail.c_str()));
        json_object_set_new(row.get(), "expected", json_string(check.passed ? "PASS" : "contract replay PASS"));
        json_object_set_new(row.get(), "evidence_sha256",
                            json_string(sha256_bytes(check.check_id + "\n" + check.detail).c_str()));
        json_array_append_new(checks.get(), row.release());
    }
    json_object_set_new(root.get(), "checks", checks.release());
    json_object_set_new(root.get(), "validated_at", json_string(report.validated_at.c_str()));
    json_object_set_new(root.get(), "validator_independent", json_true());
    json_object_set_new(root.get(), "linked_producer_kernels", json_false());
    json_object_set_new(root.get(), "input_snapshot_before_sha256",
                        json_string(report.input_snapshot_before_sha256.c_str()));
    json_object_set_new(root.get(), "input_snapshot_after_sha256",
                        json_string(report.input_snapshot_after_sha256.c_str()));
    return root;
}

void write_validation_receipt(ArtifactValidationReport& report) {
    if (!report.all_pass || report.run_id.empty() || !is_lower_sha256(report.producer_receipt_sha256) ||
        !is_lower_sha256(report.producer_executable_sha256) || !is_lower_sha256(report.validator_executable_sha256) ||
        report.validation_profile != "DATASET_GATE" || report.production_claim_allowed ||
        report.producer_hostname.empty() || report.producer_kernel_release.empty() ||
        report.validator_hostname.empty() || report.validator_kernel_release.empty() ||
        !is_lower_sha256(report.input_mount_identity_sha256) || !is_lower_sha256(report.schema_catalog_sha256) ||
        !is_lower_sha256(report.science_parameters_sha256) || !is_lower_sha256(report.input_snapshot_before_sha256) ||
        !is_lower_sha256(report.input_snapshot_after_sha256) || report.checks.empty()) {
        return;
    }
    report.validation_receipt_path = report.validation_receipt_path.lexically_normal();
    const std::filesystem::path temporary =
        report.validation_receipt_path.parent_path() /
        (".validation_receipt.json.tmp." + std::to_string(static_cast<long long>(::getpid())));
    JsonPtr root = report_receipt_json(report);
    char* encoded = json_dumps(root.get(), JSON_INDENT(2) | JSON_ENSURE_ASCII | JSON_PRESERVE_ORDER);
    if (encoded == nullptr) {
        reject("VALIDATION_RECEIPT_WRITE", "cannot encode validation receipt");
    }
    std::string bytes(encoded);
    std::free(encoded);
    bytes.push_back('\n');
    const int descriptor =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        reject("VALIDATION_RECEIPT_WRITE", "cannot create temporary validation receipt");
    }
    std::size_t offset = 0;
    bool write_ok = true;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            write_ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (write_ok && ::fsync(descriptor) != 0) {
        write_ok = false;
    }
    ::close(descriptor);
    if (!write_ok || ::rename(temporary.c_str(), report.validation_receipt_path.c_str()) != 0) {
        reject("VALIDATION_RECEIPT_WRITE", "cannot atomically publish validation receipt");
    }
    const int directory =
        ::open(report.validation_receipt_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0 || ::fsync(directory) != 0) {
        if (directory >= 0) {
            ::close(directory);
        }
        reject("VALIDATION_RECEIPT_WRITE", "cannot fsync validation receipt directory");
    }
    ::close(directory);
    report.validation_receipt_written = true;
    report.validation_receipt_sha256 = sha256_bytes(bytes);
}

std::string json_string_from_report(const ArtifactValidationReport& report) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "command", json_string("validate"));
    json_object_set_new(root.get(), "status", json_string(report.all_pass ? "PASS" : "FAIL"));
    json_object_set_new(root.get(), "all_pass", json_boolean(report.all_pass));
    json_object_set_new(root.get(), "run_id", json_string(report.run_id.c_str()));
    json_object_set_new(root.get(), "validation_receipt_written", json_boolean(report.validation_receipt_written));
    json_object_set_new(root.get(), "input_content_replayed", json_boolean(report.input_content_replayed));
    json_object_set_new(root.get(), "publication_snapshot_captured",
                        json_boolean(report.publication_snapshot_captured));
    json_object_set_new(root.get(), "validation_profile", json_string(report.validation_profile.c_str()));
    json_object_set_new(root.get(), "production_claim_allowed", json_boolean(report.production_claim_allowed));
    if (!report.validation_receipt_sha256.empty()) {
        json_object_set_new(root.get(), "validation_receipt_sha256",
                            json_string(report.validation_receipt_sha256.c_str()));
    }
    JsonPtr checks(json_array());
    for (const ValidationCheck& check : report.checks) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "check_id", json_string(check.check_id.c_str()));
        json_object_set_new(row.get(), "status", json_string(check.passed ? "PASS" : "FAIL"));
        json_object_set_new(row.get(), "detail", json_string(check.detail.c_str()));
        json_array_append_new(checks.get(), row.release());
    }
    json_object_set_new(root.get(), "checks", checks.release());
    char* encoded = json_dumps(root.get(), JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    if (encoded == nullptr) {
        return "{\"all_pass\":false,\"status\":\"FAIL\",\"checks\":[]}";
    }
    std::string result(encoded);
    std::free(encoded);
    return result;
}

void copy_json_field(json_t* destination, const json_t* source, const char* field, const std::string& check_id) {
    const json_t* value = json_object_get(source, field);
    if (value == nullptr || json_object_set(destination, field, const_cast<json_t*>(value)) != 0) {
        reject(check_id, std::string("cannot copy JSON field: ") + field);
    }
}

JsonPtr state_event(std::uint64_t sequence, const std::string& state, const std::string& at, const std::string& actor,
                    const std::optional<std::string>& previous) {
    JsonPtr event(json_object());
    json_object_set_new(event.get(), "sequence", json_integer(static_cast<json_int_t>(sequence)));
    json_object_set_new(event.get(), "state", json_string(state.c_str()));
    json_object_set_new(event.get(), "at", json_string(at.c_str()));
    json_object_set_new(event.get(), "actor_executable_sha256", json_string(actor.c_str()));
    json_object_set_new(event.get(), "previous_event_sha256",
                        previous.has_value() ? json_string(previous->c_str()) : json_null());
    return event;
}

std::string canonical_state_event_sha256(const json_t* event) {
    return sha256_bytes(canonical_compact_sorted_json(event, "RUN_RECEIPT_FINALIZE") + "\n");
}

std::uint64_t regular_file_count(const std::filesystem::path& root) {
    std::uint64_t count = 0U;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end; iterator != end;
         iterator.increment(error)) {
        if (error) {
            reject("RUN_RECEIPT_FINALIZE", "cannot enumerate final file count: " + error.message());
        }
        const auto status = iterator->symlink_status(error);
        if (error || std::filesystem::is_symlink(status)) {
            reject("RUN_RECEIPT_FINALIZE", "unsafe path while counting final files");
        }
        if (std::filesystem::is_regular_file(status)) {
            if (count == std::numeric_limits<std::uint64_t>::max()) {
                reject("RUN_RECEIPT_FINALIZE", "final file count overflows");
            }
            ++count;
        }
    }
    return count;
}

std::string build_dataset_gate_run_receipt(const std::filesystem::path& repo_root,
                                           const std::filesystem::path& staging_root, const Catalog& catalog,
                                           const ProducerReceipt& producer, const ArtifactValidationReport& report) {
    if (!report.all_pass || !report.validation_receipt_written || !report.scientific_conservation_replayed ||
        !is_lower_sha256(report.validation_receipt_sha256)) {
        reject("RUN_RECEIPT_FINALIZE", "full science validation receipt is required before freeze");
    }
    JsonPtr root(json_object());
    if (!root) {
        reject("RUN_RECEIPT_FINALIZE", "cannot allocate run receipt");
    }
    json_object_set_new(root.get(), "schema_name", json_string("longlineage.run_receipt"));
    json_object_set_new(root.get(), "schema_version", json_string("1.0.0"));
    json_object_set_new(root.get(), "run_id", json_string(producer.run_id.c_str()));
    json_object_set_new(root.get(), "state", json_string("VALIDATED_FROZEN_DATASET_GATE"));
    json_object_set_new(root.get(), "validation_profile", json_string("DATASET_GATE"));
    json_object_set_new(root.get(), "production_claim_allowed", json_false());
    copy_json_field(root.get(), producer.run_receipt_draft.get(), "production_executable", "RUN_RECEIPT_FINALIZE");
    json_object_set_new(root.get(), "producer_hostname", json_string(producer.producer_hostname.c_str()));
    json_object_set_new(root.get(), "producer_kernel_release", json_string(producer.producer_kernel_release.c_str()));
    json_object_set_new(root.get(), "validator_hostname", json_string(report.validator_hostname.c_str()));
    json_object_set_new(root.get(), "validator_kernel_release", json_string(report.validator_kernel_release.c_str()));
    json_object_set_new(root.get(), "input_mount_identity_sha256",
                        json_string(producer.input_mount_identity_sha256.c_str()));
    json_object_set_new(root.get(), "manifest_sha256", json_string(producer.manifest_sha256.c_str()));
    copy_json_field(root.get(), producer.run_receipt_draft.get(), "input_lock_sha256", "RUN_RECEIPT_FINALIZE");
    copy_json_field(root.get(), producer.run_receipt_draft.get(), "phase_ledger_sha256", "RUN_RECEIPT_FINALIZE");
    if (json_object_set(root.get(), "artifacts", producer.artifacts_raw.get()) != 0) {
        reject("RUN_RECEIPT_FINALIZE", "cannot bind producer artifacts");
    }
    json_object_set_new(root.get(), "truth_fields_seen", json_integer(0));
    json_object_set_new(root.get(), "input_snapshot_before_sha256", json_string(producer.input_before.c_str()));
    json_object_set_new(root.get(), "input_snapshot_after_sha256", json_string(producer.input_after.c_str()));
    json_object_set_new(root.get(), "schema_catalog_sha256", json_string(producer.catalog_sha256.c_str()));
    json_object_set_new(root.get(), "science_parameters_sha256", json_string(producer.science_sha256.c_str()));

    JsonPtr history(json_array());
    JsonPtr running =
        state_event(0U, "RUNNING", producer.finished_at, producer.producer_executable_sha256, std::nullopt);
    const std::string running_sha = canonical_state_event_sha256(running.get());
    JsonPtr validated =
        state_event(1U, "VALIDATED", report.validated_at, report.validator_executable_sha256, running_sha);
    const std::string validated_sha = canonical_state_event_sha256(validated.get());
    JsonPtr frozen = state_event(2U, "VALIDATED_FROZEN_DATASET_GATE", report.validated_at,
                                 report.validator_executable_sha256, validated_sha);
    json_array_append_new(history.get(), running.release());
    json_array_append_new(history.get(), validated.release());
    json_array_append_new(history.get(), frozen.release());
    json_object_set_new(root.get(), "state_history", history.release());

    const json_t* source_performance = json_object_get(producer.run_receipt_draft.get(), "performance");
    JsonPtr performance(json_deep_copy(source_performance));
    if (!performance) {
        reject("RUN_RECEIPT_FINALIZE", "cannot retain producer performance");
    }
    const std::uint64_t existing_files = regular_file_count(staging_root);
    if (existing_files == std::numeric_limits<std::uint64_t>::max()) {
        reject("RUN_RECEIPT_FINALIZE", "final file count overflows");
    }
    json_object_set_new(performance.get(), "final_file_count",
                        json_integer(static_cast<json_int_t>(existing_files + 1U)));
    json_object_set_new(root.get(), "performance", performance.release());
    json_object_set_new(root.get(), "producer_receipt_sha256", json_string(producer.physical_sha256.c_str()));
    json_object_set_new(root.get(), "validation_receipt_sha256", json_string(report.validation_receipt_sha256.c_str()));
    const std::string checksums_sha = sha256_file(
        require_regular_under(staging_root, catalog.artifacts.at("checksums").relative_path, "RUN_RECEIPT_FINALIZE"),
        "RUN_RECEIPT_FINALIZE");
    json_object_set_new(root.get(), "checksums_sha256", json_string(checksums_sha.c_str()));

    SchemaStore store(repo_root);
    const SharedJson schema = store.load_relative("schema/core/run_receipt.schema.json", "RUN_RECEIPT_FINALIZE");
    const std::string canonical = canonical_json(root.get(), schema.get(), schema, store) + "\n";
    JsonPtr replayed;
    json_error_t error{};
    replayed.reset(json_loadb(canonical.data(), canonical.size(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
    if (!replayed) {
        reject("RUN_RECEIPT_FINALIZE", "canonical final run receipt cannot be replayed");
    }
    validate_json_schema_value(replayed.get(), schema.get(), schema, store);
    return canonical;
}

std::string finalize_report_json(const DatasetGateFinalizeReport& report) {
    JsonPtr root(json_object());
    json_object_set_new(root.get(), "command", json_string("validate-and-freeze"));
    json_object_set_new(root.get(), "status", json_string(report.dataset_gate_frozen ? "PASS" : "FAIL"));
    json_object_set_new(root.get(), "dataset_gate_frozen", json_boolean(report.dataset_gate_frozen));
    json_object_set_new(root.get(), "production_claim_allowed", json_false());
    json_object_set_new(root.get(), "validation",
                        parse_embedded_json(json_string_from_report(report.validation), "FINALIZE_REPORT").release());
    JsonPtr publication(json_object());
    json_object_set_new(publication.get(), "status", json_string(artifact::to_string(report.publication.status)));
    json_object_set_new(publication.get(), "message", json_string(report.publication.message.c_str()));
    if (!report.publication.run_receipt_sha256.empty()) {
        json_object_set_new(publication.get(), "run_receipt_sha256",
                            json_string(report.publication.run_receipt_sha256.c_str()));
    }
    json_object_set_new(root.get(), "publication", publication.release());
    JsonPtr inspection(json_object());
    json_object_set_new(inspection.get(), "state", json_string(artifact::to_string(report.inspection.state)));
    json_object_set_new(inspection.get(), "query_visible", json_boolean(report.inspection.query_visible));
    json_object_set_new(inspection.get(), "production_query_allowed", json_false());
    json_object_set_new(inspection.get(), "message", json_string(report.inspection.message.c_str()));
    json_object_set_new(root.get(), "inspection", inspection.release());
    char* encoded = json_dumps(root.get(), JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    if (encoded == nullptr) {
        return "{\"dataset_gate_frozen\":false,\"status\":\"FAIL\"}";
    }
    std::string result(encoded);
    std::free(encoded);
    return result;
}

}  // namespace

ArtifactValidationReport ArtifactValidator::validate(const ArtifactValidationOptions& options) {
    ArtifactValidationReport report;
    report.validation_receipt_path = options.run_root / "validation_receipt.json";
    try {
        if (options.repo_root.empty() || options.run_root.empty() || options.validator_executable.empty() ||
            !options.repo_root.is_absolute() || !options.run_root.is_absolute() ||
            !options.validator_executable.is_absolute()) {
            reject("ROOT_LAYOUT", "repo, run and validator executable paths must be absolute");
        }
        struct stat run_status {};
        if (::lstat(options.run_root.c_str(), &run_status) != 0 || !S_ISDIR(run_status.st_mode) ||
            S_ISLNK(run_status.st_mode)) {
            reject("ROOT_LAYOUT", "run root must be a real staging directory");
        }
        report.checks.push_back({"ROOT_LAYOUT", true, "real staging directory verified"});

        const Catalog catalog = load_catalog(options.repo_root);
        report.checks.push_back({"CATALOG_CONTRACT", true, "catalog identity and membership parsed"});

        std::error_code error;
        if (std::filesystem::exists(options.run_root / "validation_receipt.json", error) ||
            std::filesystem::exists(options.run_root / "run_receipt.json", error)) {
            reject("FORGED_RECEIPT", "pre-existing validation or run receipt is forbidden");
        }
        report.checks.push_back({"FORGED_RECEIPT", true, "no pre-existing validator-authored receipt"});

        const ProducerReceipt producer = load_producer_receipt(options.repo_root, options.run_root, catalog);
        report.run_id = producer.run_id;
        report.producer_receipt_sha256 = producer.physical_sha256;
        report.producer_executable_sha256 = producer.producer_executable_sha256;
        report.producer_hostname = producer.producer_hostname;
        report.producer_kernel_release = producer.producer_kernel_release;
        report.input_mount_identity_sha256 = producer.input_mount_identity_sha256;
        report.input_snapshot_before_sha256 = producer.input_before;
        report.input_snapshot_after_sha256 = producer.input_after;
        report.schema_catalog_sha256 = producer.catalog_sha256;
        report.science_parameters_sha256 = producer.science_sha256;
        report.validator_executable_sha256 = sha256_file(options.validator_executable, "VALIDATOR_EXECUTABLE_IDENTITY");
        std::tie(report.validator_hostname, report.validator_kernel_release) = local_host_kernel();
        report.checks.push_back({"VALIDATOR_ENVIRONMENT", true, "validator hostname and kernel release recorded"});
        report.checks.push_back({"PRODUCER_RECEIPT", true, "READY producer receipt replayed"});

        const InputContentReplay input_replay = validate_input_content(options, catalog, producer);
        report.input_content_replayed = input_replay.complete;
        report.checks.push_back(
            {"INPUT_CONTENT_REPLAY", true,
             input_replay.complete
                 ? "manifest, repository bindings, exact input bytes, identity snapshot, lock and lineage replayed"
                 : "reduced synthetic physical-fault fixture; canonical input content replay not claimed"});

        validate_membership(catalog, producer);
        report.checks.push_back({"ARTIFACT_MEMBERSHIP", true, "producer artifact set equals catalog membership"});

        if (input_replay.complete) {
            validate_static_provenance_graph(catalog, producer);
            report.checks.push_back(
                {"STATIC_PROVENANCE_GRAPH", true,
                 "science and metadata transforms/dependencies match the independent static graph"});
        }

        validate_file_census(options.run_root, catalog, producer);
        report.checks.push_back({"FILE_CENSUS", true, "staging file census contains no missing or extra path"});

        const std::map<std::string, ParsedArtifact> parsed =
            validate_artifacts(options.repo_root, options.run_root, catalog, producer);
        report.checks.push_back(
            {"ARTIFACT_REPLAY", true, "artifact bytes, keys, semantic digests and indexes replayed"});

        const bool full_science = validate_scientific_conservation(catalog, producer, parsed, input_replay);
        report.scientific_conservation_replayed = full_science;
        report.checks.push_back(
            {"SCIENTIFIC_CONSERVATION", true,
             full_science
                 ? "site/read/methyl/M1/LLM/co-occurrence/exact/FDR/topology/summary replayed independently"
                 : "reduced synthetic physical-fault fixture; canonical DATASET_GATE science replay not claimed"});

        validate_artifact_catalog_rows(catalog, producer, parsed);
        report.checks.push_back({"ARTIFACT_CATALOG_REPLAY", true, "artifact catalog mirrors producer receipt"});

        validate_data_lineage_rows(catalog, producer, parsed);
        report.checks.push_back({"DATA_LINEAGE_REPLAY", true, "data lineage mirrors immutable artifact inputs"});

        validate_semantic_digest_table(catalog, producer, parsed);
        report.checks.push_back({"SEMANTIC_DIGEST", true, "semantic digest table matches producer artifacts"});

        validate_checksums(options.run_root, catalog, producer);
        report.checks.push_back({"CHECKSUM_REPLAY", true, "checksum manifest set and hashes replayed"});

        report.publication_snapshot = capture_publication_snapshot(options.run_root, catalog, producer);
        report.checks.push_back(
            {"PUBLICATION_BASELINE", true, "validated artifact, index, producer receipt and checksum bytes captured"});

        report.all_pass = true;
    } catch (const ValidationError& error) {
        report.checks.push_back({error.check_id(), false, error.what()});
        report.all_pass = false;
    } catch (const std::exception& error) {
        report.checks.push_back({"INTERNAL_VALIDATOR_ERROR", false, error.what()});
        report.all_pass = false;
    }

    if (options.write_validation_receipt) {
        try {
            report.validated_at = rfc3339_now();
            write_validation_receipt(report);
            if (report.validation_receipt_written) {
                const Catalog catalog = load_catalog(options.repo_root);
                append_validation_receipt_snapshot(options.run_root, catalog, report);
            }
        } catch (const ValidationError& error) {
            report.checks.push_back({error.check_id(), false, error.what()});
            report.all_pass = false;
        } catch (const std::exception& error) {
            report.checks.push_back({"VALIDATION_RECEIPT_WRITE", false, error.what()});
            report.all_pass = false;
        }
    }
    return report;
}

std::string render_report_json(const ArtifactValidationReport& report) { return json_string_from_report(report); }

DatasetGateFinalizeReport ArtifactValidator::validate_and_freeze(const DatasetGateFinalizeOptions& options) {
    DatasetGateFinalizeReport report;
    report.validation = validate(options.validation);
    if (!report.validation.all_pass || !report.validation.validation_receipt_written ||
        !report.validation.scientific_conservation_replayed || !report.validation.input_content_replayed ||
        !report.validation.publication_snapshot_captured) {
        report.publication = {
            artifact::RunRootStatus::kStateConflict,
            "atomic freeze requires full input/science replay, publication snapshot and durable validation receipt",
            {}};
        if (!report.validation.run_id.empty() && options.output_base.is_absolute()) {
            report.inspection = artifact::RunRootTransaction::inspect(options.output_base, report.validation.run_id);
        }
        return report;
    }
    try {
        if (options.output_base.empty() || !options.output_base.is_absolute()) {
            reject("RUN_RECEIPT_FINALIZE", "absolute output base is required");
        }
        PublicationLock publication_lock(options.output_base);
        const std::filesystem::path expected_staging =
            (options.output_base / ".staging" / report.validation.run_id).lexically_normal();
        if (options.validation.run_root.lexically_normal() != expected_staging) {
            reject("RUN_RECEIPT_FINALIZE", "validated run root is not output_base/.staging/run_id");
        }
        const Catalog catalog = load_catalog(options.validation.repo_root);
        const ProducerReceipt producer =
            load_producer_receipt(options.validation.repo_root, options.validation.run_root, catalog);
        if (!validate_input_content(options.validation, catalog, producer).complete) {
            reject("PRE_FREEZE_REPLAY", "canonical input content replay was not repeated before freeze");
        }
        verify_publication_snapshot(options.validation.run_root, report.validation.publication_snapshot, std::nullopt);
        const std::string canonical_receipt = build_dataset_gate_run_receipt(
            options.validation.repo_root, options.validation.run_root, catalog, producer, report.validation);

        artifact::RunRootCreateResult attached =
            artifact::RunRootTransaction::open_existing_staging(options.output_base, report.validation.run_id);
        if (!attached.result.ok() || !attached.transaction) {
            report.publication = attached.result;
            report.inspection = artifact::RunRootTransaction::inspect(options.output_base, report.validation.run_id);
            return report;
        }
        artifact::RunRootResult prepared = attached.transaction->prepare_run_receipt(canonical_receipt);
        if (!prepared.ok()) {
            report.publication = prepared;
            report.inspection = artifact::RunRootTransaction::inspect(options.output_base, report.validation.run_id);
            return report;
        }
        const std::string expected_digest = prepared.run_receipt_sha256;
        verify_publication_snapshot(options.validation.run_root, report.validation.publication_snapshot,
                                    expected_digest);
        artifact::RunRootResult renamed = attached.transaction->rename_staging_to_final();
        report.publication = renamed;
        report.inspection = artifact::RunRootTransaction::inspect(options.output_base, report.validation.run_id);
        if (!renamed.ok() || options.stop_after_atomic_rename) {
            return report;
        }
        if (options.before_publication_replay_for_test) {
            options.before_publication_replay_for_test(report.inspection.final_root);
        }
        verify_publication_snapshot(report.inspection.final_root, report.validation.publication_snapshot,
                                    expected_digest);
        report.publication = attached.transaction->publish_run_receipt(expected_digest);
        report.inspection = artifact::RunRootTransaction::inspect(options.output_base, report.validation.run_id);
        report.dataset_gate_frozen = report.publication.ok() &&
                                     report.inspection.state == artifact::RunRootObservedState::kPublished &&
                                     !report.validation.production_claim_allowed;
    } catch (const ValidationError& error) {
        report.validation.checks.push_back({error.check_id(), false, error.what()});
        report.validation.all_pass = false;
        report.publication = {artifact::RunRootStatus::kStateConflict, error.what(), {}};
        if (!report.validation.run_id.empty() && options.output_base.is_absolute()) {
            report.inspection = artifact::RunRootTransaction::inspect(options.output_base, report.validation.run_id);
        }
    } catch (const std::exception& error) {
        report.validation.checks.push_back({"RUN_RECEIPT_FINALIZE", false, error.what()});
        report.validation.all_pass = false;
        report.publication = {artifact::RunRootStatus::kIoError, error.what(), {}};
        if (!report.validation.run_id.empty() && options.output_base.is_absolute()) {
            report.inspection = artifact::RunRootTransaction::inspect(options.output_base, report.validation.run_id);
        }
    }
    return report;
}

std::string render_finalize_report_json(const DatasetGateFinalizeReport& report) {
    return finalize_report_json(report);
}

}  // namespace longlineage::validation
