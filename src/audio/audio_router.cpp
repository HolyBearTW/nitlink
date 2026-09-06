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
#include <cstring>
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

// Pump geometry. kFifoCapacityMs bounds how far capture may run ahead before
// the oldest audio is discarded; kFifoTargetMs is the fill the drift control
// steers toward and kFifoDeadbandMs the band it leaves alone. kRenderQueueMs
// is how much audio stays queued inside the render endpoint: enough to ride
// out scheduling jitter, small enough not to add noticeable latency.
static constexpr int   kFifoCapacityMs  = 400;
static constexpr int   kFifoTargetMs    = 40;
static constexpr int   kFifoDeadbandMs  = 10;
static constexpr int   kRenderQueueMs   = 30;
static constexpr int   kStatsIntervalMs = 5000;
static constexpr DWORD kPumpWaitMs      = 200;

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

AudioRouter::AudioRouter()
{
    m_captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_renderEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_stopEvent    = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
}

AudioRouter::~AudioRouter()
{
    Shutdown();
    if (m_captureEvent) CloseHandle(m_captureEvent);
    if (m_renderEvent)  CloseHandle(m_renderEvent);
    if (m_stopEvent)    CloseHandle(m_stopEvent);
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

    m_lastCaptureError = S_OK;
    if (m_stopEvent) ResetEvent(m_stopEvent);
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
    if (m_stopEvent) SetEvent(m_stopEvent);
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
    if (FAILED(hr)) { AudioLog(L"capture Activate failed " + HrString(hr)); m_lastCaptureError = hr; return false; }

    hr = m_captureClient->GetMixFormat(&m_captureFormat);
    if (FAILED(hr)) { AudioLog(L"capture GetMixFormat failed " + HrString(hr)); return false; }

    // Initialize with shared mode, 100ms buffer for safety
    hr = m_captureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        100 * REFTIMES_PER_MILLISEC,
        0,
        m_captureFormat,
        nullptr);
    if (FAILED(hr)) { AudioLog(L"capture Initialize failed " + HrString(hr)); m_lastCaptureError = hr; return false; }
    hr = m_captureClient->SetEventHandle(m_captureEvent);
    if (FAILED(hr)) { AudioLog(L"capture SetEventHandle failed " + HrString(hr)); return false; }
    FifoReset(m_captureFormat->nBlockAlign, m_captureFormat->nSamplesPerSec);

    hr = m_captureClient->GetBufferSize(&m_captureBufferFrames);
    if (FAILED(hr)) return false;

    hr = m_captureClient->GetService(IID_PPV_ARGS(&m_captureService));
    if (FAILED(hr)) return false;

    hr = m_captureClient->Start();
    if (FAILED(hr)) { AudioLog(L"captureClient->Start failed " + HrString(hr)); return false; }

    m_lastCaptureError = S_OK;
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
        kConvertFlags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
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
            AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            100 * REFTIMES_PER_MILLISEC, 0, m_renderFormat, nullptr);
        if (FAILED(hr)) { AudioLog(L"render Initialize failed " + HrString(hr)); return false; }
    }

    hr = m_renderClient->SetEventHandle(m_renderEvent);
    if (FAILED(hr)) { AudioLog(L"render SetEventHandle failed " + HrString(hr)); return false; }
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

        // Sleep until either endpoint has something to do, or the stop event
        // fires. Both sides are serviced on every wake, whichever event set
        // it: capture may have queued more than one packet, and the render
        // side is topped up from the FIFO regardless of which clock ticked.
        HANDLE waits[3] = { m_stopEvent, m_captureEvent, m_renderEvent };
        const DWORD wake = WaitForMultipleObjects(3, waits, FALSE, kPumpWaitMs);
        if (wake == WAIT_OBJECT_0) break;
        if (wake == WAIT_FAILED) {
            AudioLog(L"WaitForMultipleObjects failed; retrying");
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            continue;
        }
        if (!DrainCapture()) continue;
        if (!FillRender())   continue;
        LogStatsIfDue();
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

