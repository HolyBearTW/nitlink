#include "settings_state_json.h"

#include <sstream>

namespace NitLink {

std::wstring JsonEscapeWide(std::wstring_view value)
{
    std::wstring out;
    out.reserve(value.size() + 4);
    for (wchar_t c : value) {
        switch (c) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\b': out += L"\\b"; break;
        case L'\f': out += L"\\f"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (c < 0x20) {
                static constexpr wchar_t hex[] = L"0123456789abcdef";
                out += L"\\u00";
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    return out;
}

std::wstring SerializeWebViewCoreSettingsProperties(const WebViewCoreSettingsState& state)
{
    std::wostringstream json;
    bool first = true;
    const auto separator = [&] {
        if (!first) json << L",";
        first = false;
    };
    const auto stringProperty = [&](std::wstring_view name, std::wstring_view value) {
        separator();
        json << L"\"" << name << L"\":\"" << JsonEscapeWide(value) << L"\"";
    };
    const auto boolProperty = [&](std::wstring_view name, bool value) {
        separator();
        json << L"\"" << name << L"\":" << (value ? L"true" : L"false");
    };

    stringProperty(L"languagePreference", state.languagePreference);
    stringProperty(L"locale", state.locale);

    separator();
    const auto& update = state.holyBearUpdate;
    json << L"\"holyBearUpdate\":{";
    json << L"\"current\":\"" << JsonEscapeWide(update.current) << L"\",";
    json << L"\"latest\":\"" << JsonEscapeWide(update.latest) << L"\",";
    json << L"\"lastCheckUtc\":" << update.lastCheckUtc << L",";
    json << L"\"automatic\":" << (update.automatic ? L"true" : L"false") << L",";
    json << L"\"busy\":" << (update.busy ? L"true" : L"false") << L",";
    json << L"\"status\":\"" << JsonEscapeWide(update.status) << L"\",";
    json << L"\"hasNewer\":" << (update.hasNewer ? L"true" : L"false") << L",";
    json << L"\"unread\":" << (update.unread ? L"true" : L"false") << L",";
    json << L"\"url\":\"" << JsonEscapeWide(update.url) << L"\"}";

    stringProperty(L"manualHdmiSource", state.manualHdmiSource);
    stringProperty(L"manualHdmiSourceCustom", state.manualHdmiSourceCustom);
    stringProperty(L"effectiveHdmiSource", state.effectiveHdmiSource);

    separator();
    json << L"\"manualHdmiSourceOptions\":[";
    for (size_t i = 0; i < state.manualHdmiSourceOptions.size(); ++i) {
        if (i) json << L",";
        const auto& option = state.manualHdmiSourceOptions[i];
        json << L"{\"value\":\"" << JsonEscapeWide(option.value)
             << L"\",\"label\":\"" << JsonEscapeWide(option.label) << L"\"}";
    }
    json << L"]";

    boolProperty(L"preventSleep", state.preventSleep);
    stringProperty(L"noSignalMode", state.noSignalMode);
    stringProperty(L"noSignalImage", state.noSignalImage);
    boolProperty(L"noSignalImageAvailable", state.noSignalImageAvailable);
    stringProperty(L"noSignalFit", state.noSignalFit);
    boolProperty(L"noSignalDimImage", state.noSignalDimImage);

    separator();
    json << L"\"captureDevices\":[";
    for (size_t i = 0; i < state.captureDevices.size(); ++i) {
        if (i) json << L",";
        json << L"\"" << JsonEscapeWide(state.captureDevices[i]) << L"\"";
    }
    json << L"]";
    stringProperty(L"activeDevice", state.activeDevice);
    stringProperty(L"preferredDevice", state.preferredDevice);
    return json.str();
}

std::wstring SerializeWebViewCoreSettingsState(const WebViewCoreSettingsState& state)
{
    return L"{\"state\":{" + SerializeWebViewCoreSettingsProperties(state) + L"}}";
}

std::wstring NextNoSignalMode(std::wstring_view currentMode)
{
    return currentMode == L"image" ? L"default" : L"image";
}

} // namespace NitLink
