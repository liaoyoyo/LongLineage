// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/artifact/dataset_artifacts.hpp"

#include <htslib/bgzf.h>
#include <jansson.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "longlineage/artifact/bgzf_tsv_writer.hpp"

namespace longlineage::artifact {
namespace {

using EvpContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

EvpContext new_sha256_context() {
    EvpContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("unable to initialize SHA-256 context");
    }
    return context;
}

void digest_bytes(EVP_MD_CTX* context, const void* data, std::size_t size) {
    if (size != 0 && EVP_DigestUpdate(context, data, size) != 1) {
        throw std::runtime_error("unable to update SHA-256 context");
    }
}

std::array<unsigned char, 32> finish_digest_raw(EVP_MD_CTX* context) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> buffer{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context, buffer.data(), &size) != 1 || size != 32U) {
        throw std::runtime_error("unable to finalize SHA-256 context");
    }
    std::array<unsigned char, 32> output{};
    std::copy_n(buffer.begin(), output.size(), output.begin());
    return output;
}

std::string hex_digest(const std::array<unsigned char, 32>& digest) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const unsigned char value : digest) {
        stream << std::setw(2) << static_cast<unsigned int>(value);
    }
    return stream.str();
}

std::string finish_digest(EVP_MD_CTX* context) { return hex_digest(finish_digest_raw(context)); }

void checked_logical_add(std::uint64_t& destination, std::size_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() - destination) {
        throw std::overflow_error("logical artifact byte count overflows");
    }
    destination += static_cast<std::uint64_t>(size);
}

void ensure_parent(const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        throw std::invalid_argument("artifact path is empty");
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

void write_bgzf(BGZF* output, std::string_view bytes, const std::filesystem::path& path) {
    const ssize_t written = bgzf_write(output, bytes.data(), bytes.size());
    if (written < 0 || static_cast<std::size_t>(written) != bytes.size()) {
        throw std::runtime_error("short BGZF write: " + path.string());
    }
}

std::string join_tsv(const std::vector<std::string>& fields) {
    std::size_t size = fields.empty() ? 0U : fields.size() - 1U;
    for (const auto& field : fields) {
        if (field.find_first_of("\t\n\r") != std::string::npos || field.find('\0') != std::string::npos) {
            throw std::invalid_argument("TSV field contains a forbidden control byte");
        }
        size += field.size();
    }
    std::string line;
    line.reserve(size);
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0U) {
            line.push_back('\t');
        }
        line.append(fields[index]);
    }
    return line;
}

struct IndexGroup {
    std::uint32_t dataset_order{0};
    std::uint64_t record_order{0};
    std::uint64_t first_virtual_offset{0};
    std::uint64_t past_end_virtual_offset{0};
    std::uint64_t logical_rows{0};
    EvpContext digest{new_sha256_context()};
    std::string range_semantic_sha256;
};

void finish_current_group(std::vector<IndexGroup>& groups, std::uint64_t stable_past_end) {
    if (groups.empty()) {
        return;
    }
    IndexGroup& group = groups.back();
    if (group.first_virtual_offset >= stable_past_end) {
        throw std::runtime_error("BGZF group boundary did not advance after flush");
    }
    group.past_end_virtual_offset = stable_past_end;
    if (group.range_semantic_sha256.empty()) {
        group.range_semantic_sha256 = finish_digest(group.digest.get());
    }
}

void update_group(std::vector<IndexGroup>& groups, std::uint32_t dataset_order, std::uint64_t record_order,
                  std::uint64_t stable_begin, std::string_view canonical_bytes) {
    if (!groups.empty()) {
        const auto& previous = groups.back();
        if (dataset_order < previous.dataset_order ||
            (dataset_order == previous.dataset_order && record_order < previous.record_order)) {
            throw std::invalid_argument("artifact index key is out of canonical order");
        }
    }
    if (groups.empty() || groups.back().dataset_order != dataset_order || groups.back().record_order != record_order) {
        IndexGroup group;
        group.dataset_order = dataset_order;
        group.record_order = record_order;
        group.first_virtual_offset = stable_begin;
        group.logical_rows = 1;
        digest_bytes(group.digest.get(), canonical_bytes.data(), canonical_bytes.size());
        groups.push_back(std::move(group));
        return;
    }
    IndexGroup& group = groups.back();
    ++group.logical_rows;
    digest_bytes(group.digest.get(), canonical_bytes.data(), canonical_bytes.size());
}

