// SPDX-License-Identifier: GPL-3.0-only

// Reuse the bounded physical-fault fixture in the same translation unit.  Its
// renamed entrypoint is not invoked; the fixture remains explicitly
// non-canonical and therefore must never pass the DATASET_GATE science freeze.
#define main validator_fault_embedded_entrypoint
#include "test_validator_fault.cpp"
#undef main

#include <sys/stat.h>

#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

#include "longlineage/artifact/run_root.hpp"
#include "longlineage/validation/artifact_validator.hpp"

namespace {

using longlineage::artifact::RunRootObservedState;
using longlineage::artifact::RunRootStatus;
using longlineage::artifact::RunRootTransaction;
using longlineage::validation::ArtifactValidationOptions;
using longlineage::validation::ArtifactValidator;
using longlineage::validation::DatasetGateFinalizeOptions;

constexpr const char* kFullRunId = "full-science-run";
constexpr const char* kProducerSha = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

enum class ScienceFault {
    kNone,
    kSiteReadJoinCount,
    kMethylInterval,
    kM1JoinedCount,
    kPairCell,
    kExactDecision,
    kGlobalFdr,
    kCallabilityFormal,
    kM2PrecedenceRank,
    kM2FailPartitionCase,
    kM2SummaryDoubleCount,
    kTopologyMappingIncomplete,
    kSummaryCount,
    kSummaryPhaseScope,
    kSummaryDatasetScope,
    kSummaryM1Representation,
    kSiteReadDatasetScope,
    kSelfConsistentProvenanceGraph,
};

struct CanonicalMeta {
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
    std::vector<std::string> first;
    std::vector<std::string> last;
    std::string transform;
    struct InputBinding {
        std::string source_kind;
        std::string source_id;
        std::string digest_kind;
        std::string sha256;
    };
    std::vector<InputBinding> inputs;
};

struct IndexedRange {
    std::string dataset_order;
    std::string record_order;
    std::uint64_t first = 0;
    std::uint64_t past_end = 0;
    std::uint64_t rows = 0;
    std::string semantic;
};

struct LockedInputFixture {
    std::string role;
    std::filesystem::path path;
    std::uint64_t size{0};
    std::string sha256;
};

std::string source_root() { return std::filesystem::canonical(LONGLINEAGE_SOURCE_DIR).string(); }

std::string sci17(double value) {
    check(std::isfinite(value) && !(value == 0.0 && std::signbit(value)), "cannot encode fixture SCI17");
    std::ostringstream raw;
    raw.imbue(std::locale::classic());
    raw << std::scientific << std::setprecision(16) << value;
    std::string encoded = raw.str();
    const std::size_t exponent = encoded.find('e');
    check(exponent != std::string::npos && exponent + 2U < encoded.size(), "fixture SCI17 exponent is absent");
    std::string digits = encoded.substr(exponent + 2U);
    while (digits.size() < 3U) {
        digits.insert(digits.begin(), '0');
    }
    return encoded.substr(0, exponent + 2U) + digits;
}

std::string dump_preserve(const json_t* value) {
    char* encoded = json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_PRESERVE_ORDER);
    check(encoded != nullptr, "cannot encode canonical fixture JSON");
    std::string result(encoded);
    std::free(encoded);
    return result;
}

std::string join_fields(const std::vector<std::string>& fields) {
    std::string row;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0U) {
            row.push_back('\t');
        }
        row.append(fields[index]);
    }
    return row;
}

std::vector<std::string> split_fields(const std::string& row) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin <= row.size()) {
        const std::size_t tab = row.find('\t', begin);
        fields.push_back(row.substr(begin, tab == std::string::npos ? std::string::npos : tab - begin));
        if (tab == std::string::npos) {
            break;
        }
        begin = tab + 1U;
    }
    return fields;
}

std::vector<std::string> schema_header(const std::filesystem::path& schema_path) {
    JsonPtr schema = load_json(schema_path);
    const json_t* header = json_object_get(schema.get(), "header");
    check(json_is_array(header), "fixture TSV schema lacks header");
    std::vector<std::string> result;
    result.reserve(json_array_size(header));
    for (std::size_t index = 0; index < json_array_size(header); ++index) {
        const json_t* field = json_array_get(header, index);
        check(json_is_string(field), "fixture TSV schema header is malformed");
        result.emplace_back(json_string_value(field));
    }
    return result;
}

void set_field(const std::vector<std::string>& header, std::vector<std::string>& row, const std::string& name,
               std::string value) {
    const auto found = std::find(header.begin(), header.end(), name);
    check(found != header.end(), "fixture field is absent: " + name);
    row[static_cast<std::size_t>(std::distance(header.begin(), found))] = std::move(value);
}

std::string read_id(std::uint64_t value) {
    std::ostringstream encoded;
    encoded << std::hex << std::nouppercase << std::setfill('0') << std::setw(64) << value;
    return encoded.str();
}

JsonPtr string_array_json(const std::vector<std::string>& values) {
    JsonPtr result(json_array());
    for (const std::string& value : values) {
        json_array_append_new(result.get(), json_string(value.c_str()));
    }
    return result;
}

JsonPtr canonical_artifact_record(const CanonicalMeta& meta) {
    JsonPtr record(json_object());
    json_object_set_new(record.get(), "artifact_id", json_string(meta.id.c_str()));
    json_object_set_new(record.get(), "role", json_string(meta.id.c_str()));
    json_object_set_new(record.get(), "relative_path", json_string(meta.path.c_str()));
    json_object_set_new(record.get(), "schema_name", json_string(meta.schema_name.c_str()));
    json_object_set_new(record.get(), "schema_version", json_string(meta.schema_version.c_str()));
    json_object_set_new(record.get(), "format", json_string(meta.format.c_str()));
    json_object_set_new(record.get(), "size_bytes", json_integer(static_cast<json_int_t>(meta.size)));
    json_object_set_new(record.get(), "physical_sha256", json_string(meta.physical.c_str()));
    json_object_set_new(record.get(), "logical_rows", json_integer(static_cast<json_int_t>(meta.rows)));
    json_object_set_new(record.get(), "semantic_sha256", json_string(meta.semantic.c_str()));
    if (meta.index.has_value()) {
        JsonPtr index(json_object());
        json_object_set_new(index.get(), "relative_path", json_string(meta.index->path.c_str()));
        json_object_set_new(index.get(), "schema_name", json_string("longlineage.site_index"));
        json_object_set_new(index.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(index.get(), "size_bytes", json_integer(static_cast<json_int_t>(meta.index->size)));
        json_object_set_new(index.get(), "physical_sha256", json_string(meta.index->physical.c_str()));
        json_object_set_new(index.get(), "logical_rows", json_integer(static_cast<json_int_t>(meta.index->rows)));
        json_object_set_new(index.get(), "semantic_sha256", json_string(meta.index->semantic.c_str()));
        json_object_set_new(record.get(), "index", index.release());
    } else {
        json_object_set_new(record.get(), "index", json_null());
    }
    json_object_set_new(record.get(), "sensitivity", json_string("SYNTHETIC_PUBLIC"));
    json_object_set_new(record.get(), "transform_id", json_string(meta.transform.c_str()));
    json_object_set_new(record.get(), "producer_executable_sha256", json_string(kProducerSha));
    JsonPtr inputs(json_array());
    for (const auto& input : meta.inputs) {
        JsonPtr row(json_object());
        json_object_set_new(row.get(), "source_kind", json_string(input.source_kind.c_str()));
        json_object_set_new(row.get(), "source_id", json_string(input.source_id.c_str()));
        json_object_set_new(row.get(), "digest_kind", json_string(input.digest_kind.c_str()));
        json_object_set_new(row.get(), "sha256", json_string(input.sha256.c_str()));
        json_array_append_new(inputs.get(), row.release());
    }
    json_object_set_new(record.get(), "inputs", inputs.release());
    if (meta.rows == 0U) {
        json_object_set_new(record.get(), "primary_key_first", json_null());
        json_object_set_new(record.get(), "primary_key_last", json_null());
    } else {
        json_object_set_new(record.get(), "primary_key_first", string_array_json(meta.first).release());
        json_object_set_new(record.get(), "primary_key_last", string_array_json(meta.last).release());
    }
    return record;
}

CanonicalMeta::InputBinding run_artifact_input(const CanonicalMeta& meta) {
    return {"RUN_ARTIFACT", meta.id, "SEMANTIC_SHA256", meta.semantic};
}

IndexMeta write_site_index(const std::filesystem::path& run_root, const std::string& artifact_id,
                           const std::string& relative_path, const std::vector<IndexedRange>& ranges) {
    const std::vector<std::string> header = {"artifact_id",          "dataset_order",           "record_order",
                                             "first_virtual_offset", "past_end_virtual_offset", "logical_rows",
                                             "range_semantic_sha256"};
    std::vector<std::string> rows;
    rows.reserve(ranges.size());
    for (const IndexedRange& range : ranges) {
        rows.push_back(artifact_id + "\t" + range.dataset_order + "\t" + range.record_order + "\t" +
                       std::to_string(range.first) + "\t" + std::to_string(range.past_end) + "\t" +
                       std::to_string(range.rows) + "\t" + range.semantic);
    }
    std::vector<std::string> lines = {"##longlineage_schema=longlineage.site_index", "##schema_version=1.0.0",
                                      std::string("##run_id=") + kFullRunId, "#" + join_fields(header)};
    lines.insert(lines.end(), rows.begin(), rows.end());
    static_cast<void>(write_bgzf_lines(run_root / relative_path, lines, 4U));
    return {relative_path, std::filesystem::file_size(run_root / relative_path), sha256_file(run_root / relative_path),
            static_cast<std::uint64_t>(rows.size()), semantic_tsv("longlineage.site_index", "1.0.0", header, rows)};
}

std::vector<IndexedRange> group_line_ranges(const std::vector<std::string>& rows,
                                            const std::vector<std::pair<std::uint64_t, std::uint64_t>>& offsets,
                                            std::size_t dataset_column, std::size_t record_column) {
    check(rows.size() == offsets.size(), "fixture row/offset count differs");
    std::vector<IndexedRange> ranges;
    std::string group_bytes;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const std::vector<std::string> fields = split_fields(rows[index]);
        check(dataset_column < fields.size() && record_column < fields.size(), "fixture index key column is absent");
        const bool starts = ranges.empty() || ranges.back().dataset_order != fields[dataset_column] ||
                            ranges.back().record_order != fields[record_column];
        if (starts) {
            if (!ranges.empty()) {
                ranges.back().semantic = sha256_bytes(group_bytes);
                group_bytes.clear();
            }
            ranges.push_back(
                {fields[dataset_column], fields[record_column], offsets[index].first, offsets[index].second, 0U, {}});
        }
        ranges.back().past_end = offsets[index].second;
        ++ranges.back().rows;
        group_bytes.append(rows[index]);
        group_bytes.push_back('\n');
    }
    if (!ranges.empty()) {
        ranges.back().semantic = sha256_bytes(group_bytes);
    }
    return ranges;
}

