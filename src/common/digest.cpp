// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/common/digest.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

namespace longlineage {
namespace {

using EvpContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

[[nodiscard]] std::string openssl_error(std::string_view operation) {
    std::string detail(operation);
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return detail;
    }
    std::array<char, 256> buffer{};
    ERR_error_string_n(code, buffer.data(), buffer.size());
    detail.append(": ");
    detail.append(buffer.data());
    return detail;
}

[[nodiscard]] std::string hex(const unsigned char* bytes, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        output[index * 2] = kHex[bytes[index] >> 4U];
        output[index * 2 + 1] = kHex[bytes[index] & 0x0fU];
    }
    return output;
}

[[nodiscard]] constexpr std::uint64_t rotate_right(std::uint64_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (64U - bits));
}

[[nodiscard]] constexpr std::uint64_t load_little_endian_64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

void store_little_endian_64(std::uint64_t value, std::uint8_t* output) noexcept {
    for (unsigned index = 0; index < 8; ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

constexpr std::array<std::uint64_t, 8> kBlake2bIv = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

constexpr std::array<std::array<std::uint8_t, 16>, 12> kBlake2bSigma = {{
    {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
    {{14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}},
    {{11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}},
    {{7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8}},
    {{9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}},
    {{2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9}},
    {{12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}},
    {{13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10}},
    {{6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}},
    {{10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}},
    {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
    {{14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}},
}};

void blake2b_mix(std::array<std::uint64_t, 16>& state, unsigned a, unsigned b, unsigned c, unsigned d, std::uint64_t x,
                 std::uint64_t y) noexcept {
    state[a] = state[a] + state[b] + x;
    state[d] = rotate_right(state[d] ^ state[a], 32);
    state[c] += state[d];
    state[b] = rotate_right(state[b] ^ state[c], 24);
    state[a] = state[a] + state[b] + y;
    state[d] = rotate_right(state[d] ^ state[a], 16);
    state[c] += state[d];
    state[b] = rotate_right(state[b] ^ state[c], 63);
}

void blake2b_compress(std::array<std::uint64_t, 8>& hash, const std::uint8_t* block, std::uint64_t count,
                      bool last) noexcept {
    std::array<std::uint64_t, 16> message{};
    std::array<std::uint64_t, 16> state{};
    for (unsigned index = 0; index < 16; ++index) {
        message[index] = load_little_endian_64(block + index * 8U);
    }
    for (unsigned index = 0; index < 8; ++index) {
        state[index] = hash[index];
        state[index + 8] = kBlake2bIv[index];
    }
    state[12] ^= count;
    if (last) {
        state[14] = ~state[14];
    }
    for (unsigned round = 0; round < 12; ++round) {
        const auto& permutation = kBlake2bSigma[round];
        blake2b_mix(state, 0, 4, 8, 12, message[permutation[0]], message[permutation[1]]);
        blake2b_mix(state, 1, 5, 9, 13, message[permutation[2]], message[permutation[3]]);
        blake2b_mix(state, 2, 6, 10, 14, message[permutation[4]], message[permutation[5]]);
        blake2b_mix(state, 3, 7, 11, 15, message[permutation[6]], message[permutation[7]]);
        blake2b_mix(state, 0, 5, 10, 15, message[permutation[8]], message[permutation[9]]);
        blake2b_mix(state, 1, 6, 11, 12, message[permutation[10]], message[permutation[11]]);
        blake2b_mix(state, 2, 7, 8, 13, message[permutation[12]], message[permutation[13]]);
        blake2b_mix(state, 3, 4, 9, 14, message[permutation[14]], message[permutation[15]]);
    }
    for (unsigned index = 0; index < 8; ++index) {
        hash[index] ^= state[index] ^ state[index + 8];
    }
}

}  // namespace

ParseResult<std::string> sha256_hex(std::string_view bytes) {
    ERR_clear_error();
    EvpContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, openssl_error("EVP_MD_CTX_new failed"));
    }
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, openssl_error("EVP_DigestInit_ex failed"));
    }
    if (!bytes.empty() && EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, openssl_error("EVP_DigestUpdate failed"));
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 || digest_size != 32) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, openssl_error("EVP_DigestFinal_ex failed"));
    }
    std::string output = hex(digest.data(), digest_size);
    return bytes.empty() ? ParseResult<std::string>::success_empty(std::move(output))
                         : ParseResult<std::string>::success(std::move(output));
}

ParseResult<std::string> sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return ParseResult<std::string>::failure(ParseReason::kIoError,
                                                 "Cannot open file for SHA-256: " + path.string());
    }

    ERR_clear_error();
    EvpContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, openssl_error("EVP_MD_CTX_new failed"));
    }
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, openssl_error("EVP_DigestInit_ex failed"));
    }

    std::array<char, 1U << 20U> buffer{};
    std::uint64_t total = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) {
                return ParseResult<std::string>::failure(ParseReason::kIoError,
                                                         openssl_error("EVP_DigestUpdate failed"));
            }
            total += static_cast<std::uint64_t>(count);
        }
    }
    if (!input.eof()) {
        return ParseResult<std::string>::failure(ParseReason::kIoError,
                                                 "I/O error while hashing file: " + path.string());
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 || digest_size != 32) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, openssl_error("EVP_DigestFinal_ex failed"));
    }
    std::string output = hex(digest.data(), digest_size);
    return total == 0 ? ParseResult<std::string>::success_empty(std::move(output))
                      : ParseResult<std::string>::success(std::move(output));
}

std::string blake2b_64_hex(std::string_view bytes) {
    constexpr std::size_t kBlockSize = 128;
    constexpr std::uint64_t kDigestSize = 8;
    std::array<std::uint64_t, 8> hash = kBlake2bIv;
    hash[0] ^= 0x01010000ULL ^ kDigestSize;

    std::uint64_t count = 0;
    const auto* cursor = reinterpret_cast<const std::uint8_t*>(bytes.data());
    std::size_t remaining = bytes.size();
    while (remaining > kBlockSize) {
        count += kBlockSize;
        blake2b_compress(hash, cursor, count, false);
        cursor += kBlockSize;
        remaining -= kBlockSize;
    }

    std::array<std::uint8_t, kBlockSize> final_block{};
    if (remaining > 0) {
        std::copy(cursor, cursor + remaining, final_block.begin());
    }
    count += remaining;
    blake2b_compress(hash, final_block.data(), count, true);

    std::array<std::uint8_t, kDigestSize> digest{};
    store_little_endian_64(hash[0], digest.data());
    return hex(digest.data(), digest.size());
}

}  // namespace longlineage
