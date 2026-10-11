#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace NitLink {

// Discord details/state need at least two Unicode scalars. A UTF-16 surrogate
// pair is one character; invalid Unicode never becomes an activity field.
constexpr bool HasMinimumDiscordActivityCharacters(std::wstring_view value) noexcept {
    std::size_t characters = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<uint32_t>(value[i]);
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == value.size() || value[i] < 0xdc00 || value[i] > 0xdfff)
                return false;
        } else if ((c >= 0xdc00 && c <= 0xdfff) || c > 0x10ffff) {
            return false;
        }
        ++characters;
    }
    return characters >= 2;
}

// These fields are optional: use meaningful fallback information when supplied,
// otherwise omit them. Local labels are never padded, repeated or truncated.
constexpr std::wstring_view GetDiscordActivityField(std::wstring_view value,
    std::wstring_view fallback = {}) noexcept {
    if (HasMinimumDiscordActivityCharacters(value)) return value;
    return HasMinimumDiscordActivityCharacters(fallback) ? fallback : std::wstring_view{};
}

} // namespace NitLink
