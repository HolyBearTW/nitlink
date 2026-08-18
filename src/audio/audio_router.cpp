#include "audio_router.h"
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <mmreg.h>
#include <avrt.h>
#include <debugapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <new>
#include <sstream>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Avrt.lib")

namespace NitLink {

// Reference time helpers (Windows uses 100-ns units)
static constexpr REFERENCE_TIME REFTIMES_PER_SEC      = 10000000;
static constexpr REFERENCE_TIME REFTIMES_PER_MILLISEC = 10000;

// How long to wait before re-attempting an endpoint that failed to open.
static constexpr int kRetryIntervalMs = 500;

// KSDATAFORMAT_SUBTYPE_* live in ksmedia.h, which drags in the whole kernel
// streaming header chain. These two are the only ones the router needs.
static const GUID kSubtypeIeeeFloat =
    {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID kSubtypePcm =
    {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
// Stands in for GUID_NULL, which would pull in a uuid.lib dependency for one
// sentinel value.
static const GUID kSubtypeUnknown = {0,0,0,{0,0,0,0,0,0,0,0}};

static void AudioLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/Audio] " + msg + L"\n").c_str());
}

static std::wstring HrString(HRESULT hr) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << (unsigned long)hr;
    return ss.str();
}

static std::wstring DescribeFormat(const WAVEFORMATEX* fmt) {
    if (!fmt) return L"(none)";
    std::wstringstream ss;
    ss << fmt->nSamplesPerSec << L" Hz, " << fmt->nChannels << L"ch, "
       << fmt->wBitsPerSample << L"-bit";
    return ss.str();
}

// Resolves the effective sample encoding, unwrapping WAVE_FORMAT_EXTENSIBLE.
static GUID EffectiveSubFormat(const WAVEFORMATEX* fmt) {
    if (!fmt) return kSubtypeUnknown;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return kSubtypeIeeeFloat;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM)        return kSubtypePcm;
    return kSubtypeUnknown;
}

static WAVEFORMATEX* CloneWaveFormat(const WAVEFORMATEX* src) {
    if (!src) return nullptr;
    const size_t bytes = sizeof(WAVEFORMATEX) + src->cbSize;
    WAVEFORMATEX* dst = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(bytes));
    if (!dst) return nullptr;
    memcpy(dst, src, bytes);
    return dst;
}

static bool SameWaveFormat(const WAVEFORMATEX* a, const WAVEFORMATEX* b) {
    if (!a || !b) return false;
    if (a->nChannels      != b->nChannels)      return false;
    if (a->nSamplesPerSec != b->nSamplesPerSec) return false;
    if (a->wBitsPerSample != b->wBitsPerSample) return false;
    if (a->nBlockAlign    != b->nBlockAlign)    return false;
    return IsEqualGUID(EffectiveSubFormat(a), EffectiveSubFormat(b)) != 0;
}

