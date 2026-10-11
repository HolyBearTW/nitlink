#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NitLink {

struct HolyBearUpdateSettingsState {
    std::wstring current;
    std::wstring latest;
    uint64_t lastCheckUtc = 0;
    bool automatic = false;
    bool busy = false;
    std::wstring status;
    bool hasNewer = false;
    bool unread = false;
    std::wstring url;
};

struct ManualHdmiSourceOptionState {
    std::wstring value;
    std::wstring label;
};

// The state fields shared by the About panel, source pickers, and No Signal
// controls. Keeping their production serializer pure lets the WebView runtime
// test exercise the exact JSON that PushSettingsState sends.
struct WebViewCoreSettingsState {
    std::wstring languagePreference;
    std::wstring locale;
    HolyBearUpdateSettingsState holyBearUpdate;
    std::wstring manualHdmiSource;
    std::wstring manualHdmiSourceCustom;
    std::wstring effectiveHdmiSource;
    std::vector<ManualHdmiSourceOptionState> manualHdmiSourceOptions;
    bool preventSleep = false;
    std::wstring noSignalMode;
    std::wstring noSignalImage;
    bool noSignalImageAvailable = false;
    std::wstring noSignalFit;
    bool noSignalDimImage = true;
    std::vector<std::wstring> captureDevices;
    std::wstring activeDevice;
    std::wstring preferredDevice;
};

std::wstring JsonEscapeWide(std::wstring_view value);
std::wstring SerializeWebViewCoreSettingsProperties(const WebViewCoreSettingsState& state);
std::wstring SerializeWebViewCoreSettingsState(const WebViewCoreSettingsState& state);
std::wstring NextNoSignalMode(std::wstring_view currentMode);

} // namespace NitLink
