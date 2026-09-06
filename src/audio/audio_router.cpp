#include "audio_router.h"
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <avrt.h>
#include <debugapi.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Avrt.lib")

namespace NitLink {

// Reference time helpers (Windows uses 100-ns units)
static constexpr REFERENCE_TIME REFTIMES_PER_SEC      = 10000000;
static constexpr REFERENCE_TIME REFTIMES_PER_MILLISEC = 10000;

static void AudioLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/Audio] " + msg + L"\n").c_str());
}

// True when a WASAPI mix format carries IEEE float samples, either as a plain
// WAVE_FORMAT_IEEE_FLOAT tag or a WAVE_FORMAT_EXTENSIBLE whose SubFormat is IEEE
// float. The capture to render fast path reinterprets sample bytes as float, so
// this gates that cast.
static bool IsFloatFormat(const WAVEFORMATEX* wf) {
    if (!wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

AudioRouter::AudioRouter() = default;

AudioRouter::~AudioRouter()
{
    Shutdown();
}

bool AudioRouter::Initialize(const std::wstring& nameHint)
{
    AudioLog(L"Initialize: begin");

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&m_enumerator));
    if (FAILED(hr)) {
        AudioLog(L"Initialize: failed to create device enumerator");
        return false;
    }

    ComPtr<IMMDevice> captureDevice;
    if (!FindCaptureDevice(nameHint, captureDevice)) {
        AudioLog(L"Initialize: capture device not found");
        return false;
    }

    if (!SetupCapture(captureDevice.Get())) {
        AudioLog(L"Initialize: SetupCapture failed");
        return false;
    }

    if (!SetupRender()) {
        AudioLog(L"Initialize: SetupRender failed");
        return false;
    }

    // Decide how the worker bridges the two formats now that both are known,
    // before the worker thread starts (m_routeMode is read on that thread).
    ChooseRouteMode();

    // Start streaming
    hr = m_captureClient->Start();
    if (FAILED(hr)) { AudioLog(L"captureClient->Start failed"); return false; }
    hr = m_renderClient->Start();
    if (FAILED(hr)) { AudioLog(L"renderClient->Start failed"); return false; }

    m_running = true;
    m_thread = std::thread(&AudioRouter::RouteLoop, this);

    AudioLog(L"Initialize: streaming started");
    return true;
}

bool AudioRouter::FindCaptureDevice(const std::wstring& nameHint, ComPtr<IMMDevice>& outDevice)
{
    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = m_enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return false;

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        ComPtr<IPropertyStore> props;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) continue;

        PROPVARIANT name;
        PropVariantInit(&name);
        props->GetValue(PKEY_Device_FriendlyName, &name);

        std::wstring friendlyName = (name.vt == VT_LPWSTR && name.pwszVal) ? name.pwszVal : L"";
        PropVariantClear(&name);

        {
            std::wstringstream ss;
            ss << L"Found capture device: " << friendlyName;
            AudioLog(ss.str());
        }

        // Match against hint (case-insensitive substring)
        if (!nameHint.empty()) {
            std::wstring lowerName = friendlyName;
            std::wstring lowerHint = nameHint;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);
            std::transform(lowerHint.begin(), lowerHint.end(), lowerHint.begin(), ::towlower);
            if (lowerName.find(lowerHint) != std::wstring::npos) {
                outDevice = device;
                AudioLog(L"Matched capture device: " + friendlyName);
                return true;
            }
        }
    }

    AudioLog(L"No matching capture device found");
    return false;
}

bool AudioRouter::SetupCapture(IMMDevice* captureDevice)
{
    HRESULT hr = captureDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)m_captureClient.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_captureClient->GetMixFormat(&m_captureFormat);
    if (FAILED(hr)) return false;

    // Initialize with shared mode, 100ms buffer for safety
    hr = m_captureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0, // No event-driven (poll instead -- simpler)
        100 * REFTIMES_PER_MILLISEC,
        0,
        m_captureFormat,
        nullptr);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"capture Initialize failed 0x" << std::hex << hr;
        AudioLog(ss.str());
        return false;
    }

    hr = m_captureClient->GetBufferSize(&m_captureBufferFrames);
    if (FAILED(hr)) return false;

    hr = m_captureClient->GetService(IID_PPV_ARGS(&m_captureService));
    if (FAILED(hr)) return false;

    {
        std::wstringstream ss;
        ss << L"Capture format: " << m_captureFormat->nSamplesPerSec << L" Hz, "
           << m_captureFormat->nChannels << L"ch, "
           << m_captureFormat->wBitsPerSample << L"-bit";
        AudioLog(ss.str());
    }

    return true;
}

