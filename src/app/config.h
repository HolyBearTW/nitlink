#pragma once

#include <map>
#include <string>
#include <cstdint>

namespace NitLink {

// Per-game settings overlay. When a user picks a game from the dropdown,
// THIS subset of the main Config is saved under that game's id. Next time
// they select the same game, these values override the global defaults.
//
// Why only a subset and not the full Config? Because window size,
// audio device, hotkey state, etc. are per-user-machine, not per-game.
// What changes per game is the picture pipeline.
struct GameSettings {
    bool        nisEnabled     = false;
    bool        colorExpansion = false;
};

// Capture format override: user-selected resolution / fps / pixel format
// from the F1 Source picker. Each numeric field at 0 means "Auto" for that
// dimension; empty format string means "Auto" for format. All-Auto means
// full automatic negotiation (the default).
//
// The override is best-effort: if the requested combination is unavailable
// for the live source, CaptureDevice falls back to Auto and surfaces a
// toast notification via the JSON state push. The override is preserved in
// config across that fallback so the next source change retries it; the
// user has to explicitly switch a dropdown back to Auto to remove it.
struct CaptureFormatOverride {
    uint32_t     width  = 0;
    uint32_t     height = 0;
    uint32_t     fps    = 0;
    std::wstring format;  // "NV12" / "P010" / "BGRA" / "" for Auto

    bool isFullAuto() const {
        return width == 0 && height == 0 && fps == 0 && format.empty();
    }
};

struct Config {
    // Window
    uint32_t windowWidth  = 1920;
    uint32_t windowHeight = 1080;
    
    // PiP mode
    uint32_t pipWidth   = 480;
    uint32_t pipHeight  = 270;
    float    pipOpacity = 0.9f;
    // Persisted PiP top-left screen coordinates. -1 / -1 means "no saved
    // position": Window::SetPiP falls back to the monitor's bottom-right
    // default. Updated each time the user nudges the PiP window via the
    // Ctrl/Ctrl+Shift+Arrow hotkeys; written to disk on Application::Shutdown
    // through the existing Config::Save path.
    int32_t  pipX       = -1;
    int32_t  pipY       = -1;
    
    // Audio
    std::wstring audioOutputDevice = L""; // Empty = system default
    float        audioVolume       = 1.0f;
    bool         audioMuted        = false;

    // Capture
    std::wstring preferredDevice = L""; // Empty = first available

    // Manual capture format overrides, keyed by capture device name.
    // Each card remembers its own pick (4K Pro at 4K, 4K S at 1080p+240,
    // etc.). Missing entry = all-Auto = full automatic negotiation.
    // Lookup via GetOverride() below; write via captureFormatOverrides[name].
    std::map<std::wstring, CaptureFormatOverride> captureFormatOverrides;

    // Read-only lookup for the per-device override map. Returns the
    // saved override for `deviceName`, or all-Auto defaults if no entry
    // exists. Does NOT insert. Use captureFormatOverrides[name]
    // directly when writing.
    CaptureFormatOverride GetOverride(const std::wstring& deviceName) const {
        auto it = captureFormatOverrides.find(deviceName);
        if (it == captureFormatOverrides.end()) return {};
        return it->second;
    }

    // Display pipeline knobs
    bool         colorExpansion  = false;  // Limited (16-235) -> full (0-255). Default OFF: PS5 over HDMI typically sends full range, and applying expansion to full-range data crushes blacks. Toggle ON only if your blacks look gray.

    // HDR output: when enabled and the display supports HDR, the renderer
    // switches to FP16 backbuffer with scRGB colorspace.
    bool         hdrEnabled      = false;

    // HDR auto-from-source: fallback heuristic when direct HDR signal
    // detection is unavailable (HID error, MCU non-responsive, or a
    // device family without a detection path). When the connected
    // source identifier matches a known HDR-capable console (PS5,
    // future Xbox Series X|S, Switch 2), default the pipeline to HDR
    // even without a direct signal-state confirmation. User can still
    // Alt+H to switch to SDR for the current session. Set to false to
    // keep purely config-driven behavior (hdrEnabled alone decides).
    bool         hdrAutoFromSource = true;

    // VRR present pacing.
    // When ON: Present is gated by the GPU frame differ. With Independent
    //   Flip + ALLOW_TEARING active (both set on the swap chain), the
    //   monitor's VRR (G-Sync / FreeSync) follows the actual source unique-
    //   frame rate instead of the Elgato's constant 60 Hz HDMI delivery
    //   rate. Verified on LG C3 OLED via the C3's Game Dashboard refresh-
    //   rate indicator.
    // When OFF: Present every iteration. Same end-to-end pipeline, no
    //   differ-driven skip.
    //
    // Default is OFF. ON is unsafe on fixed-refresh displays: low-motion
    // game intros and splash screens drop the differ-classified unique-
    // frame rate to near zero, so gating Present on that signal collapses
    // visible cadence into the single digits and looks like stutter to
    // the user. The toggle is exposed in the F1 settings panel so users
    // on G-Sync / FreeSync panels can opt in.
    bool         vrrPresentPacing = false;

    // Low-latency present mode (Alt+L, default ON). When ON: wait the swap
    // chain at the top of the loop, then read the freshest captured frame and
    // present it on arrival for the lowest input lag. When OFF: the frame is
    // read first and the swap-chain wait moves into BeginFrame, so the held
    // frame ages up to one refresh before it is presented. The present stays
    // tearing-allowed either way; OFF only adds input lag. Default ON because
    // lowest latency is the point.
    bool         lowLatency = true;

    // (hdrMode string field stripped: Reference/Vibrant was an earlier
    //  fake-HDR pipeline. The current path negotiates real HDR10 via
    //  P010 capture, with the Elgato hardware tone-map disabled at
    //  startup through IKsPropertySet on the 4K Pro and a vendor HID
    //  Output Report on the 4K S.)

    // Image upscaling (NIS)
    bool         nisEnabled      = false;
    int          nisScaleMode    = 2;     // 0=1.5x, 1=2x, 2=Match window (NIS supports 1x..2x only)
    float        nisSharpness    = 0.5f;  // 0..1
    
    // Shader pipeline
    bool         enableShaders  = false;
    std::wstring shaderProfile  = L"default";
    
    // Performance overlay
    bool showOverlay = false;
    
    // PSN (for game detection)
    std::string psnNpsso = ""; // Auth token
    
    // Hotkeys
    bool enableGlobalHotkeys = true;

    // ===== Per-game settings =====
    //
    // The currently selected game id (matches a GameEntry::id from
    // game_database.h), or empty string if no game is selected.
    // When non-empty AND gameSettings[currentGameId] exists, those values
    // override the global nisEnabled/colorExpansion above when
    // applied at startup or game-change time.
    std::string  currentGameId = "";

    // Per-game settings keyed by game id. Persisted in the config file as
    // flat keys: "game.spider-man-2.nis_enabled = true", etc.
    std::map<std::string, GameSettings> gameSettings;

    bool Load(const std::string& path);
    bool Save(const std::string& path);
};

} // namespace NitLink
