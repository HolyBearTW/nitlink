#pragma once

#include <windows.h>
#include <string>

namespace NitLink {

// Exact model aliases: never infer this policy from the vendor alone.
inline bool IsElgato4K60ProMk2(const std::wstring& deviceName)
{
    return CompareStringOrdinal(deviceName.c_str(), -1,
               L"Game Capture 4K60 Pro MK.2", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(deviceName.c_str(), -1,
               L"Elgato Game Capture 4K60 Pro MK.2", -1, TRUE) == CSTR_EQUAL;
}

inline bool UsesStandardLimitedP010Chroma(const std::wstring& deviceName,
                                         bool isP010, bool sourceFullRange)
{
    return isP010 && !sourceFullRange && IsElgato4K60ProMk2(deviceName);
}

inline bool IsElgatoDevice(const std::wstring& deviceName)
{
    std::wstring lower = deviceName;
    if (!lower.empty()) {
        CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    }
    // This driver omits the vendor prefix. An exact alias avoids enabling
    // vendor controls for unrelated devices with generic capture names.
    return lower.find(L"elgato") != std::wstring::npos ||
           lower == L"game capture 4k60 pro mk.2";
}

} // namespace NitLink
