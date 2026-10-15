#include "pipe_transport.h"
#include <algorithm>
#include "discord_rpc.h"
#include "activity_fields.h"
#include "app/game_identity.h"
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <debugapi.h>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>

namespace NitLink {

static void RPCLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/Discord] " + msg + L"\n").c_str());
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
                              const std::wstring& activityName)
{
    if (details.size() > 1024 || state.size() > 1024 || largeImageText.size() > 1024 || largeImageKey.size() > 1024)
        return;
    std::lock_guard<std::mutex> lock(m_activityMutex);
    const auto previousName=m_pendingName;
    m_pendingName = !activityName.empty() && Games::ValidText(activityName,128) ? activityName : L"NitLink";
    if(previousName!=m_pendingName&&m_nameState.load()!=NameState::Unsupported)m_nameState=NameState::Unverified;
    m_pendingDetails        = GetDiscordActivityField(details);
    m_pendingState          = GetDiscordActivityField(state);
    m_pendingStartTimeUnix  = std::chrono::duration_cast<std::chrono::seconds>(
        startTime.time_since_epoch()).count();
    const std::wstring imageUrl(largeImageKey.begin(), largeImageKey.end());
    const bool external = Games::IsCoverUrl(imageUrl);
    const bool asset = !largeImageKey.empty() && largeImageKey.size() <= 64 &&
        std::all_of(largeImageKey.begin(), largeImageKey.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
    const auto previousImage = m_pendingLargeImageKey;
    m_pendingLargeImageKey = external || asset ? largeImageKey : "nitlink-logo";
    if (external && m_rejectedImageKey == largeImageKey) {
        m_pendingLargeImageKey = "nitlink-logo"; m_imageState = ImageState::Fallback;
    } else if (external) {
        // Repeated source updates do not erase a prior acknowledgement.
        if (m_imageState.load() != ImageState::ExternalAccepted || previousImage != largeImageKey)
            m_imageState = ImageState::ExternalPending;
    } else m_imageState = ImageState::Asset;
    m_pendingLargeImageText = largeImageText;
    m_activityDirty         = true;
    m_clearRequested        = false;
}

void DiscordRPC::ClearActivity()
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_clearRequested = true;
    m_activityDirty  = false;
}

void DiscordRPC::WorkerLoop()
{
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    struct Apartment {
        bool close;
        ~Apartment() { if (close) { winrt::clear_factory_cache(); RoUninitialize(); } }
    } apartment{SUCCEEDED(initialized)};
    std::string activityNonce, externalImage, sentName;
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
            if (opcode == static_cast<uint32_t>(Opcode::Frame) && (!externalImage.empty() || !sentName.empty()) && SUCCEEDED(initialized)) {
                try {
                    using namespace winrt::Windows::Data::Json;
                    JsonObject reply;
                    if (!JsonObject::TryParse(winrt::to_hstring(payload), reply) ||
                        reply.GetNamedString(L"cmd", L"") != L"SET_ACTIVITY" ||
                        winrt::to_string(reply.GetNamedString(L"nonce", L"")) != activityNonce) continue;
                    bool rejected = reply.GetNamedString(L"evt", L"") == L"ERROR";
                    bool accepted = false;
                    bool nameAccepted = false;
                    if (!rejected && reply.HasKey(L"data") && reply.GetNamedValue(L"data").ValueType() == JsonValueType::Object) {
                        const auto data = reply.GetNamedObject(L"data");
                        nameAccepted = !sentName.empty() && winrt::to_string(data.GetNamedString(L"name",L"")) == sentName;
                        if (data.HasKey(L"assets") && data.GetNamedValue(L"assets").ValueType() == JsonValueType::Object) {
                            const auto assets = data.GetNamedObject(L"assets");
                            const auto returned = winrt::to_string(assets.GetNamedString(L"large_image", L""));
                            accepted = returned == externalImage || returned.starts_with("mp:external/");
                            rejected = !accepted;
                        } else rejected = true;
                    }
                    std::lock_guard lock(m_activityMutex);
                    if (!sentName.empty() && WideToUtf8(m_pendingName) == sentName && !m_clearRequested) {
                        const bool error=reply.GetNamedString(L"evt",L"")==L"ERROR";
                        if(!error)m_nameState = nameAccepted ? NameState::Accepted : NameState::Unsupported;
                        if (error&&externalImage.empty()) {
                            m_nameState=NameState::Unsupported;
                            m_activityDirty = true; // Retry the same activity without name.
                            rejected = false; // Name fallback precedes the independent image fallback.
                        }
                    }
                    if (!externalImage.empty() && m_pendingLargeImageKey == externalImage && !m_clearRequested) {
                        if (rejected) {
                            m_rejectedImageKey = externalImage;
                            m_pendingLargeImageKey = "nitlink-logo";
                            m_activityDirty = true; m_imageState = ImageState::Fallback;
                            RPCLog(L"External image activity rejected; retrying NitLink art with the same text/timestamp");
                        } else if (accepted) m_imageState = ImageState::ExternalAccepted;
                    }
                    // An accepted activity/URL is not proof of rendered pixels.
                } catch (...) { /* Malformed/unrelated replies do not change text or timing. */ }
            }
        }
        if (!m_connected) continue;
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
                    << "\"large_image\":\"" << EscapeJson(largeKey) << "\","
                    << "\"large_text\":\""  << EscapeJson(WideToUtf8(largeText)) << "\""
                    << "}";
            }
            cmd << "}}}";
        }

        sentActivity = true;
        lastWasClear = doClear;
        if (!SendFrame(Opcode::Frame, cmd.str())) disconnected(L"Discord activity write failed");
        lastSend = std::chrono::steady_clock::now();
    }
    m_connected = false;
    m_running = false;
}

void DiscordRPC::RetryImage(){std::lock_guard lock(m_activityMutex);m_rejectedImageKey.clear();}
const wchar_t* DiscordRPC::ImageStatus() const {
    if (!m_connected) return L"unavailable";
    switch (m_imageState.load()) {
    case ImageState::Asset: return L"asset";
    case ImageState::ExternalPending: return L"externalPending";
    case ImageState::ExternalAccepted: return L"externalAccepted";
    case ImageState::Fallback: return L"fallback";
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