// Applies volume in place. Only the two encodings WASAPI actually hands back
// for a shared-mode mix format are handled; anything else plays at unity
// rather than being reinterpreted as the wrong sample type.
static bool ScaleInPlace(BYTE* data, UINT32 frames, const WAVEFORMATEX* fmt, float vol) {
    const GUID sub = EffectiveSubFormat(fmt);
    const size_t sampleCount = (size_t)frames * fmt->nChannels;

    if (IsEqualGUID(sub, kSubtypeIeeeFloat) && fmt->wBitsPerSample == 32) {
        float* s = reinterpret_cast<float*>(data);
        for (size_t i = 0; i < sampleCount; i++) s[i] *= vol;
        return true;
    }
    if (IsEqualGUID(sub, kSubtypePcm) && fmt->wBitsPerSample == 16) {
        int16_t* s = reinterpret_cast<int16_t*>(data);
        for (size_t i = 0; i < sampleCount; i++) {
            s[i] = (int16_t)std::clamp((int)lrintf(s[i] * vol), -32768, 32767);
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Endpoint change notifications
// ---------------------------------------------------------------------------

// Watches for the events that invalidate an open IAudioClient: the default
// playback device changing (headset swap, or a vendor control panel toggling
// its virtual surround endpoint in and out), and a tracked device being
// disabled or unplugged. Callbacks arrive on an MMDevice system thread, so
// they only set flags; the worker does the actual rebuild.
class AudioRouter::EndpointNotifier : public IMMNotificationClient {
public:
    explicit EndpointNotifier(AudioRouter* owner) : m_owner(owner) {}

    // Called before Release so a late callback cannot touch a dead router.
    void Detach() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_owner = nullptr;
    }

    void SetDeviceIds(const std::wstring& captureId, const std::wstring& renderId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_captureId = captureId;
        m_renderId  = renderId;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IMMNotificationClient))) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_owner) {
                m_owner->m_restartRender = true;
                AudioLog(L"Notify: default render endpoint changed");
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD newState) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_owner || !id) return S_OK;

        if (!m_captureId.empty() && m_captureId == id) {
            if (newState != DEVICE_STATE_ACTIVE) {
                m_owner->m_restartCapture = true;
                AudioLog(L"Notify: capture endpoint left the active state");
            }
        } else if (!m_renderId.empty() && m_renderId == id) {
            if (newState != DEVICE_STATE_ACTIVE) {
                m_owner->m_restartRender = true;
                AudioLog(L"Notify: render endpoint left the active state");
            }
        } else if (newState == DEVICE_STATE_ACTIVE && m_captureId.empty()) {
            // A device came back while the capture side is down: worth a
            // re-scan, since this is the card being plugged back in.
            m_owner->m_restartCapture = true;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_owner && m_captureId.empty()) m_owner->m_restartCapture = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_owner || !id) return S_OK;
        if (!m_captureId.empty() && m_captureId == id) m_owner->m_restartCapture = true;
        if (!m_renderId.empty()  && m_renderId  == id) m_owner->m_restartRender  = true;
        return S_OK;
    }

    // Ignored on purpose. An in-place format change (Sound control panel, or a
    // driver reconfiguring its mix format) invalidates the client, and the
    // AUDCLNT_E_DEVICE_INVALIDATED check in the pump catches that within one
    // poll interval without needing to filter property keys here.
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    LONG         m_ref = 1;
    std::mutex   m_mutex;
    AudioRouter* m_owner = nullptr;
    std::wstring m_captureId;
    std::wstring m_renderId;
};

// ---------------------------------------------------------------------------
// AudioRouter
// ---------------------------------------------------------------------------

AudioRouter::AudioRouter() = default;

AudioRouter::~AudioRouter()
{
    Shutdown();
}

bool AudioRouter::Initialize(const std::wstring& nameHint)
{
    AudioLog(L"Initialize: begin");

    // Re-initialising over a live router is legal (SwitchCaptureDevice does
    // exactly that); make sure the previous worker is gone first.
    Shutdown();

    m_nameHint = nameHint.empty() ? std::wstring(L"Elgato") : nameHint;
    m_setupFailureLogged = false;
    {
        std::lock_guard<std::mutex> lock(m_startMutex);
        m_startAttempted = false;
        m_startSucceeded = false;
    }

    m_running = true;
    m_thread = std::thread(&AudioRouter::RouteLoop, this);

    // Wait for the worker's first open attempt so callers keep the original
    // "Initialize() reports whether audio came up" contract. Either way the
    // worker stays alive and keeps retrying in the background.
    std::unique_lock<std::mutex> lock(m_startMutex);
    m_startCv.wait_for(lock, std::chrono::seconds(3),
                       [this] { return m_startAttempted; });
    return m_startSucceeded;
}

void AudioRouter::Shutdown()
{
    const bool wasRunning = m_running.exchange(false);
    if (m_thread.joinable()) {
        if (wasRunning) AudioLog(L"Shutdown: stopping");
        m_thread.join();
    }
    m_streaming = false;
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

    return false;
}

