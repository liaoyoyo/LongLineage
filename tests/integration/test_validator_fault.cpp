// SPDX-License-Identifier: GPL-3.0-only

#include <fcntl.h>
#include <htslib/bgzf.h>
#include <htslib/kstring.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

struct JsonDeleter {
    void operator()(json_t* value) const noexcept {
        if (value != nullptr) {
            json_decref(value);
        }
    }
};

using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Sha256 final {
   public:
    Sha256() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        check(context_ != nullptr && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1,
              "cannot initialize fixture SHA-256");
    }

    void update(std::string_view bytes) {
        check(bytes.empty() || EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) == 1,
              "cannot update fixture SHA-256");
    }

    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> raw{};
        unsigned int size = 0;
        check(EVP_DigestFinal_ex(context_.get(), raw.data(), &size) == 1 && size == 32U,
              "cannot finalize fixture SHA-256");
        constexpr char kHex[] = "0123456789abcdef";
        std::string digest;
        digest.reserve(64U);
        for (unsigned int index = 0; index < size; ++index) {
            const unsigned char byte = raw[index];
            digest.push_back(kHex[(byte >> 4U) & 0x0fU]);
            digest.push_back(kHex[byte & 0x0fU]);
        }
        return digest;
    }

   private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

std::string sha256_bytes(std::string_view bytes) {
    Sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "cannot hash fixture file: " + path.string());
    Sha256 digest;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            digest.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    check(input.eof(), "fixture hash read failed");
    return digest.finish();
}

void write_text(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "cannot create fixture file: " + path.string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(output), "cannot write fixture file: " + path.string());
}

std::string dump_json(const json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    check(encoded != nullptr, "cannot encode fixture JSON");
    std::string result(encoded);
    std::free(encoded);
    return result;
}

void write_json(const std::filesystem::path& path, const json_t* value) { write_text(path, dump_json(value) + "\n"); }

JsonPtr load_json(const std::filesystem::path& path) {
    json_error_t error{};
    JsonPtr value(json_load_file(path.c_str(), JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &error));
    check(value != nullptr, "cannot reopen fixture JSON: " + path.string());
    return value;
}

struct BgzfWriteResult {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> data_offsets;
};

BgzfWriteResult write_bgzf_lines(const std::filesystem::path& path, const std::vector<std::string>& lines,
                                 std::size_t data_begin) {
    std::filesystem::create_directories(path.parent_path());
    BGZF* output = bgzf_open(path.c_str(), "w");
    check(output != nullptr, "cannot create fixture BGZF");
    for (const std::string& line : lines) {
        const std::string bytes = line + "\n";
        const ssize_t written = bgzf_write(output, bytes.data(), bytes.size());
        check(written == static_cast<ssize_t>(bytes.size()), "fixture BGZF short write");
    }
    check(bgzf_close(output) == 0, "cannot close fixture BGZF");

    // Re-read after close because the past-end virtual offset of the final
    // logical record changes when BGZF flushes the data block and EOF marker.
    BGZF* input = bgzf_open(path.c_str(), "r");
    check(input != nullptr, "cannot reopen fixture BGZF");
    BgzfWriteResult result;
    kstring_t buffer{0, 0, nullptr};
    std::size_t line_index = 0;
    while (true) {
        const int64_t begin = bgzf_tell(input);
        const int length = bgzf_getline(input, '\n', &buffer);
        if (length == -1) {
            break;
        }
        check(length >= 0, "cannot replay fixture BGZF");
        const int64_t end = bgzf_tell(input);
        check(begin >= 0 && end >= 0, "cannot observe fixture virtual offsets");
        if (line_index >= data_begin) {
            result.data_offsets.emplace_back(static_cast<std::uint64_t>(begin), static_cast<std::uint64_t>(end));
        }
        ++line_index;
    }
    std::free(buffer.s);
    check(bgzf_close(input) == 0, "cannot close replayed fixture BGZF");
    check(line_index == lines.size(), "fixture BGZF line count changed after close");
    return result;
}

std::string semantic_tsv(const std::string& schema_name, const std::string& schema_version,
                         const std::vector<std::string>& header, const std::vector<std::string>& rows) {
    std::string bytes = schema_name + "\t" + schema_version + "\n";
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (index != 0U) {
            bytes.push_back('\t');
        }
        bytes.append(header[index]);
    }
    bytes.push_back('\n');
    for (const std::string& row : rows) {
        bytes.append(row);
        bytes.push_back('\n');
    }
    return sha256_bytes(bytes);
}

std::string semantic_jsonl(const std::string& schema_name, const std::string& schema_version,
                           const std::vector<JsonPtr>& rows) {
    std::string bytes = schema_name + "\t" + schema_version + "\n";
    for (const JsonPtr& row : rows) {
        bytes.append(dump_json(row.get()));
        bytes.push_back('\n');
    }
    return sha256_bytes(bytes);
}

enum class Fault {
    kNone,
    kMissing,
    kExtra,
    kTruncate,
    kHash,
    kOrder,
    kDuplicate,
    kForgedReceipt,
    kTruth,
    kIncompleteWinner,
};

struct IndexMeta {
    std::string path;
    std::uint64_t size = 0;
    std::string physical;
    std::uint64_t rows = 0;
    std::string semantic;
};

