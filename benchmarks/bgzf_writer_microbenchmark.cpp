// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "longlineage/artifact/bgzf_tsv_writer.hpp"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: longlineage-benchmark-bgzf-writer OUTPUT ROWS WRITER_THREADS\n";
        return 2;
    }
    const std::filesystem::path output = argv[1];
    const std::size_t rows = std::stoull(argv[2]);
    const int writer_threads = std::stoi(argv[3]);
    std::vector<std::string> row = {
        "0",
        "synthetic-read-00000000000000000000000000000000",
        "1.2345678901234567e-001",
        "ALT",
        "synthetic-payload-for-lossless-output-throughput-measurement",
    };

    const auto started = std::chrono::steady_clock::now();
    longlineage::artifact::BgzfTsvWriter writer(
        output, "longlineage.writer_microbenchmark", "1.0.0", "local-microbenchmark",
        {"record_order", "read_id", "value", "allele", "payload"}, writer_threads);
    for (std::size_t index = 0; index < rows; ++index) {
        row[0] = std::to_string(index);
        writer.write_row(row);
    }
    const auto receipt = writer.close();
    const double wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "{\"rows\":" << receipt.row_count << ",\"logical_bytes\":" << receipt.logical_bytes
              << ",\"physical_bytes\":" << receipt.physical_bytes << ",\"writer_threads\":" << writer_threads
              << ",\"wall_seconds\":" << wall_seconds << ",\"semantic_sha256\":\""
              << receipt.semantic_sha256 << "\"}\n";
    return 0;
}
