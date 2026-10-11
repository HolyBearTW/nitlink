#include "renderer/dx11_renderer.h"
#include "app/present_cap_policy.h"
#include "renderer/present_cap_clock.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace NitLink {
struct DX11RendererTestAccess {
    static void Initialize(DX11Renderer& renderer)
    {
        // An offscreen WARP target: no capture card, visible window, monitor
        // mode change or driver profile is involved in this regression test.
        D3D_FEATURE_LEVEL featureLevel{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &renderer.m_device, &featureLevel, &renderer.m_context))) {
            throw std::runtime_error("cannot create WARP fixture");
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 8;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
        if (FAILED(renderer.m_device->CreateTexture2D(&desc, nullptr, &target)) ||
            FAILED(renderer.m_device->CreateRenderTargetView(target.Get(), nullptr, &renderer.m_rtv))) {
            throw std::runtime_error("cannot create offscreen fixture target");
        }
        renderer.m_windowWidth = renderer.m_windowHeight = 8;
        // A signaled queue-ready event deliberately does not limit refresh.
        renderer.m_frameLatencyWaitable = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!renderer.m_frameLatencyWaitable) throw std::runtime_error("cannot create wait fixture event");
    }
    static void StartInterval(DX11Renderer& renderer)
    {
        const auto interval = std::chrono::nanoseconds(static_cast<long long>(
            1.0e9 / (renderer.GetPresentCapHz() > 0.0 ? renderer.GetPresentCapHz() : 95.0)));
        renderer.m_presentCapDeadline = std::chrono::steady_clock::now() + interval;
    }
    static void PinMarker(DX11Renderer& renderer, bool pinned, double hz)
    {
        renderer.m_presentCapFromMarker = pinned;
        if (pinned) renderer.m_vrrCapHz = hz;
    }
    static bool HasDeadline(const DX11Renderer& renderer)
    {
        return renderer.m_presentCapDeadline != std::chrono::steady_clock::time_point{};
    }
    static auto Deadline(const DX11Renderer& renderer)
    {
        return renderer.m_presentCapDeadline;
    }
};
}