CanonicalMeta build_indexed_tsv(const std::filesystem::path& run_root, std::string id, std::string path,
                                std::string schema_name, const std::vector<std::string>& header,
                                const std::vector<std::vector<std::string>>& field_rows, std::size_t dataset_column,
                                std::size_t record_column, std::vector<std::string> primary_first,
                                std::vector<std::string> primary_last, std::string schema_version = "1.0.0") {
    std::vector<std::string> rows;
    rows.reserve(field_rows.size());
    for (const auto& fields : field_rows) {
        check(fields.size() == header.size(), id + ": fixture TSV field count differs from header");
        rows.push_back(join_fields(fields));
    }
    std::vector<std::string> lines = {"##longlineage_schema=" + schema_name, "##schema_version=" + schema_version,
                                      std::string("##run_id=") + kFullRunId, "#" + join_fields(header)};
    lines.insert(lines.end(), rows.begin(), rows.end());
    const BgzfWriteResult written = write_bgzf_lines(run_root / path, lines, 4U);
    const std::vector<IndexedRange> ranges =
        group_line_ranges(rows, written.data_offsets, dataset_column, record_column);
    const std::string index_path = "indexes/" + id + ".site_index.tsv.bgz";

    CanonicalMeta meta;
    meta.id = std::move(id);
    meta.path = std::move(path);
    meta.schema_name = std::move(schema_name);
    meta.schema_version = std::move(schema_version);
    meta.format = "TSV_BGZF";
    meta.size = std::filesystem::file_size(run_root / meta.path);
    meta.physical = sha256_file(run_root / meta.path);
    meta.rows = static_cast<std::uint64_t>(rows.size());
    meta.semantic = semantic_tsv(meta.schema_name, meta.schema_version, header, rows);
    meta.index = write_site_index(run_root, meta.id, index_path, ranges);
    meta.first = std::move(primary_first);
    meta.last = std::move(primary_last);
    meta.transform = "synthetic_to_" + meta.id;
    return meta;
}

CanonicalMeta build_jsonl(const std::filesystem::path& run_root, std::string id, std::string path,
                          std::string schema_name, std::string schema_version, const std::vector<JsonPtr>& records,
                          const std::vector<std::pair<std::string, std::string>>& index_keys,
                          std::vector<std::string> primary_first, std::vector<std::string> primary_last,
                          bool declared_index = false) {
    std::vector<std::string> rows;
    rows.reserve(records.size());
    for (const JsonPtr& record : records) {
        rows.push_back(dump_preserve(record.get()));
    }
    const BgzfWriteResult written = write_bgzf_lines(run_root / path, rows, 0U);
    CanonicalMeta meta;
    meta.id = std::move(id);
    meta.path = std::move(path);
    meta.schema_name = std::move(schema_name);
    meta.schema_version = std::move(schema_version);
    meta.format = "JSONL_BGZF";
    meta.size = std::filesystem::file_size(run_root / meta.path);
    meta.physical = sha256_file(run_root / meta.path);
    meta.rows = static_cast<std::uint64_t>(rows.size());
    std::string semantic_bytes = meta.schema_name + "\t" + meta.schema_version + "\n";
    for (const std::string& row : rows) {
        semantic_bytes.append(row);
        semantic_bytes.push_back('\n');
    }
    meta.semantic = sha256_bytes(semantic_bytes);
    if (declared_index) {
        check(index_keys.size() == rows.size(), meta.id + ": JSONL index-key count differs");
        std::vector<IndexedRange> ranges;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            ranges.push_back({index_keys[index].first, index_keys[index].second, written.data_offsets[index].first,
                              written.data_offsets[index].second, 1U, sha256_bytes(rows[index] + "\n")});
        }
        meta.index = write_site_index(run_root, meta.id, "indexes/" + meta.id + ".site_index.tsv.bgz", ranges);
    }
    meta.first = std::move(primary_first);
    meta.last = std::move(primary_last);
    meta.transform = "synthetic_to_" + meta.id;
    return meta;
}

void append_u16(std::string& bytes, std::uint16_t value) {
    for (unsigned int shift = 0; shift < 16U; shift += 8U) {
        bytes.push_back(static_cast<char>((value >> shift) & static_cast<std::uint16_t>(0xffU)));
    }
}

void append_u32(std::string& bytes, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<char>((value >> shift) & static_cast<std::uint32_t>(0xffU)));
    }
}

void append_u64(std::string& bytes, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<char>((value >> shift) & static_cast<std::uint64_t>(0xffU)));
    }
}

std::string hex_to_bytes(const std::string& encoded) {
    check(encoded.size() % 2U == 0U, "fixture hex has odd length");
    const auto nibble = [](char value) -> unsigned int {
        if (value >= '0' && value <= '9') {
            return static_cast<unsigned int>(value - '0');
        }
        check(value >= 'a' && value <= 'f', "fixture hex contains invalid character");
        return 10U + static_cast<unsigned int>(value - 'a');
    };
    std::string bytes;
    bytes.reserve(encoded.size() / 2U);
    for (std::size_t index = 0; index < encoded.size(); index += 2U) {
        bytes.push_back(static_cast<char>((nibble(encoded[index]) << 4U) | nibble(encoded[index + 1U])));
    }
    return bytes;
}

CanonicalMeta build_llm(const std::filesystem::path& run_root, bool replay_semantic_edge_cases) {
    std::vector<std::string> bodies;
    std::vector<std::string> frames;
    const std::vector<std::pair<std::uint64_t, std::uint32_t>> dimensions =
        replay_semantic_edge_cases ? std::vector<std::pair<std::uint64_t, std::uint32_t>>{{0U, 15U}, {1U, 8U}}
                                   : std::vector<std::pair<std::uint64_t, std::uint32_t>>{{0U, 12U}, {1U, 6U}};
    for (const auto& [site_order, dimension] : dimensions) {
        const std::uint64_t values =
            static_cast<std::uint64_t>(dimension) * static_cast<std::uint64_t>(dimension - 1U) / 2U;
        const std::uint64_t mask_bytes = (values + 7U) / 8U;
        std::string body = "LLM1";
        append_u16(body, 1U);
        append_u16(body, 0U);
        append_u32(body, 0U);
        append_u64(body, site_order);
        append_u32(body, dimension);
        append_u64(body, values);
        append_u64(body, mask_bytes);
        body.append(static_cast<std::size_t>(values * 8U), '\0');
        body.append(static_cast<std::size_t>(mask_bytes), '\0');
        bodies.push_back(body);
        frames.push_back(body + hex_to_bytes(sha256_bytes(body)));
    }

    const std::filesystem::path path = run_root / "bernoulli_upper.llm.bgz";
    std::filesystem::create_directories(path.parent_path());
    BGZF* output = bgzf_open(path.c_str(), "w");
    check(output != nullptr, "cannot create canonical LLM fixture");
    for (const std::string& frame : frames) {
        check(bgzf_write(output, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size()),
              "canonical LLM fixture short write");
    }
    check(bgzf_close(output) == 0, "cannot close canonical LLM fixture");

    BGZF* input = bgzf_open(path.c_str(), "r");
    check(input != nullptr, "cannot reopen canonical LLM fixture");
    std::vector<IndexedRange> ranges;
    std::vector<char> buffer;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const int64_t begin = bgzf_tell(input);
        buffer.resize(frames[index].size());
        check(begin >= 0 && bgzf_read(input, buffer.data(), buffer.size()) == static_cast<ssize_t>(buffer.size()),
              "cannot replay canonical LLM fixture offsets");
        const int64_t end = bgzf_tell(input);
        check(end >= 0, "cannot observe canonical LLM fixture past-end");
        ranges.push_back({"0", std::to_string(index), static_cast<std::uint64_t>(begin),
                          static_cast<std::uint64_t>(end), 1U, sha256_bytes(bodies[index])});
    }
    check(bgzf_close(input) == 0, "cannot close canonical LLM replay");

    std::string semantic_bytes = "longlineage.bernoulli_upper\t1.0.0\n";
    for (const std::string& body : bodies) {
        semantic_bytes.append(body);
    }
    CanonicalMeta meta;
    meta.id = "bernoulli_upper";
    meta.path = "bernoulli_upper.llm.bgz";
    meta.schema_name = "longlineage.bernoulli_upper";
    meta.format = "LLM_BGZF";
    meta.size = std::filesystem::file_size(path);
    meta.physical = sha256_file(path);
    meta.rows = 2U;
    meta.semantic = sha256_bytes(semantic_bytes);
    meta.index = write_site_index(run_root, meta.id, "indexes/bernoulli_upper.site_index.tsv.bgz", ranges);
    meta.first = {"0", "0"};
    meta.last = {"0", "1"};
    meta.transform = "synthetic_to_bernoulli_upper";
    return meta;
}

class CanonicalScienceFixture final {
   public:
    CanonicalScienceFixture(std::filesystem::path run_root, ScienceFault fault = ScienceFault::kNone,
                            bool empty_topology = false, bool replay_semantic_edge_cases = false,
                            std::uint64_t peak_threads = 1U, bool hcc1395_profile = false)
        : repo_(source_root()),
          run_(std::move(run_root)),
          fault_(fault),
          empty_topology_(empty_topology),
          replay_semantic_edge_cases_(replay_semantic_edge_cases),
          peak_threads_(peak_threads),
          hcc1395_profile_(hcc1395_profile) {
        std::filesystem::create_directories(run_ / "indexes");
        std::filesystem::create_directories(run_ / "receipts");
        build_inputs_and_manifest();
        build();
    }

    [[nodiscard]] const std::filesystem::path& repo() const noexcept { return repo_; }
    [[nodiscard]] const std::filesystem::path& run() const noexcept { return run_; }
    [[nodiscard]] const std::filesystem::path& manifest() const noexcept { return manifest_; }
    [[nodiscard]] const std::filesystem::path& first_input() const noexcept { return inputs_.front().path; }

