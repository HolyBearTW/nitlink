#include "app/frame_rate_stats.h"
#include "capture/frame_differ.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void ObserveStaticContent(NitLink::FrameDiffer& differ, ID3D11DeviceContext* context,
                          ID3D11ShaderResourceView* input)
{
    for (unsigned int attempt = 0; attempt < 500; ++attempt) {
        differ.Process(context, input);
        context->Flush();
        if (differ.HasFrameClassification()) {
            Require(!differ.WasPreviousFrameNew(),
                    "an observed identical-image comparison measures static content");
            const auto rates = NitLink::SampleFrameRates({120, 0, 117}, {}, 1.0, true);
            Require(rates.HudFpsAvailable() && rates.HudFps() == 0 &&
                    rates.captureFps == 120 && rates.presentFps == 117,
                    "observed static content is a known zero, independent of other rates");
            return;
        }
        Sleep(1); // Test-only allowance for WARP's asynchronous command execution.
    }
    throw std::runtime_error("WARP did not produce a content classification within 500 attempts");
}

} // namespace

int main()
{
    using Microsoft::WRL::ComPtr;
    using namespace NitLink;
    try {
        FrameDiffer differ;
        Require(!differ.HasFrameClassification() && differ.WasPreviousFrameNew(),
                "startup telemetry is Unknown while source pacing stays conservative");
        differ.Process(nullptr, nullptr);
        Require(!differ.HasFrameClassification(), "uninitialized Process cannot invent a measurement");

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           nullptr, 0, D3D11_SDK_VERSION,
                                           &device, &level, &context)),
                "WARP device creation must succeed");
        Require(differ.Initialize(device.Get()), "WARP differ initialization must succeed");
        Require(!differ.HasFrameClassification(), "GPU initialization alone is not a measurement");

        constexpr unsigned int width = 8;
        constexpr unsigned int height = 8;
        std::array<unsigned int, width * height> black{};
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA pixels{};
        pixels.pSysMem = black.data();
        pixels.SysMemPitch = width * sizeof(unsigned int);
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> input;
        Require(SUCCEEDED(device->CreateTexture2D(&description, &pixels, &texture)) &&
                SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &input)),
                "static input texture and SRV creation must succeed");

        for (int startupFrame = 0; startupFrame < 2; ++startupFrame) {
            differ.Process(context.Get(), input.Get());
            context->Flush();
            Require(!differ.HasFrameClassification() && differ.WasPreviousFrameNew(),
                    "warmup does not turn conservative pacing into a measured content FPS");
        }
        ObserveStaticContent(differ, context.Get(), input.Get());
        differ.Process(context.Get(), nullptr);
        Require(!differ.HasFrameClassification() && !differ.WasPreviousFrameNew(),
                "missing input invalidates telemetry without replacing the pacing verdict");

        differ.Reset();
        Require(!differ.HasFrameClassification() && differ.WasPreviousFrameNew(),
                "stream reset is Unknown and retains the conservative fresh-frame pacing guard");
        for (int resetFrame = 0; resetFrame < 2; ++resetFrame) {
            differ.Process(context.Get(), input.Get());
            context->Flush();
            Require(!differ.HasFrameClassification() && differ.WasPreviousFrameNew(),
                    "old-stream readbacks cannot become new-stream measurements");
        }
        ObserveStaticContent(differ, context.Get(), input.Get());
        differ.Shutdown();
        Require(!differ.HasFrameClassification(), "shutdown makes content estimation Unknown");
        std::cout << "PASS WARP content availability, observed static zero, warmup/reset and pacing verdicts\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
