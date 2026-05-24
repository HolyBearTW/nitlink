#pragma once

#include "renderer/window.h"
#include "renderer/dx11_renderer.h"
#include "capture/capture_device.h"
#include "capture/device_enumerator.h"
#include "capture/frame_buffer.h"
#include "capture/frame_differ.h"
#include "capture/hdr_source_poller.h"
#include "capture/placeholder_detector.h"
#include "input/hotkey_manager.h"
#include "audio/audio_router.h"
#include "overlay/overlay.h"
#include "upscale/nis_upscaler.h"
#include "WebViewSettings.h"
#include "config.h"
#include "discord/discord_rpc.h"

#include <memory>
#include <string>
#include <atomic>
#include <chrono>

namespace NitLink {

class Application {
public:
    Application();
    ~Application();

    bool Initialize(HINSTANCE hInstance, int nCmdShow);
    void Run();
    void Shutdown();

    // Display mode control
    void ToggleFullscreen();
    void TogglePiP();
    void ToggleOverlay();
    void ToggleSettings();
    void TakeScreenshot();


    // State
    bool IsFullscreen() const { return m_isFullscreen; }
    bool IsPiP() const { return m_isPiP; }

private:
    void ProcessFrame();
    void UpdateTaskbarIcon(const std::wstring& iconPath);

    // Push current config + live stats to the WebView2 settings UI so its
    // toggles/sliders/labels reflect reality. Called on open, on hotkey-
    // driven state changes (Alt+H for HDR), and on the once-per-second
    // stats tick while the menu is visible.
    void PushSettingsState();

    // Game selector helpers. ApplyGameSettings loads the per-game settings
    // from Config::gameSettings[gameId] into the live Config and notifies
    // the renderer. UpdateDiscordForCurrentGame pushes the current game's
    // title/art-key to Rich Presence (or clears it when gameId is empty).
    void ApplyGameSettings(const std::string& gameId);
    void UpdateDiscordForCurrentGame();

    // Capture-side HDR/SDR reconciliation. Compares the format the capture
    // device is currently running (P010 vs BGRA/NV12) against what the
    // current combination of m_config->hdrEnabled, m_sourceIsHDR10, and
    // m_hdrDetectionAvailable says it SHOULD be. If they diverge, performs
    // a full StopCapture -> Close -> RequestP010(new) -> Open -> rebuild
    // FrameBuffer -> re-push renderer row-order/range hints -> StartCapture
    // cycle. Idempotent: safe to call every run-loop iteration; returns
    // immediately when no swap is needed.
    //
    // MUST be called on the main thread only. Internally joins the capture
    // worker, which would deadlock if called from the worker itself.
    //
    // Triggered by:
    //   - Alt+H (user toggles m_config->hdrEnabled)
    //   - HDR source poller (m_sourceIsHDR10 changes when the console
    //     switches between SDR and HDR output modes: see m_hdrPoller)
    //
    // Returns true if a format swap happened, false if the device was
    // already in the desired format or the swap failed (in which case
    // m_config->hdrEnabled is rolled back to match reality, same as the
    // renderer-side fallback below).
    //
    // When force=true, the no-op shortcut (wantP010 == isP010) is bypassed
    // and a full StopCapture -> Close -> Open -> StartCapture cycle runs
    // unconditionally. Used by the run loop when CaptureDevice signals
    // ConsumeNeedsReopen(): the capture worker has already exited after
    // a fatal stream condition (MF_SOURCE_READERF_ERROR, media-type change,
    // retry budget exhausted) and the current format may or may not need
    // to change, but the reader and worker thread must be rebuilt either
    // way.
    bool ReconcileCaptureFormat(bool force = false);

    // Switch the active capture device at runtime. The new device is
    // looked up by friendly name through
    // DeviceEnumerator::PickPreferredDevice (exact, then substring, then
    // the standard fallback chain). On a successful match this updates
    // m_currentDeviceInfo and m_config->preferredDevice, re-reads the
    // Elgato InfoFrame for the new device, then forces a full reconcile
    // so the rest of the pipeline (Elgato property + HID tonemap calls,
    // P010/NV12 negotiation with retry, framebuffer rebuild, frame
    // differ + placeholder detector reset, renderer state sync, capture
    // restart) runs against the new device through the existing
    // ReconcileCaptureFormat path.
    //
    // Returns true on success. Returns false and rolls back to the
    // previous device when:
    //   - no enumerated device matches deviceName, OR
    //   - the new device opens but reconcile reports failure, OR
    //   - some other internal state is inconsistent.
    //
    // No-op (and returns true) when deviceName already resolves to the
    // active device; the call still updates and persists the saved
    // preference in that case so the explicit user confirmation sticks.
    //
    // Runtime entry point for switching capture devices. The F1 settings
    // panel's Source row (driven by nitlink-menu.html from the
    // captureDevices and activeDevice fields in PushSettingsState) is
    // the primary caller, but the function is UI-agnostic: any consumer
    // that can route a friendly device name to this method dispatches
    // the same switch.
    bool SwitchCaptureDevice(const std::wstring& deviceName);

