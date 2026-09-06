#include "discord_rpc.h"
#include <debugapi.h>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>

namespace NitLink {

static void RPCLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/Discord] " + msg + L"\n").c_str());
}

DiscordRPC::DiscordRPC() = default;

DiscordRPC::~DiscordRPC()
{
    Disconnect();
}

std::string DiscordRPC::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
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
    if (applicationId.empty()) {
        RPCLog(L"Connect: empty application ID, RPC disabled");
        return false;
    }

    m_applicationId = applicationId;

    // Discord may have multiple pipes if multiple clients are running.
    // Try 0..9 and use the first one that connects.
    for (int i = 0; i < 10; i++) {
        wchar_t pipeName[64];
        swprintf(pipeName, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            m_pipe = h;
            break;
        }
    }

    if (m_pipe == INVALID_HANDLE_VALUE) {
        RPCLog(L"Connect: no Discord client found (is Discord running?)");
        return false;
    }

    // Send HANDSHAKE: {"v": 1, "client_id": "..."}
    std::string handshake = "{\"v\":1,\"client_id\":\"" +
                             EscapeJson(m_applicationId) + "\"}";
    if (!SendFrame(Opcode::Handshake, handshake)) {
        RPCLog(L"Connect: handshake send failed");
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    // Read the READY frame back. The payload is not parsed; Discord just
    // needs the pipe drained so it doesn't back up.
    //
    // Deadline guard: ReadFile on the Discord IPC pipe is blocking with no
    // timeout, so if Discord opens the pipe but never replies to the
    // handshake (recent Discord builds rate-limit / silently drop RPC
    // handshakes from unverified app IDs), Initialize hangs forever and
    // the user sees the main window stuck on its background colour with no
    // capture ever starting. PeekNamedPipe allows waiting for at least the
    // 8-byte frame header to arrive within a short window; if it doesn't,
    // Discord is treated as unavailable and startup continues. Discord
    // normally replies in well under 100 ms, so a 1.5 s deadline is generous.
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(1500);
        DWORD bytesAvailable = 0;
        bool  ready          = false;
        while (clock::now() < deadline) {
            if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr,
                                &bytesAvailable, nullptr)) {
                RPCLog(L"Connect: PeekNamedPipe failed during handshake wait");
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
                return false;
            }
            if (bytesAvailable >= sizeof(uint32_t) * 2) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!ready) {
            RPCLog(L"Connect: handshake reply timeout (Discord not responding, RPC disabled for session)");
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
            return false;
        }
    }
    Opcode op; std::string payload;
    if (!ReadFrame(op, payload)) {
        RPCLog(L"Connect: handshake reply read failed");
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    m_connected = true;
    m_running   = true;
    m_worker    = std::thread(&DiscordRPC::WorkerLoop, this);
    RPCLog(L"Connect: success");
    return true;
}

