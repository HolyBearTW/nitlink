#pragma once

#include <string>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace NitLink {

// Routes audio from the capture card to the system's default audio output.
// Uses WASAPI: one IAudioClient in capture mode reading from the Elgato's
// "Digital Audio Interface" device, another in render mode writing to the
// default playback device. A worker thread bridges them with minimal latency.
//
// Both endpoints are opened, used, and rebuilt entirely on the worker thread.
// Endpoints are not assumed to survive the life of the process: the default
// playback device can change, a device's format can be reconfigured, and the
// capture card can be unplugged. Any of those invalidates the affected
// IAudioClient, and the worker rebuilds it in place rather than falling
// permanently silent.
class AudioRouter {
public:
    AudioRouter();
    ~AudioRouter();

    // captureDeviceNameHint: substring to match against device friendly name,
    // e.g. L"Elgato" or L"4K Pro". An empty hint falls back to L"Elgato";
    // there is no "first capture device found" behaviour, because matching a
    // random microphone would be worse than reporting no audio.
    //
    // Returns whether the first endpoint-open attempt succeeded. A false
    // return does not mean audio is dead for the run: the worker stays alive
    // and keeps retrying, so plugging the card back in recovers on its own.
    bool Initialize(const std::wstring& captureDeviceNameHint = L"Elgato");
    void Shutdown();

    void SetVolume(float volume); // 0.0 - 1.0
    void SetMuted(bool muted);

    float GetVolume() const { return m_volume; }
    bool  IsMuted()   const { return m_muted; }
    bool  IsRunning() const { return m_running; }

    // True only while both endpoints are open and frames are moving. Distinct
    // from IsRunning(), which stays true while the worker retries a lost
    // device.
    bool  IsStreaming() const { return m_streaming; }

    // HRESULT of the most recent failed capture endpoint setup, S_OK once an
    // attempt succeeds. E_ACCESSDENIED (0x80070005) means the Windows
    // Microphone privacy switch is blocking desktop apps, capture cards
    // included.
    HRESULT LastCaptureError() const { return m_lastCaptureError.load(); }

    // Routing health, refreshed by the worker: FIFO fill in milliseconds and
    // cumulative frames of silence written on an empty FIFO (underruns),
    // frames discarded on a full FIFO (overruns), and single-frame drift
    // corrections (slips).
    uint32_t FifoFillMs() const { return m_fillMs.load(); }
    uint64_t Underruns()  const { return m_underrunFrames.load(); }
    uint64_t Overruns()   const { return m_overrunFrames.load(); }
    uint64_t Slips()      const { return m_slipCount.load(); }

private:
    class EndpointNotifier;

    bool FindCaptureDevice(const std::wstring& nameHint, ComPtr<IMMDevice>& outDevice);
    bool SetupCapture();
    bool SetupRender();
    void TeardownCapture();
    void TeardownRender();

    // Brings both endpoints up if they are down and services any pending
    // rebuild requests. Returns true when the pipeline is ready to pump.
    bool EnsureEndpoints();

    // Classifies a WASAPI failure and flags the affected side for rebuild.
    void HandleStreamError(HRESULT hr, const wchar_t* what, bool captureSide);

    void RouteLoop();

    // One wake of the pump: move every ready capture packet into the FIFO,
    // then top the render endpoint up from it. Each returns false after a
    // stream error was handed to HandleStreamError.
    bool DrainCapture();
    bool FillRender();
    void FifoReset(UINT32 bytesPerFrame, UINT32 samplesPerSec);
    void FifoPush(const BYTE* data, UINT32 frames, bool silent);
    UINT32 FifoPop(BYTE* out, UINT32 frames);
    void FifoSkip(UINT32 frames);
    void LogStatsIfDue();

    std::wstring m_nameHint = L"Elgato";

    ComPtr<IMMDeviceEnumerator> m_enumerator;
    ComPtr<IMMNotificationClient> m_notifier;

    ComPtr<IAudioClient>        m_captureClient;
    ComPtr<IAudioCaptureClient> m_captureService;
    WAVEFORMATEX*               m_captureFormat = nullptr;
    UINT32                      m_captureBufferFrames = 0;
    std::wstring                m_captureDeviceId;

    ComPtr<IAudioClient>        m_renderClient;
    ComPtr<IAudioRenderClient>  m_renderService;
    WAVEFORMATEX*               m_renderFormat = nullptr;
    UINT32                      m_renderBufferFrames = 0;
    std::wstring                m_renderDeviceId;

    // Endpoint events. The engine signals m_captureEvent when a packet is
    // ready and m_renderEvent when the render buffer wants data; the worker
    // waits on both together with m_stopEvent instead of polling on a timer.
    HANDLE m_captureEvent = nullptr;
    HANDLE m_renderEvent  = nullptr;
    HANDLE m_stopEvent    = nullptr;

    // FIFO between the two endpoint clocks, in capture-format frames. The
    // capture card and the playback device each run on their own clock, so
    // over minutes one side outruns the other. The worker holds the fill
    // near a target by slipping single frames, which is inaudible, instead
    // of letting the render buffer drain (gaps) or overflow (dropped packet
    // tails); both showed up as periodic stutter on USB cards.
    std::vector<BYTE> m_fifo;
    size_t   m_fifoHead      = 0;      // read offset, bytes
    size_t   m_fifoBytes     = 0;      // bytes held
    bool     m_fifoPrimed    = false;  // fill reached the target once
    UINT32   m_bytesPerFrame = 0;
    UINT32   m_samplesPerSec = 0;

    std::atomic<HRESULT>  m_lastCaptureError{S_OK};
    std::atomic<uint32_t> m_fillMs{0};
    std::atomic<uint64_t> m_underrunFrames{0};
    std::atomic<uint64_t> m_overrunFrames{0};
    std::atomic<uint64_t> m_slipCount{0};

    // Per-window counters for the periodic stats line.
    uint64_t m_winIn = 0, m_winOut = 0, m_winUnderrun = 0, m_winOverrun = 0;
    uint64_t m_winSlipDrop = 0, m_winSlipDup = 0;
    uint32_t m_winFillMin = 0xFFFFFFFFu, m_winFillMax = 0;
    std::chrono::steady_clock::time_point m_winStart{};

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_streaming{false};
    std::atomic<float> m_volume{1.0f};
    std::atomic<bool>  m_muted{false};

    // Set from IMMNotificationClient callbacks (a system thread) and consumed
    // by the worker. The endpoint notifications are the fast path; the
    // AUDCLNT_E_DEVICE_INVALIDATED checks in the pump are the backstop that
    // catches everything else, including in-place format changes.
    std::atomic<bool>  m_restartCapture{false};
    std::atomic<bool>  m_restartRender{false};

    // Reports the first attempt's outcome back to Initialize().
    std::mutex              m_startMutex;
    std::condition_variable m_startCv;
    bool                    m_startAttempted = false;
    bool                    m_startSucceeded = false;

    // Suppresses per-retry log spam while an endpoint is unavailable.
    bool m_setupFailureLogged = false;
};

} // namespace NitLink
