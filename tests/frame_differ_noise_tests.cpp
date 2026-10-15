#include "capture/frame_differ.h"
#include "app/frame_rate_stats.h"
#include "app/source_cadence.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace NitLink;
using Microsoft::WRL::ComPtr;

namespace {
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Fixture {
    static constexpr unsigned width = 640, height = 360;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> input;
    ComPtr<ID3D11Query> completed;
    FrameDiffer differ;
    SourceCadence cadence;
    DXGI_FORMAT format;

    explicit Fixture(DXGI_FORMAT kind) : format(kind) {
        Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "WARP device");
        Require(differ.Initialize(device.Get()), "real FrameDiffer initializes");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
            SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &input)), "input texture");
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        Require(SUCCEEDED(device->CreateQuery(&query, &completed)), "completion query");
    }

    enum class Content { Noisy30, Exact30, True60, Slow60, Static, TileMotion };
    bool Frame(Content content, unsigned frame) {
        // Equivalent luma delivered as NV12's R8 plane, P010's R16 plane,
        // and SDR BGRA. A distributed +1/255 perturbation creates nonexact
        // repeats near 0.00065, below tile rescue but above the old threshold.
        const unsigned bpp = format == DXGI_FORMAT_R8_UNORM ? 1 :
            format == DXGI_FORMAT_R16_UNORM ? 2 : 4;
        std::vector<unsigned char> pixels(width * height * bpp);
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            unsigned value = 64;
            if (content == Content::Noisy30 || content == Content::Exact30)
                value += (frame / 2 % 2) * 8;
            if (content == Content::Noisy30 && frame % 2 && x % 6 == 0) ++value;
            if (content == Content::True60) value += (frame % 2) * 8;
            if (content == Content::Slow60) {
                constexpr unsigned levels[] = {0, 1, 9, 8};
                value += levels[frame % 4];
            }
            if (content == Content::TileMotion && x < 80 && y < 45) value += (frame % 2) * 4;
            auto* p = pixels.data() + (y * width + x) * bpp;
            p[0] = static_cast<unsigned char>(value);
            if (bpp == 2) p[1] = p[0]; // value * 257, full-range R16 luma sample
            if (bpp == 4) { p[1] = p[2] = p[0]; p[3] = 255; }
        }
        context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), width * bpp, 0);
        differ.Process(context.Get(), input.Get());
        context->End(completed.Get()); context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        HRESULT hr;
        // Waiting is confined to this offscreen test. Production readback
        // remains nonblocking and unmodified.
        while ((hr = context->GetData(completed.Get(), nullptr, 0, 0)) == S_FALSE &&
               std::chrono::steady_clock::now() < deadline) Sleep(1);
        Require(hr == S_OK, "WARP command completion");
        const bool fresh = differ.WasPreviousFrameNew();
        if (differ.HasFrameClassification()) cadence.OnClassifiedFrame(fresh);
        else cadence.OnUnclassifiedFrame();
        const bool present = fresh || cadence.ShouldHold();
        if (present) cadence.OnPresent();
        return present;
    }
    void Reset() {
        differ.Reset(); cadence.Reset();
        Require(!differ.NoiseFloorValid() && !differ.HasFrameClassification() &&
                differ.WasPreviousFrameNew() && cadence.CadenceFrames() == 1,
                "capture reset clears adaptive state and conservatively restarts pacing");
    }
};
}

int main() {
    try {
        for (auto format : {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
            Fixture f(format);
            for (auto content : {Fixture::Content::Noisy30, Fixture::Content::Exact30,
                    Fixture::Content::True60, Fixture::Content::Slow60, Fixture::Content::Static}) {
                f.Reset(); FrameRateCounters counters;
                for (unsigned i = 0; i < 120; ++i) {
                    const bool present = f.Frame(content, i);
                    if (i >= 60) {
                        Require(f.differ.HasFrameClassification(), "completed GPU comparison is available");
                        ++counters.captured;
                        counters.content += f.differ.WasPreviousFrameNew();
                        counters.presented += present;
                    }
                }
                const auto stats = SampleFrameRates(counters, {}, 1.0, true);
                const bool half = content == Fixture::Content::Noisy30 || content == Fixture::Content::Exact30;
                const unsigned expected = half ? 30 : content == Fixture::Content::Static ? 0 : 60;
                Require(stats.captureFps == 60 && stats.HudFps() == expected && stats.HudFpsAvailable(),
                        "actual GPU classifier feeds the correct content FPS/HUD independently of capture");
                Require(f.differ.NoiseFloorValid() == (content == Fixture::Content::Noisy30),
                        "only nonexact alternating repeats train the floor");
                if (half) Require(stats.presentFps == 30 && f.cadence.CadenceFrames() == 2,
                        "Source Frame Rate pacing presents 30 of 60 and retains cadence 2");
                if (content == Fixture::Content::True60 || content == Fixture::Content::Slow60)
                    Require(stats.presentFps == 60, "real 60 FPS and small motion keep every present");
                std::cout << "PASS WARP format=" << format << " case=" << int(content)
                          << " capture/content/present=" << stats.captureFps << '/'
                          << stats.contentFps << '/' << stats.presentFps << '\n';
            }
            // Tile rescue must still promote localized motion after a static
            // stretch even though the global mean is below the base threshold.
            for (unsigned i = 0; i < 80; ++i) {
                f.Frame(Fixture::Content::TileMotion, i);
                if (i > 20) Require(f.differ.WasPreviousFrameNew(), "tile rescue survives adaptive port");
            }
            f.Reset();
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
