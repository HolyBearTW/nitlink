#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <memory>
#include "app/game_identity.h"

namespace NitLink {
namespace Games { struct Image; }

// Reuse Nintendo Taiwan's existing image CDN, with one fixed padding preset.
// This is a public CDN URL, not a new service or an upload of a local edit.
inline constexpr std::wstring_view DiscordTaiwanPadding =
    L"?w=512&h=512&fit=pad&bg=rgb:181d24&fm=png";
inline bool IsDiscordCoverUrl(std::wstring_view url) {
    if (url.size() > 300) return false;
    if (Games::IsCoverUrl(url)) return true;
    if (!url.ends_with(DiscordTaiwanPadding)) return false;
    const auto original = std::wstring(url.substr(0, url.size()-DiscordTaiwanPadding.size())) + L"?w=400&fm=webp";
    return Games::IsNintendoTaiwanLineupImageUrl(original);
}

// A game/library ID and a WebView/cache URI are never Discord image assets.
// The original public image remains a separate candidate for edited artwork.
inline std::string DiscordCoverImage(const Games::SavedGame* game, bool locallyValidated) {
    if (!game || !locallyValidated || !Games::IsCoverUrl(game->coverUrl)) return {};
    auto url = game->coverUrl;
    if (game->coverShape != L"square" && game->artworkMode != L"crop" &&
        Games::IsNintendoTaiwanLineupImageUrl(url))
        url = url.substr(0, url.find(L'?')) + std::wstring(DiscordTaiwanPadding);
    if (!IsDiscordCoverUrl(url)) return {};
    std::string image;
    image.reserve(url.size());
    // IsCoverUrl permits ASCII URLs only; game display names never take this path.
    for (const auto code : url) image.push_back(static_cast<char>(code));
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
