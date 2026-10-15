#pragma once

#include "config.h"
#include <optional>
#include <string_view>

namespace NitLink {

struct SettingsMessage {
    std::wstring action;
    std::wstring text;
    double number = 0;
    uint32_t request = 0, gameId = 0, coverId = 0, page = 0;
    std::wstring shape;
    std::wstring coverProvider=L"all",catalogueQuery;
    std::wstring artworkMode;
    double cropX=.5,cropY=.5,artworkZoom=1;
    std::wstring key;
    CaptureFormatOverride format;
};

// Only complete, bounded messages matching an action's schema cross the bridge.
std::optional<SettingsMessage> ParseSettingsMessage(std::wstring_view json);

} // namespace NitLink