void AudioRouter::FifoReset(UINT32 bytesPerFrame, UINT32 samplesPerSec)
{
    m_bytesPerFrame = bytesPerFrame;
    m_samplesPerSec = samplesPerSec;
    const size_t frames = (size_t)samplesPerSec * kFifoCapacityMs / 1000;
    m_fifo.assign(frames * bytesPerFrame, 0);
    m_fifoHead   = 0;
    m_fifoBytes  = 0;
    m_fifoPrimed = false;
    m_winStart   = std::chrono::steady_clock::now();
    m_winIn = m_winOut = m_winUnderrun = m_winOverrun = m_winSlipDrop = m_winSlipDup = 0;
    m_winFillMin = 0xFFFFFFFFu;
    m_winFillMax = 0;
}

void AudioRouter::FifoPush(const BYTE* data, UINT32 frames, bool silent)
{
    if (m_fifo.empty() || m_bytesPerFrame == 0) return;
    size_t bytes = (size_t)frames * m_bytesPerFrame;
    const size_t cap = m_fifo.size();
    if (bytes > cap) {
        data  += (bytes - cap);
        bytes  = cap;
    }
    const size_t free = cap - m_fifoBytes;
    if (bytes > free) {
        // Capture is ahead of playback and the FIFO is full: discard the
        // oldest audio so latency stays bounded. Counted as overrun.
        const size_t drop = bytes - free;
        m_fifoHead   = (m_fifoHead + drop) % cap;
        m_fifoBytes -= drop;
        m_winOverrun    += drop / m_bytesPerFrame;
        m_overrunFrames += drop / m_bytesPerFrame;
    }
    const size_t tail  = (m_fifoHead + m_fifoBytes) % cap;
    const size_t first = (std::min)(bytes, cap - tail);
    if (silent) {
        memset(m_fifo.data() + tail, 0, first);
        if (bytes > first) memset(m_fifo.data(), 0, bytes - first);
    } else {
        memcpy(m_fifo.data() + tail, data, first);
        if (bytes > first) memcpy(m_fifo.data(), data + first, bytes - first);
    }
    m_fifoBytes += bytes;
}

UINT32 AudioRouter::FifoPop(BYTE* out, UINT32 frames)
{
    if (m_fifo.empty() || m_bytesPerFrame == 0) return 0;
    size_t bytes = (std::min)((size_t)frames * m_bytesPerFrame, m_fifoBytes);
    bytes -= bytes % m_bytesPerFrame;
    const size_t cap   = m_fifo.size();
    const size_t first = (std::min)(bytes, cap - m_fifoHead);
    memcpy(out, m_fifo.data() + m_fifoHead, first);
    if (bytes > first) memcpy(out + first, m_fifo.data(), bytes - first);
    m_fifoHead   = (m_fifoHead + bytes) % cap;
    m_fifoBytes -= bytes;
    return (UINT32)(bytes / m_bytesPerFrame);
}

void AudioRouter::FifoSkip(UINT32 frames)
{
    if (m_fifo.empty() || m_bytesPerFrame == 0) return;
    const size_t bytes = (std::min)((size_t)frames * m_bytesPerFrame, m_fifoBytes);
    m_fifoHead   = (m_fifoHead + bytes) % m_fifo.size();
    m_fifoBytes -= bytes;
}

bool AudioRouter::DrainCapture()
{
    UINT32 packetLength = 0;
    HRESULT hr = m_captureService->GetNextPacketSize(&packetLength);
    if (FAILED(hr)) { HandleStreamError(hr, L"GetNextPacketSize", true); return false; }
    while (packetLength != 0 && m_running) {
        BYTE*  data   = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        hr = m_captureService->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) { HandleStreamError(hr, L"capture GetBuffer", true); return false; }
        FifoPush(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
        m_winIn += frames;
        hr = m_captureService->ReleaseBuffer(frames);
        if (FAILED(hr)) { HandleStreamError(hr, L"capture ReleaseBuffer", true); return false; }
        hr = m_captureService->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) { HandleStreamError(hr, L"GetNextPacketSize", true); return false; }
    }
    return true;
}