bool AudioRouter::SetupRender()
{
    ComPtr<IMMDevice> renderDevice;
    HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice);
    if (FAILED(hr)) return false;

    hr = renderDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)m_renderClient.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_renderClient->GetMixFormat(&m_renderFormat);
    if (FAILED(hr)) return false;

    // Use 100ms buffer
    hr = m_renderClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        100 * REFTIMES_PER_MILLISEC,
        0,
        m_renderFormat,
        nullptr);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"render Initialize failed 0x" << std::hex << hr;
        AudioLog(ss.str());
        return false;
    }

    hr = m_renderClient->GetBufferSize(&m_renderBufferFrames);
    if (FAILED(hr)) return false;

    hr = m_renderClient->GetService(IID_PPV_ARGS(&m_renderService));
    if (FAILED(hr)) return false;

    {
        std::wstringstream ss;
        ss << L"Render format: " << m_renderFormat->nSamplesPerSec << L" Hz, "
           << m_renderFormat->nChannels << L"ch, "
           << m_renderFormat->wBitsPerSample << L"-bit";
        AudioLog(ss.str());
    }

    return true;
}

void AudioRouter::ChooseRouteMode()
{
    m_routeMode     = RouteMode::Silence;
    m_formatWarning = false;
    m_formatWarningText.clear();

    if (!m_captureFormat || !m_renderFormat) {
        m_formatWarning     = true;
        m_formatWarningText = L"NitLink could not read the audio device formats, so game "
                              L"audio is muted. Video is unaffected.";
        AudioLog(L"Route mode: missing format, muting audio");
        return;
    }

    const bool sameRate  = m_captureFormat->nSamplesPerSec == m_renderFormat->nSamplesPerSec;
    const bool bothFloat = IsFloatFormat(m_captureFormat) && IsFloatFormat(m_renderFormat);
    const WORD capCh     = m_captureFormat->nChannels;
    const WORD renCh     = m_renderFormat->nChannels;

    // Identical formats: copy straight through.
    if (sameRate && bothFloat &&
        m_captureFormat->wBitsPerSample == m_renderFormat->wBitsPerSample &&
        capCh == renCh) {
        m_routeMode = RouteMode::DirectCopy;
        AudioLog(L"Route mode: direct copy (matching formats)");
        return;
    }

    // Stereo float capture into a 5.1/7.1 float playback device at the same
    // rate: map L/R onto the front pair, zero the rest. This is the common
    // surround case and stays a safe, channel-bounded copy.
    if (sameRate && bothFloat && capCh == 2 && (renCh == 6 || renCh == 8)) {
        m_routeMode = RouteMode::StereoToSurround;
        AudioLog(L"Route mode: stereo capture mapped to surround front channels");
        return;
    }

    // Anything else (different sample rate, non-float, or a channel layout this
    // path does not map) would require real resampling / a channel matrix.
    // Mute and surface a one-time explanation rather than guess or over-read.
    m_routeMode     = RouteMode::Silence;
    m_formatWarning = true;
    std::wstringstream ss;
    ss << L"NitLink can't route this audio combination yet, so game audio is muted.\n\n"
       << L"Capture device: " << m_captureFormat->nSamplesPerSec << L" Hz, " << capCh << L" ch\n"
       << L"Playback device: " << m_renderFormat->nSamplesPerSec << L" Hz, " << renCh << L" ch\n\n"
       << L"Routing works when both devices use 32-bit float at the same sample rate and the "
       << L"playback device is stereo or 5.1/7.1 surround.\n\n"
       << L"To fix: open Windows Sound settings and set your default playback device to a stereo "
       << L"format at the capture device's sample rate, or send game audio through your TV or "
       << L"receiver directly. Video is unaffected.";
    m_formatWarningText = ss.str();
    AudioLog(L"Route mode: unsupported format combination, muting audio");
}

