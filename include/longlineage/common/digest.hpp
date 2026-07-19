// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "longlineage/common/parse_result.hpp"

namespace longlineage {

// SHA-256 is delegated to OpenSSL EVP so provider failures remain observable.
[[nodiscard]] ParseResult<std::string> sha256_hex(std::string_view bytes);
[[nodiscard]] ParseResult<std::string> sha256_file(const std::filesystem::path& path);

// This is BLAKE2b configured with digest_size=8, not a truncation of BLAKE2b-512.
[[nodiscard]] std::string blake2b_64_hex(std::string_view bytes);

}  // namespace longlineage
