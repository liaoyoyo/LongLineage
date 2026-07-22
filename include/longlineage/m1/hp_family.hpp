// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string_view>

namespace longlineage::m1 {

inline constexpr std::string_view kHpFamilyRegistryVersion = "1.0.0";

enum class HpFamily {
    kHp1Side,
    kHp2Side,
    kHp3Ambiguous,
    kHp4Both,
    kUntagged,
};

[[nodiscard]] HpFamily hp_family(std::string_view normalized_or_legacy_hp) noexcept;
[[nodiscard]] std::string_view to_string(HpFamily family) noexcept;

}  // namespace longlineage::m1