bool AudioRouter::FillRender()
{
    if (m_bytesPerFrame == 0 || m_samplesPerSec == 0) return true;
    UINT32 padding = 0;
    HRESULT hr = m_renderClient->GetCurrentPadding(&padding);
    if (FAILED(hr)) { HandleStreamError(hr, L"GetCurrentPadding", false); return false; }

    const auto msToFrames = [this](int ms) {
        return (UINT32)((uint64_t)m_samplesPerSec * (uint64_t)ms / 1000ull);
    };
    const UINT32 queueTarget = (std::min)(m_renderBufferFrames, msToFrames(kRenderQueueMs));
    if (padding >= queueTarget) return true;
    const UINT32 want = queueTarget - padding;

    UINT32 fill = (UINT32)(m_fifoBytes / m_bytesPerFrame);
    const uint32_t fillMs = (uint32_t)((uint64_t)fill * 1000ull / m_samplesPerSec);
    m_fillMs = fillMs;
    const UINT32 target = msToFrames(kFifoTargetMs);
    const UINT32 band   = msToFrames(kFifoDeadbandMs);

    // Priming: play silence until the FIFO holds the target once, so steady
    // state starts with headroom instead of climbing to it one slip at a time.
    if (!m_fifoPrimed) {
        if (fill < target) {
            BYTE* out = nullptr;
            hr = m_renderService->GetBuffer(want, &out);
            if (FAILED(hr)) { HandleStreamError(hr, L"render GetBuffer", false); return false; }
            m_renderService->ReleaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT);
            return true;
        }
        m_fifoPrimed = true;
    }
    m_winFillMin = (std::min)(m_winFillMin, fillMs);
    m_winFillMax = (std::max)(m_winFillMax, fillMs);

    // Drift control, decided on the fill before this wake consumes anything.
    // Above the band: drop one frame this wake. Below it: repeat one frame.
    // One frame per wake is far more correction than any real clock offset
    // needs, so the fill settles inside the band and the slips stop.
    bool dup = false;
    if (fill > target + band && fill > 1) {
        FifoSkip(1);
        fill--;
        m_winSlipDrop++;
        m_slipCount++;
    } else if (fill > 0 && fill + band < target) {
        dup = true;
    }

    const UINT32 toWrite = (std::min)(want, fill + (dup ? 1u : 0u));
    if (toWrite == 0) {
        // Nothing buffered: keep the engine fed with silence so the stream
        // never stops, and count the gap.
        BYTE* out = nullptr;
        hr = m_renderService->GetBuffer(want, &out);
        if (FAILED(hr)) { HandleStreamError(hr, L"render GetBuffer", false); return false; }
        m_renderService->ReleaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT);
        m_winUnderrun   += want;
        m_underrunFrames += want;
        return true;
    }
    BYTE* out = nullptr;
    hr = m_renderService->GetBuffer(toWrite, &out);
    if (FAILED(hr)) { HandleStreamError(hr, L"render GetBuffer", false); return false; }
    UINT32 written = FifoPop(out, (std::min)(toWrite, fill));
    if (dup && written > 0 && written < toWrite) {
        memcpy(out + (size_t)written * m_bytesPerFrame,
               out + (size_t)(written - 1) * m_bytesPerFrame, m_bytesPerFrame);
        written++;
        m_winSlipDup++;
        m_slipCount++;
    }
    if (written < toWrite) {
        memset(out + (size_t)written * m_bytesPerFrame, 0,
               (size_t)(toWrite - written) * m_bytesPerFrame);
        m_winUnderrun    += toWrite - written;
        m_underrunFrames += toWrite - written;
    }
    DWORD releaseFlags = 0;
    if (m_muted.load()) {
        releaseFlags = AUDCLNT_BUFFERFLAGS_SILENT;
    } else {
        const float vol = m_volume.load();
        if (vol < 0.999f) ScaleInPlace(out, toWrite, m_renderFormat, vol);
    }
    hr = m_renderService->ReleaseBuffer(toWrite, releaseFlags);
    if (FAILED(hr)) { HandleStreamError(hr, L"render ReleaseBuffer", false); return false; }
    m_winOut += toWrite;
    return true;
}

void AudioRouter::LogStatsIfDue()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - m_winStart < std::chrono::milliseconds(kStatsIntervalMs)) return;
    std::wstringstream ss;
    ss << L"stats: fill " << m_fillMs.load() << L" ms (min "
       << (m_winFillMin == 0xFFFFFFFFu ? 0u : m_winFillMin) << L", max " << m_winFillMax
       << L"), in " << m_winIn << L", out " << m_winOut
       << L", underrun " << m_winUnderrun << L", overrun " << m_winOverrun
       << L", slip +" << m_winSlipDrop << L"/-" << m_winSlipDup;
    AudioLog(ss.str());
    m_winStart = now;
    m_winIn = m_winOut = m_winUnderrun = m_winOverrun = m_winSlipDrop = m_winSlipDup = 0;
    m_winFillMin = 0xFFFFFFFFu;
    m_winFillMax = 0;
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
