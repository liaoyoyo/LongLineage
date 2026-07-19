// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace longlineage {

enum class ParseStatus {
    kOk,
    kOkEmpty,
    kError,
};

enum class ParseReason {
    kNone,
    kMissingRequiredField,
    kMalformedValue,
    kUnsupportedValue,
    kIndexError,
    kIoError,
};

[[nodiscard]] constexpr std::string_view to_string(ParseStatus status) noexcept {
    switch (status) {
        case ParseStatus::kOk:
            return "OK";
        case ParseStatus::kOkEmpty:
            return "OK_EMPTY";
        case ParseStatus::kError:
            return "ERROR";
    }
    return "ERROR";
}

[[nodiscard]] constexpr std::string_view to_string(ParseReason reason) noexcept {
    switch (reason) {
        case ParseReason::kNone:
            return "NONE";
        case ParseReason::kMissingRequiredField:
            return "MISSING_REQUIRED_FIELD";
        case ParseReason::kMalformedValue:
            return "MALFORMED_VALUE";
        case ParseReason::kUnsupportedValue:
            return "UNSUPPORTED_VALUE";
        case ParseReason::kIndexError:
            return "INDEX_ERROR";
        case ParseReason::kIoError:
            return "IO_ERROR";
    }
    return "MALFORMED_VALUE";
}

template <typename T>
struct ParseResult {
    std::optional<T> value;
    ParseStatus status = ParseStatus::kError;
    ParseReason reason = ParseReason::kMalformedValue;
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return status == ParseStatus::kOk || status == ParseStatus::kOkEmpty; }

    [[nodiscard]] bool empty() const noexcept { return status == ParseStatus::kOkEmpty; }

    [[nodiscard]] static ParseResult success(T parsed_value) {
        return ParseResult{std::move(parsed_value), ParseStatus::kOk, ParseReason::kNone, {}};
    }

    [[nodiscard]] static ParseResult success_empty(T parsed_value) {
        return ParseResult{std::move(parsed_value), ParseStatus::kOkEmpty, ParseReason::kNone, {}};
    }

    [[nodiscard]] static ParseResult failure(ParseReason error_reason, std::string error_detail) {
        return ParseResult{std::nullopt, ParseStatus::kError, error_reason, std::move(error_detail)};
    }
};

}  // namespace longlineage
