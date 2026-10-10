// These tests exercise CPU-only helpers without opening an audio endpoint or
// capture device. Including the implementations keeps their internals local.
#include "../src/audio/audio_router.cpp"
#include "../src/capture/dshow_capture.cpp"
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
class Sample final : public IMediaSample {
public:
    BYTE data[16]{};
    long length = 16;
    ULONG references = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** value) override { if (value) *value = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE GetPointer(BYTE** value) override { *value = data; return S_OK; }
    long STDMETHODCALLTYPE GetSize() override { return sizeof(data); }
    HRESULT STDMETHODCALLTYPE GetTime(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTime(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsSyncPoint() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSyncPoint(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsPreroll() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetPreroll(BOOL) override { return E_NOTIMPL; }
    long STDMETHODCALLTYPE GetActualDataLength() override { return length; }
    HRESULT STDMETHODCALLTYPE SetActualDataLength(long) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMediaType(AM_MEDIA_TYPE**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsDiscontinuity() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetDiscontinuity(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
};

// Deterministic WASAPI doubles: enforce exclusive packet/event contracts.
struct ExclusiveState {
    int activations=0, initializes=0, gets=0, releases=0, paddings=0, starts=0;
    UINT32 frames=48, lastFlags=0;
    REFERENCE_TIME minimum=10000;
    bool alignment=false, unsupported=false;
    HRESULT getError=S_OK, startError=S_OK;
    std::vector<REFERENCE_TIME> periods;
    std::vector<BYTE> bytes=std::vector<BYTE>(4096);
};
class FakeRender final : public IAudioRenderClient {
    ULONG refs=1; ExclusiveState& s;
public:
    explicit FakeRender(ExclusiveState& state):s(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** p) override { *p=nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n=--refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames,BYTE** p) override {
        Check(frames==s.frames,"exclusive GetBuffer must request a whole block");
        ++s.gets; *p=nullptr;
        if (FAILED(s.getError)) return s.getError;
        *p=s.bytes.data(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames,DWORD flags) override {
        Check(frames==s.frames,"exclusive ReleaseBuffer must release a whole block");
        ++s.releases; s.lastFlags=flags; return S_OK;
    }
};
class FakeAudio final : public IAudioClient {
    ULONG refs=1; ExclusiveState& s;
public:
    explicit FakeAudio(ExclusiveState& state):s(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** p) override { *p=nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n=--refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode,DWORD flags,REFERENCE_TIME duration,
        REFERENCE_TIME period,const WAVEFORMATEX*,LPCGUID) override {
        Check(mode==AUDCLNT_SHAREMODE_EXCLUSIVE && flags==AUDCLNT_STREAMFLAGS_EVENTCALLBACK,"exclusive event flags");
        Check(period==duration && period>=s.minimum,"exclusive equal buffer and period >= driver minimum");
        s.periods.push_back(period); ++s.initializes;
        if (s.alignment && s.initializes==1) return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* n) override { *n=s.frames; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* n) override { *n=20000; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*) override { ++s.paddings; return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode,const WAVEFORMATEX*,WAVEFORMATEX**) override {
        Check(mode==AUDCLNT_SHAREMODE_EXCLUSIVE,"probe exclusive format");
        return s.unsupported ? AUDCLNT_E_UNSUPPORTED_FORMAT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal,REFERENCE_TIME* minimum) override {
        *normal=100000; *minimum=s.minimum; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        Check(s.releases>0,"exclusive stream must be primed before Start"); ++s.starts; return s.startError;
    }
    HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetService(REFIID id,void** p) override {
        if (id!=__uuidof(IAudioRenderClient)) { *p=nullptr; return E_NOINTERFACE; }
        *p=static_cast<IAudioRenderClient*>(new FakeRender(s)); return S_OK;
    }
};
class FakeDevice final : public IMMDevice {
    ULONG refs=1; ExclusiveState& s;
public:
    explicit FakeDevice(ExclusiveState& state):s(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** p) override { *p=nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE Activate(REFIID id,DWORD,PROPVARIANT*,void** p) override {
        Check(id==__uuidof(IAudioClient),"activate a fresh audio client"); ++s.activations;
        *p=static_cast<IAudioClient*>(new FakeAudio(s)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD,IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD*) override { return E_NOTIMPL; }
};
namespace NitLink {
struct AudioRouterTestAccess {
    static void PacketDrift() {
        WAVEFORMATEX pcm{WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
        ExclusiveState state; state.frames=144; state.minimum=30000;
        FakeDevice device(state); AudioRouter router;
        router.SetLatency(3,3,true);
        router.m_captureFormat=CloneWaveFormat(&pcm);
        Check(router.FifoReset(4,48000),"packet regression FIFO");
        std::wstring failure;
        Check(router.SetupExclusive(&device,&pcm,failure),"packet regression exclusive setup");
        router.m_restartCapture=false;
        std::vector<int16_t> packet(480*2,12000);
        int nextPacketMs=10;
        uint64_t settledUnderruns=0;
        // Exercise the real ring, phase, FIR and FillRender path. Fake only the
        // endpoint and elapsed control time; there is no real-time test sleep.
        for (int i=1;i<=6000;++i) {
            while (nextPacketMs<=i*3) {
                router.FifoPush(reinterpret_cast<const BYTE*>(packet.data()),480,false);
                nextPacketMs+=10;
            }
            router.m_lastDrift=std::chrono::steady_clock::now()-std::chrono::milliseconds(3);
            Check(router.FillRender(true),"packetized exclusive pump");
            if (i==1000) settledUnderruns=router.Underruns();
            if (i>1000) Check(router.Underruns()==settledUnderruns,"drift must not repeatedly starve packetized audio");
        }
        Check(router.Overruns()==0 && !router.m_restartCapture,"no trim/reload hides packet drift failures");
        Check(router.m_fifoTargetMs==3 && router.m_renderQueueTargetMs==3,"packet guard preserves user targets");
        router.TeardownRender(); router.TeardownCapture();
    }
    static void Recovery() {
        AudioRouter router;
        router.SetLatency(3,3,true);
        router.m_restartCapture=false; router.m_restartRender=false;
        for (int i=0;i<300;++i) {
            router.m_captureClockRate.Sample(i*480,1000000000ULL+i*100000ULL,48000,i*.01);
            router.m_renderClockRate.Sample(i*480,1000000000ULL+i*100000ULL,48000,i*.01);
        }
        router.ResetDrift(true);
        Check(router.m_captureClockRate.Valid(2.99) && router.m_renderClockRate.Valid(2.99),
            "ordinary re-prime preserves both clock estimates");
        router.ObserveRecovery(0,0,.001,.003);
        router.ObserveRecovery(.1,0,.001,.003);
        Check(!router.m_restartCapture,"small isolated underruns keep endpoints alive");
        router.ObserveRecovery(.2,0,.001,.003);
        Check(router.m_restartCapture && router.AutomaticReloads()==1,"threshold schedules both endpoint rebuilds");
        router.m_restartCapture=false; router.ResetDrift();
        Check(!router.m_captureClockRate.Valid(2.99),"endpoint rebuild clears stale clock identity");
        router.ObserveRecovery(1,.1,.03,.003);
        Check(!router.m_restartCapture && router.AutomaticReloads()==1,"endpoint reset preserves recovery cooldown");
        Check(router.m_fifoTargetMs==3 && router.m_renderQueueTargetMs==3,"recovery never changes either user buffer target");
        router.m_driftEnabled=false;
        router.ObserveRecovery(20,.1,.03,.003);
        Check(!router.m_restartCapture,"drift disabled also disables automatic threshold reloads");
    }
    static void Focus() {
        AudioRouter router;
        const auto t=ExclusiveFocusGate::Clock::now();
        router.SetExclusive(true); router.m_restartRender=false; router.m_retryExclusive=false;
        router.UpdateExclusiveFocus(false,t);
        Check(!router.m_exclusivePermitted && !router.m_restartRender,"background startup remains shared");
        router.UpdateExclusiveFocus(true,t);
        router.UpdateExclusiveFocus(true,t+std::chrono::milliseconds(199));
        Check(!router.m_exclusivePermitted,"exclusive restoration waits for stable focus");
        router.UpdateExclusiveFocus(true,t+std::chrono::milliseconds(200));
        Check(router.m_exclusivePermitted && router.m_restartRender && router.m_retryExclusive,"foreground restores requested exclusive mode");
        router.m_restartRender=false;
        router.UpdateExclusiveFocus(true,t+std::chrono::milliseconds(210));
        Check(!router.m_restartRender,"stable focus never repeatedly restarts audio");
        router.UpdateExclusiveFocus(false,t+std::chrono::milliseconds(211));
        Check(!router.m_exclusivePermitted && router.m_restartRender && router.m_exclusiveRequested,
            "focus loss immediately requests shared without changing saved preference");
        router.m_restartRender=false;
        router.UpdateExclusiveFocus(true,t+std::chrono::milliseconds(220));
        router.UpdateExclusiveFocus(false,t+std::chrono::milliseconds(250));
        router.UpdateExclusiveFocus(true,t+std::chrono::milliseconds(300));
        router.UpdateExclusiveFocus(true,t+std::chrono::milliseconds(499));
        Check(!router.m_exclusivePermitted,"brief activation resets the recovery delay");
        router.SetExclusive(false); router.m_restartRender=false;
        router.UpdateExclusiveFocus(true,t+std::chrono::milliseconds(500));
        Check(!router.m_restartRender && !router.m_exclusiveRequested,"focus changes leave explicitly shared audio alone");

        HWND owner=CreateWindowExW(0,L"STATIC",L"audio-focus-test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            0,0,200,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        HWND popup=CreateWindowExW(0,L"STATIC",L"settings-test",WS_POPUP|WS_VISIBLE,
            0,0,100,50,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
        HWND child=CreateWindowExW(0,L"STATIC",L"webview-test",WS_CHILD|WS_VISIBLE,
            0,0,50,20,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(owner && popup && child,"focus test windows created");
        Check(IsAudioWindowForeground(owner,owner) && IsAudioWindowForeground(owner,popup) &&
            IsAudioWindowForeground(owner,child),"main, owned settings and child controls share foreground identity");
        Check(!IsAudioWindowForeground(owner,nullptr) && !IsAudioWindowForeground(owner,GetDesktopWindow()),
            "other or absent foreground releases exclusive");
        ShowWindow(owner,SW_MINIMIZE);
        Check(!IsAudioWindowForeground(owner,owner),"minimized main window releases exclusive");
        ShowWindow(owner,SW_RESTORE); ShowWindow(owner,SW_HIDE);
        Check(!IsAudioWindowForeground(owner,popup),"hidden main window releases exclusive");
        DestroyWindow(owner);
        Check(!IsAudioWindowForeground(owner,owner),"destroyed main window cannot reacquire exclusive");
    }
    static void Exclusive() {
        WAVEFORMATEX pcm{WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
        {
            ExclusiveState state; state.alignment=true; state.frames=64;
            FakeDevice device(state); ComPtr<IAudioClient> client; ExclusiveResult result;
            Check(OpenExclusive(&device,&pcm,nullptr,client,result),"aligned exclusive initialization");
            Check(state.activations==2 && state.initializes==2,"alignment uses a fresh client");
            Check(state.periods[0]==10000 && state.periods[1]==13333 && result.frames==64 && result.aligned,
                "default minimum then exact driver frame alignment");
        }
        {
            ExclusiveState state; state.minimum=0;
            FakeDevice device(state); ComPtr<IAudioClient> client; ExclusiveResult result;
            Check(!OpenExclusive(&device,&pcm,nullptr,client,result) && state.initializes==0,"invalid minimum rejected");
        }
        ExclusiveState state; FakeDevice device(state); AudioRouter router;
        router.m_captureFormat=CloneWaveFormat(&pcm);
        Check(router.FifoReset(4,48000),"exclusive source FIFO");
        std::wstring failure;
        Check(router.SetupExclusive(&device,&pcm,failure),"exclusive setup");
        Check(state.periods.size()==1 && state.periods[0]==state.minimum && state.starts==1,
            "exclusive defaults to minimum, independent of shared queue target");
        Check(router.ExclusiveActive() && router.RenderPeriodUs()==1000 && state.lastFlags==AUDCLNT_BUFFERFLAGS_SILENT,
            "exclusive prime and active telemetry");
        const int primingGets=state.gets;
        Check(router.FillRender(false) && state.gets==primingGets,"capture wake must not acquire exclusive buffer");
        Check(router.FillRender(true) && state.releases==2 && router.Underruns()==0,
            "waiting for FIFO submits whole silent block without startup underrun");
        std::vector<int16_t> audio(2000*2,12000);
        router.FifoPush(reinterpret_cast<const BYTE*>(audio.data()),2000,false);
        Check(router.FillRender(true) && state.lastFlags==0,"exclusive event writes converted audio");
        Check(state.paddings==0,"exclusive path never queries shared padding");
        router.m_fifoBytes=router.m_sinc.History()*4;
        Check(router.FillRender(true) && state.lastFlags==AUDCLNT_BUFFERFLAGS_SILENT && router.Underruns()==48,
            "starvation releases a whole silent block and reprimes");
        router.m_driftEnabled=false; router.m_fifoPrimed=true;
        Check(router.FillRender(true) && router.Underruns()==96 && router.DriftPpm()==0,
            "disabling correction must not disable underrun accounting");
        state.getError=AUDCLNT_E_BUFFER_ERROR;
        Check(!router.FillRender(true) && router.m_restartRender && !router.m_exclusiveFailure.empty(),
            "exclusive render error schedules shared fallback");
        router.SetExclusive(true); router.SetExclusive(false);
        Check(router.m_retryExclusive && !router.m_exclusiveRequested,"explicit mode change clears retry latch");
        router.TeardownRender();
        Check(!router.ExclusiveActive(),"teardown clears actual exclusive state");
        state.getError=S_OK; state.startError=AUDCLNT_E_DEVICE_IN_USE;
        Check(!router.SetupExclusive(&device,&pcm,failure) && failure.find(L"start exclusive")!=std::wstring::npos,
            "exclusive Start failure is reported for fallback");
        router.TeardownRender();
        state.unsupported=true;
        Check(!router.SetupExclusive(&device,&pcm,failure) && failure.find(L"no convertible")!=std::wstring::npos,
            "unsupported formats report fallback");
        router.TeardownRender(); router.TeardownCapture();
    }
    static void Run() {
        Focus();
        Exclusive();
        PacketDrift();
        AudioRouter router;
        Check(!router.FifoReset(UINT32_MAX, UINT32_MAX), "FIFO allocation cap");
        Check(router.FifoReset(8, 48000), "valid FIFO");
        router.FifoPush(nullptr, 100000, true);
        Check(router.m_fifoBytes == router.m_fifo.size(), "oversized silent packet bounded");
        float samples[4] = {1, 1, 1, 1};
        Check(router.FifoPop(reinterpret_cast<BYTE*>(samples), 2) == 2 && samples[0] == 0 && samples[3] == 0, "silent FIFO produces zeroes");
        const auto size = router.m_fifoBytes;
        router.FifoPush(nullptr, 2, false);
        Check(size == router.m_fifoBytes, "null data not copied");
        // Native stereo PCM16 -> float32 at a different sample rate, ring wrap.
        WAVEFORMATEX pcm{WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
        WAVEFORMATEX wave{WAVE_FORMAT_IEEE_FLOAT,2,44100,352800,8,32,0};
        router.m_captureFormat=CloneWaveFormat(&pcm);
        router.m_renderFormat=CloneWaveFormat(&wave);
        router.m_conversionReady=router.PrepareConversion();
        Check(router.m_conversionReady,"native PCM16 to float32 conversion prepared");
        Check(router.FifoReset(4,48000),"native FIFO reset");
        router.m_fifoHead=router.m_fifo.size()-20;
        std::vector<int16_t> input(2048*2);
        for (int i=0;i<2048;++i) { input[2*i]=int16_t(12000*std::sin(i*0.01)); input[2*i+1]=-input[2*i]; }
        router.FifoPush(reinterpret_cast<BYTE*>(input.data()),2048,false);
        std::vector<float> out(1000*2);
        Check(router.FifoResample(reinterpret_cast<BYTE*>(out.data()),1000,48000.0/44100)==1000,"different-rate wrapped FIFO");
        for (int i=0;i<1000;++i) Check(std::abs(out[2*i]+out[2*i+1])<1e-6,"stereo phase locked");
        Check(router.m_fifoBytes/4==2048-1088,"source-rate frame accounting");
        Check(router.FifoResample(reinterpret_cast<BYTE*>(out.data()),1000,48000.0/44100)<1000,"FIR lookahead prevents overread");
        // Post-pump telemetry uses each endpoint's own rate, subtracting FIR history.
        router.m_fifoBytes=(480+router.m_sinc.History())*4;
        router.PublishQueue(441);
        const uint64_t snapshot=router.QueueSnapshotUs();
        Check((snapshot>>32)==10000 && uint32_t(snapshot)==10000,"coherent different-rate occupancy");
        // Partition invariance: packet boundaries cannot reset filter phase/history.
        const auto render=[&](bool split) {
            router.FifoReset(4,48000);
            router.FifoPush(reinterpret_cast<BYTE*>(input.data()),2048,false);
            std::vector<float> result(1000*2);
            if (!split) Check(router.FifoResample(reinterpret_cast<BYTE*>(result.data()),1000,1.001)==1000,"continuous resample");
            else for (unsigned i=0;i<1000;i+=100)
                Check(router.FifoResample(reinterpret_cast<BYTE*>(result.data()+2*i),100,1.001)==100,"partitioned resample");
            return result;
        };
        const auto whole=render(false), parts=render(true);
        for (size_t i=0;i<whole.size();++i) Check(std::abs(whole[i]-parts[i])<1e-6,"continuous FIR phase across calls");
        CoTaskMemFree(router.m_captureFormat); router.m_captureFormat=nullptr;
        CoTaskMemFree(router.m_renderFormat); router.m_renderFormat=nullptr;
        router.SetLatency(-10, 999, false);
        Check(router.m_fifoTargetMs == 3 && router.m_renderQueueTargetMs == 100 && !router.m_driftEnabled,
              "latency setter clamps all entry points");
        router.SetVolume(0.5f); router.SetVolume(NAN);
        Check(router.m_volume == 0.5f, "NaN volume ignored");
    }
};
}

int main() {
    using namespace NitLink;
    try {
        WAVEFORMATEX wave{WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0};
        Check(ValidWaveFormat(&wave), "stereo float layout");
        float samples[4] = {1, -1, 0.5f, -0.5f};
        Check(ScaleInPlace(reinterpret_cast<BYTE*>(samples), 2, &wave, 0.5f) && samples[0] == 0.5f && samples[3] == -0.25f, "valid volume scaling");
        wave.nBlockAlign = 4;
        Check(!ValidWaveFormat(&wave) && !ScaleInPlace(reinterpret_cast<BYTE*>(samples), 2, &wave, 1), "short audio frame layout rejected");
        wave.nBlockAlign = 8; wave.nChannels = 65535;
        Check(!ValidWaveFormat(&wave), "channel cap");
        wave.nChannels = 2; wave.cbSize = 65535;
        Check(!ValidWaveFormat(&wave) && !CloneWaveFormat(&wave), "format extension cap");
        WAVEFORMATEXTENSIBLE ext{};
        ext.Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 288000, 6, 24, 22};
        ext.SubFormat = kSubtypePcm; ext.Samples.wValidBitsPerSample = 24;
        Check(ValidWaveFormat(&ext.Format), "24-bit extensible PCM supported");
        ext.Samples.wValidBitsPerSample = 32;
        Check(!ValidWaveFormat(&ext.Format), "invalid valid-bits field");
        AudioRouterTestAccess::Recovery();
        AudioRouterTestAccess::Run();

        VIDEOINFOHEADER info{};
        info.bmiHeader.biWidth = 1920; info.bmiHeader.biHeight = -1080; info.AvgTimePerFrame = 166667;
        AM_MEDIA_TYPE mt{}; mt.majortype = MEDIATYPE_Video; mt.subtype = MFVideoFormat_NV12;
        mt.formattype = FORMAT_VideoInfo; mt.pbFormat = reinterpret_cast<BYTE*>(&info); mt.cbFormat = sizeof(info);
        Check(ValidNV12Type(&mt), "valid DirectShow NV12 metadata");
        info.bmiHeader.biHeight = LONG_MIN;
        Check(!ValidNV12Type(&mt), "LONG_MIN height rejected");
        info.bmiHeader.biHeight = 1080; info.bmiHeader.biWidth = 1921;
        Check(!ValidNV12Type(&mt), "odd planar dimensions rejected");
        info.bmiHeader.biWidth = 1920;
        AM_MEDIA_TYPE copy{};
        Check(SUCCEEDED(CopyMT(&copy, &mt)) && copy.pbFormat != mt.pbFormat && copy.cbFormat == sizeof(info), "media type deep copy");
        FreeMTContents(&copy);
        mt.cbFormat = 65537;
        Check(FAILED(CopyMT(&copy, &mt)) && !copy.pbFormat && !copy.pUnk, "oversized media format leaves empty destination");
        mt.cbFormat = sizeof(info); mt.pbFormat = nullptr;
        Check(FAILED(CopyMT(&copy, &mt)) && !ValidNV12Type(&mt), "missing media format bytes");
        Sample sample;
        mt.pbFormat = reinterpret_cast<BYTE*>(&info); mt.pUnk = &sample;
        Check(SUCCEEDED(CopyMT(&copy, &mt)) && sample.references == 2, "media type retains owned COM reference");
        FreeMTContents(&copy);
        Check(sample.references == 1, "media type releases exactly one reference");
        mt.cbFormat = 65537;
        Check(FAILED(CopyMT(&copy, &mt)) && sample.references == 1 && !copy.pUnk, "rejected copy does not acquire caller reference");
        DShowGraph graph;
        unsigned delivered = 0;
        graph.cb = [&](const uint8_t* data, uint32_t size, int64_t, int64_t, uint64_t) {
            Check(data == sample.data && size == sizeof(sample.data), "sample callback bounded to allocation");
            ++delivered;
        };
        sample.length = 17; graph.OnSample(&sample);
        sample.length = LONG_MIN; graph.OnSample(&sample);
        sample.length = LONG_MAX; graph.OnSample(&sample);
        Check(delivered == 0, "invalid sample lengths never reach callback");
        sample.length = 16; graph.OnSample(&sample);
        Check(delivered == 1, "normal sample after invalid samples");
        std::cout << "Audio and DirectShow metadata security tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
