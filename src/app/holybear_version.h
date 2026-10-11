#pragma once
#include <string_view>
#include <array>
#include <cstdint>
#include <optional>
namespace NitLink::HolyBear {
inline constexpr std::wstring_view Version = L"1.2.4.1-holybear";
inline constexpr std::wstring_view ReleasePrefix = L"https://github.com/HolyBearTW/nitlink/releases/tag/";
using NumericVersion = std::array<uint16_t, 4>;
inline std::optional<NumericVersion> ParseVersion(std::wstring_view value) {
    if (value.starts_with(L"v")) value.remove_prefix(1);
    if (!value.ends_with(L"-holybear")) return {};
    value.remove_suffix(9);
    NumericVersion result{};
    unsigned count = 0;
    while (!value.empty() && count < result.size()) {
        const auto end = value.find(L'.');
        const auto number = value.substr(0, end);
        if (number.empty() || number.size() > 5 || (number.size() > 1 && number[0] == L'0')) return {};
        unsigned n = 0;
        for (auto c : number) {
            if (c < L'0' || c > L'9') return {};
            n = n * 10 + c - L'0';
        }
        if (n > 65535) return {};
        result[count++] = static_cast<uint16_t>(n);
        if (end == value.npos) { value = {}; break; }
        value.remove_prefix(end + 1);
        if (value.empty()) return {};
    }
    return value.empty() && count >= 3 ? std::optional(result) : std::nullopt;
}
inline bool IsReleaseUrl(std::wstring_view value) {
    return value.starts_with(ReleasePrefix) && value.substr(ReleasePrefix.size()).starts_with(L"v") &&
           ParseVersion(value.substr(ReleasePrefix.size())).has_value();
}
}