SiteIndexWriteReceipt write_index(const std::filesystem::path& path, std::string_view run_id,
                                  std::string_view artifact_id, const std::vector<IndexGroup>& groups) {
    BgzfTsvWriter writer(path, "longlineage.site_index", "1.0.0", std::string(run_id),
                         {"artifact_id", "dataset_order", "record_order", "first_virtual_offset",
                          "past_end_virtual_offset", "logical_rows", "range_semantic_sha256"},
                         1);
    for (const IndexGroup& group : groups) {
        if (group.range_semantic_sha256.size() != 64U) {
            throw std::logic_error("site-index group digest is not finalized");
        }
        writer.write_row({std::string(artifact_id), std::to_string(group.dataset_order),
                          std::to_string(group.record_order), std::to_string(group.first_virtual_offset),
                          std::to_string(group.past_end_virtual_offset), std::to_string(group.logical_rows),
                          group.range_semantic_sha256});
    }
    const TsvArtifactWriteReceipt receipt = writer.close();
    return SiteIndexWriteReceipt{
        receipt.path, receipt.row_count, receipt.physical_bytes, receipt.semantic_sha256, receipt.physical_sha256,
    };
}

void open_bgzf(BGZF*& output, const std::filesystem::path& path, int compression_threads) {
    if (compression_threads < 1) {
        throw std::invalid_argument("BGZF compression thread count must be positive");
    }
    ensure_parent(path);
    output = bgzf_open(path.c_str(), "w");
    if (output == nullptr) {
        throw std::runtime_error("cannot open BGZF output: " + path.string());
    }
    if (compression_threads > 1 && bgzf_mt(output, compression_threads, 256) != 0) {
        BGZF* closing = output;
        output = nullptr;
        bgzf_close(closing);
        throw std::runtime_error("cannot initialize BGZF compression threads");
    }
}

void close_bgzf(BGZF*& output, const std::filesystem::path& path) {
    if (output == nullptr) {
        throw std::logic_error("BGZF output is already closed");
    }
    BGZF* closing = output;
    output = nullptr;
    if (bgzf_close(closing) != 0) {
        throw std::runtime_error("cannot close BGZF output: " + path.string());
    }
}

void abort_bgzf(BGZF*& output) noexcept {
    if (output != nullptr) {
        BGZF* closing = output;
        output = nullptr;
        bgzf_close(closing);
    }
}

std::uint64_t virtual_offset(BGZF* output) {
    const std::int64_t value = bgzf_tell(output);
    if (value < 0) {
        throw std::runtime_error("cannot observe BGZF virtual offset");
    }
    return static_cast<std::uint64_t>(value);
}

std::uint64_t flush_and_virtual_offset(BGZF* output, const std::filesystem::path& path) {
    if (bgzf_flush(output) != 0) {
        throw std::runtime_error("cannot flush BGZF output: " + path.string());
    }
    return virtual_offset(output);
}

template <typename Integer>
void append_little(std::vector<unsigned char>& bytes, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes.push_back(static_cast<unsigned char>(value >> (index * 8U)));
    }
}

void append_little_double(std::vector<unsigned char>& bytes, double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_little(bytes, bits);
}

}  // namespace