   private:
    void build_inputs_and_manifest() {
        input_root_ = run_.parent_path() / "synthetic-input-authority";
        std::filesystem::create_directories(input_root_);
        const std::array<const char*, 8> roles = {
            "raw_bam",
            "raw_bam_index",
            "pass_biallelic_ssnv_vcf",
            "pass_biallelic_ssnv_vcf_index",
            "latest_hp_ps_sidecar",
            "latest_hp_ps_sidecar_index",
            "reference_fasta",
            "reference_fai",
        };
        for (const char* role : roles) {
            const std::filesystem::path path = input_root_ / (std::string(role) + ".synthetic");
            write_text(path, std::string("synthetic locked input: ") + role + "\n");
            const std::filesystem::path canonical = std::filesystem::canonical(path);
            inputs_.push_back({role, canonical, std::filesystem::file_size(canonical), sha256_file(canonical)});
        }

        JsonPtr manifest(json_object());
        json_object_set_new(manifest.get(), "schema_name", json_string("longlineage.production_manifest"));
        json_object_set_new(manifest.get(), "schema_version", json_string(hcc1395_profile_ ? "1.1.0" : "1.0.0"));
        json_object_set_new(manifest.get(), "run_id", json_string(kFullRunId));
        json_object_set_new(manifest.get(), "authority_profile",
                            json_string(hcc1395_profile_ ? "HCC1395_DATASET_GATE" : "SYNTHETIC"));
        json_object_set_new(manifest.get(), "output_root", json_string(run_.string().c_str()));
        JsonPtr datasets(json_array());
        JsonPtr dataset(json_object());
        json_object_set_new(dataset.get(), "dataset_id", json_string("synthetic"));
        json_object_set_new(dataset.get(), "dataset_order", json_integer(0));
        JsonPtr files(json_array());
        for (const LockedInputFixture& input : inputs_) {
            JsonPtr file(json_object());
            json_object_set_new(file.get(), "role", json_string(input.role.c_str()));
            json_object_set_new(file.get(), "path", json_string(input.path.string().c_str()));
            json_object_set_new(file.get(), "size_bytes", json_integer(static_cast<json_int_t>(input.size)));
            json_object_set_new(file.get(), "sha256", json_string(input.sha256.c_str()));
            json_array_append_new(files.get(), file.release());
        }
        json_object_set_new(dataset.get(), "files", files.release());
        json_array_append_new(datasets.get(), dataset.release());
        json_object_set_new(manifest.get(), "datasets", datasets.release());
        JsonPtr runtime(json_object());
        json_object_set_new(runtime.get(), "compute_workers", json_integer(1));
        json_object_set_new(runtime.get(), "writer_threads", json_integer(1));
        json_object_set_new(runtime.get(), "coordinator_slots", json_integer(2));
        json_object_set_new(runtime.get(), "buffer_bytes", json_integer(1048576));
        json_object_set_new(runtime.get(), "max_focal_sites_per_block", json_integer(4096));
        json_object_set_new(runtime.get(), "max_estimated_alignments_per_block", json_integer(250000));
        json_object_set_new(runtime.get(), "halo_bp", json_integer(5000));
        json_object_set_new(manifest.get(), "runtime", runtime.release());
        JsonPtr bindings(json_object());
        const std::vector<std::pair<const char*, const char*>> contracts = {
            {"science_parameters_sha256", "contracts/v1/science_parameters.json"},
            {"schema_catalog_sha256", "schema/catalog.json"},
            {"status_reason_registry_sha256", "contracts/v1/status_reason_codes.tsv"},
            {"type_registry_sha256", "contracts/v1/type_registry.tsv"},
            {"transform_registry_sha256", "contracts/v1/transform_registry.tsv"},
            {"authority_manifest_sha256", "oracle/authority_manifest.json"},
            {"source_to_target_manifest_sha256", "provenance/source_to_target_manifest.json"},
            {"production_input_authority_sha256", "oracle/production_input_authority.json"},
            {"schema_id_registry_sha256", "schema/id_registry.json"},
            {"release_attestation_sha256", "state/release_attestation.json"},
        };
        for (const auto& [field, relative] : contracts) {
            const std::string digest = sha256_file(repo_ / relative);
            json_object_set_new(bindings.get(), field, json_string(digest.c_str()));
        }
        if (hcc1395_profile_) {
            const std::string digest = sha256_file(repo_ / "oracle/hcc1395_dataset_gate_input_authority.json");
            json_object_set_new(bindings.get(), "dataset_gate_input_authority_sha256", json_string(digest.c_str()));
        }
        json_object_set_new(manifest.get(), "contract_bindings", bindings.release());
        manifest_ = input_root_ / "production_manifest.json";
        write_json(manifest_, manifest.get());
        manifest_ = std::filesystem::canonical(manifest_);
        manifest_sha256_ = sha256_file(manifest_);

        std::ostringstream snapshot;
        std::ostringstream locks;
        snapshot << "longlineage.input_snapshot\t1.1.0\n";
        locks << "longlineage.input_lock\t1.0.0\n";
        for (const LockedInputFixture& input : inputs_) {
            struct stat status {};
            check(::stat(input.path.c_str(), &status) == 0, "cannot stat synthetic locked input");
            snapshot << "0\tsynthetic\t" << input.role << '\t' << input.path.string() << '\t'
                     << static_cast<std::uint64_t>(status.st_dev) << '\t' << static_cast<std::uint64_t>(status.st_ino)
                     << '\t' << input.size << '\t' << static_cast<std::int64_t>(status.st_mtim.tv_sec) << '\t'
                     << static_cast<std::int64_t>(status.st_mtim.tv_nsec) << '\t'
                     << static_cast<std::int64_t>(status.st_ctim.tv_sec) << '\t'
                     << static_cast<std::int64_t>(status.st_ctim.tv_nsec) << '\t' << input.sha256 << '\n';
            locks << "0\tsynthetic\t" << input.role << '\t' << input.path.string() << '\t' << input.size << '\t'
                  << input.sha256 << '\n';
            manifest_inputs_.push_back({"MANIFEST_INPUT", "synthetic:" + input.role, "PHYSICAL_SHA256", input.sha256});
        }
        input_snapshot_sha256_ = sha256_bytes(snapshot.str());
        input_lock_sha256_ = sha256_bytes(locks.str());
        manifest_inputs_.push_back({"MANIFEST_INPUT", "run_manifest", "PHYSICAL_SHA256", manifest_sha256_});
        if (hcc1395_profile_) {
            manifest_inputs_.push_back({"CONTRACT", "hcc1395_dataset_gate_input_authority", "PHYSICAL_SHA256",
                                        sha256_file(repo_ / "oracle/hcc1395_dataset_gate_input_authority.json")});
        }
    }

    CanonicalMeta build_site_reads() const {
        const auto header = schema_header(repo_ / "schema/records/site_reads.record.json");
        std::vector<std::vector<std::string>> rows;
        const std::uint64_t maximum_read = replay_semantic_edge_cases_ ? 15U : 12U;
        for (std::uint64_t site = 0; site < 2U; ++site) {
            for (std::uint64_t read = 1; read <= maximum_read; ++read) {
                const bool first_site = site == 0U;
                const bool partner_alt = read > 6U && (read <= 12U || (replay_semantic_edge_cases_ && read >= 14U));
                const std::string allele = first_site || partner_alt ? "A" : "R";
                rows.push_back({"0",
                                fault_ == ScienceFault::kSiteReadDatasetScope ? "wrong-dataset" : "synthetic",
                                std::to_string(site),
                                "chr1",
                                first_site ? "100" : "200",
                                first_site ? "C" : "G",
                                first_site ? "T" : "A",
                                read_id(read),
                                "0",
                                "0000000000000000",
                                std::string(64U, '1'),
                                "50",
                                "250",
                                "60",
                                "+",
                                "200",
                                allele,
                                "30",
                                "1",
                                "1",
                                "RAW_ALL_PRODUCTION_SIDECAR_V2",
                                "1",
                                "EXACT_UNIQUE"});
            }
        }
        if (fault_ == ScienceFault::kSiteReadJoinCount) {
            set_field(header, rows.front(), "full_identity_count", "2");
        }
        CanonicalMeta meta =
            build_indexed_tsv(run_, "site_reads", "site_reads.tsv.bgz", "longlineage.site_reads", header, rows, 0U, 2U,
                              {"0", "0", read_id(1U)}, {"0", "1", read_id(maximum_read)});
        meta.inputs = manifest_inputs_;
        return meta;
    }

    CanonicalMeta build_methyl_calls() const {
        const auto header = schema_header(repo_ / "schema/records/methyl_calls.record.json");
        std::vector<std::vector<std::string>> rows;
        const double lower = 128.0 / 256.0;
        const double upper = 129.0 / 256.0;
        const std::uint64_t maximum_read = replay_semantic_edge_cases_ ? 15U : 12U;
        for (std::uint64_t site = 0; site < 2U; ++site) {
            for (std::uint64_t read = 1U; read <= maximum_read; ++read) {
                const bool focal_alt =
                    site == 0U ? (!replay_semantic_edge_cases_ || read != 15U)
                               : (read > 6U && (read <= 12U || (replay_semantic_edge_cases_ && read >= 14U)));
                if (!focal_alt) {
                    continue;
                }
                for (std::uint64_t cpg = 0; cpg < 3U; ++cpg) {
                    rows.push_back({"0", std::to_string(site), read_id(read), std::to_string(cpg),
                                    std::to_string(10U + cpg), std::to_string((site == 0U ? 100U : 200U) + cpg), "C",
                                    "m", "+", "QUESTION", "0", std::to_string(cpg), "200", "128", sci17(lower),
                                    sci17(upper), "UNKNOWN"});
                }
            }
        }
        if (fault_ == ScienceFault::kMethylInterval) {
            set_field(header, rows.front(), "probability_upper", sci17(0.75));
        }
        return build_indexed_tsv(run_, "methyl_calls", "methyl_calls.tsv.bgz", "longlineage.methyl_calls", header, rows,
                                 0U, 1U, {"0", "0", read_id(1U), "0"}, {"0", "1", read_id(maximum_read), "2"});
    }

    CanonicalMeta build_m1_sites() const {
        const auto header = schema_header(repo_ / "schema/records/m1_sites.record.json");
        std::vector<std::vector<std::string>> rows;
        for (std::uint64_t site = 0; site < 2U; ++site) {
            const std::string joined =
                replay_semantic_edge_cases_ ? (site == 0U ? "15" : "8") : (site == 0U ? "12" : "6");
            const std::string after_peel = replay_semantic_edge_cases_ && site == 0U ? "14" : joined;
            rows.push_back({"0",
                            fault_ == ScienceFault::kSiteReadDatasetScope ? "wrong-dataset" : "synthetic",
                            std::to_string(site),
                            "chr1",
                            site == 0U ? "100" : "200",
                            site == 0U ? "C" : "G",
                            site == 0U ? "T" : "A",
                            "EVALUABLE",
                            "NONE",
                            joined,
                            after_peel,
                            std::string(64U, '2'),
                            std::string(64U, '3'),
                            "2",
                            std::string(64U, '4'),
                            "2",
                            "2",
                            sci17(1.0),
                            sci17(1.0),
                            std::string(64U, '5'),
                            std::string(64U, '6'),
                            "true",
                            "PASS",
                            "PASS",
                            "PASS",
                            "PASS",
                            "PASS",
                            "PASS",
                            "PASS",
                            "PASS"});
        }
        if (fault_ == ScienceFault::kM1JoinedCount) {
            set_field(header, rows.front(), "n_alt_joined", "11");
        }
        return build_indexed_tsv(run_, "m1_sites", "m1_sites.tsv.bgz", "longlineage.m1_sites", header, rows, 0U, 2U,
                                 {"0", "0"}, {"0", "1"});
    }

    JsonPtr assignment_run(std::uint64_t seed_order, const std::vector<std::string>& labels) const {
        JsonPtr run(json_object());
        json_object_set_new(run.get(), "seed_order", json_integer(static_cast<json_int_t>(seed_order)));
        json_object_set_new(run.get(), "seed_u64", json_string(std::to_string(seed_order + 1U).c_str()));
        json_object_set_new(run.get(), "group_count", json_integer(2));
        json_object_set_new(run.get(), "labels", string_array_json(labels).release());
        json_object_set_new(run.get(), "partition_sha256", json_string(std::string(64U, '7').c_str()));
        json_object_set_new(run.get(), "split_trace", json_array());
        return run;
    }

