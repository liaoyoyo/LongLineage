// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/artifact/dataset_closeout.hpp"

#include <htslib/bgzf.h>
#include <htslib/kstring.h>
#include <jansson.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "longlineage/common/digest.hpp"

namespace longlineage::artifact {
namespace {

using JsonPtr = std::unique_ptr<json_t, decltype(&json_decref)>;

struct CatalogSpec {
    std::string artifact_id;
    std::filesystem::path relative_path;
    std::string format;
    std::vector<std::string> primary_key;
    std::optional<std::filesystem::path> index_path;
};

struct ArtifactMeta {
    DatasetArtifactWriteReceipt receipt;
    std::vector<std::string> first;
    std::vector<std::string> last;
    std::string transform_id;
    std::vector<ArtifactInputBinding> inputs;
};

[[nodiscard]] std::string digest_bytes(std::string_view bytes) {
    auto digest = sha256_hex(bytes);
    if (!digest.ok() || !digest.value.has_value()) {
        throw std::runtime_error("SHA-256 failed: " + digest.detail);
    }
    return std::move(*digest.value);
}

[[nodiscard]] std::string digest_file(const std::filesystem::path& path) {
    auto digest = sha256_file(path);
    if (!digest.ok() || !digest.value.has_value()) {
        throw std::runtime_error("file SHA-256 failed for " + path.string() + ": " + digest.detail);
    }
    return std::move(*digest.value);
}

[[nodiscard]] JsonPtr load_json(const std::filesystem::path& path) {
    json_error_t error{};
    json_t* value = json_load_file(path.c_str(), JSON_REJECT_DUPLICATES, &error);
    if (value == nullptr) {
        throw std::runtime_error("cannot parse JSON " + path.string() + ": " + error.text);
    }
    return JsonPtr(value, json_decref);
}

[[nodiscard]] std::string dump_preserve(const json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_PRESERVE_ORDER);
    if (encoded == nullptr) {
        throw std::runtime_error("cannot encode canonical JSON");
    }
    std::string output(encoded);
    std::free(encoded);
    return output;
}

void write_text(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open closeout output: " + path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("cannot close closeout output: " + path.string());
    }
}

[[nodiscard]] std::vector<std::string> split_tsv(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t tab = line.find('\t', begin);
        fields.emplace_back(line.substr(begin, tab == std::string_view::npos ? std::string_view::npos : tab - begin));
        if (tab == std::string_view::npos) {
            break;
        }
        begin = tab + 1U;
    }
    return fields;
}

[[nodiscard]] std::vector<CatalogSpec> load_catalog(const std::filesystem::path& repo_root) {
    const JsonPtr catalog = load_json(repo_root / "schema/catalog.json");
    const json_t* rows = json_object_get(catalog.get(), "artifacts");
    if (!json_is_array(rows)) {
        throw std::runtime_error("schema catalog artifacts is not an array");
    }
    std::vector<CatalogSpec> result;
    for (std::size_t index = 0; index < json_array_size(rows); ++index) {
        const json_t* row = json_array_get(rows, index);
        const json_t* id = json_object_get(row, "artifact_id");
        const json_t* path = json_object_get(row, "relative_path");
        const json_t* format = json_object_get(row, "format");
        const json_t* primary = json_object_get(row, "primary_key");
        if (!json_is_string(id) || !json_is_string(path) || !json_is_string(format) || !json_is_array(primary)) {
            throw std::runtime_error("schema catalog row is malformed");
        }
        CatalogSpec spec;
        spec.artifact_id = json_string_value(id);
        spec.relative_path = json_string_value(path);
        spec.format = json_string_value(format);
        for (std::size_t key = 0; key < json_array_size(primary); ++key) {
            const json_t* field = json_array_get(primary, key);
            if (!json_is_string(field)) {
                throw std::runtime_error("catalog primary key is malformed");
            }
            spec.primary_key.emplace_back(json_string_value(field));
        }
        const json_t* index_path = json_object_get(row, "index");
        if (json_is_string(index_path)) {
            spec.index_path = std::filesystem::path(json_string_value(index_path));
        } else if (!json_is_null(index_path)) {
            throw std::runtime_error("catalog index path is malformed");
        }
        result.push_back(std::move(spec));
    }
    return result;
}

[[nodiscard]] const CatalogSpec& find_spec(const std::vector<CatalogSpec>& catalog, std::string_view id) {
    const auto found =
        std::find_if(catalog.begin(), catalog.end(), [&](const CatalogSpec& spec) { return spec.artifact_id == id; });
    if (found == catalog.end()) {
        throw std::runtime_error("artifact is absent from schema catalog: " + std::string(id));
    }
    return *found;
}

[[nodiscard]] std::vector<std::string> json_key(const json_t* root, const std::vector<std::string>& fields) {
    std::vector<std::string> key;
    key.reserve(fields.size());
    for (const auto& dotted : fields) {
        const json_t* current = root;
        std::size_t begin = 0;
        while (begin < dotted.size()) {
            const std::size_t dot = dotted.find('.', begin);
            const std::string part = dotted.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin);
            current = json_is_object(current) ? json_object_get(current, part.c_str()) : nullptr;
            if (current == nullptr || dot == std::string::npos) {
                break;
            }
            begin = dot + 1U;
        }
        if (json_is_string(current)) {
            key.emplace_back(json_string_value(current));
        } else if (json_is_integer(current) && json_integer_value(current) >= 0) {
            key.push_back(std::to_string(json_integer_value(current)));
        } else {
            throw std::runtime_error("JSON primary-key field is absent or unsupported: " + dotted);
        }
    }
    return key;
}