std::string canonical_float64(double value) {
    if (!std::isfinite(value) || (value == 0.0 && std::signbit(value))) {
        throw std::invalid_argument("canonical float must be finite and not negative zero");
    }
    std::ostringstream raw;
    raw.imbue(std::locale::classic());
    raw << std::scientific << std::setprecision(16) << value;
    const std::string encoded = raw.str();
    const std::size_t exponent = encoded.find('e');
    if (exponent == std::string::npos || exponent + 2U >= encoded.size()) {
        throw std::runtime_error("cannot encode LONGLINEAGE_SCI17 value");
    }
    const char sign = encoded[exponent + 1U];
    std::string digits = encoded.substr(exponent + 2U);
    while (digits.size() < 3U) {
        digits.insert(digits.begin(), '0');
    }
    return encoded.substr(0, exponent + 1U) + sign + digits;
}

std::string canonical_json_quote(std::string_view value) {
    json_t* string = json_stringn(value.data(), value.size());
    if (string == nullptr) {
        throw std::invalid_argument("JSON string is not valid UTF-8");
    }
    char* encoded = json_dumps(string, JSON_ENCODE_ANY | JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(string);
    if (encoded == nullptr) {
        throw std::runtime_error("cannot encode JSON string");
    }
    std::string output(encoded);
    std::free(encoded);
    return output;
}

std::pair<std::string_view, std::string_view> canonical_ml_probability_interval(std::uint8_t ml_raw) {
    static const auto kIntervals = [] {
        std::array<std::pair<std::string, std::string>, 256> intervals;
        for (std::size_t raw = 0; raw < intervals.size(); ++raw) {
            const double lower = static_cast<double>(raw) / 256.0;
            const double upper = raw == 255U ? 1.0 : static_cast<double>(raw + 1U) / 256.0;
            intervals[raw] = {
                canonical_float64(lower),
                canonical_float64(upper),
            };
        }
        return intervals;
    }();
    const auto& interval = kIntervals[ml_raw];
    return {interval.first, interval.second};
}

DatasetArtifactWriteReceipt write_canonical_json_artifact(const std::filesystem::path& path, std::string artifact_id,
                                                          std::string schema_name, std::string schema_version,
                                                          std::string canonical_record) {
    if (artifact_id.empty() || schema_name.empty() || schema_version.empty() || canonical_record.empty() ||
        canonical_record.front() != '{' || canonical_record.back() != '}' ||
        canonical_record.find_first_of("\n\r\0") != std::string::npos) {
        throw std::invalid_argument("canonical JSON artifact identity/record is malformed");
    }
    ensure_parent(path);
    canonical_record.push_back('\n');
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open JSON artifact: " + path.string());
    }
    output.write(canonical_record.data(), static_cast<std::streamsize>(canonical_record.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("cannot close JSON artifact: " + path.string());
    }
    const std::string semantic = schema_name + "\t" + schema_version + "\n" + canonical_record;
    return DatasetArtifactWriteReceipt{
        std::move(artifact_id),
        path,
        std::move(schema_name),
        std::move(schema_version),
        "JSON",
        1,
        static_cast<std::uint64_t>(semantic.size()),
        std::filesystem::file_size(path),
        sha256_hex(semantic),
        sha256_file(path),
        {},
    };
}

struct IndexedBgzfTsvWriter::Impl {
    std::filesystem::path path;
    std::filesystem::path index_path;
    std::string artifact_id;
    std::string schema_name;
    std::string schema_version;
    std::string run_id;
    std::vector<std::string> header;
    BGZF* output{nullptr};
    EvpContext semantic{new_sha256_context()};
    std::vector<IndexGroup> groups;
    std::uint64_t rows{0};
    std::uint64_t logical_bytes{0};
    bool stable_group_boundaries{false};
    bool closed{false};
    std::optional<DatasetArtifactWriteReceipt> receipt;

    ~Impl() { abort_bgzf(output); }
};

IndexedBgzfTsvWriter::IndexedBgzfTsvWriter(std::filesystem::path path, std::filesystem::path index_path,
                                           std::string artifact_id, std::string schema_name, std::string schema_version,
                                           std::string run_id, std::vector<std::string> header, int compression_threads)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->index_path = std::move(index_path);
    impl_->artifact_id = std::move(artifact_id);
    impl_->schema_name = std::move(schema_name);
    impl_->schema_version = std::move(schema_version);
    impl_->run_id = std::move(run_id);
    impl_->header = std::move(header);
    impl_->stable_group_boundaries = compression_threads > 1;
    if (impl_->artifact_id.empty() || impl_->schema_name.empty() || impl_->schema_version.empty() ||
        impl_->run_id.empty() || impl_->header.empty()) {
        throw std::invalid_argument("indexed TSV writer identity/header is incomplete");
    }
    std::set<std::string> unique;
    for (const auto& field : impl_->header) {
        if (field.empty() || !unique.insert(field).second) {
            throw std::invalid_argument("indexed TSV header is empty or duplicated");
        }
    }
    open_bgzf(impl_->output, impl_->path, compression_threads);
    try {
        const std::string encoded_header = join_tsv(impl_->header);
        write_bgzf(impl_->output, "##longlineage_schema=" + impl_->schema_name + "\n", impl_->path);
        write_bgzf(impl_->output, "##schema_version=" + impl_->schema_version + "\n", impl_->path);
        write_bgzf(impl_->output, "##run_id=" + impl_->run_id + "\n", impl_->path);
        write_bgzf(impl_->output, "#" + encoded_header + "\n", impl_->path);
        const std::string semantic_preamble =
            impl_->schema_name + "\t" + impl_->schema_version + "\n" + encoded_header + "\n";
        digest_bytes(impl_->semantic.get(), semantic_preamble.data(), semantic_preamble.size());
        checked_logical_add(impl_->logical_bytes, semantic_preamble.size());
    } catch (...) {
        abort_bgzf(impl_->output);
        throw;
    }
}

