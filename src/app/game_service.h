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
inline void SelectArtwork(SavedGame& game,const Cover& cover){
    if(game.coverId!=cover.id||game.coverUrl!=cover.url){game.artworkMode=L"original";game.finalImageKey.clear();game.sourceImageKey.clear();game.cropX=game.cropY=.5;game.artworkZoom=1;}
    static_cast<ArtworkMetadata&>(game)=static_cast<const ArtworkMetadata&>(cover);
    game.coverId=cover.id;game.coverUrl=cover.url;game.coverThumb=cover.thumb;
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
    std::wstring officialStatus;
};
struct Image { std::vector<unsigned char> png; };
enum class Operation { Search, Covers, Resolve, CurrentImage, Image, LibraryImages, RenderArtwork };
struct Request {
    Operation operation = Operation::Search;
    uint32_t request = 0, gameId = 0, coverId = 0, page = 0;
    std::wstring query, locale, shape = L"square", imageUrl;
    std::wstring coverProvider=L"all",catalogueQuery;
    std::vector<SavedGame> library;
    std::wstring artworkMode,finalImageKey,sourceImageKey;
    double cropX=.5,cropY=.5,artworkZoom=1;
};
struct Result {
    Operation operation = Operation::Search;
    uint32_t request = 0, gameId = 0, coverId = 0;
    uint64_t generation = 0;
    std::wstring locale, query, shape, imageUri, imageStatus;
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
    std::wstring LoadImage(std::wstring_view url, const Cancelled&);
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
