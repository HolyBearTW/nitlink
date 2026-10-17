#include "image_proxy.h"
#include "app/game_service.h"
#include <winhttp.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <objidl.h>
#include <chrono>
#include <vector>

namespace NitLink {
namespace {
using Microsoft::WRL::ComPtr;
struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
struct Progress {
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD error = 0, bytes = 0;
    ~Progress() { if (ready) CloseHandle(ready); if (closed) CloseHandle(closed); }
};
void CALLBACK HttpCallback(HINTERNET, DWORD_PTR context, DWORD status, void* data, DWORD length) {
    auto* p = reinterpret_cast<Progress*>(context);
    if (!p) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { SetEvent(p->closed); return; }
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
        p->error = static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError;
    else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) p->bytes = length;
    else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
             status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) return;
    SetEvent(p->ready);
}
struct RequestHandle {
    HINTERNET value;
    Progress& progress;
    bool registered = false;
    ~RequestHandle() {
        if (!value) return;
        WinHttpCloseHandle(value);
        // Closing an asynchronous WinHTTP request cancels outstanding I/O.
        // The final callback must release its context before the stack unwinds.
        if (registered) WaitForSingleObject(progress.closed, INFINITE);
    }
};
bool ValidImage(const std::vector<unsigned char>& bytes, const std::wstring& mime, std::shared_ptr<const Games::Image>& preview) {
    ComPtr<IWICImagingFactory> factory;
    if (bytes.empty() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ULONG written = 0;
    if (FAILED(stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written)) ||
        written != bytes.size() || FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder))) return false;
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) ||
        (mime == L"image/png" && container != GUID_ContainerFormatPng) ||
        (mime == L"image/jpeg" && container != GUID_ContainerFormatJpeg)) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0, height = 0, frames = 0;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames != 1 ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        !width || !height || width > 4096 || height > 4096 || uint64_t(width)*height > 8*1024*1024) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(frame.Get(),
        GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    std::vector<BYTE> pixels(static_cast<size_t>(width)*height*4);
    if(FAILED(converter->CopyPixels(nullptr, width*4, static_cast<UINT>(pixels.size()), pixels.data())))return false;
    // Reuse the pixels actually returned by Discord's proxy for the local
    // reference preview. Encoding failure does not imply a remote image failure.
    ComPtr<IWICBitmapScaler> scaler;
    const double scale=std::min(1.0,512.0/std::max(width,height));
    if(FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(),
        std::max(1u,static_cast<UINT>(width*scale)),std::max(1u,static_cast<UINT>(height*scale)),WICBitmapInterpolationModeFant)))return true;
    ComPtr<IStream> output;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> encoded;
    if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&output)) ||
       FAILED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)) ||
       FAILED(encoder->Initialize(output.Get(),WICBitmapEncoderNoCache)) ||
       FAILED(encoder->CreateNewFrame(&encoded,nullptr)) || FAILED(encoded->Initialize(nullptr)) ||
       FAILED(encoded->WriteSource(scaler.Get(),nullptr)) || FAILED(encoded->Commit()) || FAILED(encoder->Commit()))return true;
    STATSTG stat{};
    if(FAILED(output->Stat(&stat,STATFLAG_NONAME)) || !stat.cbSize.QuadPart || stat.cbSize.QuadPart>2*1024*1024)return true;
    auto image=std::make_shared<Games::Image>();image->png.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read=0;
    if(SUCCEEDED(output->Seek({},STREAM_SEEK_SET,nullptr)) &&
       SUCCEEDED(output->Read(image->png.data(),static_cast<ULONG>(image->png.size()),&read)) && read==image->png.size())preview=std::move(image);
    return true;
}
}

std::wstring DiscordImageProxyPath(std::string_view proxyImage, std::string_view original) {
    if (original.size() > 512 || !Games::IsCoverUrl(std::wstring(original.begin(), original.end())) ||
        !proxyImage.starts_with("mp:external/")) return {};
    proxyImage.remove_prefix(12);
    const auto slash = proxyImage.find('/');
    if (slash == proxyImage.npos || slash == 0 || slash > 128 ||
        !std::all_of(proxyImage.begin(), proxyImage.begin()+slash, [](char c) {
            return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='_';
        }) || proxyImage.substr(slash) != "/https/" + std::string(original.substr(8))) return {};
    return L"/external/" + std::wstring(proxyImage.begin(), proxyImage.end());
}

