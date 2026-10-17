#pragma once
#include "game_identity.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace NitLink::Games {
struct ArtworkCrop { uint32_t x=0,y=0,width=0,height=0; };
struct Cover : ArtworkMetadata {
    uint32_t id = 0;
    std::wstring url, thumb, author, languageSource, shape;
    bool languageVerified = false;
    std::wstring productTitle,productPage,productPlatform,productRelease;
    std::optional<ArtworkCrop> crop;
};
struct Game {
    uint32_t id = 0, steamAppId = 0;
    std::wstring name, displayName, source;
    std::vector<Cover> artworkSources;
    std::wstring displayNameSource;
};
// Taiwan is primary for Switch. A Hong Kong eShop CDN image is a fallback
// only when Taiwan has no mapping. The Hong Kong Store image endpoint is
// excluded: Discord's image proxy consistently rejects its JPEGs with 415.
inline std::vector<Cover> PreferredOfficialArtwork(std::vector<Cover> covers, std::wstring_view platform) {
    const bool taiwan=platform==L"switch" && std::any_of(covers.begin(),covers.end(),[](const Cover& c){return c.provider==L"nintendo-tw";});
    std::erase_if(covers,[&](const Cover& c){
        if(platform==L"playstation")return c.provider!=L"playstation-tw";
        if(platform!=L"switch")return true;
        return taiwan ? c.provider!=L"nintendo-tw" : c.provider!=L"nintendo-hk-eshop";
    });
    return covers;
}
inline const Cover* PreferredSquareArtwork(const std::vector<Cover>& covers) {
    const auto found=std::find_if(covers.begin(),covers.end(),[](const Cover& cover){
        return cover.shape==L"square"&&cover.provider!=L"nintendo-hk-store";
    });
    return found==covers.end()?nullptr:&*found;
}
inline const Cover* PreferredTaiwanLandscapeArtwork(const std::vector<Cover>& covers) {
    const auto found=std::find_if(covers.begin(),covers.end(),[](const Cover& cover){
        return cover.provider==L"nintendo-tw"&&cover.sourceLabel==L"Official Nintendo Taiwan"&&
            cover.region==L"TW"&&cover.isOfficial&&cover.shape==L"landscape";
    });
    return found==covers.end()?nullptr:&*found;
}
inline int SearchThumbnailArtworkPriority(const Cover& cover) {
    if(cover.isOfficial&&cover.provider!=L"nintendo-hk-store"&&cover.shape==L"landscape")return 0;
    if(!cover.isOfficial&&cover.provider==L"steamgriddb"&&cover.shape==L"landscape")return 1;
    if(cover.isOfficial&&cover.provider!=L"nintendo-hk-store"&&cover.shape==L"portrait")return 2;
    if(!cover.isOfficial&&cover.provider==L"steamgriddb"&&cover.shape==L"portrait")return 3;
    return 4;
}
inline int ActivityArtworkPriority(const Cover& cover) {
    if(cover.provider!=L"nintendo-hk-store"&&cover.shape==L"square")return 0;
    if(cover.isOfficial&&cover.provider!=L"nintendo-hk-store"&&cover.shape==L"landscape")return 1;
    if(cover.isOfficial&&cover.provider!=L"nintendo-hk-store"&&cover.shape==L"portrait")return 2;
    if(!cover.isOfficial&&cover.provider==L"steamgriddb"&&cover.shape==L"portrait")return 3;
    return 4;
}
inline const Cover* PreferredActivityArtwork(const std::vector<Cover>& covers) {
    const Cover* preferred=nullptr;
    int priority=4;
    for(const auto& cover:covers){
        const auto candidate=ActivityArtworkPriority(cover);
        if(candidate<priority){preferred=&cover;priority=candidate;}
    }
    return preferred;
}
inline void SelectArtwork(SavedGame& game,const Cover& cover){
    if(game.coverId!=cover.id||game.coverUrl!=cover.url){game.artworkMode=L"original";game.finalImageKey.clear();game.sourceImageKey.clear();game.cropX=game.cropY=.5;game.artworkZoom=1;}
    static_cast<ArtworkMetadata&>(game)=static_cast<const ArtworkMetadata&>(cover);
    game.coverId=cover.id;game.coverUrl=cover.url;game.coverThumb=cover.thumb;game.coverShape=cover.shape;
}
struct Reply {
    std::wstring status = L"invalid";
    std::vector<Game> games;
    std::vector<Cover> covers;
    std::vector<Cover> artworkSources;
    size_t workerCount=0;
    std::map<std::wstring, std::wstring> lookups;
    std::vector<std::wstring> queriedTerms, resolvedQueries, suggestions;
    size_t totalCount = 0;
    uint32_t page = 0;
    bool hasMore = false;
    std::wstring officialStatus,officialProvider;
};
struct Image { std::vector<unsigned char> png; uint32_t width=0,height=0; };
enum class Operation { Search, Covers, Resolve, CurrentImage, Image, LibraryImages, RenderArtwork };
struct Request {
    Operation operation = Operation::Search;
    uint32_t request = 0, gameId = 0, coverId = 0, page = 0;
    std::wstring query, locale, shape = L"square", imageUrl;
    std::wstring coverProvider=L"all",catalogueQuery;
    std::wstring platform=L"unknown";
    std::vector<SavedGame> library;
    std::wstring artworkMode,finalImageKey,sourceImageKey;
    double cropX=.5,cropY=.5,artworkZoom=1;
};
struct Result {
    Operation operation = Operation::Search;
    uint32_t request = 0, gameId = 0, coverId = 0;
    uint64_t generation = 0;
    std::wstring locale, query, shape, imageUri, imageStatus,coverProvider=L"all";
    std::wstring platform;
    Reply reply;
    std::wstring artworkMode,finalImageKey,sourceImageKey,sourceImageUri;
    double cropX=.5,cropY=.5,artworkZoom=1;
    std::optional<SavedGame> renderedGame;
};
using Cancelled = std::function<bool()>;
// Replaceable search provider: improvements behind this Worker do not require
// shipping an updated EXE. No API secret crosses into NitLink.
class Provider {
public:
    virtual ~Provider() = default;
    virtual Reply Search(std::wstring_view query, std::wstring_view locale, const Cancelled&) = 0;
    virtual Reply Covers(uint32_t gameId, std::wstring_view shape, std::wstring_view locale, uint32_t page, const Cancelled&) = 0;
};
class ArtworkProvider {
public:
    virtual ~ArtworkProvider()=default;
    virtual std::vector<Game> Search(std::wstring_view query,std::wstring_view locale)=0;
    virtual std::optional<Game> Lookup(uint32_t gameId,std::wstring_view locale)=0;
    virtual bool Allows(std::wstring_view url)=0;
    virtual std::optional<ArtworkCrop> Crop(std::wstring_view url)=0;
};
std::unique_ptr<ArtworkProvider> CreateNintendoTaiwanArtworkProvider();
std::unique_ptr<ArtworkProvider> CreateOfficialArtworkProvider();
class GameService {
public:
    explicit GameService(std::unique_ptr<Provider> provider = {});
    ~GameService();
    void Submit(Request request); // Coalesces queued work; never joins on the UI thread.
    void Cancel();
    std::vector<Result> TakeResults();
    std::shared_ptr<const Image> FindImage(std::wstring_view uri) const;
    void PinImage(std::wstring uri,std::wstring source = {});
private:
    void Run(std::stop_token stop);
    void Publish(Result result, const Cancelled&);
    std::wstring LoadArtworkImage(std::wstring_view url, const Cancelled&,std::wstring* actualShape=nullptr);
    std::wstring LoadFinalImage(std::wstring_view key,const Cancelled&);
    std::wstring StoreImage(std::shared_ptr<Image> image,std::wstring_view identity);
    std::unique_ptr<Provider> m_provider;
    std::unique_ptr<ArtworkProvider> m_artworkProvider;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<Request> m_pending;
    std::deque<Result> m_results;
    std::map<std::wstring, std::shared_ptr<const Image>> m_images;
    std::map<std::wstring, std::wstring> m_imageUrls;
    std::deque<std::pair<std::wstring, std::wstring>> m_imageOrder;
    size_t m_imageBytes = 0;
    uint64_t m_imageSequence = 0;
    std::wstring m_pinnedImage;
    std::wstring m_pinnedSourceImage;
    std::atomic<uint64_t> m_generation{0};
    std::jthread m_worker;
};
} // namespace NitLink::Games
