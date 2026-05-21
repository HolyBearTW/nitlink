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

bool Config::Load(const std::string& path)
{
    if (!std::filesystem::exists(path)) {
        // First run -- create default config
        Save(path);
        return true;
    }

    std::ifstream file(path);
    if (!file.is_open()) return false;

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
        if (key == "window_width")    windowWidth  = std::stoul(val);
        if (key == "window_height")   windowHeight = std::stoul(val);
        if (key == "pip_width")       pipWidth     = std::stoul(val);
        if (key == "pip_height")      pipHeight    = std::stoul(val);
        if (key == "pip_opacity")     pipOpacity   = std::stof(val);
        if (key == "pip_x")           pipX         = std::stoi(val);
        if (key == "pip_y")           pipY         = std::stoi(val);
        if (key == "audio_volume")    audioVolume  = std::stof(val);
        if (key == "audio_muted")     audioMuted   = ParseBool(val);
        if (key == "color_expansion") colorExpansion = ParseBool(val);
        if (key == "nis_enabled")     nisEnabled   = ParseBool(val);
        if (key == "nis_scale_mode")  nisScaleMode = std::stoi(val);
        if (key == "nis_sharpness")   nisSharpness = std::stof(val);
        if (key == "hdr_enabled")     hdrEnabled   = ParseBool(val);
        if (key == "vrr_present_pacing") vrrPresentPacing = ParseBool(val);
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

    file << "# Display\n";
    file << "color_expansion = " << (colorExpansion ? "true" : "false") << "\n\n";

    file << "# Image Upscaling (NIS)\n";
    file << "nis_enabled = "    << (nisEnabled ? "true" : "false") << "\n";
    file << "nis_scale_mode = " << nisScaleMode << "\n";
    file << "nis_sharpness = "  << nisSharpness << "\n\n";

    file << "# HDR\n";
    file << "hdr_enabled = " << (hdrEnabled ? "true" : "false") << "\n\n";

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
