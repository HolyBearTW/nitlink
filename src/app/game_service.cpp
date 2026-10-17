#include "game_service.h"
#include "artwork_renderer.h"
#include <windows.h>
#include <winhttp.h>
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shlwapi.h>
#include <chrono>
#include <cmath>
#include <set>
#include <future>
#include <fstream>
#include <filesystem>
#include <bcrypt.h>
#include <shlobj.h>
#include <regex>
#include <cwctype>

namespace NitLink::Games {
namespace {
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
std::wstring Sha256(std::string_view data){
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    unsigned char digest[32]{};
    const auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),static_cast<ULONG>(data.size()),digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm,0);if(status<0)return {};
    constexpr wchar_t hex[]=L"0123456789abcdef";std::wstring key;
    for(auto byte:digest){key.push_back(hex[byte>>4]);key.push_back(hex[byte&15]);}return key;
}
bool KnownUnavailableArtwork(std::string_view data){
    static const auto hashes=[](){
        std::set<std::wstring> values;
        try{
            std::wstring executable(32768,L'\0');const auto size=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
            if(!size||size>=executable.size())return values;executable.resize(size);
            std::ifstream file(std::filesystem::path(executable).parent_path()/L"unavailable-artwork.json",std::ios::binary|std::ios::ate);
            if(!file||file.tellg()<=0||file.tellg()>16384)return values;
            std::string body(static_cast<size_t>(file.tellg()),'\0');file.seekg(0);if(!file.read(body.data(),static_cast<std::streamsize>(body.size())))return values;
            using namespace winrt::Windows::Data::Json;JsonObject root;
            if(!JsonObject::TryParse(winrt::to_hstring(body),root)||root.GetNamedNumber(L"version",0)!=1)return values;
            const auto list=root.GetNamedArray(L"sha256");if(list.Size()>64)return values;
            for(const auto& item:list)if(item.ValueType()==JsonValueType::String){
                const std::wstring hash(item.GetString());
                if(hash.size()==64&&std::all_of(hash.begin(),hash.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}))values.insert(hash);
            }
        }catch(...){values.clear();}return values;
    }();
    return !hashes.empty()&&hashes.contains(Sha256(data));
}
std::filesystem::path ImageCachePath(std::wstring_view url) {
    PWSTR local=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))return {};
    const auto directory=std::filesystem::path(local)/L"NitLink"/L"GameArt";CoTaskMemFree(local);
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    unsigned char digest[32]{};
    const std::string utf8="v3:"+winrt::to_string(url);
    const auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(utf8.data())),static_cast<ULONG>(utf8.size()),digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm,0);if(status<0)return {};
    constexpr wchar_t hex[]=L"0123456789abcdef";std::wstring key;
    for(auto byte:digest){key.push_back(hex[byte>>4]);key.push_back(hex[byte&15]);}
    return directory/(key+L".png");
}
struct HttpReply { DWORD status = 0, error = 0; std::string body; std::wstring mime; };
struct Handle { HINTERNET value = nullptr; ~Handle() { if (value) WinHttpCloseHandle(value); } };
struct Progress {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD error = 0, bytes = 0;
    ~Progress() { if (event) CloseHandle(event); if (closed) CloseHandle(closed); }
};
void CALLBACK HttpCallback(HINTERNET, DWORD_PTR context, DWORD status, void* data, DWORD size) {
    auto* p = reinterpret_cast<Progress*>(context);
    if (!p) return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: SetEvent(p->closed); return;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: p->error = static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError; break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: p->bytes = size; break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: break;
    default: return;
    }
    SetEvent(p->event);
}
struct AsyncRequest {
    HINTERNET value = nullptr;
    Progress& progress;
    bool registered = false;
    ~AsyncRequest() {
        if (value) {
            WinHttpCloseHandle(value);
            // Final callback protects the context's lifetime, as in UpdateChecker.
            if (registered) WaitForSingleObject(progress.closed, INFINITE);
        }
    }
};
HttpReply Fetch(const wchar_t* host, const std::wstring& path, size_t limit,
                bool image, const Cancelled& cancelled, bool html=false) {
    HttpReply reply;
    const auto deadline = Clock::now() + std::chrono::seconds(image ? 8 : 12);
    Progress p;
    if (!p.event || !p.closed) { reply.error = ERROR_NOT_ENOUGH_MEMORY; return reply; }
    Handle session{WinHttpOpen(L"NitLink-HolyBear-GameSearch/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    if (!session.value) { reply.error = GetLastError(); return reply; }
    // A cold Worker search can spend more than four seconds resolving names.
    // Keep the existing total deadline, cancellation and non-Worker limits.
    const int receiveTimeout = std::wstring_view(host) == WorkerHost ? 12000 : 4000;
    if (!WinHttpSetTimeouts(session.value, 3000, 3000, 3000, receiveTimeout)) {
        reply.error = GetLastError(); return reply;
    }
    Handle connection{WinHttpConnect(session.value, host, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) { reply.error = GetLastError(); return reply; }
    AsyncRequest request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), p};
    if (!request.value) { reply.error = GetLastError(); return reply; }
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&p);
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(request.value, HttpCallback,
            WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
            WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
            WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        reply.error = GetLastError(); return reply;
    }
    request.registered = true;
    auto await = [&] {
        while (!cancelled() && Clock::now() < deadline) {
            if (WaitForSingleObject(p.event, 40) == WAIT_OBJECT_0) {
                reply.error = p.error;
                return !reply.error;
            }
        }
        reply.error = cancelled() ? ERROR_WINHTTP_OPERATION_CANCELLED : ERROR_WINHTTP_TIMEOUT;
        return false;
    };
    if (cancelled()) { reply.error = ERROR_WINHTTP_OPERATION_CANCELLED; return reply; }
    if (!WinHttpSendRequest(request.value, image ? L"Accept: image/png, image/jpeg, image/webp\r\n" :
            html?L"Accept: text/html\r\n":L"Accept: application/json\r\n", -1, WINHTTP_NO_REQUEST_DATA, 0, 0, context) || !await() ||
        !WinHttpReceiveResponse(request.value, nullptr) || !await()) {
        if (!reply.error) reply.error = GetLastError(); return reply;
    }
    DWORD bytes = sizeof(reply.status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &reply.status, &bytes, WINHTTP_NO_HEADER_INDEX)) {
        reply.error = GetLastError(); return reply;
    }
    if (reply.status != 200) return reply;
    wchar_t mime[128]{};
    bytes = sizeof(mime);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
        mime, &bytes, WINHTTP_NO_HEADER_INDEX)) { reply.error = ERROR_INVALID_DATA; return reply; }
    reply.mime = mime;
    const auto semicolon = reply.mime.find(L';');
    if (semicolon != std::wstring::npos) reply.mime.resize(semicolon);
    if (image ? (reply.mime != L"image/png" && reply.mime != L"image/jpeg" && reply.mime != L"image/webp") :
        reply.mime != (html?L"text/html":L"application/json")) { reply.error = ERROR_INVALID_DATA; return reply; }
    char buffer[8192];
    for (;;) {
        p.bytes = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), nullptr) || !await()) {
            if (!reply.error) reply.error = GetLastError(); return reply;
        }
        if (!p.bytes) return reply;
        if (p.bytes > limit - reply.body.size()) { reply.error = ERROR_FILE_TOO_LARGE; reply.body.clear(); return reply; }
        reply.body.append(buffer, p.bytes);
    }
}
std::wstring Encode(std::wstring_view text) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string utf8(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        utf8.data(), count, nullptr, nullptr);
    std::wstring out;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (unsigned char c : utf8) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            out.push_back(c);
        else { out.push_back(L'%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    }
    return out;
}
// Read the store's ordinary public search pages. Never execute remote scripts,
// follow product links, log in, or bypass a verification page.
std::wstring HtmlText(std::string_view raw){
    std::wstring text(winrt::to_hstring(raw));
    text=std::regex_replace(text,std::wregex(L"<[^>]*>"),L"");
    const std::wregex entity(L"&(#(?:[xX][0-9a-fA-F]+|[0-9]+)|amp|quot|apos|lt|gt|nbsp);");
    std::wstring out;size_t cursor=0;
    for(std::wsregex_iterator it(text.begin(),text.end(),entity),end;it!=end;++it){
        out.append(text,cursor,static_cast<size_t>(it->position())-cursor);const auto value=(*it)[1].str();
        if(value==L"amp")out+=L'&';else if(value==L"quot")out+=L'"';else if(value==L"apos")out+=L'\'';
        else if(value==L"lt")out+=L'<';else if(value==L"gt")out+=L'>';else if(value==L"nbsp")out+=L' ';
        else{try{const bool hex=value.size()>2&&(value[1]==L'x'||value[1]==L'X');const auto scalar=std::stoul(value.substr(hex?2:1),nullptr,hex?16:10);
            if(scalar>=32&&scalar<=0x10ffff&&!(scalar>=0xd800&&scalar<=0xdfff)){if(scalar<=0xffff)out+=static_cast<wchar_t>(scalar);else{const auto n=scalar-0x10000;out+=static_cast<wchar_t>(0xd800+(n>>10));out+=static_cast<wchar_t>(0xdc00+(n&1023));}}}catch(...){}}
        cursor=static_cast<size_t>(it->position()+it->length());
    }
    out.append(text,cursor,text.size()-cursor);out=std::regex_replace(out,std::wregex(L"\\s+"),L" ");
    const auto first=out.find_first_not_of(L" "),last=out.find_last_not_of(L" ");return first==out.npos?L"":out.substr(first,last-first+1);
}
std::string HtmlAttribute(const std::string& tag,const char* key){
    std::smatch match;return std::regex_search(tag,match,std::regex(std::string("\\b")+key+"=\"([^\"]*)\""))?match[1].str():"";
}
std::wstring ProductNameKey(std::wstring_view name){
    std::wstring out;for(auto c:name)if((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||(c>=0x3040&&c<=0x9fff))out+=static_cast<wchar_t>(std::towlower(c));return out;
}
Reply NintendoStoreCovers(std::wstring_view query,uint32_t page,const Cancelled& cancelled){
    Reply out;out.page=page;
    out.officialProvider=L"nintendo-hk-store";
    const auto key=std::wstring(query)+L":"+std::to_wstring(page);
    static std::map<std::wstring,std::pair<Clock::time_point,Reply>> cache;
    static std::mutex cacheMutex;
    {std::lock_guard lock(cacheMutex);if(const auto found=cache.find(key);found!=cache.end()&&found->second.first>Clock::now())return found->second.second;}
    const auto path=(query.empty()?L"/download-code?":L"/catalogsearch/result/?q="+Encode(query)+L"&")+
        std::wstring(L"p=")+std::to_wstring(page+1)+L"&product_list_limit=24";
    const auto http=Fetch(L"store.nintendo.com.hk",path,2*1024*1024,false,cancelled,true);
    if(http.error||http.status!=200){out.status=http.status==429?L"rateLimited":http.error==ERROR_WINHTTP_TIMEOUT?L"timeout":L"upstreamUnavailable";return out;}
    const std::regex tilePattern("<li\\b[^>]*\\bclass=\"[^\"]*\\bproduct-item\\b[^\"]*\"[^>]*>([\\s\\S]*?)</li>");
    const std::regex namePattern("<a\\b[^>]*\\bclass=\"[^\"]*\\bproduct-item-link\\b[^\"]*\"[^>]*>([\\s\\S]*?)</a>");
    const std::regex imagePattern("<img\\b[^>]*\\bclass=\"[^\"]*\\bproduct-image-photo\\b[^\"]*\"[^>]*>");
    const std::regex productPattern("https://store\\.nintendo\\.com\\.hk/700[0-9]{11}");
    std::set<uint32_t> ids;
    for(std::sregex_iterator it(http.body.begin(),http.body.end(),tilePattern),end;it!=end&&!cancelled()&&out.covers.size()<96;++it){
        const auto tile=(*it)[1].str();if(tile.size()>32768)continue;std::smatch name,picture;
        if(!std::regex_search(tile,name,namePattern)||!std::regex_search(tile,picture,imagePattern))continue;
        const auto product=HtmlAttribute(name.str(),"href");if(!std::regex_match(product,productPattern))continue;
        Cover cover;cover.thumb=HtmlText(HtmlAttribute(picture.str(),"src"));if(!IsNintendoStoreImageUrl(cover.thumb))continue;
        cover.url=cover.thumb; // Exact published URL, verified before selection.
        uint32_t hash=2166136261u;for(const auto c:cover.url)hash=(hash^static_cast<uint32_t>(c))*16777619u;
        cover.id=4200000100u+hash%(0xffffffffu-4200000100u);if(!ids.insert(cover.id).second)continue;
        cover.productTitle=HtmlText(name[1].str());cover.productPage=std::wstring(winrt::to_hstring(product));
        if(cover.productTitle.empty()||!ValidText(cover.productTitle))continue;
        std::smatch platform,release;
        if(std::regex_search(tile,platform,std::regex("<p\\b[^>]*class=\"label_platform\"[^>]*>([\\s\\S]*?)</p>")))cover.productPlatform=HtmlText(platform[1].str());
        if(std::regex_search(tile,release,std::regex("<div\\b[^>]*class=\"category-product-item-released\"[^>]*>([\\s\\S]*?)</div>")))cover.productRelease=HtmlText(std::regex_replace(release[1].str(),std::regex("<span[^>]*>[\\s\\S]*?</span>"),""));
        if(cover.productPlatform.size()>64)cover.productPlatform.clear();if(cover.productRelease.size()>64)cover.productRelease.clear();
        cover.provider=L"nintendo-hk-store";cover.sourceLabel=L"Official Nintendo Hong Kong Store";cover.region=L"HK";
        cover.isOfficial=true;cover.kind=L"cover";cover.languageSource=cover.productPage;
        // Store locale/game language do not prove the language printed in art.
        out.covers.push_back(std::move(cover));
    }
    std::smatch toolbar;
    if(std::regex_search(http.body,toolbar,std::regex("id=\"toolbar-amount\"[^>]*>([\\s\\S]*?)</p>"))){
        const auto text=toolbar[1].str();const std::regex count("class=\"toolbar-number\"[^>]*>\\s*([0-9,]+)");
        for(std::sregex_iterator it(text.begin(),text.end(),count),end;it!=end;++it){auto number=(*it)[1].str();number.erase(std::remove(number.begin(),number.end(),','),number.end());try{out.totalCount=std::min<size_t>(1000000,std::stoul(number));}catch(...){}}
    }
    if(!out.totalCount)out.totalCount=out.covers.size();
    out.hasMore=out.totalCount>(static_cast<size_t>(page)+1)*24||std::regex_search(http.body,std::regex("<a\\b[^>]*class=\"[^\"]*\\baction\\s+next\\b"));
    out.status=out.covers.empty()?L"empty":L"ok";
    if(out.covers.empty()&&(http.body.find("not a robot")!=http.body.npos||http.body.find("cf-chl-")!=http.body.npos))out.status=L"upstreamUnavailable";
    if(!cancelled()&&(out.status==L"ok"||out.status==L"empty")){std::lock_guard lock(cacheMutex);if(cache.size()>=64)cache.erase(cache.begin());cache[key]={Clock::now()+std::chrono::minutes(30),out};}
    return out;
}
using namespace winrt::Windows::Data::Json;
std::wstring Text(const JsonObject& object, const wchar_t* key, size_t max = 256) {
    if (!object.HasKey(key)) return {};
    const auto value = object.GetNamedValue(key);
    if (value.ValueType() != JsonValueType::String) return {};
    const std::wstring text(value.GetString());
    return ValidText(text, max) ? text : std::wstring{};
}
uint32_t Id(const JsonObject& object, const wchar_t* key) {
    if (!object.HasKey(key)) return 0;
    const auto value = object.GetNamedValue(key);
    if (value.ValueType() != JsonValueType::Number) return 0;
    const auto number = value.GetNumber();
    return std::isfinite(number) && number >= 1 && number <= UINT32_MAX && std::floor(number) == number
        ? static_cast<uint32_t>(number) : 0;
}
Reply NintendoTaiwanCovers(std::wstring_view query,uint32_t page,const Cancelled& cancelled){
    Reply out;out.page=page;
    if(query.size()>256||page>1000){out.status=L"invalid";return out;}
    const auto key=std::wstring(query)+L":"+std::to_wstring(page);
    static std::map<std::wstring,std::pair<Clock::time_point,Reply>> cache;
    static std::mutex cacheMutex;
    {std::lock_guard lock(cacheMutex);if(const auto found=cache.find(key);found!=cache.end()&&found->second.first>Clock::now())return found->second.second;}
    std::wstring path;
    if(query.empty()){
        path=L"/tw/api/software?sftab=all&size=24";
        if(page)path+=L"&spage="+std::to_wstring(page+1);
    }else path=L"/tw/api/search?k="+Encode(query)+L"&directory=software&size=24&p="+std::to_wstring(page+1);
    const auto http=Fetch(L"www.nintendo.com",path,2*1024*1024,false,cancelled);
    if(http.error||http.status!=200){out.status=http.status==429?L"rateLimited":http.error==ERROR_WINHTTP_TIMEOUT?L"timeout":L"upstreamUnavailable";return out;}
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,http.body.data(),static_cast<int>(http.body.size()),nullptr,0);
    if(length<=0){out.status=L"invalid";return out;}
    std::wstring body(static_cast<size_t>(length),L'\0');
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,http.body.data(),static_cast<int>(http.body.size()),body.data(),length)){out.status=L"invalid";return out;}
    JsonObject root;
    if(!JsonObject::TryParse(body,root)||!root.HasKey(L"items")||root.GetNamedValue(L"items").ValueType()!=JsonValueType::Array){out.status=L"invalid";return out;}
    const auto items=root.GetNamedArray(L"items");if(items.Size()>24){out.status=L"invalid";return out;}
    if(root.HasKey(L"total")&&root.GetNamedValue(L"total").ValueType()==JsonValueType::Number){const auto total=root.GetNamedNumber(L"total");if(std::isfinite(total)&&total>=0)out.totalCount=static_cast<size_t>(std::min<double>(total,10000000));}
    out.hasMore=root.GetNamedBoolean(L"more",false)||out.totalCount>(static_cast<size_t>(page)+1)*24;
    out.officialProvider=L"nintendo-tw";
    std::set<uint32_t> ids;std::set<std::wstring> urls;
    for(const auto& value:items){
        if(cancelled())break;if(value.ValueType()!=JsonValueType::Object)continue;
        const auto item=value.GetObject();
        if(Text(item,L"region",16)!=L"taiwan")continue;
        const auto title=Text(item,L"title");const auto platform=Text(item,L"hardwareCategory",64);
        if(title.empty()||!(platform.starts_with(L"Nintendo Switch")||platform.starts_with(L"Nintendo Switch 2")))continue;
        if(!item.HasKey(L"imageHero")||item.GetNamedValue(L"imageHero").ValueType()!=JsonValueType::Object)continue;
        const auto hero=item.GetNamedObject(L"imageHero");const auto url=Text(hero,L"url",512);
        if(!IsNintendoTaiwanLineupImageUrl(url)||!urls.insert(url).second)continue;
        Cover cover;cover.url=url;cover.thumb=url;cover.productTitle=title;cover.productPlatform=platform;
        cover.productPage=L"https://www.nintendo.com/tw/software/switch";
        const auto detail=Text(item,L"pageLinkCustom",256);
        if(detail.starts_with(L"/tw/")&&detail.find(L"..") == detail.npos&&detail.find(L"\\") == detail.npos)
            cover.productPage=L"https://www.nintendo.com"+detail;
        uint32_t hash=2166136261u;for(const auto c:url)hash=(hash^static_cast<uint32_t>(c))*16777619u;
        cover.id=4200000100u+hash%(0xffffffffu-4200000100u);
        while(!ids.insert(cover.id).second)++cover.id;
        cover.provider=L"nintendo-tw";cover.sourceLabel=L"Official Nintendo Taiwan";cover.region=L"TW";
        cover.isOfficial=true;cover.kind=L"logo";cover.shape=L"landscape";cover.languageSource=cover.productPage;
        // A Taiwan listing proves origin, not the language printed in an image.
        cover.language.clear();cover.languageVerified=false;
        out.covers.push_back(std::move(cover));
    }
    out.status=out.covers.empty()?L"empty":L"ok";
    if(!out.totalCount)out.totalCount=out.covers.size();
    if(!cancelled()&&(out.status==L"ok"||out.status==L"empty")){std::lock_guard lock(cacheMutex);if(cache.size()>=64)cache.erase(cache.begin());cache[key]={Clock::now()+std::chrono::minutes(30),out};}
    return out;
}
Reply ParseReply(const HttpReply& http, bool covers) {
    Reply out;
    if (http.error == ERROR_WINHTTP_TIMEOUT) { out.status = L"timeout"; return out; }
    if (http.error && http.error != ERROR_INVALID_DATA && http.error != ERROR_FILE_TOO_LARGE) {
        out.status = L"offline"; return out;
    }
    if (http.error) return out;
    if (http.status != 200) { out.status = http.status == 429 ? L"rateLimited" : L"workerError"; return out; }
    try {
        JsonObject json;
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, http.body.data(),
            static_cast<int>(http.body.size()), nullptr, 0);
        if (length <= 0) return out;
        std::wstring wide(length, L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, http.body.data(),
            static_cast<int>(http.body.size()), wide.data(), length)) return out;
        unsigned depth = 0;
        bool inString = false, escaped = false;
        for (auto c : wide) {
            if (inString) { if (escaped) escaped = false; else if (c == L'\\') escaped = true; else if (c == L'"') inString = false; }
            else if (c == L'"') inString = true;
            else if (c == L'{' || c == L'[') { if (++depth > 16) return out; }
            else if (c == L'}' || c == L']') { if (!depth) return out; --depth; }
        }
        if (depth || inString || !JsonObject::TryParse(wide, json)) return out;
        if (!json.HasKey(L"ok") || json.GetNamedValue(L"ok").ValueType() != JsonValueType::Boolean ||
            !json.GetNamedBoolean(L"ok")) { out.status = L"workerError"; return out; }
        if (!json.HasKey(L"data") || json.GetNamedValue(L"data").ValueType() != JsonValueType::Array) return out;
        if (json.HasKey(L"lookups") && json.GetNamedValue(L"lookups").ValueType() == JsonValueType::Object) {
            const auto lookups = json.GetNamedObject(L"lookups");
            for (const auto* key : {L"wikidata", L"wikipedia", L"steam", L"steamgriddb", L"steamAppId"}) {
                auto status = Text(lookups, key, 64);
                if (!status.empty()) out.lookups[key] = std::move(status);
            }
        }
        std::set<uint32_t> seen;
        const auto data = json.GetNamedArray(L"data");
        if (data.Size() > 1000) return out; // Reject oversized record sets, never silently truncate a page.
        const auto limit = data.Size();
        for (uint32_t i = 0; i < limit; ++i) {
            const auto value = data.GetAt(i);
            if (value.ValueType() != JsonValueType::Object) continue;
            const auto object = value.GetObject();
            const auto id = Id(object, L"id");
            if (!id || !seen.insert(id).second) continue;
            if (covers) {
                Cover cover;
                cover.id = id;
                cover.url = Text(object, L"url", 512);
                cover.thumb = Text(object, L"thumb", 512);
                cover.author = Text(object, L"author", 128);
                if (!IsSteamGridCoverUrl(cover.url)) continue;
                if (!IsSteamGridCoverUrl(cover.thumb)) cover.thumb = cover.url;
                // Future provider contract: verified metadata AND provenance.
                // Worker v4 has neither; filenames/author/name are never guessed.
                cover.language = Text(object, L"language", 16);
                cover.languageSource = Text(object, L"languageSource", 128);
                cover.languageVerified = IsCoverLanguage(cover.language) && !cover.languageSource.empty() &&
                    object.HasKey(L"languageVerified") &&
                    object.GetNamedValue(L"languageVerified").ValueType() == JsonValueType::Boolean &&
                    object.GetNamedBoolean(L"languageVerified");
                if (!cover.languageVerified) { cover.language.clear(); cover.languageSource.clear(); }
                out.covers.push_back(std::move(cover));
            } else {
                Game game{id, Id(object, L"steamAppId"), Text(object, L"name"),
                    Text(object, L"displayName"), Text(object, L"source", 64)};
                if (game.name.empty()) continue;
                if (game.displayName.empty()) game.displayName = game.name;
                game.displayNameLanguage=Text(object,L"displayNameLanguage",16);
                out.games.push_back(std::move(game));
            }
        }
        out.status = (covers ? out.covers.empty() : out.games.empty()) ? L"empty" : L"ok";
        out.totalCount = covers ? out.covers.size() : out.games.size();
        if (covers) {
            if (json.HasKey(L"page")) {
                const auto page = json.GetNamedValue(L"page");
                if (page.ValueType() != JsonValueType::Number || page.GetNumber() < 0 ||
                    page.GetNumber() > 1000 || std::floor(page.GetNumber()) != page.GetNumber()) return {};
                out.page = static_cast<uint32_t>(page.GetNumber());
            }
            if (json.HasKey(L"hasMore")) {
                if (json.GetNamedValue(L"hasMore").ValueType() != JsonValueType::Boolean) return {};
                out.hasMore = json.GetNamedBoolean(L"hasMore");
            }
            const auto total = Id(json, L"total");
            if (total) out.totalCount = total;
        }
        if (json.HasKey(L"resolvedQueries") && json.GetNamedValue(L"resolvedQueries").ValueType() == JsonValueType::Array) {
            const auto terms = json.GetNamedArray(L"resolvedQueries");
            for (uint32_t i = 0; i < std::min<uint32_t>(12, terms.Size()); ++i) {
                if (terms.GetAt(i).ValueType() != JsonValueType::String) continue;
                const std::wstring term(terms.GetAt(i).GetString());
                if (ValidText(term) && !term.empty()) out.resolvedQueries.push_back(term);
            }
        }
        if (data.Size() && out.status == L"empty") out.status = L"invalid";
        for (const auto& [_, status] : out.lookups)
            if ((out.status == L"empty" || out.status == L"ok") &&
                (status == L"unavailable" || status == L"error" || status == L"timeout")) {
                // A healthy Worker response with one failed lookup backend is
                // an incomplete search, not a Worker outage. Keep true
                // transport/HTTP failures distinguishable as unavailable.
                out.status = L"partial"; break;
            }
    } catch (...) { out = {}; }
    return out;
}
class WorkerProvider final : public Provider {
    struct Cached { Reply reply; Clock::time_point until; };
    std::map<std::wstring, Cached> cache;
    struct Expansion { std::vector<std::wstring> aliases, queries, suggestions; };
    std::vector<Expansion> expansions;
    bool expansionsLoaded = false;
    void LoadExpansions() {
        if (expansionsLoaded) return;
        expansionsLoaded = true;
        try {
            std::wstring executable(32768, L'\0');
            const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            if (!size || size >= executable.size()) return;
            executable.resize(size);
            std::ifstream file(std::filesystem::path(executable).parent_path() / L"game-search-expansions.json", std::ios::binary | std::ios::ate);
            if (!file || file.tellg() <= 0 || file.tellg() > 32768) return;
            std::string body(static_cast<size_t>(file.tellg()), '\0');
            file.seekg(0); if (!file.read(body.data(), static_cast<std::streamsize>(body.size()))) return;
            JsonObject root;
            if (!JsonObject::TryParse(winrt::to_hstring(body), root) || !root.HasKey(L"groups") ||
                root.GetNamedValue(L"groups").ValueType() != JsonValueType::Array) return;
            const auto groups = root.GetNamedArray(L"groups");
            const auto strings = [](const JsonObject& object, const wchar_t* key, uint32_t limit) {
                std::vector<std::wstring> out;
                if (!object.HasKey(key) || object.GetNamedValue(key).ValueType() != JsonValueType::Array) return out;
                const auto array = object.GetNamedArray(key);
                for (uint32_t i = 0; i < std::min(limit, array.Size()); ++i) {
                    if (array.GetAt(i).ValueType() != JsonValueType::String) continue;
                    const std::wstring text(array.GetAt(i).GetString());
                    if (ValidText(text) && !text.empty()) out.push_back(text);
                }
                return out;
            };
            for (uint32_t i = 0; i < std::min<uint32_t>(16, groups.Size()); ++i) {
                if (groups.GetAt(i).ValueType() != JsonValueType::Object) continue;
                const auto group = groups.GetAt(i).GetObject();
                expansions.push_back({strings(group,L"aliases",24),strings(group,L"queries",5),strings(group,L"suggestions",8)});
            }
        } catch (...) { expansions.clear(); }
    }
    Reply Get(const std::wstring& path, bool covers, const Cancelled& cancelled) {
        const auto it = cache.find(path);
        if (it != cache.end() && Clock::now() < it->second.until) return it->second.reply;
        auto reply = ParseReply(Fetch(WorkerHost, path, 256 * 1024, false, cancelled), covers);
        if (!cancelled() && (reply.status == L"ok" || reply.status == L"empty")) {
            if (cache.size() >= 32) cache.erase(cache.begin());
            cache[path] = {reply, Clock::now() + std::chrono::minutes(covers ? 30 : 10)};
        }
        return reply;
    }
public:
    Reply Search(std::wstring_view query, std::wstring_view locale, const Cancelled& cancelled) override {
        if (query.empty() || !ValidText(query) || !IsLocale(locale)) return {};
        LoadExpansions();
        const auto primaryPath = L"/games/search?q=" + Encode(query) + L"&lang=" + std::wstring(locale);
        auto primary = Get(primaryPath, false, cancelled);
        for(auto& game:primary.games)game.displayName=LocalizedGameName(game.name,game.displayName,locale,game.displayNameLanguage);
        std::vector<std::wstring> terms{std::wstring(query)}, suggestions;
        for (const auto& group : expansions) {
            const bool match = std::any_of(group.aliases.begin(), group.aliases.end(), [&](const auto& alias) {
                return CompareStringOrdinal(alias.data(), static_cast<int>(alias.size()), query.data(),
                    static_cast<int>(query.size()), TRUE) == CSTR_EQUAL;
            });
            if (!match) continue;
            for (const auto& term : group.queries)
                if (terms.size() < 6 && std::find(terms.begin(), terms.end(), term) == terms.end()) terms.push_back(term);
            suggestions = group.suggestions;
            break;
        }
        if (primary.games.size() >= 8 || terms.size() == 1 || cancelled()) {
            primary.queriedTerms = {std::wstring(query)}; primary.suggestions = suggestions;
            return primary;
        }
        struct Pending { std::wstring term, path; Reply reply; std::future<HttpReply> network; };
        std::vector<Pending> pending;
        // At most six HTTPS requests, all on background threads. JSON parsing
        // and provider-cache mutation stay on the existing COM worker thread.
        for (const auto& term : terms) {
            Pending item;
            item.term = term; item.path = L"/games/search?q=" + Encode(term) + L"&lang=" + std::wstring(locale);
            const auto found = cache.find(item.path);
            if (item.term == query) item.reply = primary;
            else if (found != cache.end() && Clock::now() < found->second.until) item.reply = found->second.reply;
            else item.network = std::async(std::launch::async, [path = item.path, cancelled] {
                return Fetch(WorkerHost, path, 256 * 1024, false, cancelled);
            });
            pending.push_back(std::move(item));
        }
        Reply combined;
        combined.queriedTerms = terms; combined.suggestions = suggestions;
        bool unavailable = false, partial = false;
        std::set<uint32_t> ids;
        std::set<std::wstring> resolved;
        for (auto& item : pending) {
            if (item.network.valid()) {
                item.reply = ParseReply(item.network.get(), false);
                if (!cancelled() && (item.reply.status == L"ok" || item.reply.status == L"empty")) {
                    if (cache.size() >= 32) cache.erase(cache.begin());
                    cache[item.path] = {item.reply, Clock::now() + std::chrono::minutes(10)};
                }
            }
            unavailable |= item.reply.status != L"ok" && item.reply.status != L"empty" && item.reply.status != L"partial";
            partial |= item.reply.status == L"partial";
            combined.lookups[item.term + L" · worker"] = item.reply.status;
            for (const auto& [source, status] : item.reply.lookups) combined.lookups[item.term + L" · " + source] = status;
            for (const auto& term : item.reply.resolvedQueries)
                if (resolved.size() < 24 && resolved.insert(term).second) combined.resolvedQueries.push_back(term);
            for (auto game : item.reply.games)
                if (ids.insert(game.id).second){
                    game.displayName=LocalizedGameName(game.name,game.displayName,locale,game.displayNameLanguage);
                    combined.games.push_back(std::move(game));
                }
        }
        combined.totalCount = combined.games.size();
        combined.status = !combined.games.empty() ? (unavailable || partial ? L"partial" : L"ok") :
            unavailable ? L"upstreamUnavailable" : partial ? L"partial" : L"empty";
        if (terms.size() == 1 && combined.games.empty()) combined.status = pending.front().reply.status;
        return combined;
    }
    Reply Covers(uint32_t gameId, std::wstring_view shape, std::wstring_view locale, uint32_t page, const Cancelled& cancelled) override {
        if (!gameId || !IsLocale(locale) || (shape != L"square" && shape != L"portrait" && shape != L"landscape" && shape != L"all") || page > 1000) return {};
        auto reply = Get(L"/games/" + std::to_wstring(gameId) + L"/covers?shape=" + std::wstring(shape) + L"&page=" + std::to_wstring(page), true, cancelled);
        if ((reply.status == L"ok" || reply.status == L"empty") && reply.page != page) return {};
        if(shape==L"square"||shape==L"portrait"||shape==L"landscape")for(auto& cover:reply.covers)cover.shape=shape;
        std::stable_sort(reply.covers.begin(), reply.covers.end(), [&](const Cover& a, const Cover& b) {
            const auto rank = [&](const Cover& c) { return c.languageVerified ? (c.language == locale ? 0 : 2) : 1; };
            return rank(a) < rank(b);
        });
        return reply;
    }
};
// Decode on the network worker, bound dimensions/pixels, strip metadata and
// re-encode a small PNG. WebView never receives SVG, HTML or unchecked bytes.
std::shared_ptr<Image> DecodeImage(const HttpReply& reply, std::optional<ArtworkCrop> crop = {}) {
    if (reply.error || reply.status != 200 || reply.body.empty()) return {};
    if(KnownUnavailableArtwork(reply.body))return {};
    if (reply.mime == L"image/webp" && (reply.body.size() < 12 ||
        reply.body.substr(0, 4) != "RIFF" || reply.body.substr(8, 4) != "WEBP")) return {};
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return {};
    ComPtr<IStream> input;
    input.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(reply.body.data()), static_cast<UINT>(reply.body.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    if (!input || FAILED(factory->CreateDecoderFromStream(input.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return {};
    GUID format{};
    if (FAILED(decoder->GetContainerFormat(&format)) ||
        (reply.mime == L"image/png" && format != GUID_ContainerFormatPng) ||
        (reply.mime == L"image/jpeg" && format != GUID_ContainerFormatJpeg)) return {};
    UINT frames = 0, width = 0, height = 0;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames != 1 || FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 4096 || height > 4096 ||
        uint64_t(width) * height > 8 * 1024 * 1024) return {};
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmapSource> source=frame;
    ComPtr<IWICBitmapClipper> clipper;
    if(crop){
        if(crop->x>=width||crop->y>=height||crop->width>width-crop->x||crop->height>height-crop->y)return {};
        const WICRect rect{static_cast<INT>(crop->x),static_cast<INT>(crop->y),static_cast<INT>(crop->width),static_cast<INT>(crop->height)};
        if(FAILED(factory->CreateBitmapClipper(&clipper))||FAILED(clipper->Initialize(frame.Get(),&rect)))return {};
        source=clipper;width=crop->width;height=crop->height;
    }
    if (FAILED(factory->CreateBitmapScaler(&scaler))) return {};
    const double scale = std::min(1.0, 512.0 / std::max(width, height));
    const UINT scaledWidth=std::max(1u,static_cast<UINT>(width*scale));
    const UINT scaledHeight=std::max(1u,static_cast<UINT>(height*scale));
    if (FAILED(scaler->Initialize(source.Get(),scaledWidth,scaledHeight,WICBitmapInterpolationModeFant))) return {};
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(scaler.Get(),
        GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
    ComPtr<IStream> output;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &output))) return {};
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> encoded;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&encoded, nullptr)) || FAILED(encoded->Initialize(nullptr)) ||
        FAILED(encoded->WriteSource(converter.Get(), nullptr)) || FAILED(encoded->Commit()) || FAILED(encoder->Commit())) return {};
    STATSTG stat{};
    if (FAILED(output->Stat(&stat, STATFLAG_NONAME)) || !stat.cbSize.QuadPart || stat.cbSize.QuadPart > 2 * 1024 * 1024) return {};
    auto image = std::make_shared<Image>();
    image->width=scaledWidth;image->height=scaledHeight;
    image->png.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    ULONG read = 0;
    if (FAILED(output->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(output->Read(image->png.data(), static_cast<ULONG>(image->png.size()), &read)) || read != image->png.size()) return {};
    return image;
}
std::shared_ptr<Image> ReadImageCache(const std::filesystem::path& path) {
    if(path.empty())return {};
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return {};
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};std::shared_ptr<Image> image;
    if(GetFileInformationByHandle(file,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&
        info.nNumberOfLinks==1&&GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<=2*1024*1024){
        HttpReply reply;reply.status=200;reply.mime=L"image/png";reply.body.resize(static_cast<size_t>(size.QuadPart));DWORD read=0;
        if(ReadFile(file,reply.body.data(),static_cast<DWORD>(reply.body.size()),&read,nullptr)&&read==reply.body.size())image=DecodeImage(reply);
    }
    CloseHandle(file);return image;
}
void WriteImageCache(const std::filesystem::path& path,const Image& image) {
    if(path.empty())return;
    std::error_code error;std::filesystem::create_directories(path.parent_path(),error);if(error)return;
    const DWORD attributes=GetFileAttributesW(path.parent_path().c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
    // Only create a new canonical hash file; never overwrite/follow a link.
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    DWORD written=0;const bool good=WriteFile(file,image.png.data(),static_cast<DWORD>(image.png.size()),&written,nullptr)&&written==image.png.size();
    CloseHandle(file);if(!good){std::filesystem::remove(path,error);return;}
    std::vector<std::filesystem::directory_entry> files;uintmax_t bytes=0;
    for(std::filesystem::directory_iterator it(path.parent_path(),error),end;!error&&it!=end;it.increment(error)){
        const auto name=it->path().filename().wstring();
        if(name.size()!=68||!name.ends_with(L".png")||!std::all_of(name.begin(),name.begin()+64,[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}))continue;
        const auto size=it->file_size(error);if(error)break;bytes+=size;files.push_back(*it);
    }
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){std::error_code first,second;return a.last_write_time(first)<b.last_write_time(second);});
    size_t count=files.size();for(const auto& old:files){if(count<=96&&bytes<=32*1024*1024)break;if(old.path()==path)continue;
        const auto size=old.file_size(error);if(error)continue;if(std::filesystem::remove(old.path(),error)){--count;bytes-=size;}}
}
std::filesystem::path FinalImagePath(std::wstring_view key){
    if(key.size()!=64||!std::all_of(key.begin(),key.end(),[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}))return {};
    const auto ordinary=ImageCachePath(L"durable-artwork");if(ordinary.empty())return {};
    return ordinary.parent_path().parent_path()/L"CustomGameArt"/(std::wstring(key)+L".png");
}
bool SaveFinalImage(std::wstring_view key,const Image& image){
    const auto path=FinalImagePath(key);if(path.empty())return false;
    if(ReadImageCache(path))return true;
    std::error_code error;std::filesystem::create_directories(path.parent_path(),error);if(error)return false;
    const auto attributes=GetFileAttributesW(path.parent_path().c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    uintmax_t total=0;size_t count=0;
    for(std::filesystem::directory_iterator it(path.parent_path(),error),end;!error&&it!=end;it.increment(error)){
        if(++count>2048)return false;total+=it->file_size(error);if(error||total+image.png.size()>512ull*1024*1024)return false;
    }
    if(error)return false;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;const bool good=WriteFile(file,image.png.data(),static_cast<DWORD>(image.png.size()),&written,nullptr)&&written==image.png.size()&&FlushFileBuffers(file);
    CloseHandle(file);if(!good)std::filesystem::remove(path,error);return good;
}
} // namespace

