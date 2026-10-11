#pragma once

#include "app/frame_rate_stats.h"
#include "app/resync_frame_presentation.h"
#include <wrl/implements.h>

namespace NitLink {

// Exercises the real EndFrame wiring with deterministic DXGI return values.
// The existing offscreen WARP target supplies BeginFrame; no capture card,
// visible window, driver change or hardware scan-out is involved.
class PresentResultSwapChain final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDXGISwapChain3> {
public:
    HRESULT result = S_OK;
    UINT calls = 0, syncInterval = 0, flags = 0;
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT presentFlags) override {
        ++calls; syncInterval = sync; flags = presentFlags;
        return result;
    }
#define UNUSED_DXGI_METHOD(name, parameters) \
    HRESULT STDMETHODCALLTYPE name parameters override { return E_NOTIMPL; }
    UNUSED_DXGI_METHOD(SetPrivateData, (REFGUID, UINT, const void*))
    UNUSED_DXGI_METHOD(SetPrivateDataInterface, (REFGUID, const IUnknown*))
    UNUSED_DXGI_METHOD(GetPrivateData, (REFGUID, UINT*, void*))
    UNUSED_DXGI_METHOD(GetParent, (REFIID, void**))
    UNUSED_DXGI_METHOD(GetDevice, (REFIID, void**))
    UNUSED_DXGI_METHOD(GetBuffer, (UINT, REFIID, void**))
    UNUSED_DXGI_METHOD(SetFullscreenState, (BOOL, IDXGIOutput*))
    UNUSED_DXGI_METHOD(GetFullscreenState, (BOOL*, IDXGIOutput**))
    UNUSED_DXGI_METHOD(GetDesc, (DXGI_SWAP_CHAIN_DESC*))
    UNUSED_DXGI_METHOD(ResizeBuffers, (UINT, UINT, UINT, DXGI_FORMAT, UINT))
    UNUSED_DXGI_METHOD(ResizeTarget, (const DXGI_MODE_DESC*))
    UNUSED_DXGI_METHOD(GetContainingOutput, (IDXGIOutput**))
    UNUSED_DXGI_METHOD(GetFrameStatistics, (DXGI_FRAME_STATISTICS*))
    UNUSED_DXGI_METHOD(GetLastPresentCount, (UINT*))
    UNUSED_DXGI_METHOD(GetDesc1, (DXGI_SWAP_CHAIN_DESC1*))
    UNUSED_DXGI_METHOD(GetFullscreenDesc, (DXGI_SWAP_CHAIN_FULLSCREEN_DESC*))
    UNUSED_DXGI_METHOD(GetHwnd, (HWND*))
    UNUSED_DXGI_METHOD(GetCoreWindow, (REFIID, void**))
    UNUSED_DXGI_METHOD(Present1, (UINT, UINT, const DXGI_PRESENT_PARAMETERS*))
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    UNUSED_DXGI_METHOD(GetRestrictToOutput, (IDXGIOutput**))
    UNUSED_DXGI_METHOD(SetBackgroundColor, (const DXGI_RGBA*))
    UNUSED_DXGI_METHOD(GetBackgroundColor, (DXGI_RGBA*))
    UNUSED_DXGI_METHOD(SetRotation, (DXGI_MODE_ROTATION))
    UNUSED_DXGI_METHOD(GetRotation, (DXGI_MODE_ROTATION*))
    UNUSED_DXGI_METHOD(SetSourceSize, (UINT, UINT))
    UNUSED_DXGI_METHOD(GetSourceSize, (UINT*, UINT*))
    UNUSED_DXGI_METHOD(SetMaximumFrameLatency, (UINT))
    UNUSED_DXGI_METHOD(GetMaximumFrameLatency, (UINT*))
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return nullptr; }
    UNUSED_DXGI_METHOD(SetMatrixTransform, (const DXGI_MATRIX_3X2_F*))
    UNUSED_DXGI_METHOD(GetMatrixTransform, (DXGI_MATRIX_3X2_F*))
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return 0; }
    UNUSED_DXGI_METHOD(CheckColorSpaceSupport, (DXGI_COLOR_SPACE_TYPE, UINT*))
    UNUSED_DXGI_METHOD(SetColorSpace1, (DXGI_COLOR_SPACE_TYPE))
    UNUSED_DXGI_METHOD(ResizeBuffers1, (UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                      const UINT*, IUnknown* const*))
#undef UNUSED_DXGI_METHOD
};

