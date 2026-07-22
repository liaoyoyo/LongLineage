// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/m1/hp_family.hpp"

namespace longlineage::m1 {
namespace {

[[nodiscard]] constexpr bool is_ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

}  // namespace

HpFamily hp_family(std::string_view normalized_or_legacy_hp) noexcept {
    const std::string_view tag = trim(normalized_or_legacy_hp);
    if (tag == "1" || tag == "HP1" || tag == "1-1" || tag == "1-2") {
        return HpFamily::kHp1Side;
    }
    if (tag == "2" || tag == "HP2" || tag == "2-1" || tag == "2-2") {
        return HpFamily::kHp2Side;
    }
    if (tag == "3") {
        return HpFamily::kHp3Ambiguous;
    }
    if (tag == "4") {
        return HpFamily::kHp4Both;
    }
    return HpFamily::kUntagged;
}

std::string_view to_string(HpFamily family) noexcept {
    switch (family) {
        case HpFamily::kHp1Side:
            return "HP1-side";
        case HpFamily::kHp2Side:
            return "HP2-side";
        case HpFamily::kHp3Ambiguous:
            return "HP3-ambiguous";
        case HpFamily::kHp4Both:
            return "HP4-both";
        case HpFamily::kUntagged:
            return "untagged";
    }
    return "untagged";
}

}  // namespace longlineage::m1
