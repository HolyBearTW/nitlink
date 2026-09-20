#include "capture/capture_device.h"

#include <wrl/implements.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace NitLink {

struct CaptureDeviceFormatTests {
    static bool Negotiate(CaptureDevice& device, IMFMediaSource* source,
                          const wchar_t* name, bool hdr,
                          const CaptureDevice::OverrideSpec& overrideSpec = {})
    {
        device.m_deviceName = name;
        device.RequestP010(hdr);
        device.SetFormatOverride(overrideSpec);
        return device.NegotiateFormat(source);
    }

    static CaptureFormat Format(const CaptureDevice& device)
    {
        return device.m_format;
    }

    static HRESULT WriteRate(CaptureDevice& device, IMFMediaType* output)
    {
        return device.SetOutputFrameRate(output);
    }
};

} // namespace NitLink

namespace {

void Check(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("Media Foundation fixture failed");
}

bool Expect(bool condition, const char* label)
{
    std::cout << (condition ? "PASS " : "FAIL ") << label << '\n';
    return condition;
}

class DescriptorSource final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFMediaSource> {
public:
    explicit DescriptorSource(ComPtr<IMFPresentationDescriptor> descriptor)
        : m_descriptor(descriptor) {}

    HRESULT STDMETHODCALLTYPE CreatePresentationDescriptor(
        IMFPresentationDescriptor** descriptor) override
    {
        return m_descriptor.CopyTo(descriptor);
    }

    HRESULT STDMETHODCALLTYPE GetEvent(DWORD, IMFMediaEvent**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback*, IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult*, IMFMediaEvent**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType, REFGUID, HRESULT, const PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Start(IMFPresentationDescriptor*, const GUID*, const PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stop() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Pause() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Shutdown() override { return S_OK; }

private:
    ComPtr<IMFPresentationDescriptor> m_descriptor;
};

struct Mode {
    GUID subtype;
    UINT32 width;
    UINT32 height;
    UINT32 numerator;
    UINT32 denominator;
    bool hasRate = true;
};

struct Stream {
    std::vector<Mode> modes;
    bool selected = true;
};

ComPtr<IMFMediaSource> MakeSource(const std::vector<Stream>& streams)
{
    std::vector<ComPtr<IMFStreamDescriptor>> ownedStreams;
    std::vector<IMFStreamDescriptor*> rawStreams;
    for (const auto& stream : streams) {
        std::vector<ComPtr<IMFMediaType>> ownedTypes;
        std::vector<IMFMediaType*> rawTypes;
        for (const auto& mode : stream.modes) {
            ComPtr<IMFMediaType> type;
            Check(MFCreateMediaType(&type));
            Check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            Check(type->SetGUID(MF_MT_SUBTYPE, mode.subtype));
            Check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, mode.width, mode.height));
            if (mode.hasRate) {
                Check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                                         mode.numerator, mode.denominator));
            }
            rawTypes.push_back(type.Get());
            ownedTypes.push_back(type);
        }
        ComPtr<IMFStreamDescriptor> descriptor;
        Check(MFCreateStreamDescriptor(static_cast<DWORD>(ownedStreams.size()),
            static_cast<DWORD>(rawTypes.size()), rawTypes.data(), &descriptor));
        rawStreams.push_back(descriptor.Get());
        ownedStreams.push_back(descriptor);
    }
    ComPtr<IMFPresentationDescriptor> descriptor;
    Check(MFCreatePresentationDescriptor(static_cast<DWORD>(rawStreams.size()),
                                         rawStreams.data(), &descriptor));
    for (DWORD i = 0; i < static_cast<DWORD>(streams.size()); ++i) {
        Check(streams[i].selected ? descriptor->SelectStream(i)
                                 : descriptor->DeselectStream(i));
    }
    return Microsoft::WRL::Make<DescriptorSource>(descriptor);
}

bool ExpectRate(NitLink::CaptureDevice& device, UINT32 numerator,
                UINT32 denominator, const char* label)
{
    ComPtr<IMFMediaType> output;
    Check(MFCreateMediaType(&output));
    Check(NitLink::CaptureDeviceFormatTests::WriteRate(device, output.Get()));
    UINT32 actualNumerator = 0, actualDenominator = 0;
    Check(MFGetAttributeRatio(output.Get(), MF_MT_FRAME_RATE,
                              &actualNumerator, &actualDenominator));
    return Expect(actualNumerator == numerator && actualDenominator == denominator, label);
}

