// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/artifact/bgzf_tsv_writer.hpp"

#include <htslib/bgzf.h>
#include <openssl/evp.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

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

std::string finish_digest(EVP_MD_CTX* context) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context, digest.data(), &length) != 1 || length != 32) {
        throw std::runtime_error("unable to finalize SHA-256 digest");
    }
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < length; ++index) {
        encoded << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return encoded.str();
}

void digest_bytes(EVP_MD_CTX* context, const void* data, std::size_t size) {
    if (size != 0 && EVP_DigestUpdate(context, data, size) != 1) {
        throw std::runtime_error("unable to update SHA-256 digest");
    }
}

bool is_valid_utf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index]);
        if (lead <= 0x7f) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        unsigned char second_min = 0x80;
        unsigned char second_max = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuation_count = 1;
        } else if (lead == 0xe0) {
            continuation_count = 2;
            second_min = 0xa0;
        } else if ((lead >= 0xe1 && lead <= 0xec) || (lead >= 0xee && lead <= 0xef)) {
            continuation_count = 2;
        } else if (lead == 0xed) {
            continuation_count = 2;
            second_max = 0x9f;
        } else if (lead == 0xf0) {
            continuation_count = 3;
            second_min = 0x90;
        } else if (lead >= 0xf1 && lead <= 0xf3) {
            continuation_count = 3;
        } else if (lead == 0xf4) {
            continuation_count = 3;
            second_max = 0x8f;
        } else {
            return false;
        }

        if (continuation_count > value.size() - index - 1) {
            return false;
        }
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if (second < second_min || second > second_max) {
            return false;
        }
        for (std::size_t offset = 2; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if (continuation < 0x80 || continuation > 0xbf) {
                return false;
            }
        }
        index += continuation_count + 1;
    }
    return true;
}

}  // namespace

struct BgzfTsvWriter::DigestContext {
    DigestContext() : value(new_sha256_context()) {}

    EvpContext value;
};

std::string sha256_hex(std::string_view bytes) {
    EvpContext context = new_sha256_context();
    digest_bytes(context.get(), bytes.data(), bytes.size());
    return finish_digest(context.get());
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open file for SHA-256: " + path.string());
    }

    EvpContext context = new_sha256_context();
    std::array<char, 1 << 16> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            digest_bytes(context.get(), buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("I/O failure while hashing file: " + path.string());
    }
    return finish_digest(context.get());
}

BgzfTsvWriter::BgzfTsvWriter(std::filesystem::path path, std::string schema_name, std::string schema_version,
                             std::string run_id, std::vector<std::string> header, int compression_threads)
    : path_(std::move(path)),
      schema_name_(std::move(schema_name)),
      schema_version_(std::move(schema_version)),
      run_id_(std::move(run_id)),
      header_(std::move(header)),
      digest_(std::make_unique<DigestContext>()) {
    if (path_.empty()) {
        throw std::invalid_argument("BGZF TSV output path must not be empty");
    }
    if (schema_name_.empty() || schema_version_.empty() || run_id_.empty()) {
        throw std::invalid_argument("schema name, version, and run_id must not be empty");
    }
    validate_field(schema_name_, "schema name");
    validate_field(schema_version_, "schema version");
    validate_field(run_id_, "run_id");
    if (header_.empty()) {
        throw std::invalid_argument("BGZF TSV header must not be empty");
    }
    if (compression_threads < 1) {
        throw std::invalid_argument("compression_threads must be positive");
    }

    std::set<std::string> unique_fields;
    for (const std::string& field : header_) {
        validate_field(field, "header field");
        if (field.empty()) {
            throw std::invalid_argument("header field must not be empty");
        }
        if (!unique_fields.insert(field).second) {
            throw std::invalid_argument("duplicate header field: " + field);
        }
    }

    output_ = bgzf_open(path_.c_str(), "w");
    if (output_ == nullptr) {
        throw std::runtime_error("unable to open BGZF output: " + path_.string());
    }
    if (compression_threads > 1 && bgzf_mt(output_, compression_threads, 256) != 0) {
        abort_close();
        throw std::runtime_error("unable to initialize BGZF compression threads");
    }

    try {
        const std::string header_line = join_fields(header_);
        write_physical_line("##longlineage_schema=" + schema_name_);
        write_physical_line("##schema_version=" + schema_version_);
        write_physical_line("##run_id=" + run_id_);
        write_physical_line("#" + header_line);

        digest_semantic_line(schema_name_ + "\t" + schema_version_);
        digest_semantic_line(header_line);
    } catch (...) {
        abort_close();
        throw;
    }
}