bool AudioRouter::SetupCapture()
{
    ComPtr<IMMDevice> captureDevice;
    if (!FindCaptureDevice(m_nameHint, captureDevice)) return false;

    LPWSTR id = nullptr;
    if (SUCCEEDED(captureDevice->GetId(&id)) && id) {
        m_captureDeviceId = id;
        CoTaskMemFree(id);
    }

    HRESULT hr = captureDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        (void**)m_captureClient.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { AudioLog(L"capture Activate failed " + HrString(hr)); return false; }

    hr = m_captureClient->GetMixFormat(&m_captureFormat);
    if (FAILED(hr)) { AudioLog(L"capture GetMixFormat failed " + HrString(hr)); return false; }

    // Initialize with shared mode, 100ms buffer for safety
    hr = m_captureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0, // No event-driven (poll instead -- simpler)
        100 * REFTIMES_PER_MILLISEC,
        0,
        m_captureFormat,
        nullptr);
    if (FAILED(hr)) { AudioLog(L"capture Initialize failed " + HrString(hr)); return false; }

    hr = m_captureClient->GetBufferSize(&m_captureBufferFrames);
    if (FAILED(hr)) return false;

    hr = m_captureClient->GetService(IID_PPV_ARGS(&m_captureService));
    if (FAILED(hr)) return false;

    hr = m_captureClient->Start();
    if (FAILED(hr)) { AudioLog(L"captureClient->Start failed " + HrString(hr)); return false; }

    AudioLog(L"Capture format: " + DescribeFormat(m_captureFormat));
    return true;
}

bool AudioRouter::SetupRender()
{
    ComPtr<IMMDevice> renderDevice;
    HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice);
    if (FAILED(hr)) { AudioLog(L"GetDefaultAudioEndpoint failed " + HrString(hr)); return false; }

    LPWSTR id = nullptr;
    if (SUCCEEDED(renderDevice->GetId(&id)) && id) {
        m_renderDeviceId = id;
        CoTaskMemFree(id);
    }

    // Open the render side in the CAPTURE format and let the audio engine do
    // the channel matrix and sample-rate conversion.
    //
    // The endpoint's own mix format is not usable as-is: a headset running a
    // virtual surround driver reports 8 channels while the card's capture
    // endpoint is 2, and the two rates need not agree either. Copying between
    // mismatched formats is what produced the smeared, hollow audio this
    // replaces -- it read past the end of the capture buffer and scattered
    // stereo samples across eight channels. AUTOCONVERTPCM hands that problem
    // to the engine's own resampler, which is both correct and better than a
    // hand-rolled one.
    const DWORD kConvertFlags =
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = renderDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        (void**)m_renderClient.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { AudioLog(L"render Activate failed " + HrString(hr)); return false; }

    hr = m_renderClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        kConvertFlags,
        100 * REFTIMES_PER_MILLISEC,
        0,
        m_captureFormat,
        nullptr);

    if (SUCCEEDED(hr)) {
        m_renderFormat = CloneWaveFormat(m_captureFormat);
        if (!m_renderFormat) return false;
    } else {
        // A failed Initialize leaves the client unusable, so the fallback needs
        // a fresh one rather than a second Initialize on the same object.
        AudioLog(L"render Initialize with format conversion failed " + HrString(hr) +
                 L"; falling back to the endpoint mix format");

        hr = renderDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            (void**)m_renderClient.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;

        hr = m_renderClient->GetMixFormat(&m_renderFormat);
        if (FAILED(hr)) { AudioLog(L"render GetMixFormat failed " + HrString(hr)); return false; }

        // Without the engine converting, the only safe case is formats that
        // already agree. Refusing here is deliberate: emitting a mismatched
        // copy is a buffer over-read, and it sounds broken anyway.
        if (!SameWaveFormat(m_captureFormat, m_renderFormat)) {
            AudioLog(L"Cannot route audio: capture is " + DescribeFormat(m_captureFormat) +
                     L" but render endpoint is " + DescribeFormat(m_renderFormat) +
                     L" and this system rejected format conversion");
            return false;
        }

        hr = m_renderClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED, 0, 100 * REFTIMES_PER_MILLISEC, 0, m_renderFormat, nullptr);
        if (FAILED(hr)) { AudioLog(L"render Initialize failed " + HrString(hr)); return false; }
    }

    hr = m_renderClient->GetBufferSize(&m_renderBufferFrames);
    if (FAILED(hr)) return false;

    hr = m_renderClient->GetService(IID_PPV_ARGS(&m_renderService));
    if (FAILED(hr)) return false;

    hr = m_renderClient->Start();
    if (FAILED(hr)) { AudioLog(L"renderClient->Start failed " + HrString(hr)); return false; }

    AudioLog(L"Render format: " + DescribeFormat(m_renderFormat) +
             L" (endpoint conversion handled by the audio engine)");
    return true;
}