DiscordImageCheck CheckDiscordImageProxy(std::string_view proxyImage, std::string_view original, HANDLE stop) {
    DiscordImageCheck result;
    const auto initialized=CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Apartment { bool close; ~Apartment() { if (close) CoUninitialize(); } } apartment{SUCCEEDED(initialized)};
    const auto path = DiscordImageProxyPath(proxyImage, original);
    if (path.empty()) { result.state=DiscordImageCheckState::InvalidImage; result.reason=L"Invalid Discord image proxy response"; return result; }
    Progress p;
    if (!p.ready || !p.closed) { result.error=ERROR_NOT_ENOUGH_MEMORY; return result; }
    HttpHandle session{WinHttpOpen(L"NitLink-DiscordImageCheck/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    if (!session.value) { result.error=GetLastError(); return result; }
    WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 3000);
    HttpHandle connection{WinHttpConnect(session.value, L"media.discordapp.net", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) { result.error=GetLastError(); return result; }
    RequestHandle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), p};
    if (!request.value) { result.error=GetLastError(); return result; }
    DWORD disabled=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES;
    DWORD_PTR context=reinterpret_cast<DWORD_PTR>(&p);
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(request.value, HttpCallback, WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
            WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
            WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES, 0)==WINHTTP_INVALID_STATUS_CALLBACK) {
        result.error=GetLastError(); return result;
    }
    request.registered=true;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
    const auto await = [&] {
        const HANDLE events[]={stop, p.ready};
        while (std::chrono::steady_clock::now()<deadline) {
            const auto ready=WaitForMultipleObjects(2, events, FALSE, 40);
            if (ready==WAIT_OBJECT_0) { result.state=DiscordImageCheckState::Cancelled; return false; }
            if (ready==WAIT_OBJECT_0+1) { result.error=p.error; return p.error==0; }
            if (ready==WAIT_FAILED) { result.error=GetLastError(); return false; }
        }
        result.error=ERROR_WINHTTP_TIMEOUT; return false;
    };
    if (WaitForSingleObject(stop, 0)!=WAIT_TIMEOUT) { result.state=DiscordImageCheckState::Cancelled; return result; }
    if (!WinHttpSendRequest(request.value, L"Accept: image/png, image/jpeg, image/webp\r\n", -1,
        WINHTTP_NO_REQUEST_DATA, 0, 0, context) || !await() ||
        !WinHttpReceiveResponse(request.value, nullptr) || !await()) {
        if (!result.error) result.error=GetLastError(); return result;
    }
    DWORD length=sizeof(result.httpStatus);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &result.httpStatus, &length, WINHTTP_NO_HEADER_INDEX)) {
        result.error=GetLastError(); return result;
    }
    if (result.httpStatus!=200) { result.state=DiscordImageCheckState::HttpError; result.reason=L"Discord image proxy HTTP "+std::to_wstring(result.httpStatus); return result; }
    wchar_t type[128]{}; length=sizeof(type);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
        type, &length, WINHTTP_NO_HEADER_INDEX)) { result.error=GetLastError(); return result; }
    std::wstring mime(type);
    if (const auto semicolon=mime.find(L';');semicolon!=mime.npos) mime.resize(semicolon);
    if (mime!=L"image/png" && mime!=L"image/jpeg" && mime!=L"image/webp") {
        if(mime==L"image/avif"||mime==L"image/gif") {
            result.state=DiscordImageCheckState::UnsupportedFormat;
            result.reason=L"Discord proxy returned "+mime+L"; local decoder validation is unavailable";
        }else {result.state=DiscordImageCheckState::InvalidImage;result.reason=L"Discord image proxy returned a non-image response";}
        return result;
    }
    std::vector<unsigned char> bytes;
    unsigned char buffer[8192];
    for (;;) {
        p.bytes=0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), nullptr) || !await()) {
            if (!result.error) result.error=GetLastError(); return result;
        }
        if (!p.bytes) break;
        if (p.bytes>8*1024*1024-bytes.size()) { result.state=DiscordImageCheckState::InvalidImage; result.reason=L"Discord image exceeds 8 MiB"; return result; }
        bytes.insert(bytes.end(), buffer, buffer+p.bytes);
    }
    result.state=ValidImage(bytes, mime, result.previewImage) ? DiscordImageCheckState::Available : DiscordImageCheckState::InvalidImage;
    if (result.state!=DiscordImageCheckState::Available) result.reason=L"Discord image proxy returned invalid image data";
    return result;
}
} // namespace NitLink
