#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <memory>
#include "app/game_identity.h"

namespace NitLink {
namespace Games { struct Image; }

// A game/library ID and a WebView/cache URI are never Discord image assets.
// The original public image remains a separate candidate for edited artwork.
inline std::string DiscordCoverImage(const Games::SavedGame* game, bool locallyValidated) {
    if (!game || !locallyValidated || !Games::IsCoverUrl(game->coverUrl)) return {};
    std::string image;
    image.reserve(game->coverUrl.size());
    // IsCoverUrl permits ASCII URLs only; game display names never take this path.
    for (const auto code : game->coverUrl) image.push_back(static_cast<char>(code));
    return image;
}

enum class DiscordImageCheckState { Available, HttpError, InvalidImage, UnsupportedFormat, NetworkError, Cancelled };
struct DiscordImageCheck {
    DiscordImageCheckState state = DiscordImageCheckState::NetworkError;
    DWORD httpStatus = 0, error = 0;
    std::wstring reason;
    std::shared_ptr<const Games::Image> previewImage;
};

// Only the exact media-proxy result for our already approved original URL is
// eligible. No redirects, credentials, arbitrary hosts, or account credentials.
std::wstring DiscordImageProxyPath(std::string_view proxyImage, std::string_view original);
DiscordImageCheck CheckDiscordImageProxy(std::string_view proxyImage,
    std::string_view original, HANDLE stop);

} // namespace NitLink