void DiscordRPC::Disconnect()
{
    if (!m_running.exchange(false)) return;

    if (m_worker.joinable()) {
        // Kick the worker out of any parked synchronous pipe I/O before
        // joining. The worker issues blocking WriteFile (SendFrame) and
        // ReadFile (ReadFrame) calls; if Discord is alive but not draining the
        // pipe, one of those can block in the kernel indefinitely, and clearing
        // m_running alone cannot wake it, so join() would hang app exit.
        // CancelSynchronousIo targets the worker thread's in-flight call.
        // ERROR_NOT_FOUND (no call was pending) is expected and ignored. The
        // spaced retries cover the race where the worker has decided to issue
        // the call but has not yet entered the kernel when the first cancel
        // fires; the unconditional join afterward reaps the thread either way.
        const HANDLE workerHandle = m_worker.native_handle();
        for (int i = 0; i < 3; i++) {
            CancelSynchronousIo(workerHandle);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        m_worker.join();
    }

    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    m_connected = false;
    RPCLog(L"Disconnected");
}

bool DiscordRPC::SendFrame(Opcode op, const std::string& payload)
{
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    // Frame format: [opcode: u32 LE][length: u32 LE][payload bytes]
    uint32_t opc = (uint32_t)op;
    uint32_t len = (uint32_t)payload.size();
    DWORD written = 0;

    if (!WriteFile(m_pipe, &opc, sizeof(opc), &written, nullptr) || written != sizeof(opc)) return false;
    if (!WriteFile(m_pipe, &len, sizeof(len), &written, nullptr) || written != sizeof(len)) return false;
    if (len > 0) {
        if (!WriteFile(m_pipe, payload.data(), len, &written, nullptr) || written != len) return false;
    }
    return true;
}

bool DiscordRPC::ReadFrame(Opcode& op, std::string& payload)
{
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    uint32_t opc = 0, len = 0;
    DWORD got = 0;
    if (!ReadFile(m_pipe, &opc, sizeof(opc), &got, nullptr) || got != sizeof(opc)) return false;
    if (!ReadFile(m_pipe, &len, sizeof(len), &got, nullptr) || got != sizeof(len)) return false;

    op = (Opcode)opc;

    // Cap the frame length before resize(). len is an untrusted u32 straight
    // off the pipe; a corrupt or hostile frame could claim up to 4 GiB and
    // turn resize() into an OOM crash. Discord RPC frames are small JSON blobs
    // (the reference library caps the whole frame at 64 KiB), so anything
    // larger is malformed and the read is dropped.
    static constexpr uint32_t kMaxFrameLen = 64 * 1024;
    if (len > kMaxFrameLen) {
        RPCLog(L"ReadFrame: frame length exceeds cap, dropping");
        return false;
    }

    payload.resize(len);
    if (len > 0) {
        // Gate the payload ReadFile the same way the header read is gated.
        // WaitForReadableFrame before this call only guaranteed the 8 header
        // bytes; a header-only partial frame (Discord wrote the header then
        // stalled, or is shutting down) would otherwise park the worker in a
        // blocking kernel read on the missing payload, which m_running alone
        // cannot wake and which would hang Disconnect's join(). Wait for the
        // full payload to be peekable first; bail on timeout or a dropped pipe.
        if (!WaitForReadableFrame(std::chrono::milliseconds(2000), len)) return false;
        if (!ReadFile(m_pipe, payload.data(), len, &got, nullptr) || got != len) return false;
    }
    return true;
}

void DiscordRPC::SetActivity(const std::wstring& details,
                              const std::wstring& state,
                              std::chrono::system_clock::time_point startTime,
                              const std::string& largeImageKey,
                              const std::wstring& largeImageText)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_pendingDetails        = details;
    m_pendingState          = state;
    m_pendingStartTimeUnix  = std::chrono::duration_cast<std::chrono::seconds>(
        startTime.time_since_epoch()).count();
    m_pendingLargeImageKey  = largeImageKey;
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

bool DiscordRPC::WaitForReadableFrame(std::chrono::milliseconds timeout,
                                      DWORD requiredBytes)
{
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    while (m_running && clock::now() < deadline) {
        DWORD bytesAvailable = 0;
        if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
            // Pipe closed by Discord or a peek error: report not-readable so the
            // caller skips the read instead of blocking on a dead pipe.
            return false;
        }
        if (bytesAvailable >= requiredBytes) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void DiscordRPC::WorkerLoop()
{
    // Activity updates are rate-limited to once every 2 seconds (Discord has its
    // own rate limit at 5 updates per 20 seconds; staying well under it).
    auto lastSend = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    static int nonce = 1000;

    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto now = std::chrono::steady_clock::now();
        if (now - lastSend < std::chrono::seconds(2)) continue;

        std::wstring details, state, largeText;
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
                m_activityDirty = false;
            }
            m_clearRequested = false;
        }

        if (!dirty && !doClear) continue;

        DWORD pid = GetCurrentProcessId();
        std::stringstream cmd;
        cmd << "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" << (nonce++) << "\","
            << "\"args\":{\"pid\":" << pid;
        if (doClear) {
            cmd << ",\"activity\":null}}";
        } else {
            cmd << ",\"activity\":{"
                << "\"details\":\"" << EscapeJson(WideToUtf8(details)) << "\","
                << "\"state\":\""   << EscapeJson(WideToUtf8(state))   << "\","
                << "\"timestamps\":{\"start\":" << startUnix << "}";
            if (!largeKey.empty()) {
                cmd << ",\"assets\":{"
                    << "\"large_image\":\"" << EscapeJson(largeKey) << "\","
                    << "\"large_text\":\""  << EscapeJson(WideToUtf8(largeText)) << "\""
                    << "}";
            }
            cmd << "}}}";
        }

        if (!SendFrame(Opcode::Frame, cmd.str())) {
            RPCLog(L"WorkerLoop: SendFrame failed; disconnecting");
            m_connected = false;
            break;
        }
        // Drain the response (don't bother parsing it).
        //
        // Gate the header read on PeekNamedPipe (and ReadFrame gates its own
        // payload read the same way) so a silent or unresponsive Discord cannot
        // park the worker in a kernel read: Disconnect sets m_running=false and
        // the wait returns within one poll interval, so join() completes instead
        // of hanging app exit. A reply that never arrives times out and the loop
        // continues, so one dropped response does not stall later updates.
        Opcode op; std::string payload;
        if (WaitForReadableFrame(std::chrono::milliseconds(2000), sizeof(uint32_t) * 2)) {
            ReadFrame(op, payload);
        }
        lastSend = now;
    }
}

} // namespace NitLink
