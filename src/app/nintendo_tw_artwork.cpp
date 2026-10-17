#include "game_service.h"
#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <cmath>

namespace NitLink::Games {
namespace {
using namespace winrt::Windows::Data::Json;
class OfficialSeedArtworkProvider final : public ArtworkProvider {
    struct Entry { Game game; std::map<std::wstring,std::wstring> names; std::vector<std::wstring> terms; };
    std::map<uint32_t,Entry> entries;
    bool loaded=false;
    static std::wstring Text(const JsonObject& object,const wchar_t* key,size_t limit=256){
        if(!object.HasKey(key)||object.GetNamedValue(key).ValueType()!=JsonValueType::String)return {};
        const std::wstring value(object.GetNamedString(key));return ValidText(value,limit)?value:L"";
    }
    static uint32_t Id(const JsonObject& object,const wchar_t* key){
        if(!object.HasKey(key)||object.GetNamedValue(key).ValueType()!=JsonValueType::Number)return 0;
        const double value=object.GetNamedNumber(key);
        return std::isfinite(value)&&value>=1&&value<=UINT32_MAX&&std::floor(value)==value?static_cast<uint32_t>(value):0;
    }
    void Load(){
        if(loaded)return;loaded=true;
        try{
            std::wstring executable(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
            if(!length||length>=executable.size())return;executable.resize(length);
            std::ifstream file(std::filesystem::path(executable).parent_path()/L"official-artwork.json",std::ios::binary|std::ios::ate);
            if(!file||file.tellg()<=0||file.tellg()>256*1024)return;
            std::string body(static_cast<size_t>(file.tellg()),'\0');file.seekg(0);
            if(!file.read(body.data(),static_cast<std::streamsize>(body.size())))return;
            JsonObject root;
            if(!JsonObject::TryParse(winrt::to_hstring(body),root)||root.GetNamedNumber(L"version",0)!=1||
               !root.HasKey(L"games")||root.GetNamedValue(L"games").ValueType()!=JsonValueType::Array)return;
            const auto games=root.GetNamedArray(L"games");if(games.Size()>256)return;
            std::set<uint32_t> coverIds;
            for(const auto& value:games){
                if(value.ValueType()!=JsonValueType::Object)continue;const auto object=value.GetObject();
                Entry entry;auto& game=entry.game;game.id=Id(object,L"id");game.name=Text(object,L"name");
                if(!game.id||game.name.empty()||entries.contains(game.id))continue;
                if(object.HasKey(L"displayNames")&&object.GetNamedValue(L"displayNames").ValueType()==JsonValueType::Object){
                    const auto names=object.GetNamedObject(L"displayNames");
                    for(const auto* locale:{L"zh-TW",L"zh-CN",L"en-US",L"de-DE"}){const auto name=Text(names,locale);if(!name.empty())entry.names[locale]=name;}
                }
                if(object.HasKey(L"searchTerms")&&object.GetNamedValue(L"searchTerms").ValueType()==JsonValueType::Array){
                    const auto terms=object.GetNamedArray(L"searchTerms");
                    for(uint32_t i=0;i<std::min<uint32_t>(16,terms.Size());++i)if(terms.GetAt(i).ValueType()==JsonValueType::String){
                        const std::wstring term(terms.GetAt(i).GetString());if(ValidText(term)&&!term.empty())entry.terms.push_back(term);
                    }
                }
                if(!object.HasKey(L"artworkSources")||object.GetNamedValue(L"artworkSources").ValueType()!=JsonValueType::Array)continue;
                const auto sources=object.GetNamedArray(L"artworkSources");
                for(uint32_t i=0;i<std::min<uint32_t>(8,sources.Size());++i){
                    if(sources.GetAt(i).ValueType()!=JsonValueType::Object)continue;const auto source=sources.GetAt(i).GetObject();
                    Cover cover;cover.id=Id(source,L"id");cover.url=Text(source,L"url",512);cover.thumb=cover.url;
                    cover.provider=Text(source,L"provider");cover.sourceLabel=Text(source,L"sourceLabel",128);
                    cover.language=Text(source,L"language");cover.region=Text(source,L"region");cover.kind=Text(source,L"kind");
                    cover.shape=Text(source,L"shape");cover.languageSource=Text(source,L"sourcePage");
                    if(source.HasKey(L"crop")){
                        if(source.GetNamedValue(L"crop").ValueType()!=JsonValueType::Object)continue;
                        const auto crop=source.GetNamedObject(L"crop");ArtworkCrop area;
                        const double x=crop.GetNamedNumber(L"x",-1),y=crop.GetNamedNumber(L"y",-1);
                        area.width=Id(crop,L"width");area.height=Id(crop,L"height");
                        if(!std::isfinite(x)||!std::isfinite(y)||x<0||y<0||std::floor(x)!=x||std::floor(y)!=y||
                           x>4096||y>4096||!area.width||!area.height||area.width>4096||area.height>4096)continue;
                        area.x=static_cast<uint32_t>(x);area.y=static_cast<uint32_t>(y);cover.crop=area;
                    }
                    const bool tw=cover.provider==L"nintendo-tw"&&cover.sourceLabel==L"Official Nintendo Taiwan"&&cover.region==L"TW";
                    const bool hk=cover.provider==L"nintendo-hk-eshop"&&cover.sourceLabel==L"Official Nintendo eShop CDN"&&cover.region==L"HK";
                    const bool ps=cover.provider==L"playstation-tw"&&cover.sourceLabel==L"Official PlayStation Taiwan"&&cover.region==L"TW";
                    const bool imageSource=ps?IsPlayStationArtworkUrl(cover.url):IsNintendoArtworkUrl(cover.url);
                    const bool sourcePage=ps?cover.languageSource.starts_with(L"https://www.playstation.com/zh-hant-tw/games/"):
                        cover.languageSource.starts_with(L"https://www.nintendo.com/tw/")||cover.languageSource.starts_with(L"https://www.nintendo.com.tw/")||cover.languageSource.starts_with(L"https://img-eshop.cdn.nintendo.net/i/");
                    if(!cover.id||cover.id<4200000000u||!imageSource||(!tw&&!hk&&!ps)||
                       (!cover.language.empty()&&!IsCoverLanguage(cover.language))||
                       (cover.kind!=L"boxArt"&&cover.kind!=L"logo"&&cover.kind!=L"keyArt"&&cover.kind!=L"squareIcon")||
                       (cover.shape!=L"square"&&cover.shape!=L"portrait"&&cover.shape!=L"landscape")||
                       !sourcePage||
                       !source.HasKey(L"isOfficial")||source.GetNamedValue(L"isOfficial").ValueType()!=JsonValueType::Boolean||
                       !source.GetNamedBoolean(L"isOfficial")||!coverIds.insert(cover.id).second)continue;
                    cover.isOfficial=true;cover.languageVerified=!cover.language.empty();
                    if(tw){cover.author=L"台灣任天堂";cover.language=L"zh-TW";cover.languageVerified=true;}
                    else if(ps)cover.author=L"Sony Interactive Entertainment";
                    else cover.author=L"Nintendo";
                    if(game.source.empty())game.source=cover.provider;
                    game.artworkSources.push_back(std::move(cover));
                }
                if(!game.artworkSources.empty())entries.emplace(game.id,std::move(entry));
            }
        }catch(...){entries.clear();}
    }
    static bool Contains(std::wstring_view haystack,std::wstring_view needle){
        return !needle.empty()&&FindStringOrdinal(FIND_FROMSTART,haystack.data(),static_cast<int>(haystack.size()),needle.data(),static_cast<int>(needle.size()),TRUE)>=0;
    }
    static Game Localized(const Entry& entry,std::wstring_view locale){
        auto game=entry.game;const auto name=entry.names.find(std::wstring(locale));
        game.displayName=name!=entry.names.end()?name->second:game.name;game.displayNameSource=game.source;return game;
    }
public:
    std::optional<ArtworkCrop> Crop(std::wstring_view url) override{
        Load();for(const auto& [id,entry]:entries)for(const auto& cover:entry.game.artworkSources)if(cover.url==url)return cover.crop;return {};
    }
    bool Allows(std::wstring_view url) override{
        if(IsNintendoStoreImageUrl(url))return true;
        Load();for(const auto& [id,entry]:entries)for(const auto& cover:entry.game.artworkSources)if(cover.url==url)return true;return false;
    }
    std::optional<Game> Lookup(uint32_t id,std::wstring_view locale) override{
        Load();const auto found=entries.find(id);return found==entries.end()?std::nullopt:std::optional<Game>(Localized(found->second,locale));
    }
    std::vector<Game> Search(std::wstring_view query,std::wstring_view locale) override{
        Load();std::vector<Game> results;if(query.empty()||!ValidText(query))return results;
        for(const auto& [id,entry]:entries){
            bool match=Contains(entry.game.name,query)||query==std::to_wstring(id);
            for(const auto& [language,name]:entry.names)match|=Contains(name,query);
            for(const auto& term:entry.terms)match|=Contains(term,query);
            if(match)results.push_back(Localized(entry,locale));
        }return results;
    }
};
}
std::unique_ptr<ArtworkProvider> CreateOfficialArtworkProvider(){return std::make_unique<OfficialSeedArtworkProvider>();}
std::unique_ptr<ArtworkProvider> CreateNintendoTaiwanArtworkProvider(){return CreateOfficialArtworkProvider();}
}
