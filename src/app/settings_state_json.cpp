#include "settings_state_json.h"

#include <sstream>
#include <locale>

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

std::wstring SerializeArtworkMetadataProperties(const Games::ArtworkMetadata& artwork){
    const auto q=[](std::wstring_view text){return L"\""+JsonEscapeWide(text)+L"\"";};
    return L"\"provider\":"+q(artwork.provider)+L",\"sourceLabel\":"+q(artwork.sourceLabel)+L",\"language\":"+q(artwork.language)+
        L",\"region\":"+q(artwork.region)+L",\"kind\":"+q(artwork.kind)+L",\"isOfficial\":"+(artwork.isOfficial?L"true":L"false");
}
std::wstring SerializeArtworkEditProperties(const Games::SavedGame& game){
    std::wostringstream out;out.imbue(std::locale::classic());
    out<<L"\"artworkMode\":\""<<game.artworkMode<<L"\",\"finalImageKey\":\""<<game.finalImageKey<<L"\",\"sourceImageKey\":\""<<game.sourceImageKey
        <<L"\",\"cropX\":"<<game.cropX<<L",\"cropY\":"<<game.cropY<<L",\"artworkZoom\":"<<game.artworkZoom
        <<L",\"discordArtworkState\":\""<<(game.artworkMode==L"original"?L"unverified":L"localOnly")<<L"\"";
    return out.str();
}
std::wstring SerializeArtworkCandidates(const std::vector<Games::Cover>& covers){
    const auto q=[](std::wstring_view text){return L"\""+JsonEscapeWide(text)+L"\"";};
    std::wostringstream json;json<<L"[";
    for(size_t i=0;i<covers.size();++i){if(i)json<<L",";const auto& cover=covers[i];
        json<<L"{\"id\":"<<cover.id<<L","<<SerializeArtworkMetadataProperties(cover)
            <<L",\"author\":"<<q(cover.author)<<L",\"shape\":"<<q(cover.shape)<<L",\"languageSource\":"<<q(cover.languageSource)
            <<L",\"languageVerified\":"<<(cover.languageVerified?L"true":L"false");
        if(!cover.productTitle.empty())json<<L",\"productTitle\":"<<q(cover.productTitle)<<L",\"productPage\":"<<q(cover.productPage)
            <<L",\"productPlatform\":"<<q(cover.productPlatform)<<L",\"productRelease\":"<<q(cover.productRelease);
        if(cover.crop)json<<L",\"crop\":{\"x\":"<<cover.crop->x<<L",\"y\":"<<cover.crop->y<<L",\"width\":"<<cover.crop->width<<L",\"height\":"<<cover.crop->height<<L"}";
        json<<L"}";
    }json<<L"]";return json.str();
}
std::wstring SerializeGameResult(const Games::Result& result)
{
    const auto quoted = [](std::wstring_view text) { return L"\"" + JsonEscapeWide(text) + L"\""; };
    const wchar_t* kind = result.operation == Games::Operation::Search ? L"search" :
        result.operation == Games::Operation::Covers ? L"covers" :
        result.operation == Games::Operation::CurrentImage ? L"currentImage" :
        result.operation == Games::Operation::Image ? L"image" : L"resolve";
    std::wostringstream json;
    json << L"{\"gameEvent\":{\"kind\":" << quoted(kind) << L",\"request\":" << result.request
         << L",\"gameId\":" << result.gameId << L",\"coverId\":" << result.coverId
         << L",\"locale\":" << quoted(result.locale) << L",\"shape\":" << quoted(result.shape)
         << L",\"status\":" << quoted(result.reply.status) << L",\"imageStatus\":" << quoted(result.imageStatus)
         << L",\"image\":" << quoted(result.imageUri) << L",\"dataMode\":\"real\",\"total\":" << result.reply.totalCount
         << L",\"workerCount\":"<<result.reply.workerCount
         << L",\"officialStatus\":"<<quoted(result.reply.officialStatus)
         << L",\"page\":" << result.reply.page << L",\"hasMore\":" << (result.reply.hasMore ? L"true" : L"false") << L",\"games\":[";
    for (size_t i = 0; i < result.reply.games.size(); ++i) {
        if (i) json << L",";
        const auto& g = result.reply.games[i];
        json << L"{\"id\":" << g.id << L",\"name\":" << quoted(g.name)
             << L",\"displayName\":" << quoted(g.displayName) << L",\"source\":" << quoted(g.source)
             << L",\"imageStatus\":\"loading\""
             << L",\"artworkSources\":"<<SerializeArtworkCandidates(g.artworkSources)
             << L",\"displayNameSource\":"<<quoted(g.displayNameSource)<<L",\"dataMode\":\"real\"}";
    }
    json << L"],\"covers\":"<<SerializeArtworkCandidates(result.reply.covers)
         <<L",\"artworkSources\":"<<SerializeArtworkCandidates(result.reply.artworkSources)<<L",";
    const auto list = [&](const wchar_t* key, const std::vector<std::wstring>& strings) {
        json << quoted(key) << L":[";
        for (size_t i = 0; i < strings.size(); ++i) { if (i) json << L","; json << quoted(strings[i]); }
        json << L"],";
    };
    list(L"queriedTerms", result.reply.queriedTerms);
    list(L"resolvedQueries", result.reply.resolvedQueries);
    list(L"suggestions", result.reply.suggestions);
    json << L"\"lookups\":{";
    bool first = true;
    for (const auto& [key, status] : result.reply.lookups) {
        if (!first) json << L",";
        first = false;
        json << quoted(key) << L":" << quoted(status);
    }
    json << L"}}}";
    return json.str();
}
} // namespace NitLink
