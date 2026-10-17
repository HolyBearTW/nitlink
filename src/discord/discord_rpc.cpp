#include "pipe_transport.h"
#include <algorithm>
#include "discord_rpc.h"
#include "activity_fields.h"
#include "activity_reply.h"
#include "app/game_identity.h"
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <debugapi.h>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>
#include <future>
#include <optional>
#include <utility>

namespace NitLink {

static void RPCLog(const std::wstring& msg, bool persist = false) {
    OutputDebugStringW((L"[NitLink/Discord] " + msg + L"\n").c_str());
    if (!persist) return;
    // Only our outgoing activity and selected reply fields are recorded.
    // READY/user/account payloads and authentication data never enter this log.
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    wchar_t local[32768]{};
    const auto length=GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!length || length>=32768) return;
    const auto directory=std::wstring(local)+L"\\NitLink";
    CreateDirectoryW(directory.c_str(), nullptr);
    CreateDirectoryW((directory+L"\\logs").c_str(), nullptr);
    const auto path=directory+L"\\logs\\discord-rpc.log";
    HANDLE file=CreateFileW(path.c_str(), FILE_APPEND_DATA|GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file==INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart>2*1024*1024) {
        CloseHandle(file);
        MoveFileExW(path.c_str(), (path+L".previous").c_str(), MOVEFILE_REPLACE_EXISTING);
        file=CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file==INVALID_HANDLE_VALUE) return;
    }
    SYSTEMTIME time{}; GetSystemTime(&time);
    wchar_t stamp[40]{};
    swprintf(stamp, 40, L"%04u-%02u-%02uT%02u:%02u:%02uZ ",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
    const auto line=std::wstring(stamp)+msg+L"\r\n";
    const int bytes=WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
        static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (bytes>0) {
        std::string utf8(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
            static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);
        DWORD written=0; WriteFile(file, utf8.data(), bytes, &written, nullptr);
    }
    CloseHandle(file);
}

DiscordRPC::DiscordRPC() : m_stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