IndexedBgzfTsvWriter::~IndexedBgzfTsvWriter() = default;

void IndexedBgzfTsvWriter::write_row(std::uint32_t dataset_order, std::uint64_t record_order,
                                     const std::vector<std::string>& fields) {
    if (impl_->closed || impl_->output == nullptr) {
        throw std::logic_error("cannot write a closed TSV artifact");
    }
    if (fields.size() != impl_->header.size()) {
        throw std::invalid_argument("TSV row field count differs from header");
    }
    std::string canonical = join_tsv(fields);
    canonical.push_back('\n');
    const bool new_group = impl_->groups.empty() || impl_->groups.back().dataset_order != dataset_order ||
                           impl_->groups.back().record_order != record_order;
    std::uint64_t stable_begin = 0;
    if (new_group) {
        stable_begin = impl_->stable_group_boundaries ? flush_and_virtual_offset(impl_->output, impl_->path)
                                                      : virtual_offset(impl_->output);
        finish_current_group(impl_->groups, stable_begin);
    } else if (!impl_->stable_group_boundaries) {
        stable_begin = virtual_offset(impl_->output);
    }
    write_bgzf(impl_->output, canonical, impl_->path);
    std::uint64_t stable_end = 0;
    if (!impl_->stable_group_boundaries) {
        stable_end = virtual_offset(impl_->output);
        if (stable_begin >= stable_end) {
            throw std::runtime_error("BGZF virtual offset did not advance for a logical record");
        }
    }
    digest_bytes(impl_->semantic.get(), canonical.data(), canonical.size());
    checked_logical_add(impl_->logical_bytes, canonical.size());
    update_group(impl_->groups, dataset_order, record_order, stable_begin, canonical);
    if (!impl_->stable_group_boundaries) {
        impl_->groups.back().past_end_virtual_offset = stable_end;
    }
    ++impl_->rows;
}