    // Signal-loss debounce. PS5 boot logos, source switches, and
    // SDR<->HDR handshakes all produce brief windows (typically 0.5 to
    // 2.0 s) where the HDMI link is renegotiating and no fresh frames
    // arrive. Flipping straight to the full-screen "HDMI Input Signal
    // Lost" page on these routine transitions is jarring and wrong: the
    // signal is fine, the link is just handshaking.
    //
    // The decision tracks elapsed time since the run loop last accepted
    // a real source frame (m_lastGoodFrameTime, bumped at the same site
    // as UpdateCaptureTexture). Commitment to the no-signal page only
    // happens after that elapsed time crosses the active grace window.
    // During grace the renderer keeps repainting the last good frame
    // out of m_captureTexture until UpdateCaptureTexture overwrites it.
    //
    // Three grace windows apply, selected per call from the current
    // state of the pipeline:
    //   - startup grace: shorter, so a user who launches with no source
    //     connected does not stare at a black window for the full
    //     reacquire window. Initialize seeds m_lastGoodFrameTime to
    //     "now" so this window measures from session start, not from
    //     the steady-clock epoch.
    //   - placeholder grace: tighter, used once the placeholder detector
    //     has confirmed an Elgato NO SIGNAL frame. The upstream source
    //     is gone by definition, so waiting the full reacquire window
    //     would be pointless.
    //   - reacquire grace: the default, sized to cover routine HDMI
    //     renegotiations.
    //
    // A separate reacquire debounce sits in front of the grace check.
    // While elapsed is below the debounce the function is silent: no
    // log, no latch change, no user-visible transition. This suppresses
    // routine sub-frame gaps where the renderer loop briefly outruns
    // the capture producer between writes.
    //
    // FrameBuffer::IsSignalActive() is deliberately not consulted here.
    // That predicate is content-based and toggles false on legitimate
    // low-motion playback (intro logos, slow fades, paused menus), which
    // would otherwise surface a paired reacquiring / reacquired log on
    // every ~500 ms hash cycle of static content.
    //
    // Stateful: m_signalReacquiringLogged and m_signalLostLogged track
    // whether the corresponding transition has already been surfaced for
    // the current loss episode, so each transition logs exactly once.
    // Call this once per run-loop iteration and cache the result for
    // the renderer-side checks.
    bool ShouldShowNoSignal();

    // True when motion (FrameDiffer non-duplicate frame) or content
    // (PlaceholderDetector classified Real) was observed inside
    // kMotionRecencyWindowMs of `now`. Single source of truth consulted
    // by the ConfirmedPlaceholder branch in the run loop AND by the
    // early-exit at the top of ShouldShowNoSignal. Takes `now` as a
    // parameter so both call sites can pass the same instant and the
    // gate sees the exact same clock value as the surrounding logic.
    // Does NOT consider m_hasEverReceivedFrame; callers wrap with that
    // guard when the startup grace path matters.
    bool RecentSourceActivity(std::chrono::steady_clock::time_point now) const;

    // Core components
    std::unique_ptr<Window>           m_window;
    std::unique_ptr<DX11Renderer>     m_renderer;
    std::unique_ptr<CaptureDevice>    m_captureDevice;
    std::unique_ptr<FrameBuffer>      m_frameBuffer;
    std::unique_ptr<HotkeyManager>    m_hotkeyManager;
    std::unique_ptr<AudioRouter>      m_audioRouter;
    std::unique_ptr<Overlay>          m_overlay;
    std::unique_ptr<NisUpscaler>       m_nisUpscaler;
    std::unique_ptr<FrameDiffer>       m_frameDiffer;
    std::unique_ptr<WebViewSettings>   m_webviewSettings;
    bool m_settingsVisible = false;
    std::unique_ptr<Config>           m_config;
    std::unique_ptr<DiscordRPC>       m_discord;

