#pragma once

#include <windows.h>

namespace NitLink {

// Windows execution requirements belong to the calling thread, so the UI
// thread owns this request for the lifetime of visible video playback.
class PlaybackPowerRequest {
public:
    using SetStateFn = EXECUTION_STATE (WINAPI*)(EXECUTION_STATE);

    explicit PlaybackPowerRequest(SetStateFn setState = ::SetThreadExecutionState)
        : m_setState(setState) {}
    ~PlaybackPowerRequest() { SetActive(false); }
    PlaybackPowerRequest(const PlaybackPowerRequest&) = delete;
    PlaybackPowerRequest& operator=(const PlaybackPowerRequest&) = delete;

    bool SetActive(bool active)
    {
        if (active == m_active) return true;
        const EXECUTION_STATE flags = active
            ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED
            : ES_CONTINUOUS;
        if (m_setState(flags) == 0) {
            if (!m_failureReported) {
                OutputDebugStringW(L"[NitLink] Playback power request failed\n");
            }
            m_failureReported = true;
            return false;
        }
        m_failureReported = false;
        m_active = active;
        return true;
    }

    bool IsActive() const { return m_active; }

    bool SuppressesScreenSaver(WPARAM command) const
    {
        return m_active && (command & 0xFFF0) == SC_SCREENSAVE &&
               (command & SCF_ISSECURE) == 0;
    }

private:
    SetStateFn m_setState;
    bool m_active = false;
    bool m_failureReported = false;
};

} // namespace NitLink