    JsonPtr assignment_record(std::uint64_t site, const std::vector<std::string>& reads,
                              const std::vector<std::string>& labels) const {
        JsonPtr record(json_object());
        json_object_set_new(record.get(), "schema_name", json_string("longlineage.m1_assignment"));
        json_object_set_new(record.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(record.get(), "dataset_order", json_integer(0));
        json_object_set_new(record.get(), "site_order", json_integer(static_cast<json_int_t>(site)));
        json_object_set_new(record.get(), "read_ids", string_array_json(reads).release());
        json_object_set_new(record.get(), "labels", string_array_json(labels).release());
        json_object_set_new(record.get(), "readset_sha256", json_string(std::string(64U, '8').c_str()));
        json_object_set_new(record.get(), "partition_sha256", json_string(std::string(64U, '7').c_str()));
        json_object_set_new(record.get(), "representative_seed_order", json_integer(0));
        JsonPtr coarse(json_array());
        for (std::uint64_t seed = 0; seed < 10U; ++seed) {
            json_array_append_new(coarse.get(), assignment_run(seed, labels).release());
        }
        json_object_set_new(record.get(), "coarse_runs", coarse.release());
        json_object_set_new(record.get(), "fine_run", assignment_run(0U, labels).release());
        return record;
    }

    CanonicalMeta build_assignments() const {
        std::vector<std::string> focal_reads;
        std::vector<std::string> focal_labels;
        const std::uint64_t focal_assignment_maximum = replay_semantic_edge_cases_ ? 14U : 12U;
        for (std::uint64_t read = 1U; read <= focal_assignment_maximum; ++read) {
            focal_reads.push_back(read_id(read));
            focal_labels.push_back(read <= 6U ? "g1" : (read <= 12U ? "g2" : (read == 13U ? "other" : "outlier")));
        }
        std::vector<std::string> partner_reads;
        std::vector<std::string> partner_labels;
        const std::uint64_t partner_assignment_maximum = replay_semantic_edge_cases_ ? 15U : 12U;
        for (std::uint64_t read = 7U; read <= partner_assignment_maximum; ++read) {
            if (read == 13U) {
                continue;
            }
            partner_reads.push_back(read_id(read));
            partner_labels.push_back(read <= 9U ? "h1" : "h2");
        }
        std::vector<JsonPtr> records;
        records.push_back(assignment_record(0U, focal_reads, focal_labels));
        records.push_back(assignment_record(1U, partner_reads, partner_labels));
        return build_jsonl(run_, "m1_assignments", "m1_assignments.jsonl.bgz", "longlineage.m1_assignment", "1.0.0",
                           records, {{"0", "0"}, {"0", "1"}}, {"0", "0"}, {"0", "1"}, true);
    }

    CanonicalMeta build_cooccurrence_pairs() const {
        const auto header = schema_header(repo_ / "schema/records/cooccurrence_pairs-1.0.1.record.json");
        const std::string table = "[[6,0],[0,6]]";
        const double exact_p = 2.0 / 924.0;
        std::vector<std::string> row = {"0",
                                        "synthetic",
                                        "0",
                                        "chr1",
                                        "100",
                                        "C",
                                        "T",
                                        "1",
                                        "200",
                                        "G",
                                        "A",
                                        "100",
                                        "2",
                                        table,
                                        sha256_bytes(table),
                                        "12",
                                        "6",
                                        "6",
                                        "6",
                                        "7",
                                        "EXACT_IDENTIFIABLE",
                                        "ELIGIBLE_M2_EXACT_FAMILY",
                                        sci17(exact_p),
                                        "GLOBAL_EXACT",
                                        "1",
                                        sci17(exact_p),
                                        sci17(exact_p),
                                        sci17(1.0),
                                        sci17(1.0),
                                        "true",
                                        "true",
                                        "true",
                                        "999",
                                        "0",
                                        sci17(0.001),
                                        "PERMUTABLE",
                                        "true",
                                        "PASS_ALL_CORE_READS_CALLABLE",
                                        ".",
                                        ".",
                                        "12",
                                        "12",
                                        "0",
                                        "0",
                                        "0",
                                        "0",
                                        "6",
                                        "6",
                                        "0",
                                        "0",
                                        "0",
                                        "0",
                                        "0",
                                        "0",
                                        "0",
                                        "0",
                                        "0",
                                        "0",
                                        sci17(0.02),
                                        sci17(0.95),
                                        ".",
                                        ".",
                                        "NOT_IDENTIFIABLE_NO_DENOMINATOR",
                                        ".",
                                        ".",
                                        "NOT_IDENTIFIABLE_NO_DENOMINATOR",
                                        ".",
                                        ".",
                                        "NOT_IDENTIFIABLE_NO_DENOMINATOR",
                                        "false",
                                        "NOT_IDENTIFIABLE_NO_FOCAL_REF",
                                        "[]",
                                        "0",
                                        "COMPATIBILITY_ONLY_NOT_ANCESTRY"};
        check(row.size() == header.size(), "co-occurrence pair fixture field count differs");
        if (replay_semantic_edge_cases_) {
            set_field(header, row, "ar", "7");
            set_field(header, row, "aa", "8");
            set_field(header, row, "pair_read_count", "15");
            set_field(header, row, "ra_complete_read_count", "15");
        }
        if (fault_ == ScienceFault::kPairCell) {
            set_field(header, row, "ar", "5");
        } else if (fault_ == ScienceFault::kExactDecision) {
            set_field(header, row, "p_exact", sci17(exact_p * 2.0));
        } else if (fault_ == ScienceFault::kGlobalFdr) {
            set_field(header, row, "q_global_by", sci17(0.5));
        } else if (fault_ == ScienceFault::kCallabilityFormal) {
            set_field(header, row, "callability_status", "FAIL_DIFFERENTIAL_CALLABILITY_SIGNAL");
        }
        if (fault_ == ScienceFault::kM2FailPartitionCase) {
            set_field(header, row, "family_status", "INELIGIBLE_M2_SCREEN");
            set_field(header, row, "fdr_family_id", ".");
            set_field(header, row, "fdr_family_size", ".");
            set_field(header, row, "q_global_bh", ".");
            set_field(header, row, "q_global_by", ".");
            set_field(header, row, "exact_bh_discovery", "false");
            set_field(header, row, "exact_by_discovery", "false");
            set_field(header, row, "conditional_valid_permutations", "0");
            set_field(header, row, "conditional_exceedance", ".");
            set_field(header, row, "conditional_p", ".");
            set_field(header, row, "conditional_status", "NOT_RUN_NOT_EXACT_BY_DISCOVERY");
            set_field(header, row, "formal_pair_by_confirmed", "false");
        }
        return build_indexed_tsv(run_, "cooccurrence_pairs", "cooccurrence_pairs.tsv.bgz",
                                 "longlineage.cooccurrence_pairs", header, {row}, 0U, 2U, {"0", "0", "1"},
                                 {"0", "0", "1"}, "1.0.1");
    }

    CanonicalMeta build_cooccurrence_sites() const {
        const auto header = schema_header(repo_ / "schema/records/cooccurrence_sites.record.json");
        std::vector<std::vector<std::string>> rows = {
            {"0",
             "synthetic",
             "0",
             "chr1",
             "100",
             "C",
             "T",
             "2",
             "PASS",
             "ALL_MEASURED_AXES_DETERMINATE_NO_ALIGNED_CONFOUND",
             "6",
             "M2_GRID_V1",
             "499",
             "6",
             sci17(0.0),
             sci17(0.0),
             sci17(1.0),
             "1",
             "1",
             "1",
             "1",
             empty_topology_ ? "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE" : "PASS",
             empty_topology_ ? "." : "[1]",
             empty_topology_ ? "." : std::string(64U, '9')},
            {"0",
             "synthetic",
             "1",
             "chr1",
             "200",
             "G",
             "A",
             "2",
             "NOT_EVALUABLE",
             "AXIS_INDETERMINATE",
             "2",
             "M2_GRID_V1",
             "499",
             "3",
             ".",
             ".",
             ".",
             "0",
             "0",
             "0",
             "0",
             "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE",
             ".",
             "."}};
        if (fault_ == ScienceFault::kM2PrecedenceRank) {
            set_field(header, rows.front(), "m2_precedence_rank", "5");
        } else if (fault_ == ScienceFault::kM2FailPartitionCase) {
            set_field(header, rows.front(), "m2_status", "FAIL");
            set_field(header, rows.front(), "m2_reason", "HP_AXIS_CONFOUND");
            set_field(header, rows.front(), "m2_precedence_rank", "3");
            set_field(header, rows.front(), "joint_signature_status", "NOT_IDENTIFIABLE_JOINT_SIGNATURE_NOT_TESTABLE");
            set_field(header, rows.front(), "joint_partner_orders_json", ".");
            set_field(header, rows.front(), "joint_complete_readset_sha256", ".");
            set_field(header, rows.front(), "global_bh_discoveries", "0");
            set_field(header, rows.front(), "global_by_discoveries", "0");
        }
        return build_indexed_tsv(run_, "cooccurrence_sites", "cooccurrence_sites.tsv.bgz",
                                 "longlineage.cooccurrence_sites", header, rows, 0U, 2U, {"0", "0"}, {"0", "1"});
    }

    CanonicalMeta build_topology() const {
        std::vector<JsonPtr> records;
        if (empty_topology_) {
            return build_jsonl(run_, "topology_units", "topology_units.jsonl.bgz", "longlineage.topology_unit", "2.0.0",
                               records, {}, {}, {}, true);
        }
        records.push_back(load_json(repo_ / "tests/fixtures/contracts/"
                                            "topology_unit.v2.valid_complete_tie.json"));
        json_t* unit = records.front().get();
        json_object_set_new(unit, "unit_order", json_integer(0));
        json_object_set_new(unit, "unit_id", json_string("synthetic-complete"));
        JsonPtr patterns(json_array());
        for (const char* state : {"10", "11"}) {
            JsonPtr pattern(json_object());
            json_object_set_new(pattern.get(), "kind", json_string("FULL_STATE"));
            json_object_set_new(pattern.get(), "pattern", json_string(state));
            json_object_set_new(pattern.get(), "multiplicity", json_integer(6));
            json_array_append_new(patterns.get(), pattern.release());
        }
        json_object_set_new(unit, "input_patterns", patterns.release());
        json_object_set_new(unit, "observed_states", string_array_json({"10", "11"}).release());
        json_object_set_new(unit, "objective_h", json_integer(0));
        JsonPtr bounds(json_object());
        json_object_set_new(bounds.get(), "lower_bound", json_integer(0));
        json_object_set_new(bounds.get(), "upper_bound", json_integer(0));
        json_object_set_new(bounds.get(), "gap", json_integer(0));
        json_object_set_new(unit, "objective_bounds", bounds.release());

        const std::string candidate_digest = sha256_bytes(
            "schema=longlineage.exact_vertex_set.v1\n"
            "q=2\nvertices=0,2,3\n");
        JsonPtr candidate(json_object());
        json_object_set_new(candidate.get(), "vertex_set", string_array_json({"00", "10", "11"}).release());
        json_object_set_new(candidate.get(), "vertex_set_sha256", json_string(candidate_digest.c_str()));
        JsonPtr choices(json_array());
        for (const auto& [child, parent] :
             std::vector<std::pair<const char*, const char*>>{{"10", "00"}, {"11", "10"}}) {
            JsonPtr choice(json_object());
            json_object_set_new(choice.get(), "vertex", json_string(child));
            json_object_set_new(choice.get(), "parents", string_array_json({parent}).release());
            json_array_append_new(choices.get(), choice.release());
        }
        json_object_set_new(candidate.get(), "legal_parent_choices", choices.release());
        json_object_set_new(candidate.get(), "legal_parent_count", json_string("2"));
        json_object_set_new(candidate.get(), "tree_count", json_string("1"));
        JsonPtr candidates(json_array());
        json_array_append_new(candidates.get(), candidate.release());
        json_object_set_new(unit, "candidates", candidates.release());
        json_object_set_new(unit, "candidate_count", json_integer(1));
        json_object_set_new(unit, "tree_count", json_string("1"));
        const std::string family_digest = sha256_bytes(
            "schema=longlineage.candidate_family.v1\n"
            "q=2\n" +
            candidate_digest + "\t2\t1\n");
        json_object_set_new(unit, "candidate_family_digest", json_string(family_digest.c_str()));
        const std::string objective_evidence =
            std::string(json_string_value(json_object_get(unit, "objective_evidence_sha256")));
        const std::string input_evidence =
            std::string(json_string_value(json_object_get(unit, "input_evidence_sha256")));
        const std::string family_evidence = sha256_bytes(
            "schema=longlineage.complete_family_evidence.v1\n"
            "input_evidence_sha256=" +
            input_evidence + "\nobjective_evidence_sha256=" + objective_evidence +
            "\ncandidate_family_digest=" + family_digest +
            "\ncandidate_count=1\ntree_count=1\n"
            "family_enumeration_exhausted=1\n");
        json_object_set_new(unit, "family_evidence_sha256", json_string(family_evidence.c_str()));
        json_t* certificate = json_object_get(unit, "ranking_certificate");
        json_object_set_new(certificate, "evaluated_vertex_set_count", json_integer(1));
        json_t* published = json_object_get(unit, "published_rank");
        json_object_set_new(published, "best_score", json_integer(-12));
        json_object_set_new(published, "best_vertex_set_tie_class", string_array_json({candidate_digest}).release());

        JsonPtr edge_endpoint(json_object());
        json_object_set_new(edge_endpoint.get(), "status", json_string("EDGE_COMPLETE"));
        json_object_set_new(edge_endpoint.get(), "endpoint_id", json_string("ADDITIVE_PARENT_EDGE_SCORE_V1"));
        json_object_set_new(edge_endpoint.get(), "evidence_sha256",
                            json_string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        JsonPtr edge_results(json_array());
        JsonPtr edge_result(json_object());
        json_object_set_new(edge_result.get(), "candidate_vertex_set_sha256", json_string(candidate_digest.c_str()));
        json_object_set_new(edge_result.get(), "best_additive_edge_score", json_integer(0));
        json_object_set_new(edge_result.get(), "best_parent_tie_count", json_string("1"));
        JsonPtr mapping(json_array());
        for (const auto& [child, parent] :
             std::vector<std::pair<const char*, const char*>>{{"10", "00"}, {"11", "10"}}) {
            if (fault_ == ScienceFault::kTopologyMappingIncomplete && std::string(child) == "11") {
                continue;
            }
            JsonPtr edge(json_object());
            json_object_set_new(edge.get(), "child_state", json_string(child));
            json_object_set_new(edge.get(), "parent_state", json_string(parent));
            json_array_append_new(mapping.get(), edge.release());
        }
        json_object_set_new(edge_result.get(), "published_parent_mapping", mapping.release());
        json_array_append_new(edge_results.get(), edge_result.release());
        json_object_set_new(edge_endpoint.get(), "candidate_results", edge_results.release());
        json_object_set_new(edge_endpoint.get(), "reason", json_string("NONE"));
        json_object_set_new(unit, "edge_endpoint", edge_endpoint.release());
        return build_jsonl(run_, "topology_units", "topology_units.jsonl.bgz", "longlineage.topology_unit", "2.0.0",
                           records, {{"0", "0"}}, {"0", "0"}, {"0", "0"}, true);
    }

    CanonicalMeta build_summary() const {
        JsonPtr root(json_object());
        json_object_set_new(root.get(), "schema_name", json_string("longlineage.summary"));
        json_object_set_new(root.get(), "schema_version", json_string("2.0.0"));
        json_object_set_new(root.get(), "run_id", json_string(kFullRunId));
        JsonPtr scope(json_object());
        json_object_set_new(scope.get(), "task_type", json_string("B"));
        json_object_set_new(scope.get(), "completeness", json_string("FULL"));
        json_object_set_new(scope.get(), "dataset_count", json_integer(1));
        json_object_set_new(
            scope.get(), "dataset_ids",
            string_array_json({fault_ == ScienceFault::kSummaryDatasetScope ? "wrong-dataset" : "synthetic"})
                .release());
        json_object_set_new(scope.get(), "site_population", json_string("FROZEN_SYNTHETIC_TWO_SITE"));
        json_object_set_new(
            scope.get(), "m1_representation",
            json_string(fault_ == ScienceFault::kSummaryM1Representation ? "RAW_BINARY32_POINT"
                                                                         : "HISTORICAL_OBSERVED_ROUND6_NULL_ROUND4"));
        json_object_set_new(root.get(), "scope", scope.release());

        JsonPtr counts(json_object());
        const std::vector<std::pair<const char*, std::uint64_t>> values = {
            {"site_keys", 2U},
            {"site_keys_missing", 0U},
            {"site_keys_extra", 0U},
            {"site_keys_duplicate", 0U},
            {"m1_evaluable", 2U},
            {"m1_insufficient_alt_reads", 0U},
            {"m1_incomplete_distance", 0U},
            {"m1_stable_assignments", 2U},
            {"latest_tag_exact_joins", replay_semantic_edge_cases_ ? 30U : 24U},
            {"latest_tag_missing", 0U},
            {"latest_tag_conflict", 0U},
            {"latest_tag_multimatch", 0U},
            {"m2_eligible", fault_ == ScienceFault::kM2FailPartitionCase ? 0U : 1U},
            {"m2_evaluable_ineligible", fault_ == ScienceFault::kM2FailPartitionCase
                                            ? 1U
                                            : (fault_ == ScienceFault::kM2SummaryDoubleCount ? 1U : 0U)},
            {"m2_axis_indeterminate", 1U},
            {"m2_group_count_gt10", 0U},
            {"raw_expected", fault_ == ScienceFault::kSummaryCount ? (replay_semantic_edge_cases_ ? 29U : 23U)
                                                                   : (replay_semantic_edge_cases_ ? 30U : 24U)},
            {"raw_matched", replay_semantic_edge_cases_ ? 30U : 24U},
            {"raw_rg_only_duplicate_occurrences", 0U},
            {"topology_primary_hp_units", empty_topology_ ? 0U : 1U},
            {"topology_regions", empty_topology_ ? 0U : 1U},
            {"topology_fully_complete_regions", empty_topology_ ? 0U : 1U},
            {"topology_incomplete_regions", 0U},
            {"topology_incomplete_units_with_winner", 0U}};
        for (const auto& [name, value] : values) {
            json_object_set_new(counts.get(), name, json_integer(static_cast<json_int_t>(value)));
        }
        json_object_set_new(root.get(), "counts", counts.release());
        json_object_set_new(root.get(), "phase_status_scope",
                            json_string(fault_ == ScienceFault::kSummaryPhaseScope
                                            ? "PROJECT_PHASE_LEDGER"
                                            : "RUN_LOCAL_DATASET_GATE_CLOSEOUT_NOT_PROJECT_PHASE_LEDGER"));
        JsonPtr phases(json_object());
        for (const char* phase : {"P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"}) {
            const std::string status =
                std::string(phase) == "P6"
                    ? "IN_PROGRESS"
                    : (std::string(phase) == "P7" || std::string(phase) == "P8" ? "NOT_STARTED" : "VERIFIED");
            json_object_set_new(phases.get(), phase, json_string(status.c_str()));
        }
        json_object_set_new(root.get(), "phase_status", phases.release());
        const std::string canonical = dump_preserve(root.get());
        write_text(run_ / "summary.json", canonical + "\n");
        CanonicalMeta meta;
        meta.id = "summary";
        meta.path = "summary.json";
        meta.schema_name = "longlineage.summary";
        meta.schema_version = "2.0.0";
        meta.format = "JSON";
        meta.size = std::filesystem::file_size(run_ / meta.path);
        meta.physical = sha256_file(run_ / meta.path);
        meta.rows = 1U;
        meta.semantic = sha256_bytes(meta.schema_name + "\t" + meta.schema_version + "\n" + canonical + "\n");
        meta.first = {kFullRunId};
        meta.last = {kFullRunId};
        meta.transform = "synthetic_to_summary";
        return meta;
    }

    CanonicalMeta build_artifact_catalog(const std::vector<CanonicalMeta>& scientific) const {
        std::vector<const CanonicalMeta*> ordered;
        ordered.reserve(scientific.size());
        for (const CanonicalMeta& meta : scientific) {
            ordered.push_back(&meta);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const CanonicalMeta* left, const CanonicalMeta* right) { return left->id < right->id; });
        std::vector<JsonPtr> records;
        for (const CanonicalMeta* meta : ordered) {
            JsonPtr record(json_object());
            json_object_set_new(record.get(), "schema_name", json_string("longlineage.artifact_catalog_record"));
            json_object_set_new(record.get(), "schema_version", json_string("1.0.0"));
            json_object_set_new(record.get(), "run_id", json_string(kFullRunId));
            json_object_set_new(record.get(), "artifact", canonical_artifact_record(*meta).release());
            records.push_back(std::move(record));
        }
        CanonicalMeta meta =
            build_jsonl(run_, "artifact_catalog", "artifact_catalog.jsonl.bgz", "longlineage.artifact_catalog_record",
                        "1.0.0", records, {}, {ordered.front()->id}, {ordered.back()->id});
        meta.transform = "scientific_artifacts_to_catalog";
        for (const CanonicalMeta* artifact : ordered) {
            meta.inputs.push_back(run_artifact_input(*artifact));
        }
        return meta;
    }

    CanonicalMeta build_data_lineage(const std::vector<CanonicalMeta>& scientific) const {
        std::vector<const CanonicalMeta*> ordered;
        ordered.reserve(scientific.size());
        for (const CanonicalMeta& meta : scientific) {
            ordered.push_back(&meta);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const CanonicalMeta* left, const CanonicalMeta* right) { return left->id < right->id; });
        std::vector<JsonPtr> records;
        for (const CanonicalMeta* meta : ordered) {
            JsonPtr record(json_object());
            json_object_set_new(record.get(), "schema_name", json_string("longlineage.data_lineage_record"));
            json_object_set_new(record.get(), "schema_version", json_string("1.0.0"));
            json_object_set_new(record.get(), "run_id", json_string(kFullRunId));
            json_object_set_new(record.get(), "transform_id", json_string(meta->transform.c_str()));
            json_object_set_new(record.get(), "output_artifact_id", json_string(meta->id.c_str()));
            json_object_set_new(record.get(), "output_semantic_sha256", json_string(meta->semantic.c_str()));
            JsonPtr inputs(json_array());
            for (const auto& input : meta->inputs) {
                JsonPtr binding(json_object());
                json_object_set_new(binding.get(), "source_kind", json_string(input.source_kind.c_str()));
                json_object_set_new(binding.get(), "source_id", json_string(input.source_id.c_str()));
                json_object_set_new(binding.get(), "digest_kind", json_string(input.digest_kind.c_str()));
                json_object_set_new(binding.get(), "sha256", json_string(input.sha256.c_str()));
                json_array_append_new(inputs.get(), binding.release());
            }
            json_object_set_new(record.get(), "inputs", inputs.release());
            json_object_set_new(record.get(), "producer_executable_sha256", json_string(kProducerSha));
            records.push_back(std::move(record));
        }
        CanonicalMeta meta =
            build_jsonl(run_, "data_lineage", "data_lineage.jsonl.bgz", "longlineage.data_lineage_record", "1.0.0",
                        records, {}, {ordered.front()->id}, {ordered.back()->id});
        meta.transform = "scientific_artifacts_to_lineage";
        for (const CanonicalMeta* artifact : ordered) {
            meta.inputs.push_back(run_artifact_input(*artifact));
        }
        return meta;
    }