    // Background-thread monitor for HDMI source HDR<->SDR transitions.
    // Created in Initialize iff the Elgato HDR property GUID is readable
    // (4K Pro: yes, 4K S: no). On state change, run loop reads the new
    // state via AcceptUpdate, pushes to m_sourceIsHDR10 + renderer, and
    // calls ReconcileCaptureFormat to swap the capture pipeline.
    std::unique_ptr<HDRSourcePoller>  m_hdrPoller;

    // State
    std::atomic<bool> m_running{false};
    bool m_isFullscreen = false;
    bool m_isPiP = false;
    // Latched so "[NitLink/PiP] fullscreen toggle ignored while PiP is
    // active" only fires once per blocked attempt. Cleared on any
    // successful (non-PiP-blocked) ToggleFullscreen call.
    bool m_pipFullscreenBlockedLogged = false;

    // Transient toast notification. ShowToast() sets these; the run loop
    // draws Overlay::DrawToast while now < m_toastExpiry. Single-slot: a
    // new ShowToast() supersedes any previous toast. Used for short user-
    // facing status messages like "Windows HDR not engaged" so users
    // don't have to read DebugView to see why a hotkey appeared to do
    // nothing.
    std::wstring                          m_toastText;
    std::chrono::steady_clock::time_point m_toastExpiry{};
    void ShowToast(const std::wstring& text,
                   std::chrono::milliseconds duration = std::chrono::milliseconds(4000));

    bool m_showOverlay = false;
    // True if nitlink.json didn't exist when Initialize ran. Used to
    // pop the settings menu open automatically on first launch so users
    // can discover the feature toggles without hunting for F1.
    bool m_firstLaunch = false;
    std::atomic<bool> m_screenshotRequested{false};

    // Path of the most recent screenshot saved by TakeScreenshot,
    // surfaced to the WebView2 toast via PushSettingsState. One-shot:
    // cleared after emission so the same toast does not re-fire on
    // subsequent state pushes.
    std::wstring m_lastScreenshotPath;

    // Auto-detected HDR source state. Populated initially by Initialize via
    // the Elgato HDR InfoFrame read; updated at runtime by ReconcileCaptureFormat
    // when the source changes (PS5 switching between SDR and HDR menus, an
    // HDR game launching from an SDR dashboard, etc). When true AND the user
    // has the HDR toggle on, the pipeline negotiates P010 capture and uses
    // the HDR10 shader path. When false, the pipeline stays on BGRA + SDR.
    bool m_sourceIsHDR10 = false;

    // True when the Elgato HDR InfoFrame property GUID was readable at
    // Initialize. Captured once and remembered so ReconcileCaptureFormat
    // applies the same "trust the user's hdr_enabled config when detection
    // is unavailable" fallback that Initialize used. The 4K Pro sets this
    // to true; the 4K S sets it to false (no property GUID over USB).
    bool m_hdrDetectionAvailable = false;

    // The capture device opened in Initialize, cached so ReconcileCaptureFormat
    // can re-Open the same device after a format-change teardown without
    // re-enumerating. Reconcile still re-enumerates as a primary path
    // (handles user-replug edge cases) and falls back to this cached copy
    // if enumeration returns empty mid-session.
    DeviceInfo m_currentDeviceInfo{};

    // Performance tracking
    double m_captureLatencyMs = 0.0;
    double m_renderLatencyMs = 0.0;
    uint32_t m_currentFps        = 0;   // HDMI signal rate (~60 healthy)
    uint32_t m_currentContentFps = 0;   // game unique-frame rate (from differ)
    uint64_t m_uniqueFrameCount  = 0;
    uint64_t m_skippedFrameCount = 0;   // frames skipped by VRR present pacing (total)
    uint32_t m_consecutiveSkips  = 0;   // consecutive skips: used to force a Present
                                         // periodically so DWM doesn't mark the app unresponsive

    // Session timing: when the app started, used for screenshot
    // filenames and any future "session uptime" telemetry.
    std::chrono::system_clock::time_point m_sessionStartTime;

    // Signal-loss debounce state. See ShouldShowNoSignal() above for the
    // full rationale. m_lastGoodFrameTime is bumped in the run loop right
    // after every successful FrameBuffer::Read; until then it carries the
    // value Initialize set it to (steady_clock::now() at startup), which
    // is what the startup grace measures from.
    std::chrono::steady_clock::time_point m_lastGoodFrameTime{};
    bool m_hasEverReceivedFrame      = false;
    bool m_signalReacquiringLogged   = false;
    bool m_signalLostLogged          = false;