BgzfTsvWriter::~BgzfTsvWriter() { abort_close(); }

void BgzfTsvWriter::validate_field(std::string_view value, const char* role) {
    if (!is_valid_utf8(value)) {
        throw std::invalid_argument(std::string(role) + " is not valid UTF-8");
    }
    for (char character : value) {
        if (character == '\t' || character == '\n' || character == '\r' || character == '\0') {
            throw std::invalid_argument(std::string(role) + " contains a forbidden TSV control character");
        }
    }
}

std::string BgzfTsvWriter::join_fields(const std::vector<std::string>& fields) {
    std::size_t bytes = fields.empty() ? 0 : fields.size() - 1;
    for (const std::string& field : fields) {
        bytes += field.size();
    }
    std::string line;
    line.reserve(bytes);
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
            line.push_back('\t');
        }
        line.append(fields[index]);
    }
    return line;
}

void BgzfTsvWriter::write_physical_payload(std::string_view payload) {
    if (closed_ || output_ == nullptr) {
        throw std::logic_error("cannot write to a closed BGZF TSV artifact");
    }
    const ssize_t written = bgzf_write(output_, payload.data(), payload.size());
    if (written < 0 || static_cast<std::size_t>(written) != payload.size()) {
        throw std::runtime_error("short or failed BGZF write: " + path_.string());
    }
}

void BgzfTsvWriter::digest_semantic_payload(std::string_view payload) {
    if (closed_ || output_ == nullptr) {
        throw std::logic_error("cannot digest a closed BGZF TSV artifact");
    }
    if (payload.size() > std::numeric_limits<std::uint64_t>::max() - logical_bytes_) {
        throw std::overflow_error("semantic TSV byte count overflow");
    }
    digest_bytes(digest_->value.get(), payload.data(), payload.size());
    logical_bytes_ += static_cast<std::uint64_t>(payload.size());
}

void BgzfTsvWriter::write_physical_line(const std::string& line) {
    if (line.size() == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("logical TSV line is too large");
    }
    std::string payload = line;
    payload.push_back('\n');
    write_physical_payload(payload);
}

void BgzfTsvWriter::digest_semantic_line(const std::string& line) {
    if (line.size() == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("semantic TSV line is too large");
    }
    std::string payload = line;
    payload.push_back('\n');
    digest_semantic_payload(payload);
}

void BgzfTsvWriter::write_row(const std::vector<std::string>& fields) {
    if (fields.size() != header_.size()) {
        throw std::invalid_argument("TSV row field count differs from exact schema header");
    }
    for (const std::string& field : fields) {
        validate_field(field, "row field");
    }
    std::string payload = join_fields(fields);
    if (payload.size() == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("logical TSV line is too large");
    }
    payload.push_back('\n');
    write_physical_payload(payload);
    digest_semantic_payload(payload);
    ++row_count_;
}

TsvArtifactWriteReceipt BgzfTsvWriter::close() {
    if (receipt_) {
        return *receipt_;
    }
    if (closed_ || output_ == nullptr) {
        throw std::logic_error("BGZF TSV writer was aborted before close()");
    }

    BGZF* closing = output_;
    output_ = nullptr;
    if (bgzf_close(closing) != 0) {
        closed_ = true;
        throw std::runtime_error("failed to close BGZF output: " + path_.string());
    }
    closed_ = true;

    TsvArtifactWriteReceipt completed;
    completed.path = path_;
    completed.schema_name = schema_name_;
    completed.schema_version = schema_version_;
    completed.run_id = run_id_;
    completed.row_count = row_count_;
    completed.logical_bytes = logical_bytes_;
    completed.physical_bytes = std::filesystem::file_size(path_);
    completed.semantic_sha256 = finish_digest(digest_->value.get());
    completed.physical_sha256 = sha256_file(path_);
    receipt_ = std::make_unique<TsvArtifactWriteReceipt>(completed);
    return completed;
}

void BgzfTsvWriter::abort_close() noexcept {
    if (output_ != nullptr) {
        BGZF* closing = output_;
        output_ = nullptr;
        bgzf_close(closing);
    }
    closed_ = true;
}

}  // namespace longlineage::artifact
