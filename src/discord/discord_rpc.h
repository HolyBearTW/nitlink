#pragma once

#include <windows.h>
#include <string>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

namespace NitLink {

// Discord Rich Presence client over the local IPC pipe.
//
// Discord exposes a named pipe at \\.\pipe\discord-ipc-0 (or -1, -2, ...).
// The client opens it, sends a HANDSHAKE frame, then sends SET_ACTIVITY frames
// whenever the activity state changes. No external library needed: pure Win32
// plus a tiny JSON formatter.
//
// To use this for real a Discord application ID is required from
// https://discord.com/developers/applications. NitLink can ship with a
// default app ID; users can override in config.
class DiscordRPC {
public:
    DiscordRPC();
    ~DiscordRPC();

    // applicationId: from Discord Developer Portal. If empty, RPC is disabled.
    bool Connect(const std::string& applicationId);
    void Disconnect();
    bool IsConnected() const { return m_connected; }

    // Activity = "details" line shown under the app name in the user's profile.
    // Example: details = "Playing Saros", state = "Boss Fight 3 of 6"
    void SetActivity(const std::wstring& details,
                     const std::wstring& state,
                     std::chrono::system_clock::time_point startTime,
                     const std::string& largeImageKey = "",
                     const std::wstring& largeImageText = L"");

    void ClearActivity();

private:
    enum class Opcode : uint32_t {
        Handshake = 0,
        Frame     = 1,
        Close     = 2,
        Ping      = 3,
        Pong      = 4,
    };

    bool SendFrame(Opcode op, const std::string& payload);
    bool ReadFrame(Opcode& op, std::string& payload);
    void WorkerLoop();
    static std::string EscapeJson(const std::string& s);
    static std::string WideToUtf8(const std::wstring& w);

    HANDLE              m_pipe       = INVALID_HANDLE_VALUE;
    std::atomic<bool>   m_connected{false};
    std::string         m_applicationId;

    // Pending activity (set from main thread, sent from worker)
    std::mutex          m_activityMutex;
    bool                m_activityDirty = false;
    bool                m_clearRequested = false;
    std::wstring        m_pendingDetails;
    std::wstring        m_pendingState;
    int64_t             m_pendingStartTimeUnix = 0;
    std::string         m_pendingLargeImageKey;
    std::wstring        m_pendingLargeImageText;

    std::thread         m_worker;
    std::atomic<bool>   m_running{false};
};

} // namespace NitLink
