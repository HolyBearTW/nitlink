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