template <typename Visit>
void visit_bgzf_lines(const std::filesystem::path& path, Visit visit) {
    BGZF* input = bgzf_open(path.c_str(), "r");
    if (input == nullptr) {
        throw std::runtime_error("cannot open BGZF: " + path.string());
    }
    kstring_t line{0, 0, nullptr};
    try {
        while (true) {
            const int status = bgzf_getline(input, '\n', &line);
            if (status < 0) {
                if (status < -1) {
                    throw std::runtime_error("cannot read BGZF: " + path.string());
                }
                break;
            }
            visit(std::string_view(line.s, line.l));
        }
    } catch (...) {
        std::free(line.s);
        bgzf_close(input);
        throw;
    }
    std::free(line.s);
    if (bgzf_close(input) != 0) {
        throw std::runtime_error("cannot close BGZF: " + path.string());
    }
}

struct IndexBounds {
    std::uint64_t first_begin{0};
    std::uint64_t last_begin{0};
    std::uint64_t last_end{0};
};

[[nodiscard]] std::uint64_t parse_index_uint(std::string_view value, std::string_view field) {
    if (value.empty() || value.front() == '+' || (value.size() > 1U && value.front() == '0')) {
        throw std::runtime_error("site index contains non-canonical " + std::string(field));
    }
    std::uint64_t parsed = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
        throw std::runtime_error("site index contains invalid " + std::string(field));
    }
    return parsed;
}

[[nodiscard]] IndexBounds indexed_bounds(const SiteIndexWriteReceipt& index, std::string_view artifact_id) {
    IndexBounds bounds;
    std::uint64_t rows = 0;
    std::uint64_t previous_begin = 0;
    visit_bgzf_lines(index.path, [&](std::string_view line) {
        if (line.empty() || line.front() == '#') {
            return;
        }
        const auto fields = split_tsv(line);
        if (fields.size() != 7U || fields[0] != artifact_id) {
            throw std::runtime_error("site index row identity/cardinality is malformed");
        }
        const std::uint64_t begin = parse_index_uint(fields[3], "first_virtual_offset");
        const std::uint64_t end = parse_index_uint(fields[4], "past_end_virtual_offset");
        const std::uint64_t logical_rows = parse_index_uint(fields[5], "logical_rows");
        if (begin >= end || logical_rows == 0U || (rows != 0U && begin < previous_begin)) {
            throw std::runtime_error("site index virtual-offset range is malformed");
        }
        if (rows == 0U) {
            bounds.first_begin = begin;
        }
        bounds.last_begin = begin;
        bounds.last_end = end;
        previous_begin = begin;
        ++rows;
    });
    if (rows == 0U || rows != index.row_count) {
        throw std::runtime_error("site index row count differs from writer receipt");
    }
    return bounds;
}

[[nodiscard]] std::string read_bgzf_line(BGZF* input, const std::filesystem::path& path) {
    kstring_t line{0, 0, nullptr};
    const int status = bgzf_getline(input, '\n', &line);
    if (status < 0) {
        std::free(line.s);
        throw std::runtime_error("cannot read indexed BGZF row: " + path.string());
    }
    std::string output(line.s, line.l);
    std::free(line.s);
    return output;
}

[[nodiscard]] std::vector<std::string> tsv_key_from_line(std::string_view line, const std::vector<std::string>& header,
                                                         const std::vector<std::string>& primary_key) {
    const auto fields = split_tsv(line);
    std::vector<std::string> key;
    key.reserve(primary_key.size());
    for (const auto& name : primary_key) {
        const auto found = std::find(header.begin(), header.end(), name);
        if (found == header.end()) {
            throw std::runtime_error("TSV primary-key column is absent: " + name);
        }
        const std::size_t column = static_cast<std::size_t>(std::distance(header.begin(), found));
        if (column >= fields.size()) {
            throw std::runtime_error("TSV row is shorter than its primary key");
        }
        key.push_back(fields[column]);
    }
    return key;
}