DatasetArtifactWriteReceipt IndexedBgzfTsvWriter::close() {
    if (impl_->receipt.has_value()) {
        return *impl_->receipt;
    }
    const std::uint64_t final_offset = flush_and_virtual_offset(impl_->output, impl_->path);
    finish_current_group(impl_->groups, final_offset);
    close_bgzf(impl_->output, impl_->path);
    impl_->closed = true;
    const SiteIndexWriteReceipt index =
        write_index(impl_->index_path, impl_->run_id, impl_->artifact_id, impl_->groups);
    DatasetArtifactWriteReceipt receipt{
        impl_->artifact_id,
        impl_->path,
        impl_->schema_name,
        impl_->schema_version,
        "TSV_BGZF",
        impl_->rows,
        impl_->logical_bytes,
        std::filesystem::file_size(impl_->path),
        finish_digest(impl_->semantic.get()),
        sha256_file(impl_->path),
        index,
    };
    impl_->receipt = receipt;
    return receipt;
}

struct IndexedBgzfJsonlWriter::Impl {
    std::filesystem::path path;
    std::filesystem::path index_path;
    std::string artifact_id;
    std::string schema_name;
    std::string schema_version;
    std::string run_id;
    BGZF* output{nullptr};
    EvpContext semantic{new_sha256_context()};
    std::vector<IndexGroup> groups;
    std::uint64_t rows{0};
    std::uint64_t logical_bytes{0};
    bool stable_group_boundaries{false};
    bool closed{false};
    std::optional<DatasetArtifactWriteReceipt> receipt;

    ~Impl() { abort_bgzf(output); }
};

IndexedBgzfJsonlWriter::IndexedBgzfJsonlWriter(std::filesystem::path path, std::filesystem::path index_path,
                                               std::string artifact_id, std::string schema_name,
                                               std::string schema_version, std::string run_id, int compression_threads)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->index_path = std::move(index_path);
    impl_->artifact_id = std::move(artifact_id);
    impl_->schema_name = std::move(schema_name);
    impl_->schema_version = std::move(schema_version);
    impl_->run_id = std::move(run_id);
    impl_->stable_group_boundaries = compression_threads > 1;
    if (impl_->artifact_id.empty() || impl_->schema_name.empty() || impl_->schema_version.empty() ||
        impl_->run_id.empty()) {
        throw std::invalid_argument("indexed JSONL writer identity is incomplete");
    }
    open_bgzf(impl_->output, impl_->path, compression_threads);
    const std::string preamble = impl_->schema_name + "\t" + impl_->schema_version + "\n";
    digest_bytes(impl_->semantic.get(), preamble.data(), preamble.size());
    checked_logical_add(impl_->logical_bytes, preamble.size());
}

IndexedBgzfJsonlWriter::~IndexedBgzfJsonlWriter() = default;

void IndexedBgzfJsonlWriter::write_record(std::uint32_t dataset_order, std::uint64_t record_order,
                                          std::string canonical_record) {
    if (impl_->closed || impl_->output == nullptr) {
        throw std::logic_error("cannot write a closed JSONL artifact");
    }
    if (canonical_record.empty() || canonical_record.front() != '{' || canonical_record.back() != '}' ||
        canonical_record.find_first_of("\n\r\0") != std::string::npos) {
        throw std::invalid_argument("JSONL record is not one canonical object line");
    }
    canonical_record.push_back('\n');
    const bool new_group = impl_->groups.empty() || impl_->groups.back().dataset_order != dataset_order ||
                           impl_->groups.back().record_order != record_order;
    std::uint64_t stable_begin = 0;
    if (new_group) {
        stable_begin = impl_->stable_group_boundaries ? flush_and_virtual_offset(impl_->output, impl_->path)
                                                      : virtual_offset(impl_->output);
        finish_current_group(impl_->groups, stable_begin);
    } else if (!impl_->stable_group_boundaries) {
        stable_begin = virtual_offset(impl_->output);
    }
    write_bgzf(impl_->output, canonical_record, impl_->path);
    std::uint64_t stable_end = 0;
    if (!impl_->stable_group_boundaries) {
        stable_end = virtual_offset(impl_->output);
        if (stable_begin >= stable_end) {
            throw std::runtime_error("BGZF virtual offset did not advance for a logical record");
        }
    }
    digest_bytes(impl_->semantic.get(), canonical_record.data(), canonical_record.size());
    checked_logical_add(impl_->logical_bytes, canonical_record.size());
    update_group(impl_->groups, dataset_order, record_order, stable_begin, canonical_record);
    if (!impl_->stable_group_boundaries) {
        impl_->groups.back().past_end_virtual_offset = stable_end;
    }
    ++impl_->rows;
}