    CanonicalMeta build_semantic_digests(const std::vector<CanonicalMeta>& source) const {
        std::vector<const CanonicalMeta*> ordered;
        ordered.reserve(source.size());
        for (const CanonicalMeta& meta : source) {
            ordered.push_back(&meta);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const CanonicalMeta* left, const CanonicalMeta* right) { return left->id < right->id; });
        const auto header = schema_header(repo_ / "schema/records/semantic_digest.record.json");
        std::vector<std::string> rows;
        for (const CanonicalMeta* meta : ordered) {
            rows.push_back(meta->id + "\t" + meta->schema_name + "\t" + meta->schema_version + "\t" +
                           std::to_string(meta->rows) + "\t" + meta->semantic);
        }
        std::string physical = join_fields(header) + "\n";
        for (const std::string& row : rows) {
            physical.append(row);
            physical.push_back('\n');
        }
        write_text(run_ / "semantic_digests.tsv", physical);
        CanonicalMeta meta;
        meta.id = "semantic_digests";
        meta.path = "semantic_digests.tsv";
        meta.schema_name = "longlineage.semantic_digest";
        meta.format = "TSV";
        meta.size = std::filesystem::file_size(run_ / meta.path);
        meta.physical = sha256_file(run_ / meta.path);
        meta.rows = static_cast<std::uint64_t>(rows.size());
        meta.semantic = semantic_tsv(meta.schema_name, meta.schema_version, header, rows);
        meta.first = {ordered.front()->id};
        meta.last = {ordered.back()->id};
        meta.transform = "artifacts_to_semantic_digests";
        for (const CanonicalMeta* artifact : ordered) {
            meta.inputs.push_back(run_artifact_input(*artifact));
        }
        return meta;
    }

    JsonPtr run_receipt_draft() const {
        JsonPtr draft(json_object());
        JsonPtr executable(json_object());
        json_object_set_new(executable.get(), "name", json_string("longlineage"));
        json_object_set_new(executable.get(), "version", json_string("0.1.0"));
        json_object_set_new(executable.get(), "git_commit", json_string(std::string(40U, '1').c_str()));
        json_object_set_new(executable.get(), "executable_sha256", json_string(kProducerSha));
        json_object_set_new(executable.get(), "compiler", json_string("synthetic-c++17"));
        json_object_set_new(executable.get(), "htslib_version", json_string("1.18"));
        json_object_set_new(draft.get(), "production_executable", executable.release());
        json_object_set_new(draft.get(), "input_lock_sha256", json_string(input_lock_sha256_.c_str()));
        json_object_set_new(draft.get(), "phase_ledger_sha256",
                            json_string(sha256_file(repo_ / "state/phase_ledger.json").c_str()));
        JsonPtr performance(json_object());
        for (const char* name : {"wall_seconds", "user_seconds", "system_seconds"}) {
            json_object_set_new(performance.get(), name, json_real(0.0));
        }
        for (const char* name : {"memory_peak_bytes", "oom_events", "io_read_bytes", "io_write_bytes",
                                 "major_page_faults", "minor_page_faults"}) {
            json_object_set_new(performance.get(), name, json_integer(0));
        }
        json_object_set_new(performance.get(), "peak_threads", json_integer(static_cast<json_int_t>(peak_threads_)));
        json_object_set_new(performance.get(), "queue_wait_seconds", json_real(0.0));
        json_object_set_new(performance.get(), "reorder_wait_seconds", json_real(0.0));
        JsonPtr latency(json_object());
        for (const char* name : {"p50", "p95", "p99", "max"}) {
            json_object_set_new(latency.get(), name, json_real(0.0));
        }
        json_object_set_new(performance.get(), "task_latency_seconds", latency.release());
        for (const char* name : {"logical_records", "logical_bytes", "final_file_count", "transient_file_count"}) {
            json_object_set_new(performance.get(), name, json_integer(0));
        }
        json_object_set_new(performance.get(), "cache_condition", json_string("UNKNOWN"));
        json_object_set_new(draft.get(), "performance", performance.release());
        return draft;
    }

    void write_producer_receipt(const std::vector<CanonicalMeta>& artifacts) const {
        JsonPtr producer(json_object());
        json_object_set_new(producer.get(), "schema_name", json_string("longlineage.producer_receipt"));
        json_object_set_new(producer.get(), "schema_version", json_string("1.0.0"));
        json_object_set_new(producer.get(), "run_id", json_string(kFullRunId));
        json_object_set_new(producer.get(), "state", json_string("RUNNING"));
        json_object_set_new(producer.get(), "producer_outcome", json_string("READY_FOR_VALIDATION"));
        json_object_set_new(producer.get(), "producer_executable_sha256", json_string(kProducerSha));
        json_object_set_new(producer.get(), "producer_hostname", json_string("synthetic-host"));
        json_object_set_new(producer.get(), "producer_kernel_release", json_string("synthetic-kernel"));
        JsonPtr mounts(json_array());
        for (const LockedInputFixture& input : inputs_) {
            JsonPtr mount(json_object());
            json_object_set_new(mount.get(), "dataset_id", json_string("synthetic"));
            json_object_set_new(mount.get(), "role", json_string(input.role.c_str()));
            json_object_set_new(mount.get(), "canonical_path", json_string(input.path.string().c_str()));
            json_object_set_new(mount.get(), "mount_source", json_string("synthetic-source"));
            json_object_set_new(mount.get(), "filesystem_type", json_string("syntheticfs"));
            json_object_set_new(mount.get(), "readonly", json_true());
            json_object_set_new(mount.get(), "mount_options_sha256", json_string(std::string(64U, 'd').c_str()));
            json_array_append_new(mounts.get(), mount.release());
        }
        json_object_set_new(producer.get(), "input_mount_identity", mounts.release());
        json_object_set_new(producer.get(), "manifest_sha256", json_string(manifest_sha256_.c_str()));
        json_object_set_new(producer.get(), "input_snapshot_before_sha256",
                            json_string(input_snapshot_sha256_.c_str()));
        json_object_set_new(producer.get(), "input_snapshot_after_sha256", json_string(input_snapshot_sha256_.c_str()));
        json_object_set_new(producer.get(), "schema_catalog_sha256",
                            json_string(sha256_file(repo_ / "schema/catalog.json").c_str()));
        json_object_set_new(producer.get(), "science_parameters_sha256",
                            json_string(sha256_file(repo_ / "contracts/v1/science_parameters.json").c_str()));
        json_object_set_new(producer.get(), "run_receipt_draft", run_receipt_draft().release());
        JsonPtr rows(json_array());
        std::vector<const CanonicalMeta*> ordered;
        for (const CanonicalMeta& artifact : artifacts) {
            ordered.push_back(&artifact);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const CanonicalMeta* left, const CanonicalMeta* right) { return left->id < right->id; });
        for (const CanonicalMeta* artifact : ordered) {
            json_array_append_new(rows.get(), canonical_artifact_record(*artifact).release());
        }
        json_object_set_new(producer.get(), "artifacts", rows.release());
        json_object_set_new(producer.get(), "truth_fields_seen", json_integer(0));
        json_object_set_new(producer.get(), "failure_reason", json_null());
        json_object_set_new(producer.get(), "finished_at", json_string("2026-07-20T00:00:00Z"));
        write_json(run_ / "receipts/producer_receipt.json", producer.get());
    }

    void write_checksums(const std::vector<CanonicalMeta>& artifacts) const {
        std::vector<std::string> paths;
        for (const CanonicalMeta& meta : artifacts) {
            paths.push_back(meta.path);
            if (meta.index.has_value()) {
                paths.push_back(meta.index->path);
            }
        }
        paths.push_back("receipts/producer_receipt.json");
        std::sort(paths.begin(), paths.end());
        std::string rows;
        for (const std::string& path : paths) {
            rows.append(sha256_file(run_ / path) + "  " + path + "\n");
        }
        write_text(run_ / "checksums.sha256", rows);
    }

    void build() {
        std::vector<CanonicalMeta> scientific;
        scientific.push_back(build_site_reads());
        scientific.push_back(build_methyl_calls());
        scientific.push_back(build_llm(run_, replay_semantic_edge_cases_));
        scientific.push_back(build_m1_sites());
        scientific.push_back(build_assignments());
        scientific.push_back(build_cooccurrence_pairs());
        scientific.push_back(build_cooccurrence_sites());
        scientific.push_back(build_topology());
        scientific.push_back(build_summary());
        const auto find_meta = [&](std::string_view id) -> CanonicalMeta& {
            const auto found = std::find_if(scientific.begin(), scientific.end(),
                                            [&](const CanonicalMeta& row) { return row.id == id; });
            check(found != scientific.end(), "canonical graph dependency is absent: " + std::string(id));
            return *found;
        };
        const auto bind = [&](std::string_view id, const char* transform,
                              std::initializer_list<std::string_view> dependencies) {
            CanonicalMeta& output = find_meta(id);
            output.transform = transform;
            if (id != "site_reads") {
                output.inputs.clear();
                for (const std::string_view dependency : dependencies) {
                    output.inputs.push_back(run_artifact_input(find_meta(dependency)));
                }
            }
        };
        bind("site_reads", "raw_alignment_to_site_reads", {});
        bind("methyl_calls", "site_reads_to_methyl_calls", {"site_reads"});
        bind("bernoulli_upper", "methyl_calls_to_m1", {"methyl_calls"});
        bind("m1_sites", "methyl_calls_to_m1", {"methyl_calls"});
        bind("m1_assignments", "methyl_calls_to_m1", {"methyl_calls"});
        bind("cooccurrence_pairs", "m1_to_cooccurrence", {"site_reads", "m1_sites", "m1_assignments"});
        bind("cooccurrence_sites", "m1_to_cooccurrence", {"cooccurrence_pairs", "m1_sites"});
        bind("topology_units", "cooccurrence_to_topology", {"site_reads", "cooccurrence_pairs", "cooccurrence_sites"});
        bind("summary", "run_artifacts_to_summary",
             {"site_reads", "methyl_calls", "m1_sites", "m1_assignments", "cooccurrence_pairs", "cooccurrence_sites",
              "topology_units"});
        if (fault_ == ScienceFault::kSelfConsistentProvenanceGraph) {
            find_meta("methyl_calls").transform = "m1_to_cooccurrence";
        }
        CanonicalMeta catalog = build_artifact_catalog(scientific);
        CanonicalMeta lineage = build_data_lineage(scientific);
        std::vector<CanonicalMeta> digest_sources = scientific;
        digest_sources.push_back(catalog);
        digest_sources.push_back(lineage);
        CanonicalMeta semantic = build_semantic_digests(digest_sources);
        std::vector<CanonicalMeta> artifacts = std::move(scientific);
        artifacts.push_back(std::move(catalog));
        artifacts.push_back(std::move(lineage));
        artifacts.push_back(std::move(semantic));
        write_producer_receipt(artifacts);
        write_checksums(artifacts);
    }

    std::filesystem::path repo_;
    std::filesystem::path run_;
    std::filesystem::path input_root_;
    std::filesystem::path manifest_;
    std::vector<LockedInputFixture> inputs_;
    std::vector<CanonicalMeta::InputBinding> manifest_inputs_;
    std::string manifest_sha256_;
    std::string input_snapshot_sha256_;
    std::string input_lock_sha256_;
    ScienceFault fault_;
    bool empty_topology_ = false;
    bool replay_semantic_edge_cases_ = false;
    std::uint64_t peak_threads_ = 1U;
    bool hcc1395_profile_ = false;
};