struct ArtifactMeta {
    std::string id;
    std::string path;
    std::string schema_name;
    std::string schema_version = "1.0.0";
    std::string format;
    std::uint64_t size = 0;
    std::string physical;
    std::uint64_t rows = 0;
    std::string semantic;
    std::optional<IndexMeta> index;
    std::string first;
    std::string last;
    std::string transform;
};

JsonPtr artifact_record(const ArtifactMeta& meta) {
    JsonPtr record(json_object());
    json_object_set_new(record.get(), "artifact_id", json_string(meta.id.c_str()));
    json_object_set_new(record.get(), "format", json_string(meta.format.c_str()));
    JsonPtr index;
    if (meta.index.has_value()) {
        index.reset(json_object());
        json_object_set_new(index.get(), "logical_rows", json_integer(static_cast<json_int_t>(meta.index->rows)));
        json_object_set_new(index.get(), "physical_sha256", json_string(meta.index->physical.c_str()));
        json_object_set_new(index.get(), "relative_path", json_string(meta.index->path.c_str()));
        json_object_set_new(index.get(), "schema_name", json_string("longlineage.site_index"));
        json_object_set_new(index.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(index.get(), "semantic_sha256", json_string(meta.index->semantic.c_str()));
        json_object_set_new(index.get(), "size_bytes", json_integer(static_cast<json_int_t>(meta.index->size)));
    }
    json_object_set_new(record.get(), "index", index ? index.release() : json_null());
    json_object_set_new(record.get(), "inputs", json_array());
    json_object_set_new(record.get(), "logical_rows", json_integer(static_cast<json_int_t>(meta.rows)));
    json_object_set_new(record.get(), "physical_sha256", json_string(meta.physical.c_str()));
    if (meta.rows == 0U) {
        json_object_set_new(record.get(), "primary_key_first", json_null());
        json_object_set_new(record.get(), "primary_key_last", json_null());
    } else {
        JsonPtr first(json_array());
        JsonPtr last(json_array());
        json_array_append_new(first.get(), json_string(meta.first.c_str()));
        json_array_append_new(last.get(), json_string(meta.last.c_str()));
        json_object_set_new(record.get(), "primary_key_first", first.release());
        json_object_set_new(record.get(), "primary_key_last", last.release());
    }
    json_object_set_new(record.get(), "producer_executable_sha256", json_string(std::string(64U, 'c').c_str()));
    json_object_set_new(record.get(), "relative_path", json_string(meta.path.c_str()));
    json_object_set_new(record.get(), "role", json_string(meta.id.c_str()));
    json_object_set_new(record.get(), "schema_name", json_string(meta.schema_name.c_str()));
    json_object_set_new(record.get(), "schema_version", json_string(meta.schema_version.c_str()));
    json_object_set_new(record.get(), "semantic_sha256", json_string(meta.semantic.c_str()));
    json_object_set_new(record.get(), "sensitivity", json_string("SYNTHETIC_PUBLIC"));
    json_object_set_new(record.get(), "size_bytes", json_integer(static_cast<json_int_t>(meta.size)));
    json_object_set_new(record.get(), "transform_id", json_string(meta.transform.c_str()));
    return record;
}

JsonPtr catalog_artifact(const std::string& id, const std::string& path, const std::string& format,
                         const std::string& schema_path, const std::string& schema_sha,
                         const std::vector<std::string>& primary_key, const std::vector<std::string>& sort_key,
                         const std::optional<std::string>& index_path = std::nullopt,
                         const std::vector<std::string>& index_key = {}) {
    JsonPtr row(json_object());
    json_object_set_new(row.get(), "artifact_id", json_string(id.c_str()));
    json_object_set_new(row.get(), "format", json_string(format.c_str()));
    if (index_path.has_value()) {
        json_object_set_new(row.get(), "index", json_string(index_path->c_str()));
        JsonPtr keys(json_array());
        for (const std::string& key : index_key) {
            json_array_append_new(keys.get(), json_string(key.c_str()));
        }
        json_object_set_new(row.get(), "index_key", keys.release());
    } else {
        json_object_set_new(row.get(), "index", json_null());
        json_object_set_new(row.get(), "index_key", json_null());
    }
    JsonPtr primary(json_array());
    for (const std::string& key : primary_key) {
        json_array_append_new(primary.get(), json_string(key.c_str()));
    }
    json_object_set_new(row.get(), "primary_key", primary.release());
    json_object_set_new(row.get(), "producer", json_string("longlineage"));
    json_object_set_new(row.get(), "queryable", json_true());
    json_object_set_new(row.get(), "record_schema", json_string(schema_path.c_str()));
    json_object_set_new(row.get(), "record_schema_sha256", json_string(schema_sha.c_str()));
    json_object_set_new(row.get(), "relative_path", json_string(path.c_str()));
    JsonPtr sorting(json_array());
    for (const std::string& key : sort_key) {
        json_array_append_new(sorting.get(), json_string(key.c_str()));
    }
    json_object_set_new(row.get(), "sort_key", sorting.release());
    json_object_set_new(row.get(), "validator", json_string("longlineage-validate"));
    return row;
}

class Fixture final {
   public:
    Fixture(std::filesystem::path root, Fault fault)
        : root_(std::move(root)), repo_(root_ / "repo"), run_(root_ / "run") {
        std::filesystem::create_directories(repo_ / "schema" / "records");
        std::filesystem::create_directories(repo_ / "schema" / "core");
        std::filesystem::create_directories(repo_ / "contracts" / "v1");
        std::filesystem::create_directories(repo_ / "state");
        std::filesystem::create_directories(run_ / "indexes");
        std::filesystem::create_directories(run_ / "receipts");
        write_text(repo_ / "AGENTS.md", "synthetic validator fixture\n");
        write_text(repo_ / "state" / "phase_ledger.json", "{}\n");
        write_text(repo_ / "contracts" / "v1" / "science_parameters.json", "{}\n");
        write_text(repo_ / "contracts" / "v1" / "status_reason_codes.tsv",
                   "domain\tkind\tcode\tterminal\tseverity\tallowed_parent_status\trequires_reason\tforbids_winner\t"
                   "introduced_in\tdescription\ttest_id\n");
        build_schemas();
        build_catalog();
        build_run(fault);
    }

    [[nodiscard]] const std::filesystem::path& repo() const noexcept { return repo_; }
    [[nodiscard]] const std::filesystem::path& run() const noexcept { return run_; }

   private:
    void build_schemas() {
        write_text(
            repo_ / "schema" / "records" / "records.json",
            R"({"fields":[{"name":"dataset_order","required":true,"type":"uint32"},{"name":"record_order","required":true,"type":"uint64"},{"name":"value","required":true,"type":"string"}],"format":"TSV_BGZF","header":["dataset_order","record_order","value"],"schema_name":"longlineage.synthetic_records","schema_version":"1.0.0"})"
            "\n");
        write_text(
            repo_ / "schema" / "records" / "site_index.record.json",
            R"({"fields":[{"name":"artifact_id","required":true,"type":"string"},{"name":"dataset_order","required":true,"type":"uint32"},{"name":"record_order","required":true,"type":"uint64"},{"name":"first_virtual_offset","required":true,"type":"uint64"},{"name":"past_end_virtual_offset","required":true,"type":"uint64"},{"name":"logical_rows","required":true,"type":"uint64"},{"name":"range_semantic_sha256","required":true,"type":"sha256"}],"format":"TSV_BGZF","header":["artifact_id","dataset_order","record_order","first_virtual_offset","past_end_virtual_offset","logical_rows","range_semantic_sha256"],"schema_name":"longlineage.site_index","schema_version":"1.0.0"})"
            "\n");
        write_text(
            repo_ / "schema" / "records" / "topology.json",
            R"({"additionalProperties":false,"properties":{"candidate_count":{"type":"integer"},"candidates":{"items":{"type":"object"},"type":"array"},"dataset_order":{"type":"integer"},"family_state":{"type":"string"},"schema_name":{"const":"longlineage.topology_unit"},"schema_version":{"const":"1.0.0"},"unit_order":{"type":"integer"},"winner":{"oneOf":[{"type":"null"},{"type":"object"}]}},"required":["candidate_count","candidates","dataset_order","family_state","schema_name","schema_version","unit_order","winner"],"type":"object"})"
            "\n");
        write_text(
            repo_ / "schema" / "records" / "artifact_catalog.json",
            R"({"additionalProperties":false,"properties":{"artifact":{"additionalProperties":true,"type":"object"},"run_id":{"type":"string"},"schema_name":{"const":"longlineage.artifact_catalog_record"},"schema_version":{"const":"1.0.0"}},"required":["artifact","run_id","schema_name","schema_version"],"type":"object"})"
            "\n");
        write_text(
            repo_ / "schema" / "records" / "data_lineage.json",
            R"({"additionalProperties":false,"properties":{"inputs":{"items":{"type":"object"},"type":"array"},"output_artifact_id":{"type":"string"},"output_semantic_sha256":{"type":"string"},"producer_executable_sha256":{"type":"string"},"run_id":{"type":"string"},"schema_name":{"const":"longlineage.data_lineage_record"},"schema_version":{"const":"1.0.0"},"transform_id":{"type":"string"}},"required":["inputs","output_artifact_id","output_semantic_sha256","producer_executable_sha256","run_id","schema_name","schema_version","transform_id"],"type":"object"})"
            "\n");
        write_text(
            repo_ / "schema" / "records" / "semantic.json",
            R"({"fields":[{"name":"artifact_id","required":true,"type":"string"},{"name":"schema_name","required":true,"type":"string"},{"name":"schema_version","required":true,"type":"string"},{"name":"logical_rows","required":true,"type":"uint64"},{"name":"semantic_sha256","required":true,"type":"sha256"}],"format":"TSV","header":["artifact_id","schema_name","schema_version","logical_rows","semantic_sha256"],"schema_name":"longlineage.semantic_digest","schema_version":"1.0.0"})"
            "\n");
        write_text(
            repo_ / "schema" / "core" / "dummy.json",
            R"({"additionalProperties":true,"properties":{"schema_name":{"const":"longlineage.dummy"},"schema_version":{"const":"1.0.0"}},"required":["schema_name","schema_version"],"type":"object"})"
            "\n");
    }

    void build_catalog() {
        const auto digest = [&](const std::string& relative) { return sha256_file(repo_ / relative); };
        JsonPtr catalog(json_object());
        JsonPtr artifacts(json_array());
        json_array_append_new(artifacts.get(),
                              catalog_artifact("records", "records.tsv.bgz", "TSV_BGZF", "schema/records/records.json",
                                               digest("schema/records/records.json"), {"dataset_order", "record_order"},
                                               {"dataset_order", "record_order"}, "indexes/records.site_index.tsv.bgz",
                                               {"dataset_order", "record_order"})
                                  .release());
        json_array_append_new(artifacts.get(),
                              catalog_artifact("topology_units", "topology_units.jsonl.bgz", "JSONL_BGZF",
                                               "schema/records/topology.json", digest("schema/records/topology.json"),
                                               {"dataset_order", "unit_order"}, {"dataset_order", "unit_order"})
                                  .release());
        json_array_append_new(artifacts.get(), catalog_artifact("artifact_catalog", "artifact_catalog.jsonl.bgz",
                                                                "JSONL_BGZF", "schema/records/artifact_catalog.json",
                                                                digest("schema/records/artifact_catalog.json"),
                                                                {"artifact.artifact_id"}, {"artifact.artifact_id"})
                                                   .release());
        json_array_append_new(
            artifacts.get(),
            catalog_artifact("data_lineage", "data_lineage.jsonl.bgz", "JSONL_BGZF", "schema/records/data_lineage.json",
                             digest("schema/records/data_lineage.json"), {"output_artifact_id"}, {"output_artifact_id"})
                .release());
        json_array_append_new(
            artifacts.get(),
            catalog_artifact("semantic_digests", "semantic_digests.tsv", "TSV", "schema/records/semantic.json",
                             digest("schema/records/semantic.json"), {"artifact_id"}, {"artifact_id"})
                .release());
        for (const auto& row : std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"producer_receipt", "receipts/producer_receipt.json", "JSON"},
                 {"checksums", "checksums.sha256", "SHA256SUM"},
                 {"validation_receipt", "validation_receipt.json", "JSON"},
                 {"run_receipt", "run_receipt.json", "JSON"},
             }) {
            json_array_append_new(artifacts.get(), catalog_artifact(std::get<0>(row), std::get<1>(row),
                                                                    std::get<2>(row), "schema/core/dummy.json",
                                                                    digest("schema/core/dummy.json"), {"run_id"}, {})
                                                       .release());
        }
        json_object_set_new(catalog.get(), "artifacts", artifacts.release());
        JsonPtr membership(json_object());
        auto set_array = [&](const char* key, const std::vector<std::string>& values) {
            JsonPtr array(json_array());
            for (const std::string& value : values) {
                json_array_append_new(array.get(), json_string(value.c_str()));
            }
            json_object_set_new(membership.get(), key, array.release());
        };
        set_array("artifact_catalog_row_artifact_ids", {"records", "topology_units"});
        set_array("checksums_artifact_ids", {"records", "topology_units", "artifact_catalog", "data_lineage",
                                             "semantic_digests", "producer_receipt"});
        json_object_set_new(membership.get(), "checksums_include_declared_indexes", json_true());
        set_array("checksums_excluded_artifact_ids", {"checksums", "validation_receipt", "run_receipt"});
        set_array("data_lineage_output_artifact_ids", {"records", "topology_units"});
        set_array("producer_receipt_artifact_ids",
                  {"records", "topology_units", "artifact_catalog", "data_lineage", "semantic_digests"});
        set_array("run_receipt_artifact_ids",
                  {"records", "topology_units", "artifact_catalog", "data_lineage", "semantic_digests"});
        set_array("scientific_artifact_ids", {"records", "topology_units"});
        set_array("semantic_digest_artifact_ids", {"artifact_catalog", "data_lineage", "records", "topology_units"});
        json_object_set_new(catalog.get(), "run_membership", membership.release());
        json_object_set_new(catalog.get(), "schema_name", json_string("longlineage.artifact_schema_catalog"));
        json_object_set_new(catalog.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(catalog.get(), "site_index_schema", json_string("schema/records/site_index.record.json"));
        write_json(repo_ / "schema" / "catalog.json", catalog.get());
    }

    ArtifactMeta build_records(Fault fault) {
        std::vector<std::string> rows = {"0\t0\talpha", "0\t1\tbeta"};
        if (fault == Fault::kOrder) {
            rows = {"0\t1\tbeta", "0\t0\talpha"};
        } else if (fault == Fault::kDuplicate) {
            rows = {"0\t0\talpha", "0\t0\tbeta"};
        }
        const std::vector<std::string> header = {"dataset_order", "record_order", "value"};
        std::vector<std::string> lines = {
            "##longlineage_schema=longlineage.synthetic_records",
            "##schema_version=1.0.0",
            "##run_id=fixture-run",
            "#dataset_order\trecord_order\tvalue",
        };
        lines.insert(lines.end(), rows.begin(), rows.end());
        const auto offsets = write_bgzf_lines(run_ / "records.tsv.bgz", lines, 4U);

        std::vector<std::string> index_rows;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto fields_end = rows[index].find('\t', 2U);
            const std::string key = rows[index].substr(0, fields_end);
            const auto key_fields = [&]() {
                std::vector<std::string> values;
                std::size_t begin = 0;
                for (int count = 0; count < 2; ++count) {
                    const std::size_t tab = key.find('\t', begin);
                    values.push_back(key.substr(begin, tab == std::string::npos ? std::string::npos : tab - begin));
                    begin = tab == std::string::npos ? key.size() : tab + 1U;
                }
                return values;
            }();
            index_rows.push_back("records\t" + key_fields[0] + "\t" + key_fields[1] + "\t" +
                                 std::to_string(offsets.data_offsets[index].first) + "\t" +
                                 std::to_string(offsets.data_offsets[index].second) + "\t1\t" +
                                 sha256_bytes(rows[index] + "\n"));
        }
        const std::vector<std::string> index_header = {
            "artifact_id",  "dataset_order",         "record_order", "first_virtual_offset", "past_end_virtual_offset",
            "logical_rows", "range_semantic_sha256",
        };
        std::vector<std::string> index_lines = {
            "##longlineage_schema=longlineage.site_index",
            "##schema_version=1.0.0",
            "##run_id=fixture-run",
            "#artifact_id\tdataset_order\trecord_order\tfirst_virtual_offset\tpast_end_virtual_offset\tlogical_"
            "rows\trange_semantic_sha256",
        };
        index_lines.insert(index_lines.end(), index_rows.begin(), index_rows.end());
        const auto unused = write_bgzf_lines(run_ / "indexes" / "records.site_index.tsv.bgz", index_lines, 4U);
        static_cast<void>(unused);

        ArtifactMeta meta;
        meta.id = "records";
        meta.path = "records.tsv.bgz";
        meta.schema_name = "longlineage.synthetic_records";
        meta.format = "TSV_BGZF";
        meta.size = std::filesystem::file_size(run_ / meta.path);
        meta.physical = sha256_file(run_ / meta.path);
        meta.rows = rows.size();
        meta.semantic = semantic_tsv(meta.schema_name, meta.schema_version, header, rows);
        meta.index = IndexMeta{
            "indexes/records.site_index.tsv.bgz",
            std::filesystem::file_size(run_ / "indexes" / "records.site_index.tsv.bgz"),
            sha256_file(run_ / "indexes" / "records.site_index.tsv.bgz"),
            index_rows.size(),
            semantic_tsv("longlineage.site_index", "1.0.0", index_header, index_rows),
        };
        const auto first = rows.front().substr(0, rows.front().find('\t', 2U));
        const auto last = rows.back().substr(0, rows.back().find('\t', 2U));
        meta.first = first.substr(first.find('\t') + 1U);
        meta.last = last.substr(last.find('\t') + 1U);
        // Artifact records encode the full composite key. The fixture validator
        // test rewrites these below to arrays with both components.
        meta.transform = "raw_to_records";
        return meta;
    }

    ArtifactMeta build_topology(Fault fault) {
        std::vector<JsonPtr> rows;
        JsonPtr record(json_object());
        json_object_set_new(record.get(), "candidate_count", json_integer(0));
        json_object_set_new(record.get(), "candidates", json_array());
        json_object_set_new(record.get(), "dataset_order", json_integer(0));
        json_object_set_new(record.get(), "family_state", json_string("FAMILY_INCOMPLETE_CAP"));
        json_object_set_new(record.get(), "schema_name", json_string("longlineage.topology_unit"));
        json_object_set_new(record.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(record.get(), "unit_order", json_integer(0));
        if (fault == Fault::kIncompleteWinner) {
            JsonPtr winner(json_object());
            json_object_set_new(winner.get(), "candidate", json_string("forged"));
            json_object_set_new(record.get(), "winner", winner.release());
        } else {
            json_object_set_new(record.get(), "winner", json_null());
        }
        rows.push_back(std::move(record));
        std::vector<std::string> lines;
        for (const JsonPtr& row : rows) {
            lines.push_back(dump_json(row.get()));
        }
        const auto unused = write_bgzf_lines(run_ / "topology_units.jsonl.bgz", lines, 0U);
        static_cast<void>(unused);
        ArtifactMeta meta;
        meta.id = "topology_units";
        meta.path = "topology_units.jsonl.bgz";
        meta.schema_name = "longlineage.topology_unit";
        meta.format = "JSONL_BGZF";
        meta.size = std::filesystem::file_size(run_ / meta.path);
        meta.physical = sha256_file(run_ / meta.path);
        meta.rows = 1;
        meta.semantic = semantic_jsonl(meta.schema_name, meta.schema_version, rows);
        meta.first = "0";
        meta.last = "0";
        meta.transform = "records_to_topology";
        return meta;
    }

    ArtifactMeta build_jsonl_metadata(const std::string& id, const std::string& schema_name,
                                      const std::vector<JsonPtr>& rows) {
        std::vector<std::string> lines;
        for (const JsonPtr& row : rows) {
            lines.push_back(dump_json(row.get()));
        }
        const std::string path = id + ".jsonl.bgz";
        const auto unused = write_bgzf_lines(run_ / path, lines, 0U);
        static_cast<void>(unused);
        ArtifactMeta meta;
        meta.id = id;
        meta.path = path;
        meta.schema_name = schema_name;
        meta.format = "JSONL_BGZF";
        meta.size = std::filesystem::file_size(run_ / path);
        meta.physical = sha256_file(run_ / path);
        meta.rows = rows.size();
        meta.semantic = semantic_jsonl(schema_name, "1.0.0", rows);
        meta.first = id == "artifact_catalog" ? "records" : "records";
        meta.last = "topology_units";
        meta.transform = id == "artifact_catalog" ? "artifacts_to_catalog" : "artifacts_to_lineage";
        return meta;
    }

    ArtifactMeta build_semantic_digests(const std::vector<ArtifactMeta>& source) {
        std::vector<ArtifactMeta> ordered = source;
        std::sort(ordered.begin(), ordered.end(),
                  [](const ArtifactMeta& left, const ArtifactMeta& right) { return left.id < right.id; });
        std::vector<std::string> rows;
        for (const ArtifactMeta& meta : ordered) {
            rows.push_back(meta.id + "\t" + meta.schema_name + "\t" + meta.schema_version + "\t" +
                           std::to_string(meta.rows) + "\t" + meta.semantic);
        }
        const std::vector<std::string> header = {
            "artifact_id", "schema_name", "schema_version", "logical_rows", "semantic_sha256",
        };
        std::string bytes;
        for (std::size_t index = 0; index < header.size(); ++index) {
            if (index != 0U) {
                bytes.push_back('\t');
            }
            bytes.append(header[index]);
        }
        bytes.push_back('\n');
        for (const std::string& row : rows) {
            bytes.append(row);
            bytes.push_back('\n');
        }
        write_text(run_ / "semantic_digests.tsv", bytes);
        ArtifactMeta meta;
        meta.id = "semantic_digests";
        meta.path = "semantic_digests.tsv";
        meta.schema_name = "longlineage.semantic_digest";
        meta.format = "TSV";
        meta.size = bytes.size();
        meta.physical = sha256_file(run_ / meta.path);
        meta.rows = rows.size();
        meta.semantic = semantic_tsv(meta.schema_name, meta.schema_version, header, rows);
        meta.first = ordered.front().id;
        meta.last = ordered.back().id;
        meta.transform = "artifacts_to_semantic_digests";
        return meta;
    }

    JsonPtr lineage_row(const ArtifactMeta& meta) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "inputs", json_array());
        json_object_set_new(row.get(), "output_artifact_id", json_string(meta.id.c_str()));
        json_object_set_new(row.get(), "output_semantic_sha256", json_string(meta.semantic.c_str()));
        json_object_set_new(row.get(), "producer_executable_sha256", json_string(std::string(64U, 'c').c_str()));
        json_object_set_new(row.get(), "run_id", json_string("fixture-run"));
        json_object_set_new(row.get(), "schema_name", json_string("longlineage.data_lineage_record"));
        json_object_set_new(row.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(row.get(), "transform_id", json_string(meta.transform.c_str()));
        return row;
    }

    JsonPtr catalog_row(const ArtifactMeta& meta) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "artifact", artifact_record(meta).release());
        json_object_set_new(row.get(), "run_id", json_string("fixture-run"));
        json_object_set_new(row.get(), "schema_name", json_string("longlineage.artifact_catalog_record"));
        json_object_set_new(row.get(), "schema_version", json_string("1.0.0"));
        return row;
    }

    void fix_composite_keys(json_t* record, const std::string& first, const std::string& last) {
        check(json_is_object(record), "cannot set composite fixture primary key");
        JsonPtr first_key(json_array());
        JsonPtr last_key(json_array());
        json_array_append_new(first_key.get(), json_integer(0));
        json_array_append_new(first_key.get(), json_integer(std::stoll(first)));
        json_array_append_new(last_key.get(), json_integer(0));
        json_array_append_new(last_key.get(), json_integer(std::stoll(last)));
        json_object_set_new(record, "primary_key_first", first_key.release());
        json_object_set_new(record, "primary_key_last", last_key.release());
    }

    void build_run(Fault fault) {
        ArtifactMeta records = build_records(fault);
        ArtifactMeta topology = build_topology(fault);

        std::vector<JsonPtr> catalog_rows;
        JsonPtr records_catalog_row = catalog_row(records);
        fix_composite_keys(json_object_get(records_catalog_row.get(), "artifact"), records.first, records.last);
        catalog_rows.push_back(std::move(records_catalog_row));
        JsonPtr topology_catalog_row = catalog_row(topology);
        fix_composite_keys(json_object_get(topology_catalog_row.get(), "artifact"), topology.first, topology.last);
        catalog_rows.push_back(std::move(topology_catalog_row));
        ArtifactMeta artifact_catalog =
            build_jsonl_metadata("artifact_catalog", "longlineage.artifact_catalog_record", catalog_rows);

        std::vector<JsonPtr> lineage_rows;
        lineage_rows.push_back(lineage_row(records));
        lineage_rows.push_back(lineage_row(topology));
        ArtifactMeta data_lineage =
            build_jsonl_metadata("data_lineage", "longlineage.data_lineage_record", lineage_rows);

        ArtifactMeta semantic = build_semantic_digests({artifact_catalog, data_lineage, records, topology});
        std::vector<ArtifactMeta> metas = {records, topology, artifact_catalog, data_lineage, semantic};

        JsonPtr producer(json_object());
        JsonPtr artifacts(json_array());
        for (const ArtifactMeta& meta : metas) {
            JsonPtr row = artifact_record(meta);
            if (meta.id == "records") {
                fix_composite_keys(row.get(), meta.first, meta.last);
            } else if (meta.id == "topology_units") {
                fix_composite_keys(row.get(), meta.first, meta.last);
            }
            json_array_append_new(artifacts.get(), row.release());
        }
        json_object_set_new(producer.get(), "artifacts", artifacts.release());
        json_object_set_new(producer.get(), "failure_reason", json_null());
        json_object_set_new(producer.get(), "finished_at", json_string("2026-07-20T00:00:00Z"));
        JsonPtr mount_identity(json_array());
        for (const char* role :
             {"raw_bam", "raw_bam_index", "pass_biallelic_ssnv_vcf", "pass_biallelic_ssnv_vcf_index",
              "latest_hp_ps_sidecar", "latest_hp_ps_sidecar_index", "reference_fasta", "reference_fai"}) {
            JsonPtr mount(json_object());
            const std::string canonical_path = std::string("/synthetic/HCC1395/") + role;
            json_object_set_new(mount.get(), "canonical_path", json_string(canonical_path.c_str()));
            json_object_set_new(mount.get(), "dataset_id", json_string("HCC1395"));
            json_object_set_new(mount.get(), "filesystem_type", json_string("syntheticfs"));
            json_object_set_new(mount.get(), "mount_options_sha256", json_string(std::string(64U, 'd').c_str()));
            json_object_set_new(mount.get(), "mount_source", json_string("synthetic-source"));
            json_object_set_new(mount.get(), "readonly", json_true());
            json_object_set_new(mount.get(), "role", json_string(role));
            json_array_append_new(mount_identity.get(), mount.release());
        }
        json_object_set_new(producer.get(), "input_mount_identity", mount_identity.release());
        json_object_set_new(producer.get(), "input_snapshot_after_sha256", json_string(std::string(64U, 'b').c_str()));
        json_object_set_new(producer.get(), "input_snapshot_before_sha256", json_string(std::string(64U, 'b').c_str()));
        json_object_set_new(producer.get(), "manifest_sha256", json_string(std::string(64U, 'a').c_str()));
        json_object_set_new(producer.get(), "producer_executable_sha256", json_string(std::string(64U, 'c').c_str()));
        json_object_set_new(producer.get(), "producer_hostname", json_string("synthetic-host"));
        json_object_set_new(producer.get(), "producer_kernel_release", json_string("synthetic-kernel"));
        json_object_set_new(producer.get(), "producer_outcome", json_string("READY_FOR_VALIDATION"));
        JsonPtr run_receipt_draft(json_object());
        JsonPtr production_executable(json_object());
        json_object_set_new(production_executable.get(), "compiler", json_string("synthetic-c++17"));
        json_object_set_new(production_executable.get(), "executable_sha256",
                            json_string(std::string(64U, 'c').c_str()));
        json_object_set_new(production_executable.get(), "git_commit", json_string(std::string(40U, '1').c_str()));
        json_object_set_new(production_executable.get(), "htslib_version", json_string("1.18"));
        json_object_set_new(production_executable.get(), "name", json_string("longlineage"));
        json_object_set_new(production_executable.get(), "version", json_string("0.1.0"));
        json_object_set_new(run_receipt_draft.get(), "production_executable", production_executable.release());
        json_object_set_new(run_receipt_draft.get(), "input_lock_sha256", json_string(std::string(64U, 'e').c_str()));
        json_object_set_new(run_receipt_draft.get(), "phase_ledger_sha256", json_string(std::string(64U, 'f').c_str()));
        JsonPtr performance(json_object());
        for (const char* field :
             {"wall_seconds", "user_seconds", "system_seconds", "queue_wait_seconds", "reorder_wait_seconds"}) {
            json_object_set_new(performance.get(), field, json_real(0.0));
        }
        for (const char* field :
             {"memory_peak_bytes", "oom_events", "io_read_bytes", "io_write_bytes", "major_page_faults",
              "minor_page_faults", "logical_records", "logical_bytes", "final_file_count", "transient_file_count"}) {
            json_object_set_new(performance.get(), field, json_integer(0));
        }
        json_object_set_new(performance.get(), "peak_threads", json_integer(1));
        JsonPtr latency(json_object());
        for (const char* field : {"p50", "p95", "p99", "max"}) {
            json_object_set_new(latency.get(), field, json_real(0.0));
        }
        json_object_set_new(performance.get(), "task_latency_seconds", latency.release());
        json_object_set_new(performance.get(), "cache_condition", json_string("UNKNOWN"));
        json_object_set_new(run_receipt_draft.get(), "performance", performance.release());
        json_object_set_new(producer.get(), "run_receipt_draft", run_receipt_draft.release());
        json_object_set_new(producer.get(), "run_id", json_string("fixture-run"));
        json_object_set_new(producer.get(), "schema_catalog_sha256",
                            json_string(sha256_file(repo_ / "schema" / "catalog.json").c_str()));
        json_object_set_new(producer.get(), "schema_name", json_string("longlineage.producer_receipt"));
        json_object_set_new(producer.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(producer.get(), "science_parameters_sha256",
                            json_string(sha256_file(repo_ / "contracts" / "v1" / "science_parameters.json").c_str()));
        json_object_set_new(producer.get(), "state", json_string("RUNNING"));
        json_object_set_new(producer.get(), "truth_fields_seen", json_integer(fault == Fault::kTruth ? 1 : 0));
        write_json(run_ / "receipts" / "producer_receipt.json", producer.get());

        std::vector<std::string> checksum_paths;
        for (const ArtifactMeta& meta : metas) {
            checksum_paths.push_back(meta.path);
            if (meta.index.has_value()) {
                checksum_paths.push_back(meta.index->path);
            }
        }
        checksum_paths.push_back("receipts/producer_receipt.json");
        std::sort(checksum_paths.begin(), checksum_paths.end());
        std::string checksums;
        for (const std::string& path : checksum_paths) {
            checksums.append(sha256_file(run_ / path) + "  " + path + "\n");
        }
        write_text(run_ / "checksums.sha256", checksums);

        if (fault == Fault::kMissing) {
            std::filesystem::remove(run_ / "records.tsv.bgz");
        } else if (fault == Fault::kExtra) {
            write_text(run_ / "extra.bin", "extra\n");
        } else if (fault == Fault::kTruncate) {
            std::filesystem::resize_file(run_ / "records.tsv.bgz", 12U);
        } else if (fault == Fault::kHash) {
            JsonPtr forged = load_json(run_ / "receipts" / "producer_receipt.json");
            json_t* rows = json_object_get(forged.get(), "artifacts");
            json_t* record = json_array_get(rows, 0U);
            json_object_set_new(record, "physical_sha256", json_string(std::string(64U, '0').c_str()));
            write_json(run_ / "receipts" / "producer_receipt.json", forged.get());
        } else if (fault == Fault::kForgedReceipt) {
            write_text(run_ / "run_receipt.json", "{\"state\":\"VALIDATED_FROZEN\"}\n");
        }
    }

    std::filesystem::path root_;
    std::filesystem::path repo_;
    std::filesystem::path run_;
};

struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

ProcessResult run_validator(const std::filesystem::path& executable, const Fixture& fixture, bool check_only) {
    int pipe_fds[2] = {-1, -1};
    check(::pipe(pipe_fds) == 0, "cannot create validator capture pipe");
    const pid_t child = ::fork();
    check(child >= 0, "cannot fork validator");
    if (child == 0) {
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[1]);
        if (check_only) {
            ::execl(executable.c_str(), executable.c_str(), "--run-root", fixture.run().c_str(), "--repo",
                    fixture.repo().c_str(), "--check-only", static_cast<char*>(nullptr));
        } else {
            ::execl(executable.c_str(), executable.c_str(), "--run-root", fixture.run().c_str(), "--repo",
                    fixture.repo().c_str(), static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    ::close(pipe_fds[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = ::read(pipe_fds[0], buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        check(count > 0, "cannot read validator output");
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(pipe_fds[0]);
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status), "validator did not exit normally");
    return {WEXITSTATUS(status), output};
}

class Scratch final {
   public:
    Scratch() {
        path_ = std::filesystem::temp_directory_path() /
                ("longlineage-validator-fault-" + std::to_string(static_cast<long long>(::getpid())));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_);
    }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

   private:
    std::filesystem::path path_;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "usage: test_validator_fault VALIDATOR_EXECUTABLE");
        const std::filesystem::path validator = std::filesystem::canonical(argv[1]);
        Scratch scratch;

        Fixture valid(scratch.path() / "valid", Fault::kNone);
        const ProcessResult positive = run_validator(validator, valid, false);
        check(positive.exit_code == 0 && positive.output.find("\"status\":\"PASS\"") != std::string::npos,
              "positive control must pass: " + positive.output);
        check(std::filesystem::is_regular_file(valid.run() / "validation_receipt.json"),
              "positive control must publish validation_receipt.json");

        const std::vector<std::tuple<std::string, Fault, std::string>> cases = {
            {"missing", Fault::kMissing, "FILE_CENSUS"},
            {"extra", Fault::kExtra, "FILE_CENSUS"},
            {"truncate", Fault::kTruncate, "ARTIFACT_HASH"},
            {"hash", Fault::kHash, "ARTIFACT_HASH"},
            {"order", Fault::kOrder, "ARTIFACT_ORDER"},
            {"duplicate", Fault::kDuplicate, "DUPLICATE_KEY"},
            {"forged_receipt", Fault::kForgedReceipt, "FORGED_RECEIPT"},
            {"truth", Fault::kTruth, "PRODUCER_RECEIPT"},
            {"incomplete_winner", Fault::kIncompleteWinner, "TOPOLOGY_INCOMPLETE_WINNER"},
        };
        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto& [name, fault, expected_check] = cases[index];
            Fixture fixture(scratch.path() / ("case-" + std::to_string(index) + "-" + name), fault);
            const ProcessResult result = run_validator(validator, fixture, true);
            check(result.exit_code == 7, name + " must exit 7, output: " + result.output);
            check(result.output.find(expected_check) != std::string::npos,
                  name + " must fail at " + expected_check + ", output: " + result.output);
        }

        std::cout << "validator executable fault table passed cases=" << cases.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "validator fault test failure: " << error.what() << '\n';
        return 1;
    }
}