inline void RunPresentResyncIntegration(DX11Renderer& renderer)
{
    auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    auto chain = Microsoft::WRL::Make<PresentResultSwapChain>();
    require(!!chain, "create deterministic Present fixture");
    DX11RendererTestAccess::AttachSwapChain(renderer, chain.Get());
    DX11RendererTestAccess::PinMarker(renderer, false, 0);
    renderer.SetVSync(false);

    for (bool lowLatency : {false, true}) {
        const auto cap = ChoosePresentCap({0, 120, 120, 0, false, false, 0});
        require(cap.effectiveHz == 120, "Auto remains bounded at equal source/display rate");
        renderer.SetPresentCap(cap.requestedHz);
        DX11RendererTestAccess::StartInterval(renderer);
        std::uint64_t serial = 0;
        FrameRateCounters counts{};
        auto submit = [&](HRESULT result, bool live, bool fresh,
                          bool postInput = false, bool composited = true) {
            renderer.ConsumePhaseTimes();
            chain->result = result;
            const auto beforeCalls = chain->calls;
            if (lowLatency) renderer.WaitForFrameReady();
            renderer.BeginFrame(!lowLatency);
            if (fresh) DX11RendererTestAccess::CompletedUpload(renderer, ++serial);
            DX11RendererTestAccess::PreparedDraw(renderer, live, postInput, composited);
            const bool success = renderer.EndFrame();
            if (success) ++counts.presented;
            const auto phases = renderer.ConsumePhaseTimes();
            require(chain->calls == beforeCalls + 1 && phases.presents == 1,
                    "EndFrame performs and diagnoses exactly one Present");
            require(success == (result == S_OK) && phases.presentOk == (result == S_OK),
                    "FPS success and frame receipt use the same exact S_OK result");
            require(phases.presentStatuses == (result != S_OK && SUCCEEDED(result)) &&
                    phases.presentFailed == FAILED(result),
                    "non-S_OK statuses and failures remain distinct");
            require(phases.capWaits == 1 && phases.iterations == 1,
                    "both Low Latency paths keep one cap wait during resync");
            require(chain->syncInterval == 0 && chain->flags == DXGI_PRESENT_ALLOW_TEARING,
                    "integration does not change Present flags");
            return success;
        };
        counts.captured = 120;
        counts.content = 60;
        require(submit(S_OK, true, true), "live draw submits successfully");
        auto receipt = DX11RendererTestAccess::Receipt(renderer);
        require(receipt.available && receipt.compatible, "live S_OK establishes a hold receipt");
        auto rates = SampleFrameRates(counts, {}, 1.0, true);
        require(rates.captureFps == 120 && rates.contentFps == 60 && rates.presentFps == 1,
                "successful submissions remain independent of capture/content estimates");

        const auto previous = counts;
        const auto resync = DecidePresentation(false, false, true, false, true);
        require(DecideResyncFrameMode(resync, receipt) == ResyncFrameMode::Cached,
                "compatible receipt selects cached SignalResync");
        require(submit(S_OK, false, false), "cached presentation is a successful submission");
        receipt = DX11RendererTestAccess::Receipt(renderer);
        require(receipt.compatible, "cached submission retains the existing receipt");
        rates = SampleFrameRates(counts, previous, 1.0, true);
        require(rates.captureFps == 0 && rates.contentFps == 0 && rates.presentFps == 1,
                "resync never switches zero capture/content to another FPS metric");

        for (HRESULT result : {DXGI_STATUS_OCCLUDED, DXGI_ERROR_WAS_STILL_DRAWING,
                               E_FAIL, DXGI_ERROR_DEVICE_REMOVED}) {
            const auto before = counts.presented;
            require(!submit(result, true, true), "non-S_OK is excluded from successful FPS");
            require(counts.presented == before &&
                    !DX11RendererTestAccess::Receipt(renderer).available,
                    "failed or rejected fresh draw cannot create a hold receipt");
            require(renderer.ConsumeDeviceLost() == (result == DXGI_ERROR_DEVICE_REMOVED),
                    "simulated device removal preserves the recovery latch");
        }
        require(submit(S_OK, true, true, true, false),
                "Present may succeed while a post-input draw missed the backbuffer");
        require(!DX11RendererTestAccess::Receipt(renderer).available,
                "successful Present alone cannot authorize frame retention");
        require(DX11RendererTestAccess::HasDeadline(renderer),
                "fresh uploads and frame hold retain an active cap deadline");
        const auto deadline = DX11RendererTestAccess::Deadline(renderer);
        renderer.InvalidateCaptureFrame();
        renderer.ForgetResyncFrame(CaptureFrameHoldReason::DeviceSwitch);
        renderer.SetPresentCap(cap.requestedHz);
        require(DX11RendererTestAccess::Deadline(renderer) == deadline &&
                renderer.GetPresentCapHz() == cap.effectiveHz,
                "session/hold invalidation and same cap do not reset the cap clock");
        require(submit(S_OK, false, false), "status-only frame still counts its successful Present");
        require(!DX11RendererTestAccess::Receipt(renderer).available,
                "status-only submission does not establish a fresh hold receipt");
        const auto beforeRecovery = counts;
        counts.captured = 2; counts.content = 1;
        require(submit(S_OK, true, true), "fresh recovery draw submits successfully");
        require(DX11RendererTestAccess::Receipt(renderer).compatible,
                "recovery establishes a new compatible receipt");
        rates = SampleFrameRates(counts, beforeRecovery, 1.0, true);
        require(rates.captureFps == 0 && rates.contentFps == 0 && rates.presentFps == 1 &&
                counts.presented == 5,
                "buffer counter resets stay safe while successful presents remain monotonic");
    }
    std::cout << "PASS actual EndFrame S_OK/non-S_OK, resync receipt, FPS and cap-clock integration\n";
}

} // namespace NitLink