bool RunChecks()
{
    using namespace NitLink;
    using Access = CaptureDeviceFormatTests;
    constexpr auto gc553 = L"AVerMedia GC553Pro";
    bool pass = true;
    const Mode nv12 = {MFVideoFormat_NV12, 3840, 2160, 60, 1};
    const Mode p010 = {MFVideoFormat_P010, 1920, 1080, 60, 1};

    for (const UINT32 numerator : {60000u, 30000u, 24000u, 120000u}) {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_P010, 1920, 1080, numerator, 1001}}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true),
                       "fractional native mode negotiates");
        pass &= ExpectRate(device, numerator, 1001, "outgoing request retains native ratio");
    }

    {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_P010, 2560, 1440, 30, 1}, p010}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true),
                       "integer native modes negotiate");
        const auto format = Access::Format(device);
        pass &= Expect(format.width == 1920 && format.height == 1080,
                       "GC553Pro keeps high-FPS selection");
        pass &= ExpectRate(device, 60, 1, "integer native rate unchanged");
    }

    for (const CaptureDevice::OverrideSpec overrideSpec : {
             CaptureDevice::OverrideSpec{}, {1920, 1080, 60, L"P010"}}) {
        CaptureDevice device;
        auto source = MakeSource({{{nv12}}});
        pass &= Expect(!Access::Negotiate(device, source.Get(), gc553, true, overrideSpec),
                       "NV12-only enumeration rejects GC553Pro HDR request");
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, false),
                       "SDR retry negotiates after absent P010");
        const auto format = Access::Format(device);
        pass &= Expect(format.width == 3840 && format.height == 2160,
                       "SDR retry uses native dimensions");
        pass &= ExpectRate(device, 60, 1, "SDR retry uses SDR rate");
    }

    for (const Mode invalid : {
             Mode{MFVideoFormat_P010, 1920, 1080, 0, 1},
             Mode{MFVideoFormat_P010, 1920, 1080, 60000, 0},
             Mode{MFVideoFormat_P010, 1920, 1080, 60, 1, false},
             Mode{MFVideoFormat_P010, 0, 1080, 60, 1}}) {
        CaptureDevice device;
        auto source = MakeSource({{{invalid}}});
        pass &= Expect(!Access::Negotiate(device, source.Get(), gc553, true),
                       "incomplete native P010 mode rejected");
    }

    {
        CaptureDevice device;
        auto source = MakeSource({{{nv12}}, {{p010}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true),
                       "later selected stream supplies native P010");
        source = MakeSource({{{nv12}}, {{p010}, false}});
        pass &= Expect(!Access::Negotiate(device, source.Get(), gc553, true),
                       "unselected P010 stream does not grant HDR capability");
    }

    {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_P010, 1920, 1080, 60000, 1001}}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true),
                       "fractional rate initially selected");
        source = MakeSource({{{{MFVideoFormat_NV12, 1920, 1080, 120, 1}}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, false),
                       "switch to SDR negotiates");
        pass &= ExpectRate(device, 120, 1, "previous HDR rate cannot leak into SDR");
    }

    for (const auto name : {L"Elgato Game Capture 4K Pro", L"Generic capture card"}) {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_P010, 2560, 1440, 30, 1}, p010}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), name, true),
                       "existing non-GC553Pro negotiation succeeds");
        pass &= Expect(Access::Format(device).width == 2560,
                       "existing largest-width policy unchanged");
        pass &= ExpectRate(device, 30, 1, "existing integer request unchanged");
        source = MakeSource({{{nv12}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), name, true),
                       "existing generic fallback behavior unchanged");
    }
    return pass;
}

} // namespace

int main()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) {
        CoUninitialize();
        return 1;
    }
    bool pass = false;
    try {
        pass = RunChecks();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    if (FAILED(MFShutdown())) pass = false;
    CoUninitialize();
    return pass ? 0 : 1;
}