DatasetArtifactWriteReceipt IndexedBgzfJsonlWriter::close() {
    if (impl_->receipt.has_value()) {
        return *impl_->receipt;
    }
    const std::uint64_t final_offset = flush_and_virtual_offset(impl_->output, impl_->path);
    finish_current_group(impl_->groups, final_offset);
    close_bgzf(impl_->output, impl_->path);
    impl_->closed = true;
    const SiteIndexWriteReceipt index =
        write_index(impl_->index_path, impl_->run_id, impl_->artifact_id, impl_->groups);
    DatasetArtifactWriteReceipt receipt{
        impl_->artifact_id,
        impl_->path,
        impl_->schema_name,
        impl_->schema_version,
        "JSONL_BGZF",
        impl_->rows,
        impl_->logical_bytes,
        std::filesystem::file_size(impl_->path),
        finish_digest(impl_->semantic.get()),
        sha256_file(impl_->path),
        index,
    };
    impl_->receipt = receipt;
    return receipt;
}

struct IndexedLlmWriter::Impl {
    std::filesystem::path path;
    std::filesystem::path index_path;
    std::string run_id;
    BGZF* output{nullptr};
    EvpContext semantic{new_sha256_context()};
    std::vector<IndexGroup> groups;
    std::uint64_t rows{0};
    std::uint64_t logical_bytes{0};
    bool stable_group_boundaries{false};
    bool closed{false};
    std::optional<DatasetArtifactWriteReceipt> receipt;

    ~Impl() { abort_bgzf(output); }
};

IndexedLlmWriter::IndexedLlmWriter(std::filesystem::path path, std::filesystem::path index_path, std::string run_id,
                                   int compression_threads)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->index_path = std::move(index_path);
    impl_->run_id = std::move(run_id);
    impl_->stable_group_boundaries = compression_threads > 1;
    if (impl_->run_id.empty()) {
        throw std::invalid_argument("LLM run_id is empty");
    }
    open_bgzf(impl_->output, impl_->path, compression_threads);
    const std::string preamble = "longlineage.bernoulli_upper\t1.0.0\n";
    digest_bytes(impl_->semantic.get(), preamble.data(), preamble.size());
    checked_logical_add(impl_->logical_bytes, preamble.size());
}

IndexedLlmWriter::~IndexedLlmWriter() = default;