void AudioRouter::RouteLoop()
{
    // Boost thread priority for low-latency audio. The Windows pro-audio
    // task is the standard for this kind of work; it moves the thread out of
    // regular scheduling and onto the audio-thread cohort.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const float bytesPerCaptureSample = (float)(m_captureFormat->wBitsPerSample / 8) * m_captureFormat->nChannels;
    const float bytesPerRenderSample  = (float)(m_renderFormat->wBitsPerSample / 8)  * m_renderFormat->nChannels;

    // Sleep duration: 1/4 of capture buffer in ms. Polling at this rate keeps
    // latency low without burning CPU.
    DWORD pollIntervalMs = std::max<DWORD>(1,
        (DWORD)((m_captureBufferFrames * 1000.0 / m_captureFormat->nSamplesPerSec) / 4));

    while (m_running) {
        UINT32 packetLength = 0;
        HRESULT hr = m_captureService->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
            continue;
        }

        while (packetLength != 0 && m_running) {
            BYTE* captureData = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;

            hr = m_captureService->GetBuffer(&captureData, &framesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            // How many frames can be written to the render side right now?
            UINT32 padding = 0;
            m_renderClient->GetCurrentPadding(&padding);
            UINT32 framesFree = m_renderBufferFrames - padding;
            UINT32 framesToWrite = std::min(framesAvailable, framesFree);

            if (framesToWrite > 0) {
                BYTE* renderData = nullptr;
                hr = m_renderService->GetBuffer(framesToWrite, &renderData);
                if (SUCCEEDED(hr) && renderData) {
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || m_muted.load()
                            || m_routeMode == RouteMode::Silence) {
                        memset(renderData, 0, framesToWrite * (size_t)bytesPerRenderSample);
                    } else if (m_routeMode == RouteMode::StereoToSurround) {
                        // Stereo capture to a 5.1/7.1 playback device: place L and R on
                        // the front-left and front-right channels (indices 0 and 1 in
                        // the standard WAVEFORMATEXTENSIBLE channel order) and zero the
                        // rest. Reads exactly two floats per frame from the capture
                        // packet, so it cannot over-read regardless of render channels.
                        const float vol = m_volume.load();
                        const float* src = reinterpret_cast<const float*>(captureData);
                        float*       dst = reinterpret_cast<float*>(renderData);
                        const uint32_t rc = m_renderFormat->nChannels;
                        for (UINT32 frame = 0; frame < framesToWrite; ++frame) {
                            const float l = src[(size_t)frame * 2 + 0] * vol;
                            const float r = src[(size_t)frame * 2 + 1] * vol;
                            float* out = dst + (size_t)frame * rc;
                            out[0] = l;
                            out[1] = r;
                            for (uint32_t c = 2; c < rc; ++c) out[c] = 0.0f;
                        }
                    } else {
                        // Both sides are likely 32-bit float (WASAPI default mix format).
                        // Copy with volume scaling. Matching format is assumed here for
                        // simplicity; if the formats differ, this would need an SRC
                        // and channel matrix mixer, which is deferred to v0.3.
                        float vol = m_volume.load();
                        const float* src = reinterpret_cast<const float*>(captureData);
                        float* dst       = reinterpret_cast<float*>(renderData);
                        size_t sampleCount = (size_t)framesToWrite * m_renderFormat->nChannels;
                        if (vol >= 0.999f) {
                            memcpy(dst, src, sampleCount * sizeof(float));
                        } else {
                            for (size_t i = 0; i < sampleCount; i++) {
                                dst[i] = src[i] * vol;
                            }
                        }
                    }
                    m_renderService->ReleaseBuffer(framesToWrite, 0);
                }
            }

            m_captureService->ReleaseBuffer(framesAvailable);
            m_captureService->GetNextPacketSize(&packetLength);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void AudioRouter::Shutdown()
{
    // Stop the running pipeline only if it actually started. The thread is
    // created together with m_running=true, so gating the join here is safe.
    if (m_running) {
        AudioLog(L"Shutdown: stopping");
        m_running = false;
        if (m_thread.joinable()) m_thread.join();

        if (m_captureClient) m_captureClient->Stop();
        if (m_renderClient)  m_renderClient->Stop();
    }

    // Free owned resources unconditionally. A failed Initialize() can leave
    // m_captureFormat/m_renderFormat allocated (GetMixFormat already ran) while
    // m_running stayed false, so gating these on m_running leaks the formats.
    // CoTaskMemFree(nullptr) and ComPtr::Reset() on empty are both no-ops, so
    // this stays idempotent across repeated Shutdown() / destructor calls.
    if (m_captureFormat) { CoTaskMemFree(m_captureFormat); m_captureFormat = nullptr; }
    if (m_renderFormat)  { CoTaskMemFree(m_renderFormat);  m_renderFormat  = nullptr; }

    m_captureService.Reset();
    m_captureClient.Reset();
    m_renderService.Reset();
    m_renderClient.Reset();
    m_enumerator.Reset();
}

void AudioRouter::SetVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

void AudioRouter::SetMuted(bool muted)
{
    m_muted = muted;
}

} // namespace NitLink
