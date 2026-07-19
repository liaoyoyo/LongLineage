// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

#include "longlineage/common/parse_result.hpp"

namespace longlineage {

class Position1 {
   public:
    [[nodiscard]] static ParseResult<Position1> from_value(std::uint64_t value) {
        if (value == 0) {
            return ParseResult<Position1>::failure(ParseReason::kMalformedValue,
                                                   "Position1 must be positive and one-based");
        }
        return ParseResult<Position1>::success(Position1(value));
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr std::uint64_t zero_based() const noexcept { return value_ - 1; }

    friend constexpr bool operator==(Position1 lhs, Position1 rhs) noexcept { return lhs.value_ == rhs.value_; }
    friend constexpr bool operator!=(Position1 lhs, Position1 rhs) noexcept { return !(lhs == rhs); }
    friend constexpr bool operator<(Position1 lhs, Position1 rhs) noexcept { return lhs.value_ < rhs.value_; }

   private:
    explicit constexpr Position1(std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_;
};

class Interval0 {
   public:
    [[nodiscard]] static ParseResult<Interval0> from_bounds(std::uint64_t begin, std::uint64_t end) {
        if (end <= begin) {
            return ParseResult<Interval0>::failure(ParseReason::kMalformedValue,
                                                   "Interval0 end must be greater than begin");
        }
        return ParseResult<Interval0>::success(Interval0(begin, end));
    }

    [[nodiscard]] constexpr std::uint64_t begin() const noexcept { return begin_; }
    [[nodiscard]] constexpr std::uint64_t end() const noexcept { return end_; }
    [[nodiscard]] constexpr std::uint64_t size() const noexcept { return end_ - begin_; }
    [[nodiscard]] constexpr bool contains(Position1 position) const noexcept {
        return begin_ <= position.zero_based() && position.zero_based() < end_;
    }

    friend constexpr bool operator==(Interval0 lhs, Interval0 rhs) noexcept {
        return lhs.begin_ == rhs.begin_ && lhs.end_ == rhs.end_;
    }
    friend constexpr bool operator!=(Interval0 lhs, Interval0 rhs) noexcept { return !(lhs == rhs); }
    friend constexpr bool operator<(Interval0 lhs, Interval0 rhs) noexcept {
        return std::tie(lhs.begin_, lhs.end_) < std::tie(rhs.begin_, rhs.end_);
    }

   private:
    constexpr Interval0(std::uint64_t begin, std::uint64_t end) noexcept : begin_(begin), end_(end) {}
    std::uint64_t begin_;
    std::uint64_t end_;
};

class ContigId {
   public:
    [[nodiscard]] static ParseResult<ContigId> from_string(std::string value) {
        if (value.empty()) {
            return ParseResult<ContigId>::failure(ParseReason::kMalformedValue, "ContigId must not be empty");
        }
        if (value.find_first_of("\t\r\n\0", 0, 4) != std::string::npos) {
            return ParseResult<ContigId>::failure(ParseReason::kMalformedValue,
                                                  "ContigId contains a prohibited control character");
        }
        return ParseResult<ContigId>::success(ContigId(std::move(value)));
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    friend bool operator==(const ContigId& lhs, const ContigId& rhs) noexcept { return lhs.value_ == rhs.value_; }
    friend bool operator!=(const ContigId& lhs, const ContigId& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const ContigId& lhs, const ContigId& rhs) noexcept { return lhs.value_ < rhs.value_; }

   private:
    explicit ContigId(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

enum class AlleleCall : char {
    kReference = 'R',
    kAlternate = 'A',
    kOther = 'O',
    kUnobservable = 'X',
};

[[nodiscard]] constexpr char to_char(AlleleCall call) noexcept { return static_cast<char>(call); }

[[nodiscard]] constexpr std::string_view to_string(AlleleCall call) noexcept {
    switch (call) {
        case AlleleCall::kReference:
            return "R";
        case AlleleCall::kAlternate:
            return "A";
        case AlleleCall::kOther:
            return "O";
        case AlleleCall::kUnobservable:
            return "X";
    }
    return "X";
}

[[nodiscard]] inline ParseResult<AlleleCall> parse_allele_call(char value) {
    switch (value) {
        case 'R':
            return ParseResult<AlleleCall>::success(AlleleCall::kReference);
        case 'A':
            return ParseResult<AlleleCall>::success(AlleleCall::kAlternate);
        case 'O':
            return ParseResult<AlleleCall>::success(AlleleCall::kOther);
        case 'X':
            return ParseResult<AlleleCall>::success(AlleleCall::kUnobservable);
        default:
            return ParseResult<AlleleCall>::failure(ParseReason::kUnsupportedValue,
                                                    "AlleleCall must be one of R, A, O, or X");
    }
}

[[nodiscard]] inline AlleleCall classify_allele(char observed, char reference, char alternate,
                                                bool observable) noexcept {
    if (!observable) {
        return AlleleCall::kUnobservable;
    }
    const auto uppercase = [](char base) {
        return base >= 'a' && base <= 'z' ? static_cast<char>(base - ('a' - 'A')) : base;
    };
    observed = uppercase(observed);
    reference = uppercase(reference);
    alternate = uppercase(alternate);
    if (observed == reference) {
        return AlleleCall::kReference;
    }
    if (observed == alternate) {
        return AlleleCall::kAlternate;
    }
    return AlleleCall::kOther;
}

}  // namespace longlineage
