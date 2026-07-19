// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct BGZF;

namespace longlineage::artifact {

struct TsvArtifactWriteReceipt {
    std::filesystem::path path;
    std::string schema_name;
    std::string schema_version;
    std::string run_id;
    std::uint64_t row_count = 0;
    std::uint64_t logical_bytes = 0;
    std::uintmax_t physical_bytes = 0;
    std::string semantic_sha256;
    std::string physical_sha256;
};

std::string sha256_hex(std::string_view bytes);
std::string sha256_file(const std::filesystem::path& path);

// Writes an exact-header TSV as BGZF and hashes the decompressed logical stream.
//
// The semantic digest covers the exact UTF-8/LF header and record bytes, not
// BGZF blocks. Therefore compression level, block layout, and writer thread count
// cannot alter the semantic identity of an artifact.
class BgzfTsvWriter {
   public:
    BgzfTsvWriter(std::filesystem::path path, std::string schema_name, std::string schema_version, std::string run_id,
                  std::vector<std::string> header, int compression_threads = 1);
    ~BgzfTsvWriter();

    BgzfTsvWriter(const BgzfTsvWriter&) = delete;
    BgzfTsvWriter& operator=(const BgzfTsvWriter&) = delete;
    BgzfTsvWriter(BgzfTsvWriter&&) = delete;
    BgzfTsvWriter& operator=(BgzfTsvWriter&&) = delete;

    void write_row(const std::vector<std::string>& fields);
    TsvArtifactWriteReceipt close();

    std::uint64_t row_count() const noexcept { return row_count_; }

    std::uint64_t logical_bytes() const noexcept { return logical_bytes_; }

   private:
    struct DigestContext;

    static void validate_field(std::string_view value, const char* role);
    static std::string join_fields(const std::vector<std::string>& fields);
    void write_physical_line(const std::string& line);
    void digest_semantic_line(const std::string& line);
    void abort_close() noexcept;

    std::filesystem::path path_;
    std::string schema_name_;
    std::string schema_version_;
    std::string run_id_;
    std::vector<std::string> header_;
    BGZF* output_ = nullptr;
    std::unique_ptr<DigestContext> digest_;
    std::uint64_t row_count_ = 0;
    std::uint64_t logical_bytes_ = 0;
    bool closed_ = false;
    std::unique_ptr<TsvArtifactWriteReceipt> receipt_;
};

}  // namespace longlineage::artifact