std::filesystem::path test_executable() {
    std::error_code error;
    const std::filesystem::path path = std::filesystem::canonical("/proc/self/exe", error);
    check(!error, "cannot resolve dataset-gate test executable");
    return path;
}

bool has_failed_check(const longlineage::validation::ArtifactValidationReport& report, const std::string& check_id) {
    return std::any_of(report.checks.begin(), report.checks.end(),
                       [&](const auto& row) { return !row.passed && row.check_id == check_id; });
}

void inject_forbidden_draft_field(const std::filesystem::path& run_root, const std::string& field, json_t* value) {
    const std::filesystem::path path = run_root / "receipts" / "producer_receipt.json";
    JsonPtr producer = load_json(path);
    json_t* draft = json_object_get(producer.get(), "run_receipt_draft");
    check(json_is_object(draft), "fixture run receipt draft is absent");
    json_object_set_new(draft, field.c_str(), value);
    write_json(path, producer.get());
}

void test_forbidden_caller_final_fields(const std::filesystem::path& root) {
    const std::vector<std::pair<std::string, JsonPtr>> cases = [&]() {
        std::vector<std::pair<std::string, JsonPtr>> values;
        values.emplace_back("validation_receipt_sha256", JsonPtr(json_string(std::string(64U, '0').c_str())));
        values.emplace_back("validation_profile", JsonPtr(json_string("PRODUCTION_7_DATASET")));
        values.emplace_back("state", JsonPtr(json_string("VALIDATED_FROZEN")));
        return values;
    }();
    for (std::size_t index = 0; index < cases.size(); ++index) {
        Fixture fixture(root / ("forged-draft-" + std::to_string(index)), Fault::kNone);
        inject_forbidden_draft_field(fixture.run(), cases[index].first, json_incref(cases[index].second.get()));
        const auto report = ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, {}});
        check(!report.all_pass && has_failed_check(report, "RUN_RECEIPT_DRAFT"),
              "caller final field must fail at RUN_RECEIPT_DRAFT: " + cases[index].first);
        check(!std::filesystem::exists(fixture.run() / "run_receipt.json"),
              "forged draft must not create a final receipt");
    }
}