void AudioRouter::TeardownCapture()
{
    if (m_captureClient) m_captureClient->Stop();
    m_captureService.Reset();
    m_captureClient.Reset();
    if (m_captureFormat) { CoTaskMemFree(m_captureFormat); m_captureFormat = nullptr; }
    m_captureBufferFrames = 0;
    m_captureDeviceId.clear();
}

void AudioRouter::TeardownRender()
{
    if (m_renderClient) m_renderClient->Stop();
    m_renderService.Reset();
    m_renderClient.Reset();
    if (m_renderFormat) { CoTaskMemFree(m_renderFormat); m_renderFormat = nullptr; }
    m_renderBufferFrames = 0;
    m_renderDeviceId.clear();
}

bool AudioRouter::EnsureEndpoints()
{
    if (!m_enumerator) {
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator));
        if (FAILED(hr)) {
            if (!m_setupFailureLogged) {
                AudioLog(L"failed to create device enumerator " + HrString(hr));
                m_setupFailureLogged = true;
            }
            return false;
        }

        auto* notifier = new (std::nothrow) EndpointNotifier(this);
        if (notifier) {
            if (SUCCEEDED(m_enumerator->RegisterEndpointNotificationCallback(notifier))) {
                m_notifier.Attach(notifier); // takes the constructor's reference
            } else {
                notifier->Release();
                AudioLog(L"endpoint notifications unavailable; relying on stream errors");
            }
        }
    }

    // The render client is opened against the capture format, so a capture
    // rebuild forces a render rebuild too.
    if (m_restartCapture.exchange(false)) {
        TeardownCapture();
        TeardownRender();
    }
    if (m_restartRender.exchange(false)) {
        TeardownRender();
    }

    if (!m_captureService) {
        TeardownRender();
        if (!SetupCapture()) {
            TeardownCapture();
            if (!m_setupFailureLogged) {
                AudioLog(L"capture endpoint unavailable; retrying");
                m_setupFailureLogged = true;
            }
            return false;
        }
    }

    if (!m_renderService) {
        if (!SetupRender()) {
            TeardownRender();
            if (!m_setupFailureLogged) {
                AudioLog(L"render endpoint unavailable; retrying");
                m_setupFailureLogged = true;
            }
            return false;
        }
    }

    if (m_notifier) {
        static_cast<EndpointNotifier*>(m_notifier.Get())
            ->SetDeviceIds(m_captureDeviceId, m_renderDeviceId);
    }

    if (m_setupFailureLogged) {
        AudioLog(L"endpoints recovered; routing resumed");
        m_setupFailureLogged = false;
    }
    return true;
}

void AudioRouter::HandleStreamError(HRESULT hr, const wchar_t* what, bool captureSide)
{
    // Every one of these means the client is dead and has to be rebuilt.
    // Previously they were swallowed, which is why a headset change or a
    // device-format edit silenced the app for the rest of the run.
    const bool fatal = (hr == AUDCLNT_E_DEVICE_INVALIDATED) ||
                       (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) ||
                       (hr == AUDCLNT_E_NOT_INITIALIZED) ||
                       (hr == AUDCLNT_E_RESOURCES_INVALIDATED);

    AudioLog(std::wstring(what) + L" failed " + HrString(hr) +
             (fatal ? L"; rebuilding endpoint" : L""));

    if (!fatal) return;
    if (captureSide) m_restartCapture = true;
    else             m_restartRender = true;
}

