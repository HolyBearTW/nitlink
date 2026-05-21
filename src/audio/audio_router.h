#pragma once

#include <string>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace NitLink {

// Routes audio from the capture card to the system's default audio output.
// Uses WASAPI: one IAudioClient in capture mode reading from the Elgato's
// "Digital Audio Interface" device, another in render mode writing to the
// default playback device. A worker thread bridges them with minimal latency.
class AudioRouter {
public:
    AudioRouter();
    ~AudioRouter();

    // captureDeviceNameHint: substring to match against device friendly name,
    // e.g. L"Elgato" or L"4K Pro". If empty, uses the first non-default
    // microphone-style capture device.
    bool Initialize(const std::wstring& captureDeviceNameHint = L"Elgato");
    void Shutdown();

    void SetVolume(float volume); // 0.0 - 1.0
    void SetMuted(bool muted);

    float GetVolume() const { return m_volume; }
    bool  IsMuted()   const { return m_muted; }
    bool  IsRunning() const { return m_running; }

private:
    bool FindCaptureDevice(const std::wstring& nameHint, ComPtr<IMMDevice>& outDevice);
    bool SetupCapture(IMMDevice* captureDevice);
    bool SetupRender();
    void RouteLoop();

    ComPtr<IMMDeviceEnumerator> m_enumerator;

    ComPtr<IAudioClient>        m_captureClient;
    ComPtr<IAudioCaptureClient> m_captureService;
    WAVEFORMATEX*               m_captureFormat = nullptr;
    UINT32                      m_captureBufferFrames = 0;

    ComPtr<IAudioClient>        m_renderClient;
    ComPtr<IAudioRenderClient>  m_renderService;
    WAVEFORMATEX*               m_renderFormat = nullptr;
    UINT32                      m_renderBufferFrames = 0;

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    std::atomic<float> m_volume{1.0f};
    std::atomic<bool>  m_muted{false};
};

} // namespace NitLink