DiscordRPC::~DiscordRPC()
{
    Disconnect();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

std::string DiscordRPC::WideToUtf8(const std::wstring& w)
{
    if (w.empty() || w.size() > 1024) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string DiscordRPC::EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

bool DiscordRPC::Connect(const std::string& applicationId)
{
    Disconnect();
    if (!m_stopEvent || !ResetEvent(m_stopEvent) || applicationId.empty() || applicationId.size() > 32 ||
        !std::all_of(applicationId.begin(), applicationId.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;

    m_applicationId = applicationId;

    if (!OpenConnection()) return false;
    m_running = true;
    m_worker = std::thread(&DiscordRPC::WorkerLoop, this);
    return true;
}

bool DiscordRPC::OpenConnection()
{
    m_connected = false;
    if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
    m_pipe = INVALID_HANDLE_VALUE;
    if (WaitForSingleObject(m_stopEvent, 0) != WAIT_TIMEOUT) return false;
    if (m_pipeOpener) {
        m_pipe = m_pipeOpener();
    } else {
        for (int i = 0; i < 10; ++i) {
            wchar_t name[64];
            swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
            m_pipe = OpenRpcPipe(name);
            if (m_pipe != INVALID_HANDLE_VALUE) break;
        }
    }
    if (m_pipe == INVALID_HANDLE_VALUE) return false;
    const std::string handshake = "{\"v\":1,\"client_id\":\"" +
        EscapeJson(m_applicationId) + "\"}";
    Opcode op{};
    std::string payload;
    if (!SendFrame(Opcode::Handshake, handshake) || !ReadFrame(op, payload) || op != Opcode::Frame) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        RPCLog(L"Handshake failed; connection closed");
        return false;
    }
    m_connected = true;
    RPCLog(L"Connect: success");
    return true;
}

void DiscordRPC::Disconnect()
{
    m_running = false;
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_worker.joinable()) m_worker.join();

    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    m_connected = false;
    RPCLog(L"Disconnected");
}

bool DiscordRPC::SendFrame(Opcode op, const std::string& payload)
{
    return WriteRpcFrame(m_pipe, m_stopEvent, static_cast<uint32_t>(op), payload);
}

bool DiscordRPC::ReadFrame(Opcode& op, std::string& payload)
{
    uint32_t opcode = 0;
    if (!ReadRpcFrame(m_pipe, m_stopEvent, opcode, payload)) return false;
    op = static_cast<Opcode>(opcode);
    return op != Opcode::Close;
}

void DiscordRPC::SetActivity(const std::wstring& details,
                              const std::wstring& state,
                              std::chrono::system_clock::time_point startTime,
                              const std::string& largeImageKey,
                              const std::wstring& largeImageText,
                              const std::wstring& activityName,
                              const std::string& fallbackImageUrl,
                              const std::vector<std::string>& fallbackImageUrls)
{
    if (details.size() > 1024 || state.size() > 1024 || largeImageText.size() > 1024 || largeImageKey.size() > 1024)
        return;
    std::lock_guard<std::mutex> lock(m_activityMutex);
    const auto normalizedName=!activityName.empty() && Games::ValidText(activityName,128) ? activityName : L"NitLink";
    const auto startUnix=std::chrono::duration_cast<std::chrono::seconds>(startTime.time_since_epoch()).count();
    const auto normalizedText=largeImageText.size()<=128 ? GetDiscordActivityField(largeImageText) : std::wstring_view{};
    const std::wstring imageUrl(largeImageKey.begin(), largeImageKey.end());
    const bool external = IsDiscordCoverUrl(imageUrl);
    const bool asset = !largeImageKey.empty() && largeImageKey.size()<=64 && !largeImageKey.starts_with("sgdb-") &&
        !std::all_of(largeImageKey.begin(),largeImageKey.end(),[](char c){return c>='0'&&c<='9';}) &&
        std::all_of(largeImageKey.begin(),largeImageKey.end(),[](char c){return(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_';});
    const std::string requested=external||asset?largeImageKey:"";
    std::vector<std::string> fallbacks;
    const auto addFallback=[&](const std::string& candidate){
        if(!external||candidate.empty()||candidate.size()>300||candidate==requested||
           std::find(fallbacks.begin(),fallbacks.end(),candidate)!=fallbacks.end())return;
        const std::wstring wide(candidate.begin(),candidate.end());
        if(Games::IsCoverUrl(wide)&&!Games::IsNintendoStoreImageUrl(wide))fallbacks.push_back(candidate);
    };
    addFallback(fallbackImageUrl);
    for(const auto& candidate:fallbackImageUrls){if(fallbacks.size()>=12)break;addFallback(candidate);}
    const auto nextFallback=[&]()->std::string{
        const auto found=std::find_if(fallbacks.begin(),fallbacks.end(),[&](const auto& url){return !m_rejectedImages.contains(url);});
        return found==fallbacks.end()?std::string{}:*found;
    };
    if(requested!=m_requestedImageKey)m_rejectedImages.clear();
    std::string effective=requested;
    bool usingFallback=false;
    if(m_rejectedImages.contains(effective)) {
        effective=nextFallback();usingFallback=!effective.empty();
    }
    if (!m_activityDirty && !m_clearRequested && m_pendingName==normalizedName &&
        m_pendingDetails==GetDiscordActivityField(details) && m_pendingState==GetDiscordActivityField(state) &&
        m_pendingStartTimeUnix==startUnix && m_requestedImageKey==requested && m_fallbackImageUrls==fallbacks &&
        m_pendingLargeImageKey==effective &&
        m_pendingLargeImageText==normalizedText) return;
    const auto previousName=m_pendingName;
    m_pendingName = normalizedName;
    if(previousName!=m_pendingName&&m_nameState.load()!=NameState::Unsupported)m_nameState=NameState::Unverified;
    m_pendingDetails        = GetDiscordActivityField(details);
    m_pendingState          = GetDiscordActivityField(state);
    m_pendingStartTimeUnix  = startUnix;
    const bool changed=m_pendingLargeImageKey!=effective || m_requestedImageKey!=requested;
    m_requestedImageKey=requested;m_fallbackImageUrls=std::move(fallbacks);m_usingFallbackImage=usingFallback;
    m_pendingLargeImageKey=effective;
    if(changed){++m_imageRevision;m_proxyImage.reset();if(!effective.empty()||requested.empty())m_imageError.clear();}
    if(external && !effective.empty()) {
        if(changed || (m_imageState.load()!=ImageState::ExternalAccepted && m_imageState.load()!=ImageState::ExternalAvailable))m_imageState=ImageState::ExternalPending;
    } else if(!external) {
        m_imageState=asset?ImageState::Asset:ImageState::Unavailable;m_imageError.clear();
    } else {
        if(m_imageState.load()!=ImageState::RpcError)m_imageState=ImageState::ProxyFailed;
        if(m_imageError.empty())m_imageError=L"The selected cover previously failed Discord's image proxy";
    }
    m_pendingLargeImageText = normalizedText;
    m_activityDirty         = true;
    m_clearRequested        = false;
}

void DiscordRPC::ClearActivity()
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_clearRequested = true;
    m_activityDirty  = false;
    ++m_imageRevision;
    m_imageState = ImageState::Unavailable;
    m_imageError.clear();
    m_pendingLargeImageKey.clear();m_requestedImageKey.clear();m_fallbackImageUrls.clear();
    m_proxyImage.reset();m_usingFallbackImage=false;m_rejectedImages.clear();
}

void DiscordRPC::RejectImageLocked(const std::string& image,const std::wstring& reason,bool rpcError)
{
    m_rejectedImages.insert(image);m_proxyImage.reset();m_imageError=reason;
    const auto fallback=std::find_if(m_fallbackImageUrls.begin(),m_fallbackImageUrls.end(),[&](const auto& url){return !m_rejectedImages.contains(url);});
    if(fallback!=m_fallbackImageUrls.end()) {
        m_pendingLargeImageKey=*fallback;m_usingFallbackImage=true;
        ++m_imageRevision;m_imageState=ImageState::ExternalPending;
    }else {
        m_pendingLargeImageKey.clear();m_imageState=rpcError?ImageState::RpcError:ImageState::ProxyFailed;
    }
    m_activityDirty=true; // Original text and start time remain unchanged.
}

void DiscordRPC::WorkerLoop()
{
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    struct Apartment {
        bool close;
        ~Apartment() { if (close) { winrt::clear_factory_cache(); RoUninitialize(); } }
    } apartment{SUCCEEDED(initialized)};
    std::string activityNonce, externalImage, sentName;
    uint64_t sentImageRevision = 0;
    struct Probe { std::string image, proxy; uint64_t revision=0; };
    std::optional<Probe> queuedProbe, activeProbe;
    std::future<DiscordImageCheck> imageCheck;
    // Replies are drained independently of the two-second activity send limit.
    auto lastSend = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    uint64_t nonce = 1000;
    RpcFrameReader reader;
    DWORD reconnectDelay = 2000;
    auto nextReconnect = std::chrono::steady_clock::now();
    auto connectedSince = nextReconnect;
    bool sentActivity = false, lastWasClear = false;
    const auto disconnected = [&](const wchar_t* reason) {
        if (WaitForSingleObject(m_stopEvent, 0) != WAIT_TIMEOUT) return;
        RPCLog(std::wstring(reason) + L"; retry in " + std::to_wstring(reconnectDelay / 1000) + L" seconds");
        m_connected = false;
        if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        reader.Reset();
        queuedProbe.reset();
        nextReconnect = std::chrono::steady_clock::now() + std::chrono::milliseconds(reconnectDelay);
        reconnectDelay = std::min<DWORD>(reconnectDelay * 2, 30000);
        std::lock_guard<std::mutex> lock(m_activityMutex);
        if (sentActivity && !m_activityDirty && !m_clearRequested) {
            m_clearRequested = lastWasClear;
            m_activityDirty = !lastWasClear;
        }
    };

    while (m_running) {
        if (WaitForSingleObject(m_stopEvent, 500) != WAIT_TIMEOUT) break;
        auto now = std::chrono::steady_clock::now();
        if (imageCheck.valid() && imageCheck.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            DiscordImageCheck check;
            try { check=imageCheck.get(); } catch (...) { check.reason=L"Discord image check failed"; }
            std::lock_guard lock(m_activityMutex);
            if (activeProbe && activeProbe->revision==m_imageRevision &&
                activeProbe->image==m_pendingLargeImageKey && !m_clearRequested) {
                if (check.state==DiscordImageCheckState::Available) {
                    m_imageState=ImageState::ExternalAvailable;
                    m_imageError.clear();
                    m_proxyImage=check.previewImage;
                } else if (check.state!=DiscordImageCheckState::Cancelled) {
                    m_imageError=check.reason.empty() ? L"Discord image check network error "+std::to_wstring(check.error) : check.reason;
                    if (check.state==DiscordImageCheckState::HttpError || check.state==DiscordImageCheckState::InvalidImage) {
                        RejectImageLocked(activeProbe->image,m_imageError,false);
                    }
                }
                RPCLog(L"Image proxy check: large_image="+std::wstring(winrt::to_hstring(activeProbe->image))+
                    std::wstring(L"; HTTP=")+std::to_wstring(check.httpStatus)+
                    L"; imageReadable="+(check.state==DiscordImageCheckState::Available?L"true":L"false")+
                    L"; clientRendered=unverified; reason="+m_imageError, !m_pipeOpener);
            }
            activeProbe.reset();
        }
        if (!m_connected) {
            if (now < nextReconnect) continue;
            if (!OpenConnection()) { disconnected(L"Discord reconnect unavailable"); continue; }
            m_nameState=NameState::Unverified; // A new client connection must acknowledge name again.
            {std::lock_guard lock(m_activityMutex);if(m_pendingLargeImageKey.starts_with("https://"))m_imageState=ImageState::ExternalPending;}
            reader.Reset();
            connectedSince = std::chrono::steady_clock::now();
        }
        // A slow or absent reply retains presence. Only complete frames are
        // interpreted, with at most four bounded frames drained per pass.
        for (unsigned count = 0; count < 4; ++count) {
            uint32_t opcode = 0;
            std::string payload;
            const auto result = reader.Poll(m_pipe, m_stopEvent, opcode, payload);
            if (result == RpcReadResult::Pending) break;
            if (result == RpcReadResult::Failed || opcode == static_cast<uint32_t>(Opcode::Close) ||
                (opcode == static_cast<uint32_t>(Opcode::Ping) && !SendFrame(Opcode::Pong, payload))) {
                disconnected(L"Discord stream closed or invalid");
                break;
            }
            if (opcode == static_cast<uint32_t>(Opcode::Frame) && SUCCEEDED(initialized)) {
                try {
                    const auto reply=ParseSetActivityReply(payload,activityNonce,lastWasClear);
                    if (reply.kind==ActivityReplyKind::Unrelated) continue;
                    if (reply.kind==ActivityReplyKind::Malformed) {
                        RPCLog(winrt::to_hstring("Malformed RPC response: "+reply.diagnostic).c_str(), !m_pipeOpener);
                        continue; // No reconnect, resend, fallback, or false acceptance.
                    }
                    const bool rejected=reply.kind==ActivityReplyKind::Error;
                    const auto& returnedImage=reply.largeImage;
                    const auto& returnedName=reply.name;
                    const auto errorMessage=reply.message.substr(0,512);
                    const auto errorCode=reply.errorCode;
                    m_activityState=rejected?ActivityState::Error:ActivityState::Accepted;
                    RPCLog(winrt::to_hstring("RX SET_ACTIVITY nonce="+activityNonce+
                        "; result="+(rejected?"ERROR":"accepted")+"; code="+std::to_string(errorCode)+
                        "; message=\""+EscapeJson(errorMessage)+"\"; large_image=\""+EscapeJson(returnedImage)+
                        "\"; name=\""+EscapeJson(returnedName)+"\"; clientRendered=unverified").c_str(), !m_pipeOpener);
                    std::lock_guard lock(m_activityMutex);
                    if (!sentName.empty() && WideToUtf8(m_pendingName) == sentName && !m_clearRequested) {
                        if(!rejected)m_nameState = returnedName==sentName ? NameState::Accepted : NameState::Unsupported;
                        if (rejected) {
                            m_nameState=NameState::Unsupported;
                            m_activityDirty = true; // Retry the same activity without name.
                            // Never blacklist a valid cover because a combined
                            // name/image command failed. Diagnose name first.
                            continue;
                        }
                    }
                    if (!externalImage.empty() && m_pendingLargeImageKey == externalImage &&
                        sentImageRevision==m_imageRevision && !m_clearRequested) {
                        if (rejected) {
                            RejectImageLocked(externalImage,std::wstring(winrt::to_hstring("Discord SET_ACTIVITY error "+
                                std::to_string(errorCode)+": "+errorMessage)),true);
                        } else if (!DiscordImageProxyPath(returnedImage,externalImage).empty()) {
                            if(m_imageState.load()!=ImageState::ExternalAvailable) {
                                m_imageState=ImageState::ExternalAccepted;
                                queuedProbe=Probe{externalImage,returnedImage,m_imageRevision};
                            }
                        } else if (returnedImage==externalImage) {
                            m_imageState=ImageState::ExternalAccepted;
                        } else {
                            m_imageState=ImageState::ExternalUnresolved;
                            m_imageError=L"Discord accepted SET_ACTIVITY without resolving the selected cover";
                        }
                    } else if (rejected && externalImage.empty() && sentName.empty() && !lastWasClear) {
                        m_imageState=ImageState::RpcError;
                        m_imageError=std::wstring(winrt::to_hstring("Discord SET_ACTIVITY error "+
                            std::to_string(errorCode)+": "+errorMessage));
                    }
                    // An accepted activity/URL is not proof of rendered pixels.
                } catch (...) { RPCLog(L"Malformed SET_ACTIVITY response ignored", !m_pipeOpener); }
            }
        }
        if (!m_connected) continue;
        if (!imageCheck.valid() && queuedProbe) {
            activeProbe=std::exchange(queuedProbe, std::nullopt);
            const auto probe=*activeProbe;
            imageCheck=std::async(std::launch::async, [this,probe] {
                return m_imageProbe ? m_imageProbe(probe.proxy,probe.image,m_stopEvent) :
                    CheckDiscordImageProxy(probe.proxy,probe.image,m_stopEvent);
            });
        }
        now = std::chrono::steady_clock::now();
        if (now - connectedSince >= std::chrono::seconds(60)) reconnectDelay = 2000;
        if (now - lastSend < std::chrono::seconds(2)) continue;

        std::wstring details, state, largeText, name;
        std::string  largeKey;
        int64_t      startUnix = 0;
        bool         dirty = false, doClear = false;
        {
            std::lock_guard<std::mutex> lock(m_activityMutex);
            dirty   = m_activityDirty;
            doClear = m_clearRequested;
            if (dirty) {
                details   = m_pendingDetails;
                state     = m_pendingState;
                startUnix = m_pendingStartTimeUnix;
                largeKey  = m_pendingLargeImageKey;
                largeText = m_pendingLargeImageText;
                name = m_pendingName;
                sentImageRevision = m_imageRevision;
                m_activityDirty = false;
            }
            m_clearRequested = false;
        }

        if (!dirty && !doClear) continue;

        DWORD pid = GetCurrentProcessId();
        std::stringstream cmd;
        activityNonce = std::to_string(nonce++);
        externalImage = largeKey.starts_with("https://") ? largeKey : "";
        sentName = !doClear && !name.empty() && m_nameState.load()!=NameState::Unsupported ? WideToUtf8(name) : "";
        cmd << "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" << activityNonce << "\","
            << "\"args\":{\"pid\":" << pid;
        if (doClear) {
            cmd << ",\"activity\":null}}";
        } else {
            cmd << ",\"activity\":{\"timestamps\":{\"start\":" << startUnix << "}";
            if (!sentName.empty()) cmd << ",\"name\":\"" << EscapeJson(sentName) << "\"";
            // Match Discord's optional-string serialization: omit unavailable
            // source/signal fields rather than sending empty text or filler.
            if (!details.empty())
                cmd << ",\"details\":\"" << EscapeJson(WideToUtf8(details)) << "\"";
            if (!state.empty())
                cmd << ",\"state\":\"" << EscapeJson(WideToUtf8(state)) << "\"";
            if (!largeKey.empty()) {
                cmd << ",\"assets\":{"
                    << "\"large_image\":\"" << EscapeJson(largeKey) << "\"";
                if (!largeText.empty()) cmd << ",\"large_text\":\"" << EscapeJson(WideToUtf8(largeText)) << "\"";
                cmd << "}";
            }
            cmd << "}}}";
        }

        sentActivity = true;
        lastWasClear = doClear;
        m_activityState=ActivityState::Pending;
        RPCLog(winrt::to_hstring("TX "+cmd.str()).c_str(), !m_pipeOpener);
        if (!SendFrame(Opcode::Frame, cmd.str())) disconnected(L"Discord activity write failed");
        lastSend = std::chrono::steady_clock::now();
    }
    m_connected = false;
    m_running = false;
}

void DiscordRPC::RetryImage(){std::lock_guard lock(m_activityMutex);m_rejectedImages.clear();m_imageError.clear();m_proxyImage.reset();m_pendingLargeImageKey=m_requestedImageKey;m_usingFallbackImage=false;++m_imageRevision;m_activityDirty=true;}
DiscordRPC::ImageSnapshot DiscordRPC::GetImageSnapshot() const {
    std::lock_guard lock(m_activityMutex);
    return {m_pendingLargeImageKey,m_proxyImage,m_usingFallbackImage,m_imageRevision};
}
std::wstring DiscordRPC::ImageError() const { std::lock_guard lock(m_activityMutex); return m_imageError; }
const wchar_t* DiscordRPC::ActivityStatus() const {
    if(!m_connected)return L"unavailable";
    switch(m_activityState.load()) {
    case ActivityState::Pending:return L"pending";
    case ActivityState::Accepted:return L"accepted";
    case ActivityState::Error:return L"error";
    default:return L"idle";
    }
}
const wchar_t* DiscordRPC::ImageStatus() const {
    if (!m_connected) return L"unavailable";
    switch (m_imageState.load()) {
    case ImageState::Asset: return L"asset";
    case ImageState::ExternalPending: return L"externalPending";
    case ImageState::ExternalAccepted: return L"externalAccepted";
    case ImageState::ExternalAvailable: return L"externalAvailable";
    case ImageState::ExternalUnresolved: return L"externalUnresolved";
    case ImageState::ProxyFailed: return L"proxyFailed";
    case ImageState::RpcError: return L"rpcError";
    default: return L"unavailable";
    }
}
const wchar_t* DiscordRPC::NameStatus() const {
    if(!m_connected)return L"unverified";
    switch(m_nameState.load()){
    case NameState::Accepted:return L"accepted";
    case NameState::Unsupported:return L"unsupported";
    default:return L"unverified";
    }
}

} // namespace NitLink
