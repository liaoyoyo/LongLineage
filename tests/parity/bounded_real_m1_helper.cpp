// SPDX-License-Identifier: GPL-3.0-only
// Read-only bounded-real parity helper. It emits no coordinates or read names.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "longlineage/common/digest.hpp"
#include "longlineage/m1/science.hpp"

namespace {

using longlineage::m1::Matrix;

struct CsvMatrix {
    std::vector<std::string> header;
    std::vector<std::string> row_ids;
    Matrix values;
};

std::vector<std::string> split_csv_line(const std::string& line) {
    if (line.find('"') != std::string::npos) {
        throw std::runtime_error("quoted CSV fields are outside the frozen historical matrix format");
    }
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = line.find(',', begin);
        fields.push_back(line.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin));
        if (comma == std::string::npos) {
            return fields;
        }
        begin = comma + 1;
    }
}

double parse_historical_value(const std::string& token, int maximum_decimals) {
    if (token == "NA" || token == "nan" || token == "NaN" || token.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t decimal = token.find('.');
    if (decimal != std::string::npos) {
        const std::size_t exponent = token.find_first_of("eE", decimal + 1);
        const std::size_t end = exponent == std::string::npos ? token.size() : exponent;
        if (end - decimal - 1 > static_cast<std::size_t>(maximum_decimals)) {
            throw std::runtime_error("historical matrix token exceeds its fixed decimal precision");
        }
    }
    std::size_t parsed = 0;
    const double value = std::stod(token, &parsed);
    if (parsed != token.size() || !std::isfinite(value)) {
        throw std::runtime_error("historical matrix contains a malformed numeric token");
    }
    return value;
}

CsvMatrix load_csv_matrix(const std::filesystem::path& path, int maximum_decimals) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open historical matrix");
    }
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("historical matrix is empty");
    }
    CsvMatrix result;
    result.header = split_csv_line(line);
    if (result.header.size() < 2 || result.header.front() != "read_id") {
        throw std::runtime_error("historical matrix header must start with read_id");
    }
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = split_csv_line(line);
        if (fields.size() != result.header.size() || fields.front().empty()) {
            throw std::runtime_error("historical matrix row width or read_id is invalid");
        }
        result.row_ids.push_back(fields.front());
        std::vector<double> values;
        values.reserve(fields.size() - 1);
        for (std::size_t index = 1; index < fields.size(); ++index) {
            values.push_back(parse_historical_value(fields[index], maximum_decimals));
        }
        result.values.push_back(std::move(values));
    }
    if (result.row_ids.empty()) {
        throw std::runtime_error("historical matrix has no data rows");
    }
    return result;
}

std::size_t parse_workers(std::string_view value) {
    std::size_t workers = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), workers);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || workers == 0) {
        throw std::runtime_error("workers must be a positive integer");
    }
    return workers;
}

std::string status_string(longlineage::m1::M1ReadsetStatus status) {
    switch (status) {
        case longlineage::m1::M1ReadsetStatus::kInsufficientAltReads:
            return "INSUFFICIENT_ALT_READS";
        case longlineage::m1::M1ReadsetStatus::kIncompleteDistanceBelowMinimum:
            return "INCOMPLETE_DISTANCE_BELOW_MINIMUM";
        case longlineage::m1::M1ReadsetStatus::kPrimitiveParityReady:
            return "PRIMITIVE_PARITY_READY";
        case longlineage::m1::M1ReadsetStatus::kFullClusteringReady:
            return "FULL_CLUSTERING_READY";
    }
    return "INVALID_STATUS";
}

std::string sha256_or_throw(std::string_view bytes) {
    auto result = longlineage::sha256_hex(bytes);
    if (!result.ok() || !result.value.has_value()) {
        throw std::runtime_error("cannot hash opaque bounded-real key");
    }
    return *result.value;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc > 5) {
            std::cerr << "usage: bounded_real_m1_helper DISTANCE_CSV METHYLATION_CSV OPAQUE_KEY [WORKERS]\n";
            return 2;
        }
        const CsvMatrix distance = load_csv_matrix(argv[1], 6);
        const CsvMatrix methylation = load_csv_matrix(argv[2], 4);
        if (distance.values.size() != distance.values.front().size() ||
            distance.header.size() - 1 != distance.values.size()) {
            throw std::runtime_error("historical observed distance matrix is not square");
        }
        const std::vector<std::string> distance_header(distance.header.begin() + 1, distance.header.end());
        if (distance.row_ids != distance_header) {
            throw std::runtime_error("historical distance row/header identities disagree");
        }
        if (methylation.row_ids != distance.row_ids) {
            throw std::runtime_error("historical distance/methylation read order disagrees");
        }
        longlineage::m1::M1Options options;
        options.workers = argc == 5 ? parse_workers(argv[4]) : 1;
        const auto result = longlineage::m1::run_full_m1_with_observed_distance(distance.values, methylation.values,
                                                                                distance.row_ids, options);
        const std::string key_sha256 = sha256_or_throw(argv[3]);
        std::cout << "{\"schema_name\":\"longlineage.bounded_real_m1_parity\",\"schema_version\":\"1.0.0\","
                  << "\"scope\":\"BOUNDED_REAL_READ_ONLY_NOT_PRODUCTION_AUTHORITY\","
                  << "\"input_mode\":\"HISTORICAL_DISTANCE_ROUND6_NULL_METHYLATION_ROUND4\","
                  << "\"key_sha256\":\"" << key_sha256 << "\",\"status\":\"" << status_string(result.status)
                  << "\",\"n_input\":" << distance.row_ids.size()
                  << ",\"n_retained\":" << result.retained_indices.size();
        if (result.analysis.has_value()) {
            std::cout << ",\"forced_k\":" << result.forced.groups << ",\"coarse_k\":" << result.analysis->coarse_groups
                      << ",\"fine_k\":" << result.analysis->fine_groups
                      << ",\"stable_null_multigroup\":" << (result.analysis->stable_null_multigroup ? "true" : "false")
                      << ",\"partition_sha256\":\"" << result.analysis->partition_sha256 << "\",\"trace_sha256\":\""
                      << result.analysis->trace_sha256 << "\"";
        } else {
            std::cout << ",\"forced_k\":0,\"coarse_k\":0,\"fine_k\":0,"
                      << "\"stable_null_multigroup\":false,\"partition_sha256\":null,\"trace_sha256\":null";
        }
        std::cout << ",\"coordinates_emitted\":false,\"read_names_emitted\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bounded_real_m1_helper: FAIL detail=" << error.what() << '\n';
        return 2;
    }
}
