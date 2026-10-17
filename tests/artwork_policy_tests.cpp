#include "app/game_service.h"
#include "app/config.h"
#include <windows.h>
#include <winrt/base.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace NitLink;
using namespace NitLink::Games;
namespace {
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void TestPlatforms() {
    for(const auto* source:{"switch","switch2"})Check(ArtworkPlatformForSource(source,L"PS5")==L"switch","explicit Switch source wins");
    for(const auto* source:{"ps5","ps4"})Check(ArtworkPlatformForSource(source,L"Nintendo Switch 2")==L"playstation","explicit PlayStation source never uses Nintendo policy");
    for(const auto* source:{"xbox_series","xbox_one","pc"})Check(ArtworkPlatformForSource(source,L"Switch 2")==L"other","other explicit platforms never infer Nintendo from a detector");
    for(const auto* label:{L"Switch 2",L"Nintendo Switch",L"Nintendo Switch 2"})Check(ArtworkPlatformForSource("auto",label)==L"switch","known automatic Switch identity");
    Check(ArtworkPlatformForSource("other",L"PS5")==L"playstation","recognized custom source");
    Check(ArtworkPlatformForSource("auto",{} )==L"unknown","missing source remains unknown");
    Check(ArtworkPlatformForSource("other",L"My PS5 and Nintendo Switch setup")==L"other","ambiguous custom names never guess a platform");
}
void TestPriority() {
    Cover tw,hk,store,ps;tw.provider=L"nintendo-tw";tw.shape=L"portrait";
    hk.provider=L"nintendo-hk-eshop";hk.shape=L"landscape";store.provider=L"nintendo-hk-store";store.shape=L"square";
    ps.provider=L"playstation-tw";ps.shape=L"square";
    const auto primary=PreferredOfficialArtwork({hk,store,tw,ps},L"switch");
    Check(primary.size()==1&&primary.front().provider==L"nintendo-tw","Taiwan excludes Hong Kong for the same game");
    Check(std::none_of(primary.begin(),primary.end(),[](const Cover& c){return c.shape==L"square";}),"shape filtering never resurrects hidden Hong Kong alternatives");
    const auto fallback=PreferredOfficialArtwork({hk,store,ps},L"switch");
    Check(fallback.size()==1&&fallback.front().provider==L"nintendo-hk-eshop","Discord-compatible Hong Kong eShop CDN remains the fallback");
    Check(PreferredOfficialArtwork({store},L"switch").empty(),"Discord-incompatible Hong Kong Store images never enter the picker");
    const auto sony=PreferredOfficialArtwork({tw,hk,store,ps},L"playstation");
    Check(sony.size()==1&&sony.front().provider==L"playstation-tw","PlayStation only receives its own official artwork");
    Check(PreferredOfficialArtwork({tw,hk,store,ps},L"unknown").empty()&&PreferredOfficialArtwork({tw,hk,store,ps},L"other").empty(),"unknown/Xbox/PC get no automatic official injection");
}
void TestRegistryAndPersistence() {
    auto provider=CreateOfficialArtworkProvider();
    const auto astro=provider->Lookup(5454475,L"zh-TW");
    Check(astro.has_value()&&astro->displayName==L"宇宙機器人","native official registry loads localized Astro Bot");
    Check(astro->artworkSources.size()==1&&astro->artworkSources.front().provider==L"playstation-tw","native Sony provenance");
    const auto& cover=astro->artworkSources.front();
    Check(cover.language==L"en-US"&&cover.shape==L"square"&&cover.kind==L"squareIcon","Taiwan page does not imply Traditional Chinese artwork");
    Check(SearchThumbnailArtworkPriority(cover)==0,"Astro Bot's official square is eligible for search thumbnails");
    const auto ragnarok=provider->Lookup(5296432,L"zh-TW");
    Check(ragnarok.has_value()&&ragnarok->artworkSources.size()==1&&
          ragnarok->artworkSources.front().provider==L"playstation-tw"&&
          SearchThumbnailArtworkPriority(ragnarok->artworkSources.front())==0,
          "the second registered PlayStation game retains its official square thumbnail");
    Check(provider->Allows(cover.url)&&!provider->Allows(L"https://image.api.playstation.com/vulcan/other.png"),"downloads require exact approved registry membership");
    for(const auto* bad:{L"http://image.api.playstation.com/vulcan/a.png",L"https://image.api.playstation.com.evil.test/vulcan/a.png",L"https://image.api.playstation.com:443/vulcan/a.png",L"https://image.api.playstation.com/vulcan/../a.png",L"https://image.api.playstation.com/vulcan/a.png?redirect=x"})Check(!IsPlayStationArtworkUrl(bad),"Sony URL boundary rejects unsafe authority/path/query");
    SavedGame game;game.id=astro->id;game.name=astro->name;game.displayNames[L"zh-TW"]=astro->displayName;
    game.customName=L"我的宇宙機器人 🎮";game.favorite=true;game.manualCover=true;game.lastUsedUtc=1700000000;
    SelectArtwork(game,cover);game.artworkMode=L"fit";game.cropX=.25;game.cropY=.75;game.artworkZoom=1.5;
    game.finalImageKey=std::wstring(64,L'a');game.sourceImageKey=std::wstring(64,L'b');
    Check(ValidSavedGame("sgdb-5454475",game),"Sony artwork is accepted by native saved-game validation");
    auto spoof=game;spoof.provider=L"nintendo-tw";spoof.sourceLabel=L"Official Nintendo Taiwan";
    Check(!ValidSavedGame("sgdb-5454475",spoof),"Sony images cannot impersonate Nintendo provenance");
    const auto path=std::filesystem::temp_directory_path()/(L"NitLink-artwork-policy-"+std::to_wstring(GetCurrentProcessId())+L".ini");
    struct Cleanup { std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);} } cleanup{path};
    Config config;config.currentGameId="sgdb-5454475";config.onlineGames[config.currentGameId]=game;
    config.gameSettings[config.currentGameId]={true,false};
    Check(config.audioFifoMs==12&&config.audioRenderMs==10,"HolyBear audio defaults unchanged");
    Check(config.Save(path.string()),"native config saves official artwork");
    Config restored;Check(restored.Load(path.string()),"native config restores official artwork");
    const auto& record=restored.onlineGames.at(config.currentGameId);
    Check(record.coverUrl==cover.url&&record.coverShape==L"square"&&record.provider==L"playstation-tw"&&record.isOfficial&&record.region==L"TW"&&record.language==L"en-US","official URL, shape and provenance persist");
    Check(record.customName==game.customName&&record.DisplayName(L"en-US")==game.customName&&record.favorite&&record.manualCover,"library name and independent favorite/manual state persist");
    Check(record.finalImageKey==game.finalImageKey&&record.sourceImageKey==game.sourceImageKey&&record.artworkMode==L"fit"&&record.cropX==.25&&record.cropY==.75&&record.artworkZoom==1.5,"artwork edits and local cache references persist");
    restored.onlineGames.at(config.currentGameId).favorite=false;
    Check(restored.gameSettings.at(config.currentGameId).nisEnabled&&restored.gameSettings.size()==1,"unfavoriting keeps the per-game quality profile");
}
}
int main(){try{winrt::init_apartment(winrt::apartment_type::multi_threaded);TestPlatforms();TestPriority();TestRegistryAndPersistence();winrt::clear_factory_cache();std::cout<<"PASS "<<checks<<" artwork platform/priority/URL/native-config checks\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
