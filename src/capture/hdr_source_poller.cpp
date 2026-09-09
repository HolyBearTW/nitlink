#include "hdr_source_poller.h"
#include "elgato_hdr_control.h"

#include <chrono>
#include <sstream>
#include <windows.h>

namespace {

// Local logger: matches the convention in elgato_hdr_control.cpp.
// OutputDebugStringW is thread-safe, which matters because the poller's
// log calls come from a worker thread. This intentionally avoids reaching
// into application.cpp's static AppLog: that's a different translation unit
// with a different signature, and a cross-TU dependency would just
// reintroduce the linkage friction this layout avoids.
void Log(const wchar_t* msg) {
    std::wstringstream ss;
    ss << L"[NitLink/HDRPoller] " << msg << L"\n";
    OutputDebugStringW(ss.str().c_str());
}

} // anonymous namespace

namespace NitLink {

HDRSourcePoller::HDRSourcePoller() = default;

HDRSourcePoller::~HDRSourcePoller()
{
    // Defensive shutdown: if the user forgot to call Stop(), don't
    // leave a detached thread running into static destruction.
    Stop();
}

void HDRSourcePoller::Start(const std::wstring& deviceName, bool initialIsHDR10,
                          bool initialPropertyAccessible)
{
    if (m_running.load(std::memory_order_acquire)) {
        // Already running. The caller is welcome to Stop() first and
        // re-Start, but silently double-starting would leak a thread.
        return;
    }
    if (m_thread.joinable()) m_thread.join();

    // Seed the last-known state so the first probe that matches doesn't
    // fire a spurious transition report.
    m_isHDR10.store(initialIsHDR10, std::memory_order_release);
    m_hasUpdate.store(false, std::memory_order_release);
    m_stop.store(false, std::memory_order_release);

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&HDRSourcePoller::PollerThreadMain, this, deviceName,
                          initialPropertyAccessible || initialIsHDR10);

    Log(L"started");
}

void HDRSourcePoller::Stop()
{
    if (!m_running.load(std::memory_order_acquire) && !m_thread.joinable()) {
        return; // Never started, or already stopped.
    }

    m_stop.store(true, std::memory_order_release);

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_running.store(false, std::memory_order_release);
    Log(L"stopped");
}

bool HDRSourcePoller::AcceptUpdate(bool* outIsHDR10)
{
    // Atomic test-and-clear of the update flag. If the flag was set,
    // capture the current state value. The state itself doesn't need
    // CAS protection because:
    //   - The worker is the only writer to m_isHDR10.
    //   - The worker writes m_isHDR10 strictly BEFORE setting m_hasUpdate.
    //   - Exchanging m_hasUpdate (atomic release/acquire) acts as a
    //     synchronization point: if the exchange observes m_hasUpdate=true,
    //     the m_isHDR10 write that preceded it is visible to the reader.
    // So reading m_isHDR10 after a true exchange is consistent.
    const bool wasSet = m_hasUpdate.exchange(false, std::memory_order_acq_rel);
    if (!wasSet) return false;

    if (outIsHDR10) {
        *outIsHDR10 = m_isHDR10.load(std::memory_order_acquire);
    }
    return true;
}

void HDRSourcePoller::PollerThreadMain(std::wstring deviceName, bool hadAccessibleProbe)
{
    using namespace std::chrono_literals;

    // Sleep cadence: ~1 second between probes, broken into 100ms chunks
    // so a Stop() call doesn't have to wait up to a full second for the
    // worker to notice. 10 chunks x 100ms = 1s cadence.
    constexpr int kProbeIntervalChunks = 10;
    constexpr auto kSleepChunk = 100ms;

    // Track consecutive failed reads. These are not surfaced to the
    // user (would spam the log every second on a 4K S) but if the
    // FIRST few reads all fail, the loop exits early: the property GUID
    // isn't supported and polling indefinitely buys nothing.
    int consecutiveFailures = 0;
    constexpr int kMaxInitialFailures = 5; // ~5 seconds of grace

    while (!m_stop.load(std::memory_order_acquire)) {
        // Probe. quiet=true suppresses the per-call diagnostic spew from
        // elgato_hdr_control (full packet hex dump + decoded EOTF). Once
        // per second forever would render the debug output useless;
        // only state transitions and unusual conditions log from here.
        HDRSourceInfo info = ReadElgatoHDRSource(deviceName, /*quiet=*/true);

        if (!info.propertyAccessible) {
            // Either the device class doesn't support the property
            // (4K S over USB) or there was a transient driver error.
            // Don't update state; preserve last-known-good.
            consecutiveFailures++;
            if (consecutiveFailures >= kMaxInitialFailures && !hadAccessibleProbe)
            {
                // Initial probes all failed and no successful init-time
                // or worker probe established support for this
                // query. Bail out gracefully to avoid burning CPU on a
                // DirectShow open/close every second forever.
                Log(L"property unsupported on this device, stopping poll loop");
                break;
            }
        } else {
            hadAccessibleProbe = true;
            // Successful read. Reset the failure counter and look for
            // a transition vs. last-known state.
            consecutiveFailures = 0;

            const bool nowHDR10 = info.isHDR10;
            const bool wasHDR10 = m_isHDR10.load(std::memory_order_acquire);

            if (nowHDR10 != wasHDR10) {
                // State change. Write the new state, THEN set the update
                // flag (this ordering matters: AcceptUpdate relies on
                // it via the memory_order_acq_rel exchange).
                m_isHDR10.store(nowHDR10, std::memory_order_release);
                m_hasUpdate.store(true, std::memory_order_release);

                Log(nowHDR10
                    ? L"source changed SDR -> HDR10"
                    : L"source changed HDR10 -> SDR");
            }
            // Same state: no-op, just keep watching.
        }

        // Sleep until the next probe, breakable by Stop().
        for (int i = 0; i < kProbeIntervalChunks; ++i) {
            if (m_stop.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(kSleepChunk);
        }
    }
    m_running.store(false, std::memory_order_release);
}

} // namespace NitLink
