#pragma once

#include "elgato_device_identity.h"
#include <optional>

namespace NitLink {

inline bool IsElgato4KX(const std::wstring& deviceName)
{
    if (!IsElgatoDevice(deviceName)) return false;
    std::wstring lower = deviceName;
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    return lower.find(L"4k x") != std::wstring::npos;
}

// Before Open, pass the attempted format. After Open, always pass the actual
// negotiated subtype, including a successful Open that silently fell back.
// The injected sender exercises the same device guard without hardware I/O.
template<class Sender>
std::optional<bool> Apply4KXTonemapForCapture(const std::wstring& deviceName,
                                            bool captureIsP010, Sender&& send)
{
    if (!IsElgato4KX(deviceName)) return std::nullopt;
    return send(deviceName, !captureIsP010);
}

} // namespace NitLink