GameService::GameService(std::unique_ptr<Provider> provider)
    : m_provider(provider ? std::move(provider) : std::make_unique<WorkerProvider>()),
      m_artworkProvider(CreateOfficialArtworkProvider()),
      m_worker([this](std::stop_token stop) { Run(stop); }) {}
GameService::~GameService() {
    m_worker.request_stop();
    m_generation.fetch_add(1);
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}
void GameService::Submit(Request request) {
    std::lock_guard lock(m_mutex);
    ++m_generation;
    m_pending = std::move(request);
    m_results.clear();
    m_wake.notify_one();
}
void GameService::Cancel() {
    std::lock_guard lock(m_mutex);
    ++m_generation;
    m_pending.reset();
    m_results.clear();
}
std::vector<Result> GameService::TakeResults() {
    std::lock_guard lock(m_mutex);
    std::vector<Result> out;
    while (!m_results.empty()) {
        if (m_results.front().generation == m_generation.load()) out.push_back(std::move(m_results.front()));
        m_results.pop_front();
    }
    return out;
}
std::shared_ptr<const Image> GameService::FindImage(std::wstring_view uri) const {
    if (!IsImageResource(uri)) return {};
    std::lock_guard lock(m_mutex);
    const auto it = m_images.find(std::wstring(uri));
    return it == m_images.end() ? nullptr : it->second;
}
std::wstring GameService::CacheDiscordImage(std::shared_ptr<const Image> image,std::wstring_view identity) {
    if(!image){std::lock_guard lock(m_mutex);m_pinnedDiscordImage.clear();return {};}
    if(image->png.empty()||image->png.size()>2*1024*1024)return {};
    return StoreImage(std::move(image),identity,true);
}
void GameService::Publish(Result result, const Cancelled& cancelled) {
    std::lock_guard lock(m_mutex);
    // Reserve the terminal notification even if cached images fill the queue.
    if (!cancelled() && (m_results.size() < 64 || result.operation==Operation::CoverImagesComplete))
        m_results.push_back(std::move(result));
}
void GameService::PinImage(std::wstring uri,std::wstring source) {
    std::lock_guard lock(m_mutex);
    m_pinnedImage = m_images.contains(uri) ? std::move(uri) : std::wstring{};
    m_pinnedSourceImage=m_images.contains(source)?std::move(source):std::wstring{};
}
std::wstring GameService::LoadArtworkImage(std::wstring_view url, const Cancelled& cancelled,std::wstring* actualShape) {
    if (cancelled() || !IsCoverUrl(url)) return {};
    const auto recordShape=[&](const std::shared_ptr<const Image>& image){
        if(!actualShape||!image||!image->width||!image->height)return;
        *actualShape=image->width==image->height?L"square":image->width>image->height?L"landscape":L"portrait";
    };
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_imageUrls.find(std::wstring(url));
        if (it != m_imageUrls.end()) {
            const auto uri=it->second;
            if(const auto image=m_images.find(uri);image!=m_images.end())recordShape(image->second);
            const auto order=std::find_if(m_imageOrder.begin(),m_imageOrder.end(),[&](const auto& item){return item.second==uri;});
            if(order!=m_imageOrder.end()){auto recent=*order;m_imageOrder.erase(order);m_imageOrder.push_back(std::move(recent));}
            return uri;
        }
    }
    const auto slash=url.find(L'/',8);
    const auto host=std::wstring(url.substr(8,slash-8));
    const auto path=std::wstring(url.substr(slash));
    const auto cachePath=ImageCachePath(url);
    auto image=ReadImageCache(cachePath);
    if(!image&&IsPlayStationArtworkUrl(url)&&!m_artworkProvider->Allows(url))return {};
    // Nintendo URLs have a strict host/path allowlist in IsNintendoArtworkUrl.
    // Keep previously saved official art usable without per-game catalogue rows.
    if(!image){image=DecodeImage(Fetch(host.c_str(), path, 8 * 1024 * 1024, true, cancelled),m_artworkProvider->Crop(url));
        if(image&&!cancelled())WriteImageCache(cachePath,*image);}
    if (!image || cancelled()) return {};
    recordShape(image);
    return StoreImage(std::move(image),url);
}
std::wstring GameService::LoadFinalImage(std::wstring_view key,const Cancelled& cancelled){
    if(cancelled())return {};
    auto image=ReadImageCache(FinalImagePath(key));return image&&!cancelled()?StoreImage(std::move(image),L"generated:"+std::wstring(key)):L"";
}
std::wstring GameService::StoreImage(std::shared_ptr<const Image> image,std::wstring_view identity,bool discord){
    const std::wstring url(identity);
    std::lock_guard lock(m_mutex);
    if(const auto cached=m_imageUrls.find(url);cached!=m_imageUrls.end()){if(discord)m_pinnedDiscordImage=cached->second;return cached->second;}
    // A Worker page has 50 covers. Keep a complete page plus search/library
    // thumbnails so a visible validated image is not evicted before selection.
    while (!m_imageOrder.empty() && (m_images.size() >= 128 || m_imageBytes + image->png.size() > 64 * 1024 * 1024)) {
        const auto [oldUrl, uri] = m_imageOrder.front();
        m_imageOrder.pop_front();
        if (uri == m_pinnedImage||uri==m_pinnedSourceImage||uri==m_pinnedDiscordImage) { m_imageOrder.emplace_back(oldUrl, uri); continue; }
        m_imageBytes -= m_images.at(uri)->png.size();
        m_images.erase(uri); m_imageUrls.erase(oldUrl);
    }
    const auto uri = std::wstring(ImageOrigin) + std::to_wstring(++m_imageSequence) + L".png";
    if(discord)m_pinnedDiscordImage=uri;
    m_imageBytes += image->png.size();
    m_images.emplace(uri, std::move(image));
    m_imageUrls.emplace(url, uri);
    m_imageOrder.emplace_back(url, uri);
    return uri;
}
void GameService::Run(std::stop_token stop) {
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    struct Apartment {
        bool close;
        ~Apartment() { if (close) { winrt::clear_factory_cache(); RoUninitialize(); } }
    } apartment{SUCCEEDED(initialized)};
    for (;;) {
        Request request;
        uint64_t generation;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return stop.stop_requested() || m_pending.has_value(); });
            if (stop.stop_requested()) return;
            request = std::move(*m_pending); m_pending.reset(); generation = m_generation.load();
        }
        const Cancelled cancelled = [&, generation] { return stop.stop_requested() || generation != m_generation.load(); };
        Result base;
        base.operation = request.operation; base.request = request.request; base.gameId = request.gameId;
        base.locale = request.locale; base.query = request.query; base.shape = request.shape; base.coverProvider=request.coverProvider; base.generation = generation;
        base.platform=request.platform;
        const auto officialFor=[&](uint32_t id) -> std::optional<Game> {
            auto game=m_artworkProvider->Lookup(id,request.locale);
            if(game)game->artworkSources=PreferredOfficialArtwork(std::move(game->artworkSources),request.platform);
            return game && !game->artworkSources.empty()?game:std::nullopt;
        };
        try {
            if (FAILED(initialized)) { Publish(base, cancelled); continue; }
            if(request.operation==Operation::RenderArtwork){
                if(request.library.size()!=1){Publish(base,cancelled);continue;}
                auto game=request.library.front();
                auto sourceUri=LoadFinalImage(game.sourceImageKey,cancelled);
                if(sourceUri.empty())sourceUri=LoadArtworkImage(game.coverThumb.empty()?game.coverUrl:game.coverThumb,cancelled);
                const auto source=FindImage(sourceUri);
                auto rendered=source?RenderSquareArtwork(*source,request.artworkMode,request.cropX,request.cropY,request.artworkZoom):nullptr;
                if(rendered&&!cancelled()){
                    game.sourceImageKey=Sha256(std::string_view(reinterpret_cast<const char*>(source->png.data()),source->png.size()));
                    game.finalImageKey=Sha256(std::string_view(reinterpret_cast<const char*>(rendered->png.data()),rendered->png.size()));
                    if(SaveFinalImage(game.sourceImageKey,*source)&&SaveFinalImage(game.finalImageKey,*rendered)){
                        game.artworkMode=request.artworkMode;game.cropX=request.cropX;game.cropY=request.cropY;game.artworkZoom=request.artworkZoom;
                        base.renderedGame=game;base.sourceImageUri=sourceUri;base.imageUri=StoreImage(std::move(rendered),L"generated:"+game.finalImageKey);base.reply.status=L"ok";
                    }
                }
                Publish(std::move(base),cancelled);continue;
            }
            if(request.operation==Operation::LibraryImages){
                for(auto game:request.library){if(cancelled())break;if(!game.id)continue;
                    auto result=base;result.gameId=game.id;result.coverId=game.coverId;
                    if(const auto official=officialFor(game.id))result.reply.games={*official};
                    if(const auto official=officialFor(game.id);official&&!official->artworkSources.empty())
                        result.reply.games={*official};
                    if(game.coverUrl.empty())continue;
                    result.imageUri=LoadFinalImage(game.finalImageKey,cancelled);
                    result.sourceImageUri=LoadFinalImage(game.sourceImageKey,cancelled);
                    if(result.imageUri.empty()&&game.finalImageKey.empty())result.imageUri=LoadArtworkImage(game.coverThumb.empty()?game.coverUrl:game.coverThumb,cancelled);
                    if(result.sourceImageUri.empty()&&game.artworkMode==L"original")result.sourceImageUri=result.imageUri;
                    result.imageStatus=result.imageUri.empty()?L"failed":L"ok";Publish(std::move(result),cancelled);
                }continue;
            }
            if (request.operation == Operation::CurrentImage) {
                base.coverId = request.coverId;
                base.imageUri=LoadFinalImage(request.finalImageKey,cancelled);base.sourceImageUri=LoadFinalImage(request.sourceImageKey,cancelled);
                if(base.imageUri.empty()&&request.finalImageKey.empty())base.imageUri = LoadArtworkImage(request.imageUrl, cancelled);
                if(base.sourceImageUri.empty())base.sourceImageUri=base.imageUri;
                base.imageStatus = base.imageUri.empty() ? L"failed" : L"ok";
                Publish(base, cancelled); continue;
            }
            if (request.operation == Operation::Search || request.operation == Operation::Resolve) {
                base.reply = m_provider->Search(request.query, request.locale, cancelled);
                base.reply.workerCount=base.reply.games.size();
                std::set<uint32_t> ids;
                for(auto& game:base.reply.games){
                    ids.insert(game.id);
                    if(const auto official=officialFor(game.id)){
                        game.artworkSources=official->artworkSources;
                        if(request.locale==L"zh-TW"){game.displayName=official->displayName;game.displayNameSource=official->displayNameSource;}
                    }
                }
                for(auto& game:m_artworkProvider->Search(request.query,request.locale)){
                    game.artworkSources=PreferredOfficialArtwork(std::move(game.artworkSources),request.platform);
                    if(!game.artworkSources.empty()&&ids.insert(game.id).second)base.reply.games.push_back(std::move(game));
                }
                if(!base.reply.games.empty()&&base.reply.status!=L"ok")base.reply.status=L"partial";
                base.reply.totalCount=base.reply.games.size();
                base.reply.lookups[L"official-artwork"]=request.platform;
            } else {
                // Resolve the selected title in the effective locale; exact ID
                // matching is performed by Application, never by translated text.
                if (!request.query.empty()) {
                    auto resolved = base; resolved.operation = Operation::Resolve;
                    resolved.reply = m_provider->Search(request.query, request.locale, cancelled);
                    if(request.locale==L"zh-TW")if(const auto official=officialFor(request.gameId))
                        for(auto& game:resolved.reply.games)if(game.id==request.gameId)game.displayName=official->displayName;
                    Publish(std::move(resolved), cancelled);
                }
                if (cancelled()) continue;
                const auto primary=officialFor(request.gameId);
                const bool searchNintendoTaiwan=request.platform==L"switch"&&request.coverProvider!=L"community"&&!request.catalogueQuery.empty();
                Reply store;
                if(searchNintendoTaiwan){
                    store=NintendoTaiwanCovers(request.catalogueQuery,request.page,cancelled);
                    if(request.coverProvider==L"nintendo"&&(store.status!=L"ok"||store.covers.empty())&&!cancelled()){
                        auto fallback=NintendoStoreCovers(request.catalogueQuery,request.page,cancelled);
                        if(!fallback.covers.empty()||store.status!=L"ok")store=std::move(fallback);
                    }
                }
                else store.status=primary?L"ok":L"idle";
                if(cancelled())continue;
                if(request.coverProvider==L"nintendo"){
                    base.reply=store;base.reply.artworkSources=std::move(base.reply.covers);base.reply.covers.clear();
                    if(primary&&request.page==0&&base.reply.artworkSources.empty()){
                        std::set<std::wstring> known;
                        for(const auto& cover:base.reply.artworkSources)known.insert(cover.url);
                        for(const auto& cover:primary->artworkSources)
                            if((request.shape==L"all"||cover.shape==request.shape)&&known.insert(cover.url).second){base.reply.artworkSources.push_back(cover);++base.reply.totalCount;}
                    }
                }else{
                    base.reply = m_provider->Covers(request.gameId, request.shape, request.locale, request.page, cancelled);
                }
                base.reply.officialStatus=store.status;
                base.reply.officialProvider=searchNintendoTaiwan?store.officialProvider:primary&&!primary->artworkSources.empty()?primary->artworkSources.front().provider:L"";
                if(request.coverProvider==L"nintendo"&&!base.reply.artworkSources.empty())base.reply.officialProvider=base.reply.artworkSources.front().provider;
                if(request.coverProvider!=L"community"&&request.coverProvider!=L"nintendo"){
                    if(const auto official=primary)for(const auto& cover:official->artworkSources)
                        if((request.shape==L"all"||cover.shape==request.shape)&&std::none_of(base.reply.artworkSources.begin(),base.reply.artworkSources.end(),[&](const Cover& item){return item.url==cover.url;}))base.reply.artworkSources.push_back(cover);
                    if(searchNintendoTaiwan)for(const auto& cover:store.covers)
                        if((request.shape==L"all"||cover.shape==request.shape)&&std::none_of(base.reply.artworkSources.begin(),base.reply.artworkSources.end(),[&](const Cover& item){return item.url==cover.url;}))base.reply.artworkSources.push_back(cover);
                }
                if(request.coverProvider!=L"nintendo")base.reply.artworkSources=PreferredOfficialArtwork(std::move(base.reply.artworkSources),request.platform);
                if(!base.reply.artworkSources.empty()&&base.reply.status==L"invalid")base.reply.status=L"ok";
                if(!base.reply.artworkSources.empty()&&base.reply.status==L"empty")base.reply.status=L"ok";
                if(!base.reply.artworkSources.empty()&&base.reply.status!=L"ok"&&base.reply.status!=L"empty")base.reply.status=L"partial";
            }
            Publish(base, cancelled);
            if (cancelled()) continue;
            if (request.operation == Operation::Search) {
                if(request.platform==L"switch"&&!request.query.empty()){
                    const auto queryResult=NintendoTaiwanCovers(request.query,0,cancelled);
                    const auto queryKey=ProductNameKey(request.query);
                    const bool uniqueExactQuery=base.reply.games.size()==1;
                    std::map<uint32_t,std::wstring> localizedNames;
                    if(request.locale!=L"zh-TW"&&!queryResult.covers.empty()&&!cancelled()){
                        // Match the Taiwan catalogue with a locale-independent
                        // zh-TW Worker title while keeping the visible UI locale.
                        const auto localized=m_provider->Search(request.query,L"zh-TW",cancelled);
                        for(const auto& game:localized.games)if(!game.displayName.empty())localizedNames.emplace(game.id,game.displayName);
                    }
                    bool changed=false;
                    for(auto& game:base.reply.games){
                        const auto gameName=ProductNameKey(game.name);
                        const auto displayName=ProductNameKey(game.displayName);
                        const auto localized=localizedNames.find(game.id);
                        const auto localizedName=localized==localizedNames.end()?std::wstring{}:ProductNameKey(localized->second);
                        const auto found=std::find_if(queryResult.covers.begin(),queryResult.covers.end(),[&](const Cover& cover){
                            const auto title=ProductNameKey(cover.productTitle);
                            return title==gameName||title==displayName||(!localizedName.empty()&&title==localizedName)||(uniqueExactQuery&&title==queryKey);
                        });
                        if(found==queryResult.covers.end())continue;
                        if(std::none_of(game.artworkSources.begin(),game.artworkSources.end(),[&](const Cover& cover){return cover.url==found->url;})){
                            game.artworkSources.insert(game.artworkSources.begin(),*found);changed=true;
                        }
                    }
                    if(changed)Publish(base,cancelled);
                }
                // Text arrives first. Enrich every result, with known official
                // artwork first; selection/new searches still cancel this work.
                std::vector<const Game*> queue;
                for (const auto& game : base.reply.games) queue.push_back(&game);
                std::stable_sort(queue.begin(),queue.end(),[](const auto* a,const auto* b){return !a->artworkSources.empty()&&b->artworkSources.empty();});
                for (const auto* entry : queue) {
                    if(cancelled())break;
                    const auto& game = *entry;
                    auto image = base; image.reply = {}; image.operation = Operation::Image; image.gameId = game.id;
                    bool failed=false;
                    const auto tryCandidates=[&](const std::vector<Cover>& candidates){
                        for(size_t i=0;i<std::min<size_t>(3,candidates.size())&&!cancelled();++i){
                            auto cover=candidates[i];std::wstring actualShape;
                            image.imageUri=LoadArtworkImage(cover.thumb.empty()?cover.url:cover.thumb,cancelled,&actualShape);
                            if(!image.imageUri.empty()){
                                if(cover.shape.empty()&&!actualShape.empty())cover.shape=actualShape;
                                image.reply.covers={std::move(cover)};return;
                            }
                            failed=true;
                        }
                    };
                    auto officialSources=game.artworkSources;
                    if(request.platform==L"switch"&&
                       std::none_of(officialSources.begin(),officialSources.end(),[](const Cover& cover){return cover.provider==L"nintendo-tw"&&cover.shape==L"landscape";})&&
                       !game.displayName.empty()){
                        const auto catalog=NintendoTaiwanCovers(game.displayName,0,cancelled);
                        if(!cancelled()){
                            const auto displayKey=ProductNameKey(game.displayName),nameKey=ProductNameKey(game.name);
                            const auto found=std::find_if(catalog.covers.begin(),catalog.covers.end(),[&](const Cover& cover){
                                const auto title=ProductNameKey(cover.productTitle);
                                return !title.empty()&&(title==displayKey||title==nameKey);
                            });
                            if(found!=catalog.covers.end()&&std::none_of(officialSources.begin(),officialSources.end(),[&](const Cover& cover){return cover.url==found->url;}))
                                officialSources.insert(officialSources.begin(),*found);
                        }
                    }
                    const auto official=PreferredOfficialArtwork(std::move(officialSources),request.platform);
                    const auto officialFor=[&](std::wstring_view shape,int priority){
                        std::vector<Cover> covers;
                        if(shape==L"landscape")if(const auto* taiwan=PreferredTaiwanLandscapeArtwork(official))covers.push_back(*taiwan);
                        for(const auto& cover:official)if(SearchThumbnailArtworkPriority(cover)==priority&&
                            std::none_of(covers.begin(),covers.end(),[&](const Cover& item){return item.url==cover.url;}))covers.push_back(cover);
                        return covers;
                    };
                    const auto communityFor=[&](std::wstring_view shape,int priority){
                        auto reply=m_provider->Covers(game.id,shape,request.locale,0,cancelled);
                        if(reply.status!=L"ok"&&reply.status!=L"empty"&&reply.status!=L"partial")failed=true;
                        std::erase_if(reply.covers,[&](const Cover& cover){return SearchThumbnailArtworkPriority(cover)!=priority;});
                        return reply.covers;
                    };
                    // Search result tiles are landscape: Taiwan or other official
                    // landscape, community landscape, official portrait, then
                    // community portrait. Activity artwork has its own ranking.
                    tryCandidates(officialFor(L"landscape",0));
                    if(image.imageUri.empty()&&!cancelled())tryCandidates(communityFor(L"landscape",1));
                    if(image.imageUri.empty()&&!cancelled())tryCandidates(officialFor(L"portrait",2));
                    if(image.imageUri.empty()&&!cancelled())tryCandidates(communityFor(L"portrait",3));
                    if(!image.reply.covers.empty())image.shape=image.reply.covers.front().shape;
                    image.imageStatus = !image.imageUri.empty()?L"ok":failed?L"failed":L"missing";
                    Publish(std::move(image), cancelled);
                }
            } else if (request.operation == Operation::Covers) {
                // Restore the saved manual cover even when absent from this
                // shape's first page. Current art does not depend on a gallery.
                if (!request.imageUrl.empty()) {
                    auto image = base; image.reply = {}; image.operation = Operation::CurrentImage;
                    image.coverId = request.coverId;
                    image.imageUri=LoadFinalImage(request.finalImageKey,cancelled);image.sourceImageUri=LoadFinalImage(request.sourceImageKey,cancelled);
                    if(image.imageUri.empty()&&request.finalImageKey.empty())image.imageUri = LoadArtworkImage(request.imageUrl, cancelled);
                    if(image.sourceImageUri.empty()&&request.finalImageKey.empty())image.sourceImageUri=image.imageUri;
                    image.imageStatus = image.imageUri.empty() ? L"failed" : L"ok";
                    Publish(std::move(image), cancelled);
                }
                const auto imageDeadline = Clock::now() + std::chrono::seconds(30);
                auto candidates=base.reply.artworkSources;candidates.insert(candidates.end(),base.reply.covers.begin(),base.reply.covers.end());
                std::vector<Cover> loaded;
                for (const auto& cover : candidates) {
                    if (cancelled()) break;
                    auto image = base; image.reply = {}; image.reply.page=request.page;image.operation = Operation::Image; image.coverId = cover.id;
                    auto resolvedCover=cover;std::wstring actualShape;
                    if (Clock::now() < imageDeadline) image.imageUri = LoadArtworkImage(cover.thumb, cancelled,&actualShape);
                    if(resolvedCover.shape.empty()&&!actualShape.empty())resolvedCover.shape=actualShape;
                    if(!image.imageUri.empty()){
                        loaded.push_back(resolvedCover);
                    }
                    image.reply.covers={std::move(resolvedCover)};
                    image.imageStatus = image.imageUri.empty() ? L"failed" : L"ok";
                    Publish(std::move(image), cancelled);
                }
                if(!cancelled()){
                    auto complete=base;complete.operation=Operation::CoverImagesComplete;complete.reply={};
                    complete.reply.page=request.page;
                    complete.reply.covers=std::move(loaded);
                    Publish(std::move(complete),cancelled);
                }
            }
        } catch (...) { base.reply = {}; Publish(std::move(base), cancelled); }
    }
}
} // namespace NitLink::Games