namespace {
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace NitLink;
    try {
        using Clock = std::chrono::steady_clock;
        const auto now = Clock::time_point{} + std::chrono::seconds(100);
        const auto interval = std::chrono::milliseconds(10);
        Require(AdvancePresentCapDeadline({}, now, interval) == now + interval,
                "first release establishes a rate-derived deadline");
        Require(AdvancePresentCapDeadline(now, now, interval) == now + interval,
                "an on-time completed wait retains the configured interval");
        Require(AdvancePresentCapDeadline(now, now - std::chrono::milliseconds(1), interval) == now + interval,
                "an early completion cannot release before the pending deadline");
        Require(AdvancePresentCapDeadline(now, now + std::chrono::milliseconds(1), interval)
                    == now + interval + std::chrono::milliseconds(1),
                "one millisecond of lateness does not shorten the next interval");
        Require(AdvancePresentCapDeadline(now, now + interval, interval) == now + 2 * interval,
                "a full missed interval starts a fresh interval without catch-up");
        Require(AdvancePresentCapDeadline(now, now + 3 * interval, interval) == now + 4 * interval,
                "a missed deadline rebases without catch-up bursts");
        auto deadline = now;
        for (int frame = 1; frame <= 120; ++frame) {
            const auto completedAt = deadline + std::chrono::milliseconds(1);
            deadline = AdvancePresentCapDeadline(deadline, completedAt, interval);
            Require(deadline - completedAt == interval,
                    "every late completion leaves a full interval before the next release");
            Require(deadline == now + frame * (interval + std::chrono::milliseconds(1)),
                    "continuous lateness cannot accumulate catch-up debt");
        }
        // Useful rendering can finish before the next slot without adding
        // its duration to the cap interval; the subsequent wait ends at
        // that slot and schedules another full interval.
        const auto release = now;
        deadline = AdvancePresentCapDeadline({}, release, interval);
        const auto renderFinishedAt = release + std::chrono::milliseconds(2);
        Require(deadline - renderFinishedAt == std::chrono::milliseconds(8),
                "render work remains inside the interval instead of lowering the cap");
        Require(AdvancePresentCapDeadline(deadline, deadline, interval) == release + 2 * interval,
                "on-time releases maintain the configured cadence across frames");
        const auto changedInterval = std::chrono::milliseconds(5);
        Require(AdvancePresentCapDeadline({}, now, changedInterval) == now + changedInterval,
                "a reset clock starts with the changed cap interval");
        DX11Renderer renderer;
        DX11RendererTestAccess::Initialize(renderer);
        renderer.SetPresentCap(30.0);
        DX11RendererTestAccess::StartInterval(renderer);
        renderer.SetPresentCap(30.0);
        Require(DX11RendererTestAccess::HasDeadline(renderer),
                "reapplying the same cap must not restart its clock");
        renderer.SetPresentCap(95.0);
        Require(!DX11RendererTestAccess::HasDeadline(renderer),
                "a cap change clears the old interval instead of waiting at the old rate");
        DX11RendererTestAccess::StartInterval(renderer);
        renderer.SetPresentCap(0.0);
        Require(!DX11RendererTestAccess::HasDeadline(renderer),
                "turning the cap off clears its pending interval");
        renderer.SetPresentCap(95.0);
        Require(!DX11RendererTestAccess::HasDeadline(renderer),
                "re-enabling the cap cannot restore the obsolete deadline");
        const auto restartedAt = Clock::now();
        renderer.WaitForFrameReady();
        const auto restartCompletedAt = Clock::now();
        const auto restartedDeadline = DX11RendererTestAccess::Deadline(renderer);
        const auto restartedInterval = std::chrono::nanoseconds(
            static_cast<long long>(1.0e9 / renderer.GetPresentCapHz()));
        Require(restartedDeadline >= restartedAt + restartedInterval
                    && restartedDeadline <= restartCompletedAt + restartedInterval,
                "the first wait after re-enabling starts a fresh rate-derived deadline");
        for (bool lowLatency : {false, true}) {
            for (bool vsync : {false, true}) {
                for (bool marker : {false, true}) {
                    for (double cap : {0.0, 95.0}) {
                        renderer.ConsumePhaseTimes();
                        DX11RendererTestAccess::PinMarker(renderer, marker, 141.0);
                        renderer.SetVSync(vsync);
                        Require(renderer.SetPresentCap(cap) == !marker, "SetPresentCap preserves marker priority");
                        Require(renderer.GetPresentCapHz() == (marker ? 141.0 : cap), "renderer stores the actual cap");
                        DX11RendererTestAccess::StartInterval(renderer);
                        const auto pendingDeadline = DX11RendererTestAccess::Deadline(renderer);
                        const auto start = std::chrono::steady_clock::now();
                        if (lowLatency) renderer.WaitForFrameReady();
                        renderer.BeginFrame(!lowLatency);
                        const double elapsedMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start).count();
                        const auto phases = renderer.ConsumePhaseTimes();
                        const bool capped = !vsync && (marker || cap > 0.0);
                        Require(phases.iterations == 1, "both wait locations count exactly one wait");
                        Require(phases.capWaits == (capped ? 1u : 0u),
                                "BeginFrame and low-latency waits both enforce the effective cap");
                        Require(phases.waitableWaits == (capped ? 0u : 1u),
                                "VSync/Off retain the waitable path without software cap");
                        Require(phases.waitTimeouts == 0 && phases.waitFailed == 0 && phases.waitAlerted == 0,
                                "a signaled waitable succeeds without timeout");
                        if (capped) Require(elapsedMs >= 0.8 * 1000.0 / renderer.GetPresentCapHz(),
                                            "a ready waitable cannot bypass a configured rate interval");
                        if (capped) {
                            const auto nextDeadline = DX11RendererTestAccess::Deadline(renderer);
                            const auto capInterval = std::chrono::nanoseconds(
                                static_cast<long long>(1.0e9 / renderer.GetPresentCapHz()));
                            Require(nextDeadline >= pendingDeadline + capInterval,
                                    "both low-latency wait locations retain a full interval after the slot");
                        }
                        Require(renderer.ConsumePhaseTimes().iterations == 0, "phase counters consume once");
                    }
                }
            }
        }
        renderer.ConsumePhaseTimes();
        renderer.BeginFrame(false);
        Require(renderer.ConsumePhaseTimes().iterations == 0,
                "source/capture pacing does not acquire a second refresh wait");
        std::cout << "PASS offscreen WARP cap, Low Latency, VSync, Off and marker wait paths\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