void test_validation_receipt_write_failure_blocks_freeze(const std::filesystem::path& root) {
    const std::filesystem::path output = root / "write-failure-output";
    std::filesystem::create_directories(output / ".staging");
    Fixture fixture(root / "write-failure-fixture", Fault::kNone);
    const std::filesystem::path staging = output / ".staging" / "fixture-run";
    std::filesystem::rename(fixture.run(), staging);
    write_text(staging / (".validation_receipt.json.tmp." + std::to_string(static_cast<long long>(::getpid()))),
               "exclusive collision\n");
    const auto report = ArtifactValidator::validate_and_freeze(
        {{fixture.repo(), staging, test_executable(), true, {}}, output, false, {}});
    check(!report.dataset_gate_frozen && has_failed_check(report.validation, "FILE_CENSUS"),
          "a forged validation-receipt temporary must block dataset freeze at file census");
    check(!std::filesystem::exists(output / "fixture-run"),
          "validation receipt write failure must leave no final root");
    check(!std::filesystem::exists(staging / "run_receipt.json"),
          "validation receipt write failure must leave no run receipt");
}

void test_reduced_fixture_cannot_claim_dataset_gate(const std::filesystem::path& root) {
    const std::filesystem::path output = root / "reduced-output";
    std::filesystem::create_directories(output / ".staging");
    Fixture fixture(root / "reduced-fixture", Fault::kNone);
    const std::filesystem::path staging = output / ".staging" / "fixture-run";
    std::filesystem::rename(fixture.run(), staging);
    const auto report = ArtifactValidator::validate_and_freeze(
        {{fixture.repo(), staging, test_executable(), true, {}}, output, false, {}});
    check(report.validation.all_pass && !report.validation.scientific_conservation_replayed &&
              !report.dataset_gate_frozen && report.publication.status == RunRootStatus::kStateConflict,
          "reduced fixture must remain validation-only and non-frozen");
    check(!std::filesystem::exists(output / "fixture-run"), "reduced fixture must not create a final root");
}

void test_complete_science_fixture_freezes_dataset_gate(const std::filesystem::path& root) {
    const std::filesystem::path output = root / "complete-output";
    const std::filesystem::path staging = output / ".staging" / kFullRunId;
    CanonicalScienceFixture fixture(staging);
    const auto report = ArtifactValidator::validate_and_freeze(
        {{fixture.repo(), staging, test_executable(), true, fixture.manifest()}, output, false, {}});
    check(report.validation.all_pass && report.validation.scientific_conservation_replayed &&
              report.validation.validation_receipt_written && !report.validation.production_claim_allowed &&
              report.dataset_gate_frozen && report.publication.ok() &&
              report.inspection.state == RunRootObservedState::kPublished,
          "complete canonical science fixture must freeze only DATASET_GATE: " +
              longlineage::validation::render_finalize_report_json(report));
    const std::filesystem::path final_root = output / kFullRunId;
    check(std::filesystem::is_regular_file(final_root / "run_receipt.json") &&
              std::filesystem::is_regular_file(final_root / "validation_receipt.json") &&
              !std::filesystem::exists(staging),
          "complete science freeze must publish both receipts atomically");
    JsonPtr receipt = load_json(final_root / "run_receipt.json");
    check(std::string(json_string_value(json_object_get(receipt.get(), "state"))) == "VALIDATED_FROZEN_DATASET_GATE" &&
              std::string(json_string_value(json_object_get(receipt.get(), "validation_profile"))) == "DATASET_GATE" &&
              json_is_false(json_object_get(receipt.get(), "production_claim_allowed")),
          "dataset-gate receipt must never claim seven-dataset production");
}

void test_canonical_input_replay_fails_closed(const std::filesystem::path& root) {
    {
        CanonicalScienceFixture fixture(root / "missing-manifest" / ".staging" / kFullRunId);
        const auto report = ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, {}});
        check(!report.all_pass && has_failed_check(report, "INPUT_CONTENT_REPLAY"),
              "canonical validation without a manifest must fail closed");
    }
    {
        CanonicalScienceFixture fixture(root / "mutated-input" / ".staging" / kFullRunId);
        write_text(fixture.first_input(), "mutated after producer closeout\n");
        const auto report =
            ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, fixture.manifest()});
        check(!report.all_pass && has_failed_check(report, "INPUT_CONTENT_REPLAY"),
              "locked input mutation must fail independent content replay");
    }
    {
        CanonicalScienceFixture fixture(root / "mutated-manifest" / ".staging" / kFullRunId);
        JsonPtr manifest = load_json(fixture.manifest());
        json_object_set_new(manifest.get(), "unexpected", json_true());
        write_json(fixture.manifest(), manifest.get());
        const auto report =
            ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, fixture.manifest()});
        check(!report.all_pass && has_failed_check(report, "INPUT_CONTENT_REPLAY"),
              "manifest mutation must fail independent content replay");
    }
    {
        CanonicalScienceFixture fixture(root / "hcc-authority-mismatch" / ".staging" / kFullRunId, ScienceFault::kNone,
                                        false, false, 1U, true);
        const auto report =
            ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, fixture.manifest()});
        check(!report.all_pass && has_failed_check(report, "HCC1395_AUTHORITY_REPLAY"),
              "HCC1395 profile with synthetic role sizes/SHA must fail the authority replay");
    }
}

void test_repository_and_metadata_bindings_fail_closed(const std::filesystem::path& root) {
    {
        CanonicalScienceFixture fixture(root / "phase-ledger-mismatch" / ".staging" / kFullRunId);
        inject_forbidden_draft_field(fixture.run(), "phase_ledger_sha256", json_string(std::string(64U, '0').c_str()));
        const auto report =
            ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, fixture.manifest()});
        check(!report.all_pass && has_failed_check(report, "RUN_RECEIPT_DRAFT"),
              "run receipt draft must bind the current repository phase ledger");
    }
    {
        CanonicalScienceFixture fixture(root / "self-consistent-wrong-graph" / ".staging" / kFullRunId,
                                        ScienceFault::kSelfConsistentProvenanceGraph);
        const auto report =
            ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, fixture.manifest()});
        check(!report.all_pass && has_failed_check(report, "STATIC_PROVENANCE_GRAPH"),
              "self-consistent producer catalog/lineage must not override the independent static graph");
    }
    for (const auto& [name, fault, check_id] : std::vector<std::tuple<const char*, ScienceFault, const char*>>{
             {"manifest-site-m1-dataset-scope", ScienceFault::kSiteReadDatasetScope, "DATASET_SCOPE_REPLAY"},
             {"summary-dataset-scope", ScienceFault::kSummaryDatasetScope, "SUMMARY_CONSERVATION"},
             {"summary-m1-representation", ScienceFault::kSummaryM1Representation, "SUMMARY_CONSERVATION"}}) {
        CanonicalScienceFixture fixture(root / name / ".staging" / kFullRunId, fault);
        const auto report =
            ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, fixture.manifest()});
        check(!report.all_pass && has_failed_check(report, check_id),
              std::string(name) + " must fail independent dataset/metadata binding replay");
    }
}

