#pragma once
#include <memory>
#include <string_view>
namespace NitLink::Games {
struct Image;
// CPU work on the game-service worker. WIC handles bounded raster decoding and
// PNG encoding. No capture/render settings or third-party image library changes.
std::shared_ptr<Image> RenderSquareArtwork(const Image&,std::wstring_view mode,double x,double y,double zoom);
}