void IndexedLlmWriter::write_frame(std::uint32_t dataset_order, std::uint64_t site_order, const m1::Matrix& distance) {
    if (impl_->closed || impl_->output == nullptr) {
        throw std::logic_error("cannot write a closed LLM artifact");
    }
    if (distance.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("LLM dimension exceeds uint32");
    }
    const std::uint64_t dimension = static_cast<std::uint64_t>(distance.size());
    for (const auto& row : distance) {
        if (row.size() != distance.size()) {
            throw std::invalid_argument("LLM distance matrix is not square");
        }
    }
    if (dimension != 0U && dimension - 1U > std::numeric_limits<std::uint64_t>::max() / dimension) {
        throw std::overflow_error("LLM strict-upper count overflows");
    }
    const std::uint64_t value_count = dimension * (dimension - 1U) / 2U;
    const std::uint64_t mask_bytes = (value_count + 7U) / 8U;
    if (value_count > (std::numeric_limits<std::size_t>::max() - 36U) / 8U ||
        mask_bytes > std::numeric_limits<std::size_t>::max() - 36U - static_cast<std::size_t>(value_count * 8U)) {
        throw std::overflow_error("LLM frame allocation overflows");
    }
    std::vector<unsigned char> frame;
    frame.reserve(36U + static_cast<std::size_t>(value_count * 8U) + static_cast<std::size_t>(mask_bytes));
    frame.insert(frame.end(), {'L', 'L', 'M', '1'});
    append_little(frame, static_cast<std::uint16_t>(1));
    append_little(frame, static_cast<std::uint16_t>(0));
    append_little(frame, dataset_order);
    append_little(frame, site_order);
    append_little(frame, static_cast<std::uint32_t>(dimension));
    append_little(frame, value_count);
    append_little(frame, mask_bytes);
    std::vector<unsigned char> mask(static_cast<std::size_t>(mask_bytes), 0U);
    std::uint64_t value_index = 0;
    for (std::size_t row = 0; row < distance.size(); ++row) {
        for (std::size_t column = row + 1U; column < distance.size(); ++column) {
            const double value = distance[row][column];
            const bool invalid = !std::isfinite(value) || value < 0.0;
            if (invalid) {
                mask[static_cast<std::size_t>(value_index / 8U)] |=
                    static_cast<unsigned char>(1U << (value_index % 8U));
                append_little_double(frame, 0.0);
            } else {
                append_little_double(frame, value);
            }
            ++value_index;
        }
    }
    frame.insert(frame.end(), mask.begin(), mask.end());
    EvpContext frame_digest = new_sha256_context();
    digest_bytes(frame_digest.get(), frame.data(), frame.size());
    const auto raw_digest = finish_digest_raw(frame_digest.get());

    if (!impl_->groups.empty()) {
        const auto& previous = impl_->groups.back();
        if (dataset_order < previous.dataset_order ||
            (dataset_order == previous.dataset_order && site_order <= previous.record_order)) {
            throw std::invalid_argument("LLM frame key is duplicate or out of order");
        }
    }
    const std::uint64_t stable_begin = impl_->stable_group_boundaries
                                           ? flush_and_virtual_offset(impl_->output, impl_->path)
                                           : virtual_offset(impl_->output);
    finish_current_group(impl_->groups, stable_begin);
    write_bgzf(impl_->output, std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()), impl_->path);
    write_bgzf(impl_->output, std::string_view(reinterpret_cast<const char*>(raw_digest.data()), raw_digest.size()),
               impl_->path);
    std::uint64_t stable_end = 0;
    if (!impl_->stable_group_boundaries) {
        stable_end = virtual_offset(impl_->output);
        if (stable_begin >= stable_end) {
            throw std::runtime_error("BGZF virtual offset did not advance for an LLM frame");
        }
    }
    digest_bytes(impl_->semantic.get(), frame.data(), frame.size());
    checked_logical_add(impl_->logical_bytes, frame.size());

    IndexGroup group;
    group.dataset_order = dataset_order;
    group.record_order = site_order;
    group.first_virtual_offset = stable_begin;
    group.past_end_virtual_offset = stable_end;
    group.logical_rows = 1;
    group.range_semantic_sha256 = hex_digest(raw_digest);
    impl_->groups.push_back(std::move(group));
    ++impl_->rows;
}

DatasetArtifactWriteReceipt IndexedLlmWriter::close() {
    if (impl_->receipt.has_value()) {
        return *impl_->receipt;
    }
    const std::uint64_t final_offset = flush_and_virtual_offset(impl_->output, impl_->path);
    finish_current_group(impl_->groups, final_offset);
    close_bgzf(impl_->output, impl_->path);
    impl_->closed = true;
    const SiteIndexWriteReceipt index = write_index(impl_->index_path, impl_->run_id, "bernoulli_upper", impl_->groups);
    DatasetArtifactWriteReceipt receipt{
        "bernoulli_upper",
        impl_->path,
        "longlineage.bernoulli_upper",
        "1.0.0",
        "LLM_BGZF",
        impl_->rows,
        impl_->logical_bytes,
        std::filesystem::file_size(impl_->path),
        finish_digest(impl_->semantic.get()),
        sha256_file(impl_->path),
        index,
    };
    impl_->receipt = receipt;
    return receipt;
}

}  // namespace longlineage::artifact
