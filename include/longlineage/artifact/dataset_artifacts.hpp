// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "longlineage/m1/science.hpp"

namespace longlineage::artifact {

struct SiteIndexWriteReceipt {
    std::filesystem::path path;
    std::uint64_t row_count{0};
    std::uintmax_t physical_bytes{0};
    std::string semantic_sha256;
    std::string physical_sha256;
};

struct DatasetArtifactWriteReceipt {
    std::string artifact_id;
    std::filesystem::path path;
    std::string schema_name;
    std::string schema_version;
    std::string format;
    std::uint64_t logical_rows{0};
    std::uint64_t logical_bytes{0};
    std::uintmax_t physical_bytes{0};
    std::string semantic_sha256;
    std::string physical_sha256;
    SiteIndexWriteReceipt index;
};

// Canonical scalar encoders shared by the producer artifact layer. The float
// representation is the frozen LONGLINEAGE_SCI17 form independently replayed
// by longlineage-validate.
[[nodiscard]] std::string canonical_float64(double value);
[[nodiscard]] std::string canonical_json_quote(std::string_view value);
[[nodiscard]] std::pair<std::string_view, std::string_view> canonical_ml_probability_interval(std::uint8_t ml_raw);

// Writes one canonical schema-bound JSON object followed by LF. The returned
// index receipt is empty because JSON artifacts are not indexed.
[[nodiscard]] DatasetArtifactWriteReceipt write_canonical_json_artifact(const std::filesystem::path& path,
                                                                        std::string artifact_id,
                                                                        std::string schema_name,
                                                                        std::string schema_version,
                                                                        std::string canonical_record);

class IndexedBgzfTsvWriter final {
   public:
    IndexedBgzfTsvWriter(std::filesystem::path path, std::filesystem::path index_path, std::string artifact_id,
                         std::string schema_name, std::string schema_version, std::string run_id,
                         std::vector<std::string> header, int compression_threads = 1);
    ~IndexedBgzfTsvWriter();

    IndexedBgzfTsvWriter(const IndexedBgzfTsvWriter&) = delete;
    IndexedBgzfTsvWriter& operator=(const IndexedBgzfTsvWriter&) = delete;

    void write_row(std::uint32_t dataset_order, std::uint64_t record_order, const std::vector<std::string>& fields);
    [[nodiscard]] DatasetArtifactWriteReceipt close();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class IndexedBgzfJsonlWriter final {
   public:
    IndexedBgzfJsonlWriter(std::filesystem::path path, std::filesystem::path index_path, std::string artifact_id,
                           std::string schema_name, std::string schema_version, std::string run_id,
                           int compression_threads = 1);
    ~IndexedBgzfJsonlWriter();

    IndexedBgzfJsonlWriter(const IndexedBgzfJsonlWriter&) = delete;
    IndexedBgzfJsonlWriter& operator=(const IndexedBgzfJsonlWriter&) = delete;

    // canonical_record excludes the terminating LF.
    void write_record(std::uint32_t dataset_order, std::uint64_t record_order, std::string canonical_record);
    [[nodiscard]] DatasetArtifactWriteReceipt close();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class IndexedLlmWriter final {
   public:
    IndexedLlmWriter(std::filesystem::path path, std::filesystem::path index_path, std::string run_id,
                     int compression_threads = 1);
    ~IndexedLlmWriter();

    IndexedLlmWriter(const IndexedLlmWriter&) = delete;
    IndexedLlmWriter& operator=(const IndexedLlmWriter&) = delete;

    void write_frame(std::uint32_t dataset_order, std::uint64_t site_order, const m1::Matrix& distance);
    [[nodiscard]] DatasetArtifactWriteReceipt close();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace longlineage::artifact
