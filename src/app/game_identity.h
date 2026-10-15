#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <cmath>

namespace NitLink::Games {
inline constexpr wchar_t WorkerHost[] = L"holybear-game-api.nitlink.workers.dev";
inline constexpr wchar_t ImageOrigin[] = L"https://nitlink.invalid/game-images/";

// Reserved namespace. Built-in asset keys and their old quality profiles keep
// their original IDs; online profiles use sgdb-<canonical decimal ID>.
inline uint32_t ParseId(std::string_view text) {
    uint32_t id = 0;
    if (text.empty() || text.front() == '0' || text.size() > 10) return 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? id : 0;
}
inline uint32_t DynamicId(std::string_view id) {
    return id.starts_with("sgdb-") ? ParseId(id.substr(5)) : 0;
}
inline std::string GameKey(uint32_t id) { return "sgdb-" + std::to_string(id); }
inline bool IsLocale(std::wstring_view locale) {
    return locale == L"zh-TW" || locale == L"zh-CN" || locale == L"en-US";
}
inline bool IsCoverLanguage(std::wstring_view language) {
    return IsLocale(language) || language == L"ja-JP" || language == L"zxx";
}
inline bool ValidText(std::wstring_view text, size_t max = 256) {
    if (text.size() > max) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        auto c = static_cast<unsigned>(text[i]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    return true;
}
// Exact authority, paths, hex filename and raster extensions. No redirects,
// credentials, ports, fragments, traversal or arbitrary query parameters.
inline bool IsSteamGridCoverUrl(std::wstring_view url) {
    constexpr std::wstring_view origin = L"https://cdn2.steamgriddb.com/";
    if (!url.starts_with(origin)) return false;
    url.remove_prefix(origin.size());
    if (url.starts_with(L"grid/")) url.remove_prefix(5);
    else if (url.starts_with(L"thumb/")) url.remove_prefix(6);
    else return false;
    if (url.ends_with(L"?")) url.remove_suffix(1); // Legacy SGDB URLs.
    if (url.size() < 36 || url.size() > 37) return false;
    for (auto c : url.substr(0, 32))
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    const auto ext = url.substr(32);
    return ext == L".jpg" || ext == L".png" || ext == L".webp";
}
inline bool IsNintendoStoreImageUrl(std::wstring_view url){
    constexpr std::wstring_view prefix=L"https://store.nintendo.com.hk/media/catalog/product/";
    if(url.size()>512||!url.starts_with(prefix))return false;
    url.remove_prefix(prefix.size());
    if(url.empty()||url.find(L"..")!=url.npos||url.find(L"//")!=url.npos)return false;
    if(!std::all_of(url.begin(),url.end(),[](wchar_t c){return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'/'||c==L'_'||c==L'-'||c==L'.';}))return false;
    return url.ends_with(L".jpg")||url.ends_with(L".jpeg")||url.ends_with(L".png")||url.ends_with(L".webp");
}
inline bool IsNintendoArtworkUrl(std::wstring_view url) {
    if(IsNintendoStoreImageUrl(url))return true;
    if(url.size()>512)return false;
    constexpr std::wstring_view eshop=L"https://img-eshop.cdn.nintendo.net/i/";
    if(url.starts_with(eshop)){
        url.remove_prefix(eshop.size());
        return url.size()==68&&url.ends_with(L".jpg")&&std::all_of(url.begin(),url.begin()+64,[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');});
    }
    if(url.starts_with(L"https://www.nintendo.com/tw/"))url.remove_prefix(std::wstring_view(L"https://www.nintendo.com").size());
    else if(url.starts_with(L"https://www.nintendo.com.tw/"))url.remove_prefix(std::wstring_view(L"https://www.nintendo.com.tw").size());
    else return false;
    if(url.empty()||url.front()!=L'/'||url.find(L"..")!=url.npos||url.find(L"//")!=url.npos)return false;
    if(!std::all_of(url.begin(),url.end(),[](wchar_t c){return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'/'||c==L'_'||c==L'-'||c==L'.';}))return false;
    return url.ends_with(L".png")||url.ends_with(L".jpg")||url.ends_with(L".webp");
}
inline bool IsCoverUrl(std::wstring_view url) { return IsSteamGridCoverUrl(url)||IsNintendoArtworkUrl(url); }
struct ArtworkMetadata {
    std::wstring provider=L"steamgriddb", sourceLabel=L"SteamGridDB", language, region, kind=L"cover";
    bool isOfficial=false;
};
inline bool IsImageResource(std::wstring_view uri) {
    if (!uri.starts_with(ImageOrigin)) return false;
    uri.remove_prefix(std::wstring_view(ImageOrigin).size());
    if (!uri.ends_with(L".png")) return false;
    uri.remove_suffix(4);
    return !uri.empty() && uri.size() <= 20 &&
        std::all_of(uri.begin(), uri.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}
struct SavedGame : ArtworkMetadata {
    uint32_t id = 0;
    std::wstring name;
    std::map<std::wstring, std::wstring> displayNames;
    std::wstring coverUrl;
    std::wstring coverThumb;
    uint32_t coverId = 0;
    bool manualCover = false;
    bool favorite = false;
    uint64_t lastUsedUtc = 0;
    std::wstring customName;
    std::wstring artworkMode=L"original",finalImageKey,sourceImageKey;
    double cropX=.5,cropY=.5,artworkZoom=1;
    std::wstring DisplayName(std::wstring_view locale) const {
        if (!customName.empty()) return customName;
        const auto it = displayNames.find(std::wstring(locale));
        return it != displayNames.end() && !it->second.empty() ? it->second : name;
    }
};
inline bool ValidSavedGame(std::string_view key, const SavedGame& game) {
    const bool builtin = game.id == 0 && !key.empty() && key.size() <= 64 && !key.starts_with("sgdb-") &&
        std::all_of(key.begin(),key.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_';});
    if ((!builtin && (!game.id || DynamicId(key) != game.id)) || game.name.empty() || !ValidText(game.name) ||
        !ValidText(game.customName) || game.lastUsedUtc > 253402300799ull ||
        game.displayNames.size() > 3 || (!game.coverUrl.empty() && !IsCoverUrl(game.coverUrl)) ||
        (!game.coverThumb.empty() && !IsCoverUrl(game.coverThumb))) return false;
    if(!ValidText(game.sourceLabel,128)||(!game.language.empty()&&!IsCoverLanguage(game.language))||
       (!game.region.empty()&&(game.region.size()!=2||!std::all_of(game.region.begin(),game.region.end(),[](wchar_t c){return c>=L'A'&&c<=L'Z';}))))return false;
    if(IsNintendoArtworkUrl(game.coverUrl)){
        const bool tw=game.provider==L"nintendo-tw"&&game.region==L"TW"&&game.sourceLabel==L"Official Nintendo Taiwan";
        const bool hk=game.provider==L"nintendo-hk-eshop"&&game.region==L"HK"&&game.sourceLabel==L"Official Nintendo eShop CDN";
        const bool store=game.provider==L"nintendo-hk-store"&&game.region==L"HK"&&game.sourceLabel==L"Official Nintendo Hong Kong Store"&&IsNintendoStoreImageUrl(game.coverUrl);
        if(IsNintendoStoreImageUrl(game.coverUrl)&&(!store||(!game.coverThumb.empty()&&!IsNintendoStoreImageUrl(game.coverThumb))))return false;
        if((!tw&&!hk&&!store)||!game.isOfficial||(!game.language.empty()&&!IsCoverLanguage(game.language))||
           (game.kind!=L"boxArt"&&game.kind!=L"logo"&&game.kind!=L"keyArt"&&game.kind!=L"squareIcon"&&!(store&&game.kind==L"cover"))||
           (!game.coverThumb.empty()&&!IsNintendoArtworkUrl(game.coverThumb)))return false;
    }else if(game.provider!=L"steamgriddb"||game.isOfficial||(game.kind!=L"cover"&&game.kind!=L"boxArt"))return false;
    for (const auto& [locale, name] : game.displayNames)
        if (!IsLocale(locale) || !ValidText(name)) return false;
    return (game.artworkMode==L"original"||game.artworkMode==L"fit"||game.artworkMode==L"crop")&&
        std::isfinite(game.cropX)&&std::isfinite(game.cropY)&&std::isfinite(game.artworkZoom)&&
        game.cropX>=0&&game.cropX<=1&&game.cropY>=0&&game.cropY<=1&&game.artworkZoom>=1&&game.artworkZoom<=3&&
        (game.finalImageKey.empty()||(game.finalImageKey.size()==64&&std::all_of(game.finalImageKey.begin(),game.finalImageKey.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');})))&&
        (game.sourceImageKey.empty()||(game.sourceImageKey.size()==64&&std::all_of(game.sourceImageKey.begin(),game.sourceImageKey.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');})));
}
} // namespace NitLink::Games
