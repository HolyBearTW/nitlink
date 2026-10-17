#include "app/config.h"
#include "app/settings_message.h"
#include "app/game_service.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
unsigned failures = 0;
void Check(bool pass, const char* name) {
    if (!pass) { ++failures; std::cerr << "FAIL " << name << '\n'; }
}
struct SlowProvider final : NitLink::Games::Provider {
    std::atomic<bool>& started;
    explicit SlowProvider(std::atomic<bool>& started) : started(started) {}
    NitLink::Games::Reply Search(std::wstring_view query, std::wstring_view, const NitLink::Games::Cancelled& cancelled) override {
        if (query == L"slow") {
            started = true;
            while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        NitLink::Games::Reply reply;
        reply.status = L"ok";
        reply.games.push_back({query == L"slow" ? 10u : 20u, 0, L"Original", L"顯示名稱", L"fixture"});
        return reply;
    }
    NitLink::Games::Reply Covers(uint32_t, std::wstring_view, std::wstring_view, uint32_t, const NitLink::Games::Cancelled&) override { return {}; }
};
}
int main() {
    using namespace NitLink;
    using namespace Games;
    Check(DynamicId("sgdb-5303117") == 5303117 && !DynamicId("sgdb-01") && !DynamicId("sgdb-0") &&
          !DynamicId("sgdb-4294967296") && !DynamicId("spider-man-2"), "canonical dynamic IDs");
    const std::wstring url = L"https://cdn2.steamgriddb.com/grid/72654158f52bf25f9a8101f78aceaa2f.jpg";
    Check(IsCoverUrl(url) && IsCoverUrl(url + L"?"), "known CDN cover formats");
    Check(!IsCoverUrl(L"https://cdn2.steamgriddb.com.evil.test/grid/72654158f52bf25f9a8101f78aceaa2f.jpg") &&
          !IsCoverUrl(url + L"#x") && !IsCoverUrl(url + L"?redirect=evil") &&
          !IsCoverUrl(L"file:///C:/secret.png"), "cover authority/path validation");
    const std::wstring official=L"https://www.nintendo.com/tw/zelda/totk/products/img/soft/img__package.png";
    Check(IsNintendoArtworkUrl(official)&&!IsSteamGridCoverUrl(official)&&
          !IsNintendoArtworkUrl(official+L"?redirect=evil")&&
          !IsNintendoArtworkUrl(L"https://www.nintendo.com.evil.test/tw/cover.png")&&
          !IsNintendoArtworkUrl(L"https://www.nintendo.com:443/tw/cover.png")&&
          !IsNintendoArtworkUrl(L"https://www.nintendo.com/tw/../cover.png")&&
          !IsNintendoArtworkUrl(L"https://www.nintendo.com/tw/%2e%2e/cover.png"),"official artwork authority and path boundary");
    Check(IsLocale(L"de-DE") && IsCoverLanguage(L"de-DE") && !IsLocale(L"fr-FR"),
          "German search, cover, and saved-title locale is accepted");
    Cover taiwanLandscape;taiwanLandscape.provider=L"nintendo-tw";taiwanLandscape.sourceLabel=L"Official Nintendo Taiwan";
    taiwanLandscape.region=L"TW";taiwanLandscape.isOfficial=true;taiwanLandscape.shape=L"landscape";taiwanLandscape.kind=L"keyArt";
    Cover taiwanPortrait=taiwanLandscape;taiwanPortrait.shape=L"portrait";
    Cover taiwanLogo=taiwanLandscape;taiwanLogo.kind=L"logo";
    Cover hongKongLandscape=taiwanLandscape;hongKongLandscape.provider=L"nintendo-hk-eshop";
    hongKongLandscape.sourceLabel=L"Official Nintendo eShop CDN";hongKongLandscape.region=L"HK";
    const std::vector<Cover> searchArt={taiwanPortrait,taiwanLogo,hongKongLandscape,taiwanLandscape};
    Check(PreferredTaiwanLandscapeArtwork(searchArt)==&searchArt[1]&&
          PreferredTaiwanLandscapeArtwork({taiwanLandscape})!=nullptr,
          "search thumbnails prefer verified Nintendo Taiwan landscape artwork, including logo covers");
    Check(PreferredActivityArtwork(searchArt)==&searchArt[1],
          "game selection uses the Taiwan landscape logo when no square is available");
    Check(PreferredTaiwanLandscapeArtwork({taiwanPortrait,hongKongLandscape})==nullptr,
          "portrait and non-Taiwan art do not replace the existing search thumbnail");
    Check(IsImageResource(L"https://nitlink.invalid/game-images/1.png") &&
          !IsImageResource(L"https://nitlink.invalid/game-images/../secret.png"), "opaque local image resources");
    const std::wstring store=L"https://store.nintendo.com.hk/media/catalog/product/cache/55351146f864384e20b16a2043f3a323/1/1/110.jpg";
    Check(IsCoverUrl(store)&&!IsCoverUrl(store+L"?redirect=evil")&&
          !IsCoverUrl(L"https://store.nintendo.com.hk.evil.test/media/catalog/product/x.jpg")&&
          !IsCoverUrl(L"https://store.nintendo.com.hk/media/catalog/product/../x.jpg")&&
          !IsCoverUrl(L"https://store.nintendo.com.hk:443/media/catalog/product/x.jpg")&&
          !IsCoverUrl(L"https://store.nintendo.com.hk/media/catalog/product/%2e%2e/x.jpg"),"Hong Kong store image boundary");
    SavedGame storeGame;storeGame.id=35744;storeGame.name=L"The Legend of Zelda: Breath of the Wild";
    storeGame.coverUrl=storeGame.coverThumb=store;storeGame.provider=L"nintendo-hk-store";storeGame.region=L"HK";
    storeGame.sourceLabel=L"Official Nintendo Hong Kong Store";storeGame.isOfficial=true;
    Check(ValidSavedGame("sgdb-35744",storeGame),"store provenance survives native config validation");
    SavedGame localizedGame;localizedGame.id=35744;localizedGame.name=L"Breath of the Wild";
    localizedGame.displayNames={{L"en-US",L"Breath of the Wild"},{L"zh-TW",L"曠野之息"},
        {L"zh-CN",L"旷野之息"},{L"de-DE",L"Breath of the Wild"}};
    Check(ValidSavedGame("sgdb-35744",localizedGame),"saved games retain all four display locales");
    storeGame.provider=L"nintendo-tw";storeGame.region=L"TW";storeGame.sourceLabel=L"Official Nintendo Taiwan";
    Check(!ValidSavedGame("sgdb-35744",storeGame),"store image cannot impersonate Taiwan provenance");
    Check(ParseSettingsMessage(LR"({"action":"getGameCovers","value":{"gameId":35744,"shape":"all","page":1,"request":9,"provider":"nintendo","officialQuery":""}})").has_value(),"generic official catalogue browse schema");
    Check(!ParseSettingsMessage(LR"({"action":"getGameCovers","value":{"gameId":35744,"shape":"all","page":1,"request":9,"provider":"https://evil.test","officialQuery":""}})"),"renderer cannot choose network provider URL");
    Check(ParseSettingsMessage(LR"({"action":"setGame","value":"sgdb-5303117"})").has_value(), "dynamic game schema");
    Check(!ParseSettingsMessage(LR"({"action":"setGame","value":"sgdb-01"})"), "noncanonical ID rejected");
    Check(ParseSettingsMessage(LR"({"action":"searchGames","value":{"query":"薩爾達 / ゼルダ","request":7}})").has_value(), "Unicode query schema");
    Check(!ParseSettingsMessage(LR"({"action":"searchGames","value":{"query":"bad\ud800","request":7}})"), "malformed Unicode rejected");
    Check(!ParseSettingsMessage(LR"({"action":"selectGameCover","value":{"gameId":1,"coverId":2,"url":"https://evil.test/x"}})"), "renderer cannot supply cover URLs");
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 1;
    const auto path = std::filesystem::path(temp) / (L"nitlink-game-selection-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    Config saved;
    saved.currentGameId = "sgdb-5303117";
    auto& game = saved.onlineGames[saved.currentGameId];
    game.id = 5303117; game.name = L"The Legend of Zelda: Tears of the Kingdom";
    game.displayNames[L"zh-TW"] = L"薩爾達傳說 王國之淚";
    game.displayNames[L"zh-CN"] = L"塞尔达传说 王国之泪";
    game.coverUrl = url; game.coverThumb = url; game.coverId = 694138; game.manualCover = true;
    saved.gameSettings["spider-man-2"].nisEnabled = true;
    saved.gameSettings[saved.currentGameId].colorExpansion = false;
    Check(saved.Save(path.string()), "persist online game");
    Config restored;
    Check(restored.Load(path.string()), "restore online game");
    const auto found = restored.onlineGames.find(saved.currentGameId);
    Check(found != restored.onlineGames.end() && found->second.manualCover && found->second.coverUrl == url &&
          found->second.coverId == 694138 && found->second.DisplayName(L"zh-TW") == game.displayNames[L"zh-TW"], "manual art and locale roundtrip");
    Check(restored.gameSettings["spider-man-2"].nisEnabled && !restored.gameSettings[saved.currentGameId].colorExpansion,
          "old and dynamic quality profiles coexist");
    auto& officialGame=restored.onlineGames[saved.currentGameId];officialGame.coverUrl=official;officialGame.coverThumb=official;
    officialGame.coverId=4200000002u;officialGame.provider=L"nintendo-tw";officialGame.sourceLabel=L"Official Nintendo Taiwan";
    officialGame.language=L"zh-TW";officialGame.region=L"TW";officialGame.kind=L"boxArt";officialGame.isOfficial=true;officialGame.favorite=true;
    Check(restored.Save(path.string()),"save manually selected official artwork");
    Config officialRestored;Check(officialRestored.Load(path.string()),"load official artwork without searching");
    const auto& officialRecord=officialRestored.onlineGames.at(saved.currentGameId);
    Check(officialRecord.coverUrl==official&&officialRecord.provider==L"nintendo-tw"&&officialRecord.isOfficial&&
          officialRecord.language==L"zh-TW"&&officialRecord.region==L"TW"&&officialRecord.manualCover&&officialRecord.favorite,
          "official source metadata and manual choice survive restart");
    officialRestored.onlineGames.at(saved.currentGameId).isOfficial=false;
    Check(!officialRestored.Save(path.string()),"official URL cannot masquerade as unverified community artwork");
    auto& edited=officialRestored.onlineGames.at(saved.currentGameId);edited.isOfficial=true;edited.artworkMode=L"crop";
    edited.cropX=.25;edited.cropY=.75;edited.artworkZoom=1.5;edited.finalImageKey=std::wstring(64,L'a');edited.sourceImageKey=std::wstring(64,L'b');
    Check(officialRestored.Save(path.string()),"persist artwork recipe and durable file identities");
    Config editRestored;Check(editRestored.Load(path.string()),"restore artwork recipe without network");
    const auto& recipe=editRestored.onlineGames.at(saved.currentGameId);
    Check(recipe.artworkMode==L"crop"&&recipe.cropX==.25&&recipe.cropY==.75&&recipe.artworkZoom==1.5&&recipe.finalImageKey==edited.finalImageKey&&recipe.sourceImageKey==edited.sourceImageKey,
          "per-game recipe and both durable images survive config roundtrip");
    edited.finalImageKey=L"../outside";Check(!officialRestored.Save(path.string()),"reject caller-supplied filesystem paths");
    restored.onlineGames[saved.currentGameId].coverUrl = L"https://evil.test/cover.png";
    Check(!restored.Save(path.string()), "reject untrusted persisted art");
    std::error_code error; std::filesystem::remove(path, error);

    std::atomic<bool> started = false;
    GameService service(std::make_unique<SlowProvider>(started));
    Request old; old.operation = Operation::Search; old.request = 1; old.query = L"slow"; old.locale = L"zh-TW";
    service.Submit(old);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!started && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(started, "provider started off the caller thread");
    Request fresh = old; fresh.request = 2; fresh.query = L"new";
    const auto before = std::chrono::steady_clock::now(); service.Submit(fresh);
    Check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100), "new request never joins the old request");
    bool received = false, stale = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!received && std::chrono::steady_clock::now() < deadline) {
        for (const auto& result : service.TakeResults()) if (result.operation == Operation::Search) {
            stale |= result.request == 1;
            received |= result.request == 2 && result.reply.games.size() == 1 && result.reply.games[0].id == 20;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(received && !stale, "stale provider results cannot overwrite the new query");
    return failures ? 1 : 0;
}