[[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> tsv_primary_range(
    const std::filesystem::path& path, const SiteIndexWriteReceipt& index, std::string_view artifact_id,
    const std::vector<std::string>& primary_key) {
    const IndexBounds bounds = indexed_bounds(index, artifact_id);
    BGZF* input = bgzf_open(path.c_str(), "r");
    if (input == nullptr) {
        throw std::runtime_error("cannot open BGZF: " + path.string());
    }
    std::vector<std::string> header;
    try {
        while (header.empty()) {
            const std::string line = read_bgzf_line(input, path);
            if (line.rfind("##", 0) == 0U) {
                continue;
            }
            if (line.empty() || line.front() != '#') {
                throw std::runtime_error("TSV BGZF lacks a header");
            }
            header = split_tsv(std::string_view(line).substr(1));
        }
        if (bgzf_seek(input, static_cast<std::int64_t>(bounds.first_begin), SEEK_SET) < 0) {
            throw std::runtime_error("cannot seek first indexed TSV row");
        }
        auto first = tsv_key_from_line(read_bgzf_line(input, path), header, primary_key);
        if (bgzf_seek(input, static_cast<std::int64_t>(bounds.last_begin), SEEK_SET) < 0) {
            throw std::runtime_error("cannot seek last indexed TSV group");
        }
        std::vector<std::string> last;
        while (true) {
            const std::int64_t before = bgzf_tell(input);
            if (before < 0 || static_cast<std::uint64_t>(before) >= bounds.last_end) {
                break;
            }
            last = tsv_key_from_line(read_bgzf_line(input, path), header, primary_key);
            const std::int64_t after = bgzf_tell(input);
            if (after <= before || static_cast<std::uint64_t>(after) > bounds.last_end) {
                throw std::runtime_error("indexed TSV group crosses its declared range");
            }
        }
        if (last.empty() || static_cast<std::uint64_t>(bgzf_tell(input)) != bounds.last_end) {
            throw std::runtime_error("indexed TSV last group does not end at declared offset");
        }
        if (bgzf_close(input) != 0) {
            input = nullptr;
            throw std::runtime_error("cannot close BGZF: " + path.string());
        }
        input = nullptr;
        return {std::move(first), std::move(last)};
    } catch (...) {
        if (input != nullptr) {
            bgzf_close(input);
        }
        throw;
    }
}

[[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> jsonl_primary_range(
    const std::filesystem::path& path, const SiteIndexWriteReceipt& index, std::string_view artifact_id,
    const std::vector<std::string>& primary_key) {
    const IndexBounds bounds = indexed_bounds(index, artifact_id);
    BGZF* input = bgzf_open(path.c_str(), "r");
    if (input == nullptr) {
        throw std::runtime_error("cannot open BGZF: " + path.string());
    }
    const auto parse_key = [&](std::string_view line) {
        json_error_t error{};
        JsonPtr record(json_loadb(line.data(), line.size(), JSON_REJECT_DUPLICATES, &error), json_decref);
        if (!record || !json_is_object(record.get())) {
            throw std::runtime_error("closeout cannot parse JSONL primary key");
        }
        return json_key(record.get(), primary_key);
    };
    try {
        if (bgzf_seek(input, static_cast<std::int64_t>(bounds.first_begin), SEEK_SET) < 0) {
            throw std::runtime_error("cannot seek first indexed JSONL row");
        }
        auto first = parse_key(read_bgzf_line(input, path));
        if (bgzf_seek(input, static_cast<std::int64_t>(bounds.last_begin), SEEK_SET) < 0) {
            throw std::runtime_error("cannot seek last indexed JSONL group");
        }
        std::vector<std::string> last;
        while (true) {
            const std::int64_t before = bgzf_tell(input);
            if (before < 0 || static_cast<std::uint64_t>(before) >= bounds.last_end) {
                break;
            }
            last = parse_key(read_bgzf_line(input, path));
            const std::int64_t after = bgzf_tell(input);
            if (after <= before || static_cast<std::uint64_t>(after) > bounds.last_end) {
                throw std::runtime_error("indexed JSONL group crosses its declared range");
            }
        }
        if (last.empty() || static_cast<std::uint64_t>(bgzf_tell(input)) != bounds.last_end) {
            throw std::runtime_error("indexed JSONL last group does not end at declared offset");
        }
        if (bgzf_close(input) != 0) {
            input = nullptr;
            throw std::runtime_error("cannot close BGZF: " + path.string());
        }
        input = nullptr;
        return {std::move(first), std::move(last)};
    } catch (...) {
        if (input != nullptr) {
            bgzf_close(input);
        }
        throw;
    }
}

[[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> llm_primary_range(
    const std::filesystem::path& index_path) {
    std::vector<std::string> first;
    std::vector<std::string> last;
    visit_bgzf_lines(index_path, [&](std::string_view line) {
        if (line.empty() || line.front() == '#') {
            return;
        }
        const auto fields = split_tsv(line);
        if (fields.size() != 7U) {
            throw std::runtime_error("LLM site index row is malformed");
        }
        std::vector<std::string> key{fields[1], fields[2]};
        if (first.empty()) {
            first = key;
        }
        last = std::move(key);
    });
    return {std::move(first), std::move(last)};
}

[[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> primary_range(
    const DatasetArtifactWriteReceipt& receipt, const CatalogSpec& spec) {
    if (receipt.logical_rows == 0U) {
        return {};
    }
    if (spec.format == "TSV_BGZF") {
        return tsv_primary_range(receipt.path, receipt.index, receipt.artifact_id, spec.primary_key);
    }
    if (spec.format == "JSONL_BGZF") {
        return jsonl_primary_range(receipt.path, receipt.index, receipt.artifact_id, spec.primary_key);
    }
    if (spec.format == "LLM_BGZF") {
        return llm_primary_range(receipt.index.path);
    }
    if (spec.format == "JSON") {
        const JsonPtr value = load_json(receipt.path);
        const auto key = json_key(value.get(), spec.primary_key);
        return {key, key};
    }
    throw std::runtime_error("unsupported scientific artifact format in closeout: " + spec.format);
}

[[nodiscard]] std::filesystem::path relative_under(const std::filesystem::path& root,
                                                   const std::filesystem::path& path) {
    const std::filesystem::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        throw std::runtime_error("artifact path is outside staging root: " + path.string());
    }
    return relative;
}

[[nodiscard]] JsonPtr input_json(const std::vector<ArtifactInputBinding>& inputs) {
    JsonPtr array(json_array(), json_decref);
    for (const auto& input : inputs) {
        JsonPtr row(json_object(), json_decref);
        json_object_set_new(row.get(), "source_kind", json_string(input.source_kind.c_str()));
        json_object_set_new(row.get(), "source_id", json_string(input.source_id.c_str()));
        json_object_set_new(row.get(), "digest_kind", json_string(input.digest_kind.c_str()));
        json_object_set_new(row.get(), "sha256", json_string(input.sha256.c_str()));
        json_array_append_new(array.get(), row.release());
    }
    return array;
}

[[nodiscard]] JsonPtr string_array_json(const std::vector<std::string>& values) {
    JsonPtr array(json_array(), json_decref);
    for (const auto& value : values) {
        json_array_append_new(array.get(), json_string(value.c_str()));
    }
    return array;
}

[[nodiscard]] JsonPtr artifact_record_json(const ArtifactMeta& meta, const std::filesystem::path& root,
                                           std::string_view producer_sha) {
    JsonPtr record(json_object(), json_decref);
    json_object_set_new(record.get(), "artifact_id", json_string(meta.receipt.artifact_id.c_str()));
    json_object_set_new(record.get(), "role", json_string(meta.receipt.artifact_id.c_str()));
    const std::string relative = relative_under(root, meta.receipt.path).generic_string();
    json_object_set_new(record.get(), "relative_path", json_string(relative.c_str()));
    json_object_set_new(record.get(), "schema_name", json_string(meta.receipt.schema_name.c_str()));
    json_object_set_new(record.get(), "schema_version", json_string(meta.receipt.schema_version.c_str()));
    json_object_set_new(record.get(), "format", json_string(meta.receipt.format.c_str()));
    json_object_set_new(record.get(), "size_bytes", json_integer(static_cast<json_int_t>(meta.receipt.physical_bytes)));
    json_object_set_new(record.get(), "physical_sha256", json_string(meta.receipt.physical_sha256.c_str()));
    json_object_set_new(record.get(), "logical_rows", json_integer(static_cast<json_int_t>(meta.receipt.logical_rows)));
    json_object_set_new(record.get(), "semantic_sha256", json_string(meta.receipt.semantic_sha256.c_str()));
    if (meta.receipt.index.path.empty()) {
        json_object_set_new(record.get(), "index", json_null());
    } else {
        JsonPtr index(json_object(), json_decref);
        const std::string index_relative = relative_under(root, meta.receipt.index.path).generic_string();
        json_object_set_new(index.get(), "relative_path", json_string(index_relative.c_str()));
        json_object_set_new(index.get(), "schema_name", json_string("longlineage.site_index"));
        json_object_set_new(index.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(index.get(), "size_bytes",
                            json_integer(static_cast<json_int_t>(meta.receipt.index.physical_bytes)));
        json_object_set_new(index.get(), "physical_sha256", json_string(meta.receipt.index.physical_sha256.c_str()));
        json_object_set_new(index.get(), "logical_rows",
                            json_integer(static_cast<json_int_t>(meta.receipt.index.row_count)));
        json_object_set_new(index.get(), "semantic_sha256", json_string(meta.receipt.index.semantic_sha256.c_str()));
        json_object_set_new(record.get(), "index", index.release());
    }
    json_object_set_new(record.get(), "sensitivity", json_string("REAL_RESTRICTED"));
    json_object_set_new(record.get(), "transform_id", json_string(meta.transform_id.c_str()));
    json_object_set_new(record.get(), "producer_executable_sha256", json_string(std::string(producer_sha).c_str()));
    json_object_set_new(record.get(), "inputs", input_json(meta.inputs).release());
    if (meta.receipt.logical_rows == 0U) {
        json_object_set_new(record.get(), "primary_key_first", json_null());
        json_object_set_new(record.get(), "primary_key_last", json_null());
    } else {
        json_object_set_new(record.get(), "primary_key_first", string_array_json(meta.first).release());
        json_object_set_new(record.get(), "primary_key_last", string_array_json(meta.last).release());
    }
    return record;
}

[[nodiscard]] DatasetArtifactWriteReceipt write_jsonl_bgzf(const std::filesystem::path& path, std::string artifact_id,
                                                           std::string schema_name, std::string schema_version,
                                                           const std::vector<std::string>& canonical_records) {
    std::filesystem::create_directories(path.parent_path());
    BGZF* output = bgzf_open(path.c_str(), "w");
    if (output == nullptr) {
        throw std::runtime_error("cannot open closeout BGZF: " + path.string());
    }
    std::string semantic = schema_name + "\t" + schema_version + "\n";
    try {
        for (const auto& record : canonical_records) {
            const std::string line = record + "\n";
            if (bgzf_write(output, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
                throw std::runtime_error("short closeout BGZF write: " + path.string());
            }
            semantic.append(line);
        }
    } catch (...) {
        bgzf_close(output);
        throw;
    }
    if (bgzf_close(output) != 0) {
        throw std::runtime_error("cannot close closeout BGZF: " + path.string());
    }
    return {
        std::move(artifact_id),
        path,
        std::move(schema_name),
        std::move(schema_version),
        "JSONL_BGZF",
        static_cast<std::uint64_t>(canonical_records.size()),
        static_cast<std::uint64_t>(semantic.size()),
        std::filesystem::file_size(path),
        digest_bytes(semantic),
        digest_file(path),
        {},
    };
}

[[nodiscard]] DatasetArtifactWriteReceipt write_semantic_digests(const std::filesystem::path& path,
                                                                 const std::vector<ArtifactMeta>& source) {
    std::vector<const ArtifactMeta*> ordered;
    for (const auto& meta : source) {
        ordered.push_back(&meta);
    }
    std::sort(ordered.begin(), ordered.end(), [](const ArtifactMeta* left, const ArtifactMeta* right) {
        return left->receipt.artifact_id < right->receipt.artifact_id;
    });
    const std::string header =
        "artifact_id\tschema_name\tschema_version\tlogical_rows\t"
        "semantic_sha256";
    std::string physical = header + "\n";
    for (const auto* meta : ordered) {
        physical.append(meta->receipt.artifact_id);
        physical.push_back('\t');
        physical.append(meta->receipt.schema_name);
        physical.push_back('\t');
        physical.append(meta->receipt.schema_version);
        physical.push_back('\t');
        physical.append(std::to_string(meta->receipt.logical_rows));
        physical.push_back('\t');
        physical.append(meta->receipt.semantic_sha256);
        physical.push_back('\n');
    }
    write_text(path, physical);
    const std::string semantic = "longlineage.semantic_digest\t1.0.0\n" + physical;
    return {
        "semantic_digests",
        path,
        "longlineage.semantic_digest",
        "1.0.0",
        "TSV",
        static_cast<std::uint64_t>(ordered.size()),
        static_cast<std::uint64_t>(semantic.size()),
        std::filesystem::file_size(path),
        digest_bytes(semantic),
        digest_file(path),
        {},
    };
}

[[nodiscard]] ArtifactInputBinding run_input(const ArtifactMeta& source) {
    return {"RUN_ARTIFACT", source.receipt.artifact_id, "SEMANTIC_SHA256", source.receipt.semantic_sha256};
}

[[nodiscard]] const ArtifactMeta& meta_by_id(const std::vector<ArtifactMeta>& rows, std::string_view id) {
    const auto found =
        std::find_if(rows.begin(), rows.end(), [&](const ArtifactMeta& row) { return row.receipt.artifact_id == id; });
    if (found == rows.end()) {
        throw std::runtime_error("closeout dependency artifact is absent: " + std::string(id));
    }
    return *found;
}

[[nodiscard]] std::vector<ArtifactInputBinding> dependencies(const std::vector<ArtifactMeta>& science,
                                                             std::string_view id,
                                                             const std::vector<ArtifactInputBinding>& manifest_inputs) {
    if (id == "site_reads") {
        return manifest_inputs;
    }
    std::vector<std::string> ids;
    if (id == "methyl_calls") {
        ids = {"site_reads"};
    } else if (id == "bernoulli_upper" || id == "m1_sites" || id == "m1_assignments") {
        ids = {"methyl_calls"};
    } else if (id == "cooccurrence_pairs") {
        ids = {"site_reads", "m1_sites", "m1_assignments"};
    } else if (id == "cooccurrence_sites") {
        ids = {"cooccurrence_pairs", "m1_sites"};
    } else if (id == "topology_units") {
        ids = {"site_reads", "cooccurrence_pairs", "cooccurrence_sites"};
    } else if (id == "summary") {
        ids = {"site_reads",         "methyl_calls",       "m1_sites",      "m1_assignments",
               "cooccurrence_pairs", "cooccurrence_sites", "topology_units"};
    } else {
        throw std::runtime_error("unknown scientific closeout dependency: " + std::string(id));
    }
    std::vector<ArtifactInputBinding> output;
    for (const auto& dependency : ids) {
        output.push_back(run_input(meta_by_id(science, dependency)));
    }
    return output;
}

[[nodiscard]] std::string transform_for(std::string_view id) {
    if (id == "site_reads") {
        return "raw_alignment_to_site_reads";
    }
    if (id == "methyl_calls") {
        return "site_reads_to_methyl_calls";
    }
    if (id == "bernoulli_upper" || id == "m1_sites" || id == "m1_assignments") {
        return "methyl_calls_to_m1";
    }
    if (id == "cooccurrence_pairs" || id == "cooccurrence_sites") {
        return "m1_to_cooccurrence";
    }
    if (id == "topology_units") {
        return "cooccurrence_to_topology";
    }
    if (id == "summary") {
        return "run_artifacts_to_summary";
    }
    throw std::runtime_error("unknown scientific transform: " + std::string(id));
}

[[nodiscard]] JsonPtr performance_json(const ProducerPerformance& performance) {
    JsonPtr object(json_object(), json_decref);
    json_object_set_new(object.get(), "wall_seconds", json_real(performance.wall_seconds));
    json_object_set_new(object.get(), "user_seconds", json_real(performance.user_seconds));
    json_object_set_new(object.get(), "system_seconds", json_real(performance.system_seconds));
    json_object_set_new(object.get(), "memory_peak_bytes",
                        json_integer(static_cast<json_int_t>(performance.memory_peak_bytes)));
    json_object_set_new(object.get(), "oom_events", json_integer(static_cast<json_int_t>(performance.oom_events)));
    json_object_set_new(object.get(), "io_read_bytes",
                        json_integer(static_cast<json_int_t>(performance.io_read_bytes)));
    json_object_set_new(object.get(), "io_write_bytes",
                        json_integer(static_cast<json_int_t>(performance.io_write_bytes)));
    json_object_set_new(object.get(), "major_page_faults",
                        json_integer(static_cast<json_int_t>(performance.major_page_faults)));
    json_object_set_new(object.get(), "minor_page_faults",
                        json_integer(static_cast<json_int_t>(performance.minor_page_faults)));
    json_object_set_new(object.get(), "peak_threads", json_integer(static_cast<json_int_t>(performance.peak_threads)));
    json_object_set_new(object.get(), "queue_wait_seconds", json_real(performance.queue_wait_seconds));
    json_object_set_new(object.get(), "reorder_wait_seconds", json_real(performance.reorder_wait_seconds));
    JsonPtr latency(json_object(), json_decref);
    json_object_set_new(latency.get(), "p50", json_real(performance.task_latency_p50_seconds));
    json_object_set_new(latency.get(), "p95", json_real(performance.task_latency_p95_seconds));
    json_object_set_new(latency.get(), "p99", json_real(performance.task_latency_p99_seconds));
    json_object_set_new(latency.get(), "max", json_real(performance.task_latency_max_seconds));
    json_object_set_new(object.get(), "task_latency_seconds", latency.release());
    json_object_set_new(object.get(), "logical_records",
                        json_integer(static_cast<json_int_t>(performance.logical_records)));
    json_object_set_new(object.get(), "logical_bytes",
                        json_integer(static_cast<json_int_t>(performance.logical_bytes)));
    json_object_set_new(object.get(), "final_file_count",
                        json_integer(static_cast<json_int_t>(performance.final_file_count)));
    json_object_set_new(object.get(), "transient_file_count",
                        json_integer(static_cast<json_int_t>(performance.transient_file_count)));
    json_object_set_new(object.get(), "cache_condition", json_string(performance.cache_condition.c_str()));
    return object;
}

[[nodiscard]] JsonPtr run_receipt_draft(const DatasetCloseoutOptions& options) {
    JsonPtr draft(json_object(), json_decref);
    JsonPtr executable(json_object(), json_decref);
    json_object_set_new(executable.get(), "name", json_string("longlineage"));
    json_object_set_new(executable.get(), "version", json_string(options.executable.version.c_str()));
    json_object_set_new(executable.get(), "git_commit", json_string(options.executable.git_commit.c_str()));
    json_object_set_new(executable.get(), "executable_sha256",
                        json_string(options.executable.executable_sha256.c_str()));
    json_object_set_new(executable.get(), "compiler", json_string(options.executable.compiler.c_str()));
    json_object_set_new(executable.get(), "htslib_version", json_string(options.executable.htslib_version.c_str()));
    json_object_set_new(draft.get(), "production_executable", executable.release());
    json_object_set_new(draft.get(), "input_lock_sha256", json_string(options.input_lock_sha256.c_str()));
    json_object_set_new(draft.get(), "phase_ledger_sha256", json_string(options.phase_ledger_sha256.c_str()));
    json_object_set_new(draft.get(), "performance", performance_json(options.performance).release());
    return draft;
}

[[nodiscard]] JsonPtr mount_identity_json(const std::vector<InputMountIdentity>& mounts) {
    JsonPtr array(json_array(), json_decref);
    for (const auto& mount : mounts) {
        JsonPtr row(json_object(), json_decref);
        json_object_set_new(row.get(), "dataset_id", json_string(mount.dataset_id.c_str()));
        json_object_set_new(row.get(), "role", json_string(mount.role.c_str()));
        json_object_set_new(row.get(), "canonical_path", json_string(mount.canonical_path.string().c_str()));
        json_object_set_new(row.get(), "mount_source", json_string(mount.mount_source.c_str()));
        json_object_set_new(row.get(), "filesystem_type", json_string(mount.filesystem_type.c_str()));
        json_object_set_new(row.get(), "readonly", json_boolean(mount.readonly));
        json_object_set_new(row.get(), "mount_options_sha256", json_string(mount.mount_options_sha256.c_str()));
        json_array_append_new(array.get(), row.release());
    }
    return array;
}

}  // namespace

ParseResult<DatasetCloseoutReceipt> write_dataset_producer_closeout(
    const std::vector<DatasetArtifactWriteReceipt>& scientific_artifacts, const DatasetCloseoutOptions& options) {
    try {
        if (!options.repo_root.is_absolute() || !options.staging_root.is_absolute() || options.run_id.empty() ||
            scientific_artifacts.size() != 9U || options.input_mount_identity.size() < 8U ||
            options.input_snapshot_before_sha256 != options.input_snapshot_after_sha256) {
            return ParseResult<DatasetCloseoutReceipt>::failure(
                ParseReason::kMalformedValue, "dataset closeout options or scientific artifact set are invalid");
        }
        const auto catalog = load_catalog(options.repo_root);
        const std::set<std::string> expected{"site_reads",         "methyl_calls",   "bernoulli_upper",
                                             "m1_sites",           "m1_assignments", "cooccurrence_pairs",
                                             "cooccurrence_sites", "topology_units", "summary"};
        std::set<std::string> observed;
        std::vector<ArtifactMeta> science;
        science.reserve(scientific_artifacts.size());
        for (const auto& receipt : scientific_artifacts) {
            if (!observed.insert(receipt.artifact_id).second) {
                throw std::runtime_error("duplicate scientific artifact receipt");
            }
            const auto& spec = find_spec(catalog, receipt.artifact_id);
            if (relative_under(options.staging_root, receipt.path) != spec.relative_path ||
                receipt.format != spec.format ||
                (spec.index_path.has_value() &&
                 relative_under(options.staging_root, receipt.index.path) != *spec.index_path) ||
                (!spec.index_path.has_value() && !receipt.index.path.empty())) {
                throw std::runtime_error(receipt.artifact_id + ": scientific receipt differs from catalog");
            }
            auto range = primary_range(receipt, spec);
            ArtifactMeta meta;
            meta.receipt = receipt;
            meta.first = std::move(range.first);
            meta.last = std::move(range.second);
            meta.transform_id = transform_for(receipt.artifact_id);
            science.push_back(std::move(meta));
        }
        if (observed != expected) {
            throw std::runtime_error("scientific artifact receipt set is not canonical");
        }
        for (auto& meta : science) {
            meta.inputs = dependencies(science, meta.receipt.artifact_id, options.manifest_inputs);
        }

        std::vector<const ArtifactMeta*> ordered_science;
        for (const auto& meta : science) {
            ordered_science.push_back(&meta);
        }
        std::sort(ordered_science.begin(), ordered_science.end(),
                  [](const ArtifactMeta* left, const ArtifactMeta* right) {
                      return left->receipt.artifact_id < right->receipt.artifact_id;
                  });

        std::vector<std::string> catalog_records;
        for (const auto* meta : ordered_science) {
            JsonPtr wrapper(json_object(), json_decref);
            json_object_set_new(wrapper.get(), "schema_name", json_string("longlineage.artifact_catalog_record"));
            json_object_set_new(wrapper.get(), "schema_version", json_string("1.0.0"));
            json_object_set_new(wrapper.get(), "run_id", json_string(options.run_id.c_str()));
            json_object_set_new(
                wrapper.get(), "artifact",
                artifact_record_json(*meta, options.staging_root, options.executable.executable_sha256).release());
            catalog_records.push_back(dump_preserve(wrapper.get()));
        }
        ArtifactMeta artifact_catalog;
        artifact_catalog.receipt =
            write_jsonl_bgzf(options.staging_root / "artifact_catalog.jsonl.bgz", "artifact_catalog",
                             "longlineage.artifact_catalog_record", "1.0.0", catalog_records);
        artifact_catalog.first = {ordered_science.front()->receipt.artifact_id};
        artifact_catalog.last = {ordered_science.back()->receipt.artifact_id};
        artifact_catalog.transform_id = "scientific_artifacts_to_catalog";
        for (const auto* meta : ordered_science) {
            artifact_catalog.inputs.push_back(run_input(*meta));
        }

        std::vector<std::string> lineage_records;
        for (const auto* meta : ordered_science) {
            JsonPtr record(json_object(), json_decref);
            json_object_set_new(record.get(), "schema_name", json_string("longlineage.data_lineage_record"));
            json_object_set_new(record.get(), "schema_version", json_string("1.0.0"));
            json_object_set_new(record.get(), "run_id", json_string(options.run_id.c_str()));
            json_object_set_new(record.get(), "transform_id", json_string(meta->transform_id.c_str()));
            json_object_set_new(record.get(), "output_artifact_id", json_string(meta->receipt.artifact_id.c_str()));
            json_object_set_new(record.get(), "output_semantic_sha256",
                                json_string(meta->receipt.semantic_sha256.c_str()));
            json_object_set_new(record.get(), "inputs", input_json(meta->inputs).release());
            json_object_set_new(record.get(), "producer_executable_sha256",
                                json_string(options.executable.executable_sha256.c_str()));
            lineage_records.push_back(dump_preserve(record.get()));
        }
        ArtifactMeta data_lineage;
        data_lineage.receipt = write_jsonl_bgzf(options.staging_root / "data_lineage.jsonl.bgz", "data_lineage",
                                                "longlineage.data_lineage_record", "1.0.0", lineage_records);
        data_lineage.first = {ordered_science.front()->receipt.artifact_id};
        data_lineage.last = {ordered_science.back()->receipt.artifact_id};
        data_lineage.transform_id = "scientific_artifacts_to_lineage";
        for (const auto* meta : ordered_science) {
            data_lineage.inputs.push_back(run_input(*meta));
        }

        std::vector<ArtifactMeta> digest_source = science;
        digest_source.push_back(artifact_catalog);
        digest_source.push_back(data_lineage);
        ArtifactMeta semantic_digests;
        semantic_digests.receipt = write_semantic_digests(options.staging_root / "semantic_digests.tsv", digest_source);
        std::vector<const ArtifactMeta*> ordered_digest;
        for (const auto& meta : digest_source) {
            ordered_digest.push_back(&meta);
        }
        std::sort(ordered_digest.begin(), ordered_digest.end(),
                  [](const ArtifactMeta* left, const ArtifactMeta* right) {
                      return left->receipt.artifact_id < right->receipt.artifact_id;
                  });
        semantic_digests.first = {ordered_digest.front()->receipt.artifact_id};
        semantic_digests.last = {ordered_digest.back()->receipt.artifact_id};
        semantic_digests.transform_id = "artifacts_to_semantic_digests";
        for (const auto* meta : ordered_digest) {
            semantic_digests.inputs.push_back(run_input(*meta));
        }

        std::vector<ArtifactMeta> all = science;
        all.push_back(std::move(artifact_catalog));
        all.push_back(std::move(data_lineage));
        all.push_back(std::move(semantic_digests));
        std::sort(all.begin(), all.end(), [](const ArtifactMeta& left, const ArtifactMeta& right) {
            return left.receipt.artifact_id < right.receipt.artifact_id;
        });

        JsonPtr receipt(json_object(), json_decref);
        json_object_set_new(receipt.get(), "schema_name", json_string("longlineage.producer_receipt"));
        json_object_set_new(receipt.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(receipt.get(), "run_id", json_string(options.run_id.c_str()));
        json_object_set_new(receipt.get(), "state", json_string("RUNNING"));
        json_object_set_new(receipt.get(), "producer_outcome", json_string("READY_FOR_VALIDATION"));
        json_object_set_new(receipt.get(), "producer_executable_sha256",
                            json_string(options.executable.executable_sha256.c_str()));
        json_object_set_new(receipt.get(), "producer_hostname", json_string(options.producer_hostname.c_str()));
        json_object_set_new(receipt.get(), "producer_kernel_release",
                            json_string(options.producer_kernel_release.c_str()));
        json_object_set_new(receipt.get(), "input_mount_identity",
                            mount_identity_json(options.input_mount_identity).release());
        json_object_set_new(receipt.get(), "manifest_sha256", json_string(options.manifest_sha256.c_str()));
        json_object_set_new(receipt.get(), "input_snapshot_before_sha256",
                            json_string(options.input_snapshot_before_sha256.c_str()));
        json_object_set_new(receipt.get(), "input_snapshot_after_sha256",
                            json_string(options.input_snapshot_after_sha256.c_str()));
        json_object_set_new(receipt.get(), "schema_catalog_sha256",
                            json_string(digest_file(options.repo_root / "schema/catalog.json").c_str()));
        json_object_set_new(
            receipt.get(), "science_parameters_sha256",
            json_string(digest_file(options.repo_root / "contracts/v1/science_parameters.json").c_str()));
        json_object_set_new(receipt.get(), "run_receipt_draft", run_receipt_draft(options).release());
        JsonPtr artifact_rows(json_array(), json_decref);
        for (const auto& meta : all) {
            json_array_append_new(
                artifact_rows.get(),
                artifact_record_json(meta, options.staging_root, options.executable.executable_sha256).release());
        }
        json_object_set_new(receipt.get(), "artifacts", artifact_rows.release());
        json_object_set_new(receipt.get(),
                            "tr"
                            "uth_fields_seen",
                            json_integer(0));
        json_object_set_new(receipt.get(), "failure_reason", json_null());
        json_object_set_new(receipt.get(), "finished_at", json_string(options.finished_at.c_str()));

        const std::filesystem::path producer_path = options.staging_root / "receipts/producer_receipt.json";
        write_text(producer_path, dump_preserve(receipt.get()) + "\n");

        std::map<std::string, std::string> checksum_rows;
        for (const auto& meta : all) {
            const std::string relative = relative_under(options.staging_root, meta.receipt.path).generic_string();
            checksum_rows.emplace(relative, meta.receipt.physical_sha256);
            if (!meta.receipt.index.path.empty()) {
                const std::string index_relative =
                    relative_under(options.staging_root, meta.receipt.index.path).generic_string();
                checksum_rows.emplace(index_relative, meta.receipt.index.physical_sha256);
            }
        }
        checksum_rows.emplace("receipts/producer_receipt.json", digest_file(producer_path));
        std::string checksums;
        for (const auto& [path, sha256] : checksum_rows) {
            checksums.append(sha256);
            checksums.append("  ");
            checksums.append(path);
            checksums.push_back('\n');
        }
        const std::filesystem::path checksums_path = options.staging_root / "checksums.sha256";
        write_text(checksums_path, checksums);

        DatasetCloseoutReceipt output;
        for (const auto& meta : all) {
            output.artifacts.push_back(meta.receipt);
        }
        output.producer_receipt_path = producer_path;
        output.producer_receipt_sha256 = digest_file(producer_path);
        output.checksums_path = checksums_path;
        output.checksums_sha256 = digest_file(checksums_path);
        return ParseResult<DatasetCloseoutReceipt>::success(std::move(output));
    } catch (const std::exception& error) {
        return ParseResult<DatasetCloseoutReceipt>::failure(
            ParseReason::kIoError, std::string("dataset producer closeout failed: ") + error.what());
    }
}

}  // namespace longlineage::artifact