void AudioRouter::RouteLoop()
{
    // The endpoints are created and used entirely on this thread, inside the
    // MTA. main.cpp puts the UI thread in an STA because WebView2 requires it;
    // building the WASAPI clients there and calling them from here would be a
    // cross-apartment call on interfaces that were never marshalled.
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCom)) {
        AudioLog(L"CoInitializeEx failed " + HrString(hrCom));
        {
            std::lock_guard<std::mutex> lock(m_startMutex);
            m_startAttempted = true;
            m_startSucceeded = false;
        }
        m_startCv.notify_all();
        return;
    }

    // Boost thread priority for low-latency audio. The Windows pro-audio
    // task is the standard for this kind of work; it moves the thread out of
    // regular scheduling and onto the audio-thread cohort.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    bool firstAttemptReported = false;

    while (m_running) {
        const bool ready = EnsureEndpoints();

        if (!firstAttemptReported) {
            {
                std::lock_guard<std::mutex> lock(m_startMutex);
                m_startAttempted = true;
                m_startSucceeded = ready;
            }
            m_startCv.notify_all();
            firstAttemptReported = true;
        }

        if (!ready) {
            m_streaming = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            continue;
        }
        m_streaming = true;

        // Recomputed per pass: a rebuilt endpoint can come back at a different
        // rate or buffer size than the one it replaced.
        const UINT32 bytesPerFrame = m_renderFormat->nBlockAlign;
        const DWORD pollIntervalMs = std::max<DWORD>(1,
            (DWORD)((m_captureBufferFrames * 1000.0 / m_captureFormat->nSamplesPerSec) / 4));

        UINT32 packetLength = 0;
        HRESULT hr = m_captureService->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            HandleStreamError(hr, L"GetNextPacketSize", true);
            std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
            continue;
        }

        while (packetLength != 0 && m_running) {
            BYTE* captureData = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;

            hr = m_captureService->GetBuffer(&captureData, &framesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) { HandleStreamError(hr, L"capture GetBuffer", true); break; }

            // How many frames can be written to the render side right now?
            UINT32 padding = 0;
            hr = m_renderClient->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                m_captureService->ReleaseBuffer(framesAvailable);
                HandleStreamError(hr, L"GetCurrentPadding", false);
                break;
            }

            const UINT32 framesFree = (m_renderBufferFrames > padding)
                                    ? (m_renderBufferFrames - padding) : 0;
            const UINT32 framesToWrite = (std::min)(framesAvailable, framesFree);

            if (framesToWrite > 0) {
                BYTE* renderData = nullptr;
                hr = m_renderService->GetBuffer(framesToWrite, &renderData);
                if (FAILED(hr)) {
                    m_captureService->ReleaseBuffer(framesAvailable);
                    HandleStreamError(hr, L"render GetBuffer", false);
                    break;
                }

                // Capture and render now share one format -- either because the
                // engine is converting for us, or because the fallback verified
                // they already agree -- so this is a straight frame-for-frame
                // copy sized by the shared block alignment.
                const size_t bytes = (size_t)framesToWrite * bytesPerFrame;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || m_muted.load()) {
                    memset(renderData, 0, bytes);
                } else {
                    memcpy(renderData, captureData, bytes);
                    const float vol = m_volume.load();
                    if (vol < 0.999f) {
                        ScaleInPlace(renderData, framesToWrite, m_renderFormat, vol);
                    }
                }
                m_renderService->ReleaseBuffer(framesToWrite, 0);
            }

            m_captureService->ReleaseBuffer(framesAvailable);

            hr = m_captureService->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) { HandleStreamError(hr, L"GetNextPacketSize", true); break; }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);

    m_streaming = false;

    // Tear down on the thread that built everything, before the apartment goes
    // away, so no interface is released from a foreign apartment.
    if (m_notifier) {
        auto* notifier = static_cast<EndpointNotifier*>(m_notifier.Get());
        if (m_enumerator) m_enumerator->UnregisterEndpointNotificationCallback(notifier);
        notifier->Detach();
        m_notifier.Reset();
    }
    TeardownCapture();
    TeardownRender();
    m_enumerator.Reset();

    CoUninitialize();
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