void test_pre_publish_mutation_stays_query_invisible(const std::filesystem::path& root) {
    const std::filesystem::path output = root / "pre-publish-mutation-output";
    const std::filesystem::path staging = output / ".staging" / kFullRunId;
    CanonicalScienceFixture fixture(staging);
    DatasetGateFinalizeOptions options{
        {fixture.repo(), staging, test_executable(), true, fixture.manifest()},
        output,
        false,
        [](const std::filesystem::path& final_root) { write_text(final_root / "summary.json", "mutated\n"); }};
    const auto report = ArtifactValidator::validate_and_freeze(options);
    check(!report.dataset_gate_frozen && !report.validation.all_pass &&
              has_failed_check(report.validation, "PRE_FREEZE_REPLAY") &&
              report.inspection.state == RunRootObservedState::kFinalUnpublished && !report.inspection.query_visible &&
              !std::filesystem::exists(output / kFullRunId / "run_receipt.json"),
          "artifact mutation after atomic rename must remain query-invisible");
}

void test_process_thread_overflow_fails_closed(const std::filesystem::path& root) {
    CanonicalScienceFixture fixture(root / "peak-threads-47" / ".staging" / kFullRunId, ScienceFault::kNone, false,
                                    false, 47U);
    const auto report =
        ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, fixture.manifest()});
    check(!report.all_pass && has_failed_check(report, "RUN_RECEIPT_DRAFT"),
          "a process peak above the 46-slot manifest ceiling must fail closed");
}

void test_empty_topology_index_is_canonical(const std::filesystem::path& root) {
    const std::filesystem::path output = root / "empty-topology-output";
    const std::filesystem::path staging = output / ".staging" / kFullRunId;
    CanonicalScienceFixture fixture(staging, ScienceFault::kNone, true);
    const auto report = ArtifactValidator::validate_and_freeze(
        {{fixture.repo(), staging, test_executable(), true, fixture.manifest()}, output, false, {}});
    check(
        report.validation.all_pass && report.validation.scientific_conservation_replayed && report.dataset_gate_frozen,
        "zero-topology population and zero-row index must be a legal "
        "DATASET_GATE representation: " +
            longlineage::validation::render_finalize_report_json(report));
    const std::filesystem::path final_root = output / kFullRunId;
    JsonPtr producer = load_json(final_root / "receipts/producer_receipt.json");
    const json_t* artifacts = json_object_get(producer.get(), "artifacts");
    bool found = false;
    for (std::size_t index = 0; index < json_array_size(artifacts); ++index) {
        const json_t* artifact = json_array_get(artifacts, index);
        if (std::string(json_string_value(json_object_get(artifact, "artifact_id"))) != "topology_units") {
            continue;
        }
        found = true;
        const json_t* binding = json_object_get(artifact, "index");
        check(json_integer_value(json_object_get(artifact, "logical_rows")) == 0 &&
                  json_is_null(json_object_get(artifact, "primary_key_first")) &&
                  json_is_null(json_object_get(artifact, "primary_key_last")) &&
                  json_integer_value(json_object_get(binding, "logical_rows")) == 0,
              "empty topology receipt must use zero rows/null key range for "
              "both artifact and canonical index");
    }
    check(found, "empty topology producer receipt lacks topology artifact");
}

void test_m2_fail_partition_is_not_fdr_eligible(const std::filesystem::path& root) {
    const std::filesystem::path output = root / "m2-fail-output";
    const std::filesystem::path staging = output / ".staging" / kFullRunId;
    CanonicalScienceFixture fixture(staging, ScienceFault::kM2FailPartitionCase, true);
    const auto report = ArtifactValidator::validate_and_freeze(
        {{fixture.repo(), staging, test_executable(), true, fixture.manifest()}, output, false, {}});
    check(
        report.validation.all_pass && report.validation.scientific_conservation_replayed && report.dataset_gate_frozen,
        "M2 FAIL must count as evaluable-ineligible and keep descriptive "
        "exact p outside global FDR: " +
            longlineage::validation::render_finalize_report_json(report));
}

void test_producer_replay_semantic_edge_cases(const std::filesystem::path& root) {
    const std::filesystem::path output = root / "replay-semantic-output";
    const std::filesystem::path staging = output / ".staging" / kFullRunId;
    CanonicalScienceFixture fixture(staging, ScienceFault::kNone, false, true);
    const auto report = ArtifactValidator::validate_and_freeze(
        {{fixture.repo(), staging, test_executable(), true, fixture.manifest()}, output, false, {}});
    check(
        report.validation.all_pass && report.validation.scientific_conservation_replayed && report.dataset_gate_frozen,
        "zero-CpG focal ALT rows must remain in the M1 joined/LLM census, "
        "while other/outlier assignments must remain outside endpoint-A "
        "core groups: " +
            longlineage::validation::render_finalize_report_json(report));
}

void test_scientific_fault_replay(const std::filesystem::path& root) {
    const std::vector<std::tuple<const char*, ScienceFault, const char*>> cases = {
        {"site-read-join", ScienceFault::kSiteReadJoinCount, "SITE_READ_CONSERVATION"},
        {"methyl-interval", ScienceFault::kMethylInterval, "METHYL_CONSERVATION"},
        {"m1-joined", ScienceFault::kM1JoinedCount, "M1_CONSERVATION"},
        {"pair-cell", ScienceFault::kPairCell, "COOCCURRENCE_CONSERVATION"},
        {"exact-decision", ScienceFault::kExactDecision, "EXACT_STATISTIC_REPLAY"},
        {"global-fdr", ScienceFault::kGlobalFdr, "GLOBAL_FDR_REPLAY"},
        {"callability-formal", ScienceFault::kCallabilityFormal, "GLOBAL_FDR_REPLAY"},
        {"m2-rank", ScienceFault::kM2PrecedenceRank, "M2_PRECEDENCE_REPLAY"},
        {"m2-summary-double-count", ScienceFault::kM2SummaryDoubleCount, "SUMMARY_CONSERVATION"},
        {"topology-parent-mapping", ScienceFault::kTopologyMappingIncomplete, "TOPOLOGY_EDGE_REPLAY"},
        {"summary-count", ScienceFault::kSummaryCount, "SUMMARY_CONSERVATION"},
        {"summary-phase-scope", ScienceFault::kSummaryPhaseScope, "JSON_SCHEMA"}};
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& [name, fault, check_id] = cases[index];
        const std::filesystem::path run =
            root / ("science-fault-" + std::to_string(index) + "-" + name) / ".staging" / kFullRunId;
        CanonicalScienceFixture fixture(run, fault);
        const auto report =
            ArtifactValidator::validate({fixture.repo(), fixture.run(), test_executable(), false, fixture.manifest()});
        check(!report.all_pass && has_failed_check(report, check_id) &&
                  !std::filesystem::exists(fixture.run() / "validation_receipt.json") &&
                  !std::filesystem::exists(fixture.run() / "run_receipt.json"),
              std::string(name) + " must fail closed at the independent science replay: " +
                  longlineage::validation::render_report_json(report));
    }
}

void test_crash_and_digest_recovery(const std::filesystem::path& root) {
    const std::filesystem::path output = root / "atomic-output";
    std::filesystem::create_directories(output);
    auto created = RunRootTransaction::create_exclusive(output, "crash-run");
    check(created.result.ok() && created.transaction, "cannot create atomic recovery fixture");
    const std::string receipt =
        "{\"schema_name\":\"longlineage.run_receipt\","
        "\"state\":\"VALIDATED_FROZEN_DATASET_GATE\"}\n";
    const auto prepared = created.transaction->prepare_run_receipt(receipt);
    check(prepared.ok(), "cannot prepare recovery receipt");
    const auto renamed = created.transaction->rename_staging_to_final();
    check(renamed.ok(), "cannot atomically rename recovery root");
    auto inspection = RunRootTransaction::inspect(output, "crash-run");
    check(inspection.state == RunRootObservedState::kFinalUnpublished && !inspection.query_visible,
          "crash-after-rename root must remain query-invisible");
    const auto wrong = RunRootTransaction::recover_after_rename(output, "crash-run", std::string(64U, '0'));
    check(wrong.status == RunRootStatus::kStateConflict,
          "receipt-only recovery must fail closed before digest-based publication");
    inspection = RunRootTransaction::inspect(output, "crash-run");
    check(inspection.state == RunRootObservedState::kFinalUnpublished && !inspection.query_visible,
          "wrong digest must not publish the root");
    const auto recovered = RunRootTransaction::recover_after_rename(output, "crash-run", prepared.run_receipt_sha256);
    check(recovered.status == RunRootStatus::kStateConflict,
          "receipt-only recovery must remain disabled without full validator replay");
    inspection = RunRootTransaction::inspect(output, "crash-run");
    check(inspection.state == RunRootObservedState::kFinalUnpublished && !inspection.query_visible,
          "disabled recovery must keep the root query-invisible");
}

}  // namespace

int main() {
    try {
        Scratch scratch;
        test_forbidden_caller_final_fields(scratch.path());
        test_validation_receipt_write_failure_blocks_freeze(scratch.path());
        test_reduced_fixture_cannot_claim_dataset_gate(scratch.path());
        test_complete_science_fixture_freezes_dataset_gate(scratch.path());
        test_canonical_input_replay_fails_closed(scratch.path());
        test_repository_and_metadata_bindings_fail_closed(scratch.path());
        test_pre_publish_mutation_stays_query_invisible(scratch.path());
        test_process_thread_overflow_fails_closed(scratch.path());
        test_empty_topology_index_is_canonical(scratch.path());
        test_m2_fail_partition_is_not_fdr_eligible(scratch.path());
        test_producer_replay_semantic_edge_cases(scratch.path());
        test_scientific_fault_replay(scratch.path());
        test_crash_and_digest_recovery(scratch.path());
        std::cout << "validator dataset-gate tests passed: forbidden template fields, "
                     "receipt-write fail-closed, bounded-science boundary, complete "
                     "nine-artifact science replay, process-thread overflow, "
                     "canonical zero-topology index, "
                     "M2-screened formal FDR family, producer replay edge semantics, "
                     "scientific fault matrix, "
                     "input/authority/static-graph/dataset/metadata replay, pre-publish mutation rejection, "
                     "and fail-closed crash recovery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "validator dataset-gate test failure: " << error.what() << '\n';
        return 1;
    }
}