    // Elgato placeholder-frame detection.
    //
    // When HDMI is unplugged or during some handshake states the Elgato
    // emits its own NO SIGNAL placeholder as a valid frame stream. Those
    // frames pass FrameBuffer::Read just like real ones, so the existing
    // no-signal debounce never trips. PlaceholderDetector recognizes them
    // by content fingerprint (3x3 zone-luma signature, format-aware) and
    // requires multiple consecutive matches before declaring placeholder,
    // so genuinely static real content (paused game, static menu, black
    // scene transition, splash screen) does NOT get suppressed unless its
    // fingerprint actually matches a baked-in Elgato signature within a
    // small tolerance across all 9 zones. See placeholder_detector.h.
    std::unique_ptr<PlaceholderDetector> m_placeholderDetector;

    // Latched for log-once-on-transition into / out of the placeholder
    // branch in the run loop.
    bool m_inPlaceholderState = false;

    // Motion-recency suppression for false-positive ConfirmedPlaceholder.
    //
    // PlaceholderDetector matches frames by 9-zone luma fingerprint, but
    // some real content (Star Wars Jedi intro studio logo cards, other
    // dark-centered-logo splash screens) produces fingerprints pixel-
    // identical to the baked Elgato P010 placeholder for 2-3 seconds at
    // a time. Without an additional discriminator the 15-frame streak
    // completes and the no-signal logic false-triggers on a healthy
    // source.
    //
    // Discriminator: was there REAL ACTIVITY (motion OR non-placeholder
    // content) within kMotionRecencyWindowMs? A real disconnect is
    // preceded by silence: game content stops AND motion stops, because
    // the only thing arriving from the capture device is the static
    // placeholder. A game intro logo card is preceded by movement (the
    // prior animation) or by other non-placeholder content (the prior
    // studio card transition).
    //
    // m_lastMotionTime  : last time FrameDiffer reported a non-duplicate
    //                     frame (isNewFrame=true). Goes stale during a
    //                     paused game (60Hz of pixel-identical dupes
    //                     produces isNewFrame=false on every iteration).
    // m_lastContentTime : last time PlaceholderDetector classified a
    //                     frame as Real (not Candidate, not Confirmed).
    //                     Stays fresh during a paused game because the
    //                     paused frame still arrives at 60Hz and (unless
    //                     the paused screen happens to match a baked
    //                     placeholder fingerprint) is classified Real.
    //
    // The gate uses max(lastMotion, lastContent): either signal proves
    // the HDMI source is still alive. ConfirmedPlaceholder is honored
    // only when BOTH have gone stale for kMotionRecencyWindowMs.
    //
    // Both are initialized to steady_clock::now() at session start so
    // the very first frames don't trip the gate as stale; see
    // application.cpp Initialize().
    std::chrono::steady_clock::time_point m_lastMotionTime{};
    std::chrono::steady_clock::time_point m_lastContentTime{};

    // Window the motion-recency gate considers "recent". 5000 ms after
    // measurement: Star Wars Jedi's Lucasfilm logo card holds for ~2-3
    // seconds at a time, and 2000 ms left the gate expiring mid-card,
    // producing a brief "Signal: lost (reason=placeholder)" flash before
    // the card transitioned off and gameplay resumed. 5000 ms comfortably
    // covers any single static intro card a game is likely to display
    // (Respawn, Lucasfilm, EA, Unreal Engine, etc. all hold for under
    // 4 seconds individually) while still detecting a real HDMI unplug
    // in 5-6 seconds (5s gate + 1s kPlaceholderGrace below in
    // ShouldShowNoSignal). The placeholder is shown by Elgato firmware
    // for many seconds during a handshake gap, so 6-second detection
    // latency is invisible in practice and provides natural tolerance
    // for momentary cable wiggles.
    static constexpr int kMotionRecencyWindowMs = 5000;

    // Debug instrumentation: when Ctrl+F5 fires this flag is set; the next
    // successful frame's fingerprint is logged once and the flag clears.
    // Used to capture live Elgato placeholder signatures on hardware so
    // they can be baked into placeholder_detector.cpp's known list.
    bool m_dumpFingerprintRequested = false;

};

} // namespace NitLink
