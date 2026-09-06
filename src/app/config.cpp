#include "config.h"
#include <fstream>
#include <filesystem>
#include <sstream>

// Minimal key=value parser. NOT real JSON despite the .json file extension:
// the format is flat "key = value" lines with one extension: dotted keys
// for per-game settings, like:
//
//   game.spider-man-2.nis_enabled = true
//   game.spider-man-2.color_expansion = false

namespace NitLink {

// Walk past leading whitespace and return what's left.
static std::string LStrip(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Strip trailing whitespace.
static std::string RStrip(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

static std::string Trim(const std::string& s) {
    return RStrip(LStrip(s));
}

static bool ParseBool(const std::string& v) {
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

// Safe numeric parsers: the config is hand-editable, so malformed or
// out-of-range values must fall back to `def` and clamp to [lo,hi] instead
// of throwing (std::sto*) or feeding a bogus size into allocation. Note
// std::stoul wraps a leading '-' instead of throwing, so reject it explicitly.
static uint32_t ParseU32(const std::string& v, uint32_t def, uint32_t lo, uint32_t hi) {
    try {
        if (!v.empty() && v[0] == '-') return def;
        unsigned long n = std::stoul(v);
        if (n < lo) return lo;
        if (n > hi) return hi;
        return static_cast<uint32_t>(n);
    } catch (...) { return def; }
}

static int ParseI32(const std::string& v, int def, int lo, int hi) {
    try {
        int n = std::stoi(v);
        if (n < lo) return lo;
        if (n > hi) return hi;
        return n;
    } catch (...) { return def; }
}

static float ParseFloatClamped(const std::string& v, float def, float lo, float hi) {
    try {
        float n = std::stof(v);
        if (n < lo) return lo;
        if (n > hi) return hi;
        return n;
    } catch (...) { return def; }
}

bool Config::Load(const std::string& path)
{
    if (!std::filesystem::exists(path)) {
        // First run -- create default config
        Save(path);
        return true;
    }

    std::ifstream file(path);
    if (!file.is_open()) return false;

    // Pre-loop accumulators for capture-format override parsing. Two
    // schemas are accepted: the legacy flat keys from rc2 (one anonymous
    // override) and the new per-device indexed keys (rc3+). Both feed
    // into the post-loop reconciliation below.
    bool sawLegacyOverride = false;
    CaptureFormatOverride legacyOverride;
    std::map<int, std::wstring>         indexedOverrideDevice;
    std::map<int, CaptureFormatOverride> indexedOverrides;

    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '/') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        // Per-game settings: "game.<id>.<setting> = value"
        // Split on dots and route into Config::gameSettings.
        if (key.rfind("game.", 0) == 0) {
            // After "game.", find the next dot to split id from setting name.
            auto dot = key.find('.', 5);
            if (dot == std::string::npos) continue;
            std::string gameId  = key.substr(5, dot - 5);
            std::string setting = key.substr(dot + 1);
            if (gameId.empty() || setting.empty()) continue;

            // Auto-create the entry if needed
            auto& gs = gameSettings[gameId];

            if      (setting == "nis_enabled")     gs.nisEnabled     = ParseBool(val);
            else if (setting == "color_expansion") gs.colorExpansion = ParseBool(val);
            continue;
        }

        // Global settings (existing schema)
        if (key == "window_width")    windowWidth  = ParseU32(val, windowWidth, 320, 16384);
        if (key == "window_height")   windowHeight = ParseU32(val, windowHeight, 240, 16384);
        if (key == "pip_width")       pipWidth     = ParseU32(val, pipWidth, 80, 16384);
        if (key == "pip_height")      pipHeight    = ParseU32(val, pipHeight, 45, 16384);
        if (key == "pip_opacity")     pipOpacity   = ParseFloatClamped(val, pipOpacity, 0.0f, 1.0f);
        if (key == "pip_x")           pipX         = ParseI32(val, pipX, -100000, 100000);
        if (key == "pip_y")           pipY         = ParseI32(val, pipY, -100000, 100000);
        if (key == "audio_volume")    audioVolume  = ParseFloatClamped(val, audioVolume, 0.0f, 1.0f);
        if (key == "audio_muted")     audioMuted   = ParseBool(val);
        if (key == "color_expansion") colorExpansion = ParseBool(val);
        if (key == "nis_enabled")     nisEnabled   = ParseBool(val);
        if (key == "nis_scale_mode")  nisScaleMode = ParseI32(val, nisScaleMode, 0, 2);
        if (key == "nis_sharpness")   nisSharpness = ParseFloatClamped(val, nisSharpness, 0.0f, 1.0f);
        if (key == "hdr_enabled")     hdrEnabled   = ParseBool(val);
        if (key == "hdr_auto_from_source") hdrAutoFromSource = ParseBool(val);
        if (key == "vrr_present_pacing") vrrPresentPacing = ParseBool(val);
        if (key == "low_latency")     lowLatency = ParseBool(val);
        if (key == "present_cap_hz") presentCapHz = ParseI32(val, presentCapHz, -1, 1000);
        if (key == "aspect_ratio")   aspectRatio  = val.substr(0, 16);
        if (key == "panel_side")     panelSide    = val.substr(0, 8);
        if (key == "panel_width")    panelWidth   = ParseI32(val, panelWidth, 320, 1200);
        if (key == "enable_shaders")  enableShaders = ParseBool(val);
        if (key == "show_overlay")    showOverlay   = ParseBool(val);
        if (key == "current_game")    currentGameId = val;
        if (key == "preferred_device") {
            // Capture-device friendly names produced by Windows are
            // ASCII in practice. Naive narrow-to-wide assignment is
            // sufficient for the device names this field stores; a
            // future change to handle non-ASCII names would route this
            // through MultiByteToWideChar (UTF-8 input expected).
            preferredDevice.assign(val.begin(), val.end());
        }

        // Capture format overrides. Two schemas accepted:
        //   Legacy (rc2): flat capture_override_{width,height,fps,format}
        //     keys, one anonymous override for the whole config.
        //   New (rc3+): indexed capture_override.<N>.{device,width,
        //     height,fps,format} keys per saved device. Each card
        //     remembers its own pick.
        // Both accumulate into pre-loop temps and are reconciled below.
        if (key == "capture_override_width") {
            legacyOverride.width = ParseU32(val, 0, 0, 16384);
            sawLegacyOverride = true;
        } else if (key == "capture_override_height") {
            legacyOverride.height = ParseU32(val, 0, 0, 16384);
            sawLegacyOverride = true;
        } else if (key == "capture_override_fps") {
            legacyOverride.fps = ParseU32(val, 0, 0, 1000);
            sawLegacyOverride = true;
        } else if (key == "capture_override_format") {
            // Format strings are always ASCII ("NV12" / "P010" / "BGRA"
            // / "") so the same narrow-to-wide convention as
            // preferred_device above is safe here.
            legacyOverride.format.assign(val.begin(), val.end());
            sawLegacyOverride = true;
        } else if (key.rfind("capture_override.", 0) == 0) {
            // capture_override.<N>.<field> = <value>
            // capture_override.count is parsed but ignored: the map's
            // contents post-loop are the source of truth for size.
            const auto remainder = key.substr(17);  // length of "capture_override."
            const auto dot = remainder.find('.');
            if (dot != std::string::npos) {
                try {
                    const int idx = std::stoi(remainder.substr(0, dot));
                    const std::string field = remainder.substr(dot + 1);
                    if (field == "device") {
                        // Device names are ASCII in practice (same
                        // convention as preferred_device above).
                        indexedOverrideDevice[idx].assign(val.begin(), val.end());
                    } else if (field == "width") {
                        indexedOverrides[idx].width = std::stoul(val);
                    } else if (field == "height") {
                        indexedOverrides[idx].height = std::stoul(val);
                    } else if (field == "fps") {
                        indexedOverrides[idx].fps = std::stoul(val);
                    } else if (field == "format") {
                        indexedOverrides[idx].format.assign(val.begin(), val.end());
                    }
                } catch (...) {
                    // Malformed index, skip the line silently.
                }
            }
        }
    }

    // Post-loop: transpose indexed overrides into the per-device map.
    // Entries without a device name are dropped (incomplete record).
    for (const auto& [idx, ov] : indexedOverrides) {
        const auto deviceIt = indexedOverrideDevice.find(idx);
        if (deviceIt == indexedOverrideDevice.end() || deviceIt->second.empty()) continue;
        captureFormatOverrides[deviceIt->second] = ov;
    }

    // Legacy migration: if pre-rc3 flat keys were present AND the new
    // indexed schema was empty, attribute the legacy override to the
    // currently preferred device. preferred_device is parsed earlier in
    // the file so it is already set by this point. If preferred_device
    // is empty, the legacy override is discarded (no device to attribute
    // it to).
    if (sawLegacyOverride && captureFormatOverrides.empty() && !preferredDevice.empty()) {
        captureFormatOverrides[preferredDevice] = legacyOverride;
    }

    return true;
}

bool Config::Save(const std::string& path)
{
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "# NitLink Configuration\n";
    file << "# https://github.com/nitlink-dev/nitlink\n\n";

    file << "# Window\n";
    file << "window_width = "  << windowWidth  << "\n";
    file << "window_height = " << windowHeight << "\n\n";

    file << "# Picture-in-Picture\n";
    file << "pip_width = "   << pipWidth   << "\n";
    file << "pip_height = "  << pipHeight  << "\n";
    file << "pip_opacity = " << pipOpacity << "\n";
    file << "pip_x = "       << pipX       << "\n";
    file << "pip_y = "       << pipY       << "\n\n";

    file << "# Audio\n";
    file << "audio_volume = " << audioVolume << "\n";
    file << "audio_muted = "  << (audioMuted ? "true" : "false") << "\n\n";

    file << "# Capture\n";
    file << "# Friendly name of the preferred capture device, e.g. \"Elgato 4K Pro\".\n";
    file << "# Empty falls back to the first Elgato device when present, otherwise to\n";
    file << "# the first device Media Foundation enumerates.\n";
    {
        // Explicit static_cast loop instead of iterator-pair construction
        // so MSVC does not flag the wchar_t -> char narrowing (C4244).
        // Capture-device friendly names produced by Windows are ASCII in
        // practice; non-ASCII names would need WideCharToMultiByte for
        // a proper UTF-8 round-trip.
        std::string narrow;
        narrow.reserve(preferredDevice.size());
        for (wchar_t wc : preferredDevice) {
            narrow.push_back(static_cast<char>(wc));
        }
        file << "preferred_device = " << narrow << "\n\n";
    }

    file << "# Capture format overrides (per device, F1 Source picker)\n";
    file << "# Schema: capture_override.<N>.<field> = <value>\n";
    file << "# Fields per entry: device, width, height, fps, format\n";
    file << "# Numeric fields at 0 (or empty for format) mean Auto.\n";
    file << "# Format value: NV12 / P010 / BGRA / (empty for Auto).\n";
    if (!captureFormatOverrides.empty()) {
        file << "capture_override.count = " << captureFormatOverrides.size() << "\n";
        int idx = 0;
        for (const auto& [device, ov] : captureFormatOverrides) {
            // Same narrowing convention as preferred_device. Device
            // names and format strings are ASCII in practice.
            std::string narrowDevice;
            narrowDevice.reserve(device.size());
            for (wchar_t wc : device) {
                narrowDevice.push_back(static_cast<char>(wc));
            }
            std::string narrowFormat;
            narrowFormat.reserve(ov.format.size());
            for (wchar_t wc : ov.format) {
                narrowFormat.push_back(static_cast<char>(wc));
            }
            file << "capture_override." << idx << ".device = " << narrowDevice << "\n";
            file << "capture_override." << idx << ".width = "  << ov.width  << "\n";
            file << "capture_override." << idx << ".height = " << ov.height << "\n";
            file << "capture_override." << idx << ".fps = "    << ov.fps    << "\n";
            file << "capture_override." << idx << ".format = " << narrowFormat << "\n";
            ++idx;
        }
    }
    file << "\n";

    file << "# Display\n";
    file << "color_expansion = " << (colorExpansion ? "true" : "false") << "\n\n";

    file << "# Image Upscaling (NIS)\n";
    file << "nis_enabled = "    << (nisEnabled ? "true" : "false") << "\n";
    file << "nis_scale_mode = " << nisScaleMode << "\n";
    file << "nis_sharpness = "  << nisSharpness << "\n\n";

    file << "# HDR\n";
    file << "hdr_enabled = " << (hdrEnabled ? "true" : "false") << "\n";
    file << "# When true, auto-detect HDR pipeline based on detected HDMI source\n";
    file << "# identifier on Elgato 4K S (e.g. PS5 -> assume HDR-capable, default to HDR).\n";
    file << "# Set false to keep classic config-driven behavior (hdr_enabled alone decides).\n";
    file << "hdr_auto_from_source = " << (hdrAutoFromSource ? "true" : "false") << "\n\n";

    file << "# VRR present pacing\n";
    file << "# When true, Present only fires on unique frames detected by the GPU\n";
    file << "# frame differ. Recommended only on G-Sync / FreeSync displays: the\n";
    file << "# monitor's VRR follows the source's actual unique-frame rate instead\n";
    file << "# of the Elgato's constant 60 Hz HDMI delivery rate.\n";
    file << "# On fixed-refresh displays, leave false: low-motion content like game\n";
    file << "# intros drops the differ-classified unique-frame rate to near zero,\n";
    file << "# and gating Present on that collapses visible cadence into the single\n";
    file << "# digits. Default false; toggle from the F1 settings panel.\n";
    file << "vrr_present_pacing = " << (vrrPresentPacing ? "true" : "false") << "\n\n";

    file << "# Low-latency present mode (Alt+L, default true)\n";
    file << "# When true, present each frame the instant it arrives for the\n";
    file << "# lowest input lag. When false, the frame is held after capture and\n";
    file << "# the swap-chain wait moves before present, so the picture is up to\n";
    file << "# one refresh older. The present is tearing-allowed either way;\n";
    file << "# false only adds input lag. Toggle from the F1 panel or with Alt+L.\n";
    file << "low_latency = " << (lowLatency ? "true" : "false") << "\n\n";

    file << "# Present-rate cap in Hz for the low-latency present (default 0)\n";
    file << "# 0 = automatic: monitor refresh minus 3, when that is at least the\n";
    file << "# source frame rate. 30-1000 = fixed cap. -1 = no cap.\n";
    file << "present_cap_hz = " << presentCapHz << "\n\n";

    file << "# Display aspect ratio: auto (source ratio), stretch (fill the\n";
    file << "# window), or a fixed ratio such as 4:3, 16:9, 16:10, 21:9.\n";
    file << "# Cycle with Alt+A or from the F1 panel.\n";
    file << "aspect_ratio = " << aspectRatio << "\n\n";

    file << "# F1 panel placement: right or left docks it beside the picture,\n";
    file << "# full covers the window. panel_width is in device-independent pixels.\n";
    file << "panel_side = " << panelSide << "\n";
    file << "panel_width = " << panelWidth << "\n\n";

    file << "# Shaders\n";
    file << "enable_shaders = " << (enableShaders ? "true" : "false") << "\n\n";

    file << "# Overlay\n";
    file << "show_overlay = " << (showOverlay ? "true" : "false") << "\n\n";

    // ===== Game state =====
    file << "# Current Game (empty if none selected)\n";
    file << "current_game = " << currentGameId << "\n\n";

    if (!gameSettings.empty()) {
        file << "# Per-Game Settings\n";
        file << "# Format: game.<game_id>.<setting> = value\n";
        for (const auto& [id, gs] : gameSettings) {
            file << "game." << id << ".nis_enabled = "     << (gs.nisEnabled ? "true" : "false") << "\n";
            file << "game." << id << ".color_expansion = " << (gs.colorExpansion ? "true" : "false") << "\n";
        }
        file << "\n";
    }

    return true;
}

} // namespace NitLink
