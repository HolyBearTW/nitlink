#include "application.h"
#include "WebViewSettings.h"
#include "game_database.h"
#include "capture/elgato_hdr_control.h"
#include "capture/elgato_hid_4ks.h"
#include <chrono>
#include <thread>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <debugapi.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

namespace NitLink {

static void AppLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/App] " + msg + L"\n").c_str());
}

// Predicate used to gate Elgato-specific control calls (IKsPropertySet
// HDR tonemap toggle, HDR InfoFrame property read, 4K S vendor HID
// Output Report) so non-Elgato sources like laptop webcams or
// third-party capture cards do not generate "property not supported"
// log noise. Matches the existing substring filter used inside
// elgato_hdr_control.cpp's FindDeviceFilter, so a device that the
// downstream Elgato calls would have accepted is the same device this
// predicate accepts.
static bool IsElgatoDevice(const std::wstring& deviceName)
{
    std::wstring lower = deviceName;
    if (!lower.empty()) {
        CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    }
    return lower.find(L"elgato") != std::wstring::npos;
}

// Escape a wide string for safe inclusion inside a JSON string literal.
// Handles the JSON-required escapes (" and \), the seven standard
// control-character escapes (\b \f \n \r \t), and any other control
// codepoint below 0x20 via \uXXXX. Non-control Unicode codepoints
// >= 0x20 pass through unchanged: the JSON spec accepts those directly
// in string content. WebView2's PostWebMessageAsJson handles the
// UTF-16 -> UTF-8 transcode on the wire.
//
// Used for capture-device friendly names pushed to the settings panel.
// MF-reported names are almost always ASCII, but escaping
// unconditionally keeps the JSON valid against any future driver that
// includes a quote, backslash, or non-Latin character.
static std::wstring JsonEscapeWide(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 4);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b";  break;
            case L'\f': out += L"\\f";  break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) {
                    // Control codepoint outside the seven named
                    // escapes. Codepoints 0x00..0x1F always fit in
                    // two hex digits prefixed with "00".
                    static const wchar_t hex[] = L"0123456789abcdef";
                    out += L"\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}


Application::Application() = default;
Application::~Application() { Shutdown(); }

bool Application::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    AppLog(L"Initialize: begin");

    // Load configuration. Tracks whether this was a fresh install (no
    // config file yet) so the settings menu can be opened automatically
    // on first launch: guides new users to the feature toggles instead
    // of leaving them on a black "NO SIGNAL" screen wondering what to do.
    m_firstLaunch = !std::filesystem::exists("nitlink.json");
    m_config = std::make_unique<Config>();
    m_config->Load("nitlink.json");
    AppLog(m_firstLaunch ? L"Initialize: config loaded (first launch)"
                             : L"Initialize: config loaded");

    m_window = std::make_unique<Window>();
    Window::Desc windowDesc{};
    windowDesc.title    = L"NitLink";
    windowDesc.width    = m_config->windowWidth;
    windowDesc.height   = m_config->windowHeight;
    windowDesc.darkMode = true;

    if (!m_window->Create(hInstance, windowDesc)) {
        AppLog(L"Initialize: Window::Create FAILED");
        return false;
    }
    m_window->Show(nCmdShow);
    AppLog(L"Initialize: window created and shown");

    auto [actualW, actualH] = m_window->GetClientSize();
    // Window may not have processed WM_SIZE yet after Show(), so GetClientSize()
    // can return garbage. Reject anything outside a sane monitor size range
    // and fall back to the requested size.
    if (actualW < 8 || actualH < 8 || actualW > 16384 || actualH > 16384) {
        actualW = windowDesc.width  >= 8 ? windowDesc.width  : 1280;
        actualH = windowDesc.height >= 8 ? windowDesc.height : 720;
    }
    {
        std::wstringstream ss;
        ss << L"Initialize: client size " << actualW << L"x" << actualH;
        AppLog(ss.str());
    }

    m_renderer = std::make_unique<DX11Renderer>();
    if (!m_renderer->Initialize(m_window->GetHWND(), actualW, actualH)) {
        AppLog(L"Initialize: Renderer::Initialize FAILED");
        return false;
    }
    AppLog(L"Initialize: renderer initialized");

    auto devices = DeviceEnumerator::FindCaptureDevices();
    {
        std::wstringstream ss;
        ss << L"Initialize: found " << devices.size() << L" capture device(s)";
        AppLog(ss.str());
    }
    if (devices.empty()) {
        AppLog(L"Initialize: no capture devices found");
        return false;
    }

    // Pick which device to open. The user's preferred_device field in
    // nitlink.json takes priority through PickPreferredDevice's exact-
    // then-substring match. Empty preferred name (first run) or a no-
    // match (saved device unplugged since last launch) falls back to a
    // small bias toward Elgato cards: when a laptop has both an
    // integrated webcam and an Elgato connected, the webcam tends to
    // win MF enumeration order without this bias and the user ends up
    // viewing their own face instead of the capture source.
    int chosenIdx = DeviceEnumerator::PickPreferredDevice(
        devices, m_config ? m_config->preferredDevice : std::wstring{});
    if (chosenIdx < 0 || static_cast<size_t>(chosenIdx) >= devices.size()) {
        chosenIdx = 0;
    }
    const DeviceInfo& chosen = devices[chosenIdx];
    {
        std::wstringstream ss;
        ss << L"Initialize: selected capture device '" << chosen.name
           << L"' (index " << chosenIdx << L" of " << devices.size() << L")";
        if (m_config && !m_config->preferredDevice.empty()) {
            ss << L" [preferred='" << m_config->preferredDevice << L"']";
        }
        AppLog(ss.str());
    }

    // Gate the Elgato-specific control calls (IKsPropertySet HDR
    // tonemap toggle, HDR InfoFrame property read, 4K S vendor HID
    // Output Report) on whether the selected device is actually an
    // Elgato card. Without this gate, those calls would self-no-op on
    // webcams and third-party capture cards (their internal device-
    // name and VID/PID filters reject non-matches) but would still
    // generate "property not supported" log lines for every launch
    // against a non-Elgato source.
    const bool isElgato = IsElgatoDevice(chosen.name);
    if (!isElgato) {
        AppLog(L"Initialize: selected device is not Elgato; skipping Elgato-specific HDR controls");
    }

    // Before opening the device for Media Foundation capture, talk to
    // the Elgato driver via DirectShow's IKsPropertySet and disable
    // the internal HDR->SDR tonemapping. The setting persists in
    // driver state across the DS->MF transition, so when
    // CaptureDevice::Open negotiates a Media Foundation session right
    // after this, the card will pass raw HDR10 data (P010-encoded,
    // BT.2020 PQ) instead of pre-tonemapped SDR.
    if (isElgato) {
        if (DisableElgatoTonemap(chosen.name)) {
            AppLog(L"Initialize: Elgato hardware tonemap disabled (raw HDR10 mode)");
        } else {
            AppLog(L"Initialize: Elgato tonemap toggle skipped or unsupported");
        }
    }

    // Read the HDR InfoFrame the source is sending the Elgato.
    // The pipeline uses this for auto-detect: if the source is HDR10, P010
    // will be negotiated from MF; if it's SDR, BGRA is used.
    //
    // Auto-detect may fail on some Elgato device classes (e.g. 4K S over
    // USB doesn't expose the property GUID the 4K Pro uses for this query).
    // When detection is unavailable, the user's hdrEnabled config preference
    // is trusted instead: if HDR is enabled, the user probably wants HDR.
    // P010 negotiation can still fail at the MF level, in which case the
    // fallback path below catches it.
    if (isElgato) {
        HDRSourceInfo srcInfo = ReadElgatoHDRSource(chosen.name);
        m_sourceIsHDR10        = srcInfo.isHDR10;
        m_hdrDetectionAvailable = srcInfo.propertyAccessible;
        if (srcInfo.propertyAccessible) {
            AppLog(srcInfo.isHDR10
                ? L"Initialize: source detected as HDR10 (PQ), will request P010 capture"
                : L"Initialize: source detected as SDR, will use BGRA capture");
        } else {
            AppLog(L"Initialize: HDR source detection unavailable, will use hdr_enabled config setting to decide");
            m_sourceIsHDR10 = false;
        }
    } else {
        // Non-Elgato source has no InfoFrame property. Default to SDR
        // detection state; the user's hdrEnabled config can still
        // force P010 negotiation at the MF level if they explicitly
        // request it, and MF will reject P010 cleanly on a webcam
        // through the existing retry-to-NV12 fallback below.
        m_sourceIsHDR10 = false;
        m_hdrDetectionAvailable = false;
    }

    // Cache the chosen device so ReconcileCaptureFormat can re-Open
    // the same device when a format swap is needed at runtime (Alt+H or
    // source SDR<->HDR transitions).
    m_currentDeviceInfo = chosen;

    m_captureDevice = std::make_unique<CaptureDevice>();

    // Decide capture format BEFORE opening. P010 is requested when:
    //   1. Auto-detect found an HDR10 source (4K Pro path), AND user has HDR
    //      enabled in config; OR
    //   2. Auto-detect was unavailable (4K S: no property GUID) but user
    //      has HDR enabled in config: trust the user's preference.
    //
    // Either case false -> standard BGRA/NV12 SDR pipeline. The same decision
    // is reapplied at runtime by ReconcileCaptureFormat when the user
    // toggles HDR (Alt+H) or the source's HDR state changes (PS5 launching
    // an HDR game from an SDR dashboard). Reconcile handles the teardown
    // and re-Open; this initial pass just gets the pipeline to a working state.
    //
    // The full expression is gated on isElgato. The P010 pipeline has
    // only been validated on Elgato hardware (4K Pro IKsPropertySet
    // path, 4K S vendor HID path). Non-Elgato sources fall into two
    // failure modes if P010 is requested anyway: most webcams and
    // third-party capture cards reject the P010 negotiation, which
    // triggers the BGRA retry below with misleading "P010 negotiation
    // failed" log noise; a minority accept the request and deliver
    // SDR-shaped data in a P010 container, which the BT.2020 PQ shader
    // renders as garbage. Forcing SDR when the device is not Elgato
    // keeps the first-frame behavior predictable. Users with an Elgato
    // 4K Pro / 4K S still get HDR through the normal logic; the gate
    // only changes behavior on non-Elgato selections.
    const bool userWantsHDR = m_config && m_config->hdrEnabled;
    const bool useP010 = isElgato &&
                         (m_sourceIsHDR10 ||
                          (userWantsHDR && !m_hdrDetectionAvailable));
    if (useP010) {
        AppLog(L"Initialize: P010 HDR10 capture pipeline selected");
        m_captureDevice->RequestP010(true);
    } else {
        if (userWantsHDR && !isElgato) {
            AppLog(L"Initialize: hdr_enabled is true but the selected device is not Elgato; forcing SDR capture (P010 path is only validated for Elgato hardware)");
        }
        AppLog(L"Initialize: SDR BGRA capture pipeline selected");
    }

    // Elgato 4K S vendor HID command: pair the card's internal tonemap
    // state with the upcoming Media Foundation capture format.
    //   useP010 (HDR pipeline)   -> tonemap OFF: card passes raw HDR10 PQ
    //                                            through; the shader does
    //                                            the HDR rendering.
    //   !useP010 (SDR pipeline)  -> tonemap ON:  card converts an HDR
    //                                            HDMI source to SDR
    //                                            internally so the NV12
    //                                            frames are clean SDR.
    // Sending OFF unconditionally before NV12/BGRA capture leaves the
    // card in HDR-passthrough mode while NitLink is set up to render
    // SDR, which makes the SDR picture look washed (HDR codes
    // interpreted as 100-nit-paper-white SDR). Always pair the two.
    // Skipped on non-Elgato devices since the HID protocol is
    // 4K S-specific (VID 0x0FD9 + PID 0x00AE/0x00AF).
    if (isElgato && Set4KSTonemap(/*enableTonemap=*/ !useP010)) {
        AppLog(useP010
            ? L"Initialize: 4K S HID tonemap OFF sent for raw HDR/P010"
            : L"Initialize: 4K S HID tonemap ON sent for SDR/NV12");
    }

    if (!m_captureDevice->Open(chosen)) {
        // If P010 was requested and failed, retry once without it. The
        // device might publish P010 in its capability list but reject the
        // negotiation at runtime (driver quirk, video processor limit, etc).
        if (useP010) {
            AppLog(L"Initialize: P010 negotiation failed, retrying with BGRA");
            m_captureDevice->RequestP010(false);
            // Tonemap was sent OFF above for the P010 attempt; that
            // attempt failed and the pipeline is falling back to SDR
            // NV12. Flip the 4K S HID tonemap state to ON so the card
            // converts any HDR source to SDR for the upcoming capture.
            // Same Elgato gating as the initial call above.
            if (isElgato && Set4KSTonemap(/*enableTonemap=*/ true)) {
                AppLog(L"Initialize: 4K S HID tonemap ON sent for SDR retry");
            }
            if (!m_captureDevice->Open(chosen)) {
                AppLog(L"Initialize: CaptureDevice::Open FAILED");
                return false;
            }
        } else {
            AppLog(L"Initialize: CaptureDevice::Open FAILED");
            return false;
        }
    }
    AppLog(L"Initialize: capture device opened");

    // Log every native format this device exposes. Pure diagnostic: does
    // NOT change the running pipeline. Useful when triaging: confirms
    // whether the Elgato publishes P010 (10-bit BT.2020 PQ) for the HDR10
    // path. Output goes to the [NitLink/Formats] debug channel.
    m_captureDevice->LogAvailableFormats();

    auto format = m_captureDevice->GetOutputFormat();
    m_frameBuffer = std::make_unique<FrameBuffer>(format.width, format.height, format.stride);
    AppLog(L"Initialize: frame buffer created");

    // Tell the renderer what row order the capture source is delivering.
    // The renderer's BGRA shader will conditionally flip V to handle either
    // orientation correctly. P010 / NV12 paths are unaffected (they have
    // their own dedicated shaders that already assume top-down planar data).
    if (m_renderer) m_renderer->SetSourceRowOrder(format.topDown);

    // Tell the renderer the pixel value range of the captured frames.
    // Most HDMI sources come in as limited range (16-235); the BGRA and
    // NV12 shaders apply a 16->0 / 235->255 expansion to recover full
    // dynamic range. Some drivers (notably the Elgato 4K S NV12 path)
    // deliver full-range pixels (0-255) directly; for those the shader
    // is told to skip the expansion. Without this, full-range sources
    // render with crushed blacks.
    if (m_renderer) m_renderer->SetSourceFullRange(format.fullRange);

    // Tell the renderer whether the data flowing through the capture buffer
    // is PQ-encoded HDR10. This is derived from the negotiated capture
    // format, NOT from m_sourceIsHDR10: the two answer different questions:
    //
    //   m_sourceIsHDR10  = "did source detection report HDR10 from the
    //                       upstream HDMI signal?" (always false on 4K S)
    //   captureIsP010    = "is the data in the capture buffer right now
    //                       PQ-encoded BT.2020 10-bit YUV?"
    //
    // The shader needs the second answer. On the 4K Pro they match because
    // the poller drives format selection. On the 4K S they diverge:
    // m_sourceIsHDR10 is always false but the user can still ask for P010
    // capture via Alt+H, and when they do the shader must pick the BT.2020
    // PQ path or BT.2020 reds get misinterpreted as BT.709 reds (orange to
    // red shift).
    if (m_renderer) {
        const bool captureIsP010 = IsEqualGUID(format.subtype, MFVideoFormat_P010);
        m_renderer->SetSourceIsHDR10(captureIsP010);

        // Thread the negotiated subtype GUID through to the renderer as a
        // stable enum, replacing the renderer's old per-frame byte-count
        // inference. Without this, partial frames from HDMI signal-loss
        // windows could land in the wrong format's tolerance band and get
        // uploaded as the wrong format (the green-frame failure mode).
        DX11Renderer::CaptureFormatKind rkind;
        if (IsEqualGUID(format.subtype, MFVideoFormat_P010)) {
            rkind = DX11Renderer::CaptureFormatKind::P010;
        } else if (IsEqualGUID(format.subtype, MFVideoFormat_NV12)) {
            rkind = DX11Renderer::CaptureFormatKind::NV12;
        } else {
            // MFVideoFormat_RGB32 / MFVideoFormat_ARGB32 both land here:
            // the renderer's BGRA path handles both (they share byte layout).
            rkind = DX11Renderer::CaptureFormatKind::BGRA;
        }
        m_renderer->SetSourceFormat(rkind);
    }

    m_audioRouter = std::make_unique<AudioRouter>();
    // Route audio from the Elgato capture card to the default playback device.
    if (!m_audioRouter->Initialize(L"Elgato")) {
        AppLog(L"Initialize: AudioRouter failed (continuing without audio)");
    }

    m_overlay = std::make_unique<Overlay>();
    bool overlayOk = m_overlay->Initialize(m_renderer->GetDevice(), m_renderer->GetContext(),
                                            m_renderer->GetSwapChain(), m_window->GetHWND());
    if (!overlayOk) {
        AppLog(L"Initialize: Overlay::Initialize FAILED (continuing without overlay)");
    }
    m_showOverlay = overlayOk;

    // NIS upscaler: compile compute shader, upload coefficient tables.
    // Init is best-effort: if it fails (e.g. user doesn't have compute shader
    // support), the rest of the app works fine, just no image upscaling.
    m_nisUpscaler = std::make_unique<NisUpscaler>();
    if (!m_nisUpscaler->Initialize(m_renderer->GetDevice())) {
        AppLog(L"Initialize: NisUpscaler::Initialize FAILED (continuing without NIS)");
        m_nisUpscaler.reset();
    } else {
        AppLog(L"Initialize: NIS upscaler ready");
    }

    // Frame differ: detects "is this captured frame new content vs a duplicate?"
    // Needed for real game-FPS reporting. Best-effort init.
    m_frameDiffer = std::make_unique<FrameDiffer>();
    if (!m_frameDiffer->Initialize(m_renderer->GetDevice())) {
        AppLog(L"Initialize: FrameDiffer::Initialize FAILED (continuing without)");
        m_frameDiffer.reset();
    } else {
        AppLog(L"Initialize: frame differ ready");
    }

    // Placeholder detector: recognizes Elgato NO SIGNAL placeholder frames
    // by content fingerprint so the run loop can suppress them. No init
    // failure path: it owns nothing but ~16 bytes of POD state.
    m_placeholderDetector = std::make_unique<PlaceholderDetector>();

    m_webviewSettings = std::make_unique<WebViewSettings>();
    m_webviewSettings->Initialize(m_window->GetHWND(), actualW, actualH);
    m_webviewSettings->SetMessageHandler([this](const std::wstring& msg) {
        // Messages from JS arrive as JSON strings of the form:
        //   {"action":"toggleHDR","value":true}
        //   {"action":"setVolume","value":0.75}
        //   {"action":"ready"}
        // No full JSON library is pulled in: instead the "action" field
        // and an optional "value" field are extracted via tiny string
        // search. Robust enough for the small fixed schema; if more
        // complex messages land later, nlohmann_json can be vendored.
        auto extractStr = [&](const wchar_t* key) -> std::wstring {
            std::wstring needle = std::wstring(L"\"") + key + L"\":\"";
            auto p = msg.find(needle);
            if (p == std::wstring::npos) return L"";
            p += needle.size();
            auto end = msg.find(L'"', p);
            return (end == std::wstring::npos) ? L"" : msg.substr(p, end - p);
        };
        auto extractRaw = [&](const wchar_t* key) -> std::wstring {
            // Pull a value that's a number/bool (no quotes). Returns the
            // raw token; caller parses.
            std::wstring needle = std::wstring(L"\"") + key + L"\":";
            auto p = msg.find(needle);
            if (p == std::wstring::npos) return L"";
            p += needle.size();
            auto end = msg.find_first_of(L",}", p);
            return (end == std::wstring::npos) ? L"" : msg.substr(p, end - p);
        };

        const std::wstring action = extractStr(L"action");
        if (action.empty()) return;

        AppLog(L"WebView2 msg: " + action);

        if (action == L"ready") {
            // JS finished loading and asked for an initial state dump.
            PushSettingsState();
            // First-launch experience: if there was no config file on
            // startup (fresh install), open the settings menu now so the
            // user sees the controls right away. Reset the flag so this
            // only fires once per session.
            if (m_firstLaunch) {
                m_firstLaunch = false;
                if (!m_settingsVisible) ToggleSettings();
                AppLog(L"first launch: opened settings menu automatically");
            }
            return;
        }
        if (action == L"toggleHDR" && m_config) {
            m_config->hdrEnabled = !m_config->hdrEnabled;
            // Render loop's HDR-sync block picks this up next iteration.
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"toggleColorExpansion" && m_config && m_renderer) {
            m_config->colorExpansion = !m_config->colorExpansion;
            m_renderer->SetColorExpansion(m_config->colorExpansion);
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"toggleNIS" && m_config) {
            m_config->nisEnabled = !m_config->nisEnabled;
            // Render loop reads m_config->nisEnabled each frame.
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"toggleMute" && m_config && m_audioRouter) {
            m_config->audioMuted = !m_config->audioMuted;
            m_audioRouter->SetMuted(m_config->audioMuted);
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"toggleVrrPacing" && m_config) {
            // VRR Present Pacing on/off. The run loop reads
            // m_config->vrrPresentPacing each iteration when deciding
            // whether to gate Present on the FrameDiffer's classification,
            // so no pipeline rebuild is needed: the next iteration picks
            // up the new value. FrameDiffer continues to run regardless
            // (its output feeds the HUD content-fps readout and the
            // VRR-pacing diagnostic log line), it just no longer gates
            // Present when the flag is false.
            m_config->vrrPresentPacing = !m_config->vrrPresentPacing;
            if (!m_config->vrrPresentPacing) {
                // Disabling: clear the consecutive-skip counter. The
                // counter is dormant while pacing is off (the off path
                // never reads or writes it), but the periodic VRR-pacing
                // diagnostic line would otherwise keep reporting whatever
                // value the counter held at the moment of toggle, which
                // reads as stale state in the log. Zeroing here also
                // means a subsequent re-enable starts the skip budget
                // from a clean slate. The session-monotonic
                // m_skippedFrameCount (logged as totalSkipped) is
                // intentionally left untouched so session-long diagnostic
                // comparisons stay consistent.
                m_consecutiveSkips = 0;
            }
            m_config->Save("nitlink.json");
            AppLog(m_config->vrrPresentPacing
                ? L"VRR pacing: enabled (Present gated by frame differ)"
                : L"VRR pacing: disabled (Present every iteration)");
            return;
        }
        if (action == L"setVolume" && m_config && m_audioRouter) {
            const std::wstring raw = extractRaw(L"value");
            if (!raw.empty()) {
                float v = std::wcstof(raw.c_str(), nullptr);
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                m_config->audioVolume = v;
                m_audioRouter->SetVolume(v);
                // Don't Save() on every slider tick: that would hammer
                // disk during a drag. Save happens on exit and on
                // discrete toggles instead.
            }
            return;
        }
        if (action == L"cycleScaler") {
            // Only Catmull-Rom is wired today; this is a placeholder for
            // when more scalers exist (bilinear, Lanczos, etc).
            AppLog(L"cycleScaler: only one option available today");
            return;
        }
        if (action == L"setGame" && m_config) {
            // User picked a game from the dropdown. Extract the id, load the
            // saved settings for it (or seed defaults), apply them, push the
            // game to Discord, and persist.
            const std::wstring wid = extractStr(L"value");
            std::string id;
            id.reserve(wid.size());
            for (wchar_t wc : wid) id.push_back(static_cast<char>(wc));

            m_config->currentGameId = id;
            if (!id.empty()) {
                ApplyGameSettings(id);
            }
            UpdateDiscordForCurrentGame();
            m_config->Save("nitlink.json");
            PushSettingsState();
            return;
        }
        if (action == L"clearGame" && m_config) {
            // "Clear game": go back to global defaults, drop the per-game
            // entry's link from Discord. The stored GameSettings for that
            // id are NOT deleted; if the user picks it again later,
            // their saved prefs come back.
            m_config->currentGameId = "";
            UpdateDiscordForCurrentGame();
            m_config->Save("nitlink.json");
            PushSettingsState();
            return;
        }
        if (action == L"saveGameSettings" && m_config) {
            // "Save for this game": capture the CURRENT live settings into
            // gameSettings[currentGameId]. Next time the user selects this
            // game, ApplyGameSettings loads these values back.
            if (!m_config->currentGameId.empty()) {
                auto& gs = m_config->gameSettings[m_config->currentGameId];
                gs.nisEnabled     = m_config->nisEnabled;
                gs.colorExpansion = m_config->colorExpansion;
                m_config->Save("nitlink.json");
                AppLog(L"Saved settings for game");
            }
            return;
        }

        // ===== Capture-device picker actions =====
        //
        // Backend entry points for switching capture devices at runtime.
        // The F1 settings panel's Source row (rendered by nitlink-menu.html
        // from the captureDevices and activeDevice fields pushed via
        // PushSettingsState) is the primary consumer. The bridge stays
        // UI-agnostic so a hotkey, a tray menu, or external tooling that
        // drives WebView2 directly can dispatch the same messages.
        if (action == L"getDeviceList") {
            // Re-enumerate and push state. PushSettingsState calls
            // DeviceEnumerator::FindCaptureDevices() each invocation,
            // so the JS side always receives the live list. This is
            // the lightweight cousin of refreshDevices: no log line,
            // no implication of user intent.
            PushSettingsState();
            return;
        }
        if (action == L"refreshDevices") {
            // Same effect as getDeviceList but logs the action as a
            // user-initiated refresh. Lets DebugView distinguish a UI
            // refresh-button click from an automatic state push.
            AppLog(L"WebView2: user requested device refresh");
            PushSettingsState();
            return;
        }
        if (action == L"setPreferredDevice") {
            // User picked a device from the picker. Route to the
            // strict SwitchCaptureDevice resolver: if the requested
            // name does not match a currently-connected device, the
            // switch fails and the current device stays active. Push
            // state either way so the JS dropdown re-syncs to reality:
            // on success the new device is highlighted; on failure
            // the dropdown reverts to whatever is actually open.
            const std::wstring requested = extractStr(L"value");
            if (requested.empty()) {
                AppLog(L"setPreferredDevice: empty value, ignoring");
                PushSettingsState();
                return;
            }
            const bool ok = SwitchCaptureDevice(requested);
            if (!ok) {
                AppLog(L"setPreferredDevice: switch failed for '" + requested
                       + L"'; UI will revert to the current device");
            }
            PushSettingsState();
            return;
        }
    });
    m_webviewSettings->NavigateToFile(L"nitlink-menu.html");
    m_webviewSettings->Show(false);
    AppLog(L"Initialize: WebView2 settings ready");

    // Discord Rich Presence. Connection is optional: if Discord isn't
    // running on the user's machine, Connect() returns false and RPC updates
    // are simply skipped. Application keeps working normally. The app ID is
    // hardcoded to the NitLink application registered at
    // discord.com/developers/applications.
    m_discord = std::make_unique<DiscordRPC>();
    if (m_discord->Connect("1505211372286246944")) {
        AppLog(L"Initialize: Discord RPC connected");
        // Set initial "idle" presence. This will be overwritten the moment
        // the user selects a game from the settings menu.
        m_discord->SetActivity(
            L"In NitLink",
            L"PS5 Capture Viewer",
            std::chrono::system_clock::now(),
            "nitlink-logo",
            L"NitLink"
        );
    } else {
        AppLog(L"Initialize: Discord RPC unavailable (Discord not running or RPC disabled)");
    }

    // If config restored a previously-selected game, apply its per-game
    // settings to the live pipeline and reflect on Discord. Do this AFTER
    // Discord init so UpdateDiscordForCurrentGame has somewhere to push.
    if (m_config && !m_config->currentGameId.empty()) {
        ApplyGameSettings(m_config->currentGameId);
        UpdateDiscordForCurrentGame();
        std::wstring wid;
        for (char c : m_config->currentGameId) wid.push_back(static_cast<wchar_t>(c));
        AppLog(L"Initialize: restored game " + wid);
    }

    m_hotkeyManager = std::make_unique<HotkeyManager>(m_window->GetHWND());
    m_hotkeyManager->Register("toggle_fullscreen", {VK_MENU, VK_RETURN},
        [this]() { ToggleFullscreen(); });
    m_hotkeyManager->Register("toggle_pip", {VK_MENU, 'P'},
        [this]() { TogglePiP(); });

    // PiP nudge hotkeys.
    //
    // Ctrl + arrow         : small step (20 px)
    // Ctrl + Shift + arrow : large step (80 px)
    //
    // Each press fires the callback once (HotkeyManager uses edge-trigger
    // semantics, not key-repeat), so the user taps to step. Nudge is a
    // no-op when PiP isn't active. Saved position is written back into
    // Config after each successful move so it persists across restarts.
    auto nudgePiP = [this](int dx, int dy) {
        if (!m_window) return;
        if (!m_window->NudgePiP(dx, dy)) return;
        int32_t x = 0, y = 0;
        if (m_window->GetPiPPosition(x, y) && m_config) {
            m_config->pipX = x;
            m_config->pipY = y;
        }
    };
    constexpr int kPiPNudgeSmall = 20;
    constexpr int kPiPNudgeLarge = 80;
    m_hotkeyManager->Register("pip_nudge_left",
        {VK_CONTROL, VK_LEFT},
        [nudgePiP]() { nudgePiP(-kPiPNudgeSmall, 0); });
    m_hotkeyManager->Register("pip_nudge_right",
        {VK_CONTROL, VK_RIGHT},
        [nudgePiP]() { nudgePiP( kPiPNudgeSmall, 0); });
    m_hotkeyManager->Register("pip_nudge_up",
        {VK_CONTROL, VK_UP},
        [nudgePiP]() { nudgePiP(0, -kPiPNudgeSmall); });
    m_hotkeyManager->Register("pip_nudge_down",
        {VK_CONTROL, VK_DOWN},
        [nudgePiP]() { nudgePiP(0,  kPiPNudgeSmall); });
    m_hotkeyManager->Register("pip_nudge_left_big",
        {VK_CONTROL, VK_SHIFT, VK_LEFT},
        [nudgePiP]() { nudgePiP(-kPiPNudgeLarge, 0); });
    m_hotkeyManager->Register("pip_nudge_right_big",
        {VK_CONTROL, VK_SHIFT, VK_RIGHT},
        [nudgePiP]() { nudgePiP( kPiPNudgeLarge, 0); });
    m_hotkeyManager->Register("pip_nudge_up_big",
        {VK_CONTROL, VK_SHIFT, VK_UP},
        [nudgePiP]() { nudgePiP(0, -kPiPNudgeLarge); });
    m_hotkeyManager->Register("pip_nudge_down_big",
        {VK_CONTROL, VK_SHIFT, VK_DOWN},
        [nudgePiP]() { nudgePiP(0,  kPiPNudgeLarge); });
    // Use Ctrl+S instead of F12 -- F12 is hooked by Xbox Game Bar / Game DVR
    // which can inject DLLs into this process and cause heap corruption.
    m_hotkeyManager->Register("screenshot", {VK_CONTROL, 'S'},
        [this]() { m_screenshotRequested = true; });
    // Use Ctrl+F3 (not bare F3) because when the WebView2 settings overlay
    // has keyboard focus, F3 is interpreted by the Edge runtime as "Find
    // next in page" and opens an HTML search bar, overriding the toggle.
    // Ctrl+F3 has no browser meaning so it routes cleanly back here.
    m_hotkeyManager->Register("toggle_overlay", {VK_CONTROL, VK_F3},
        [this]() { ToggleOverlay(); });
    m_hotkeyManager->Register("toggle_settings", {VK_F1},
        [this]() { ToggleSettings(); });
    // ALT+H toggles HDR instantly (works even when settings panel is hidden in HDR).
    // The actual capture-format switch happens inside the run-loop's reconcile
    // pass on the next iteration: this handler only flips the config flag.
    // ReconcileCaptureFormat picks up the divergence between desired and live
    // state and does the full StopCapture -> Close -> Open -> StartCapture cycle
    // on the main thread (the run loop's thread), which is the only place
    // joining the capture worker is safe.
    m_hotkeyManager->Register("toggle_hdr", {VK_MENU, 'H'},
        [this]() {
            if (m_config) {
                m_config->hdrEnabled = !m_config->hdrEnabled;
                AppLog(m_config->hdrEnabled ? L"HDR ON (ALT+H)" : L"HDR OFF (ALT+H)");
            }
        });
    // Ctrl+F4: HDR color-fidelity diagnostic overlay. Draws calibrated
    // reference patches over the bottom of the screen with KNOWN scRGB
    // values, enabling an A/B against a PS5-direct-to-TV signal showing
    // the same patches. If the orange patch shifts red vs native, that's a
    // measurable pipeline bias.
    m_hotkeyManager->Register("toggle_hdr_diag", {VK_CONTROL, VK_F4},
        [this]() {
            if (m_renderer) {
                const bool on = !m_renderer->IsHDRDiagModeOn();
                m_renderer->SetHDRDiagMode(on);
                AppLog(on ? L"HDR diag overlay ON (Ctrl+F4)"
                          : L"HDR diag overlay OFF (Ctrl+F4)");
            }
        });

    // Ctrl+F5: capture the current frame's placeholder fingerprint to the
    // debug-output channel. Use this while the Elgato NO SIGNAL placeholder
    // is on screen (HDMI unplugged / source powered off) to obtain the live
    // 9-byte signature, then paste the result into kKnownPlaceholders inside
    // placeholder_detector.cpp. Once baked in, the detector starts
    // suppressing matching placeholder frames automatically.
    m_hotkeyManager->Register("capture_placeholder_fingerprint",
        {VK_CONTROL, VK_F5},
        [this]() {
            m_dumpFingerprintRequested = true;
            AppLog(L"Placeholder fingerprint capture queued (Ctrl+F5), next captured frame will log its signature");
        });

    m_sessionStartTime = std::chrono::system_clock::now();
    // Anchor the signal-loss debounce timer at session start so the cold-
    // startup grace measures from here. The first successful FrameBuffer
    // read in Run() will bump this and set m_hasEverReceivedFrame=true,
    // switching the debounce to the longer reacquire-after-loss grace.
    m_lastGoodFrameTime = std::chrono::steady_clock::now();

    // Apply persisted audio + display state so the live subsystems reflect
    // whatever the user had configured when they last closed the app.
    if (m_audioRouter) {
        m_audioRouter->SetVolume(m_config->audioVolume);
        m_audioRouter->SetMuted(m_config->audioMuted);
    }
    if (m_renderer) {
        m_renderer->SetColorExpansion(m_config->colorExpansion);
    }

    // Spin up the HDR source poller iff the device class supports the
    // InfoFrame property query. The 4K Pro does; the 4K S over USB does
    // not (different driver, no GUID). When unsupported, leave m_hdrPoller
    // null: the run loop's HasUpdate() check is gated on it.
    //
    // Seed the poller with the init-time source state already captured
    // a few hundred lines up so the FIRST transition the worker reports
    // is a real change vs. the init state, not a redundant copy of it.
    if (m_hdrDetectionAvailable) {
        m_hdrPoller = std::make_unique<HDRSourcePoller>();
        m_hdrPoller->Start(m_currentDeviceInfo.name, m_sourceIsHDR10);
    } else {
        AppLog(L"Initialize: HDR source poller skipped (property unsupported on this device)");
    }

    m_captureDevice->StartCapture([this](const uint8_t* data, uint32_t size, int64_t timestamp) {
        if (m_frameBuffer) m_frameBuffer->Write(data, size, timestamp);
    });
    AppLog(L"Initialize: capture started, entering run loop");

    m_running = true;
    return true;
}

void Application::Run()
{
    using Clock = std::chrono::high_resolution_clock;
    
    auto lastFpsUpdate = Clock::now();
    uint64_t lastFramesWritten    = 0;
    uint64_t lastUniqueFrameCount = 0;

    MSG msg{};
    while (m_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!m_running) break;

        m_hotkeyManager->Poll();

        // Handle window resize
        // CRITICAL: order matters here.
        //   1. Overlay must release its D2D bitmap (which holds a ref to the
        //      old backbuffer) BEFORE the swap chain can resize.
        //   2. Renderer resizes the swap chain.
        //   3. Overlay recreates its D2D bitmap to wrap the new backbuffer.
        // Without step 1 first, DXGI's ResizeBuffers will block forever
        // waiting for the dangling D2D reference to release.
        if (m_window->WasResized()) {
            auto [w, h] = m_window->GetClientSize();
            AppLog(L"Resize event: handling resize");
            if (m_overlay) m_overlay->OnResizeBegin();
            m_renderer->Resize(w, h);
            // Update the WebView2 child controller bounds so it stays
            // sized to the new client rect. The control is always sized
            // to the full client area; only its visibility toggles.
            if (m_webviewSettings) {
                m_webviewSettings->Resize();
            }
            if (m_overlay) {
                // Always recreate overlay D2D resources on resize. In SDR
                // this rebinds to the new backbuffer; in HDR this recreates
                // the BGRA8 offscreen at the new size. Either way the
                // overlay stays functional after window resize.
                m_overlay->OnResizeEnd();
            }
            m_window->AcknowledgeResize();
            AppLog(L"Resize event: done");
        }

        // HDR source auto-detect drain. Once per second, the HDR poller's
        // worker thread reads the Elgato InfoFrame and flips m_hasUpdate
        // when it sees an EOTF transition (e.g. PS5 dashboard -> HDR game
        // launch). Pull the new state here, push it to the renderer, and
        // let ReconcileCaptureFormat (below) swap the capture format if
        // needed. AcceptUpdate is atomic test-and-clear and a cheap no-op
        // when no update is pending.
        if (m_hdrPoller) {
            bool newSourceIsHDR10 = false;
            if (m_hdrPoller->AcceptUpdate(&newSourceIsHDR10)) {
                AppLog(newSourceIsHDR10
                    ? L"HDR auto-detect: source went HDR10"
                    : L"HDR auto-detect: source went SDR");
                m_sourceIsHDR10 = newSourceIsHDR10;
                // Don't push to renderer here: ReconcileCaptureFormat will
                // re-negotiate capture format on the next line and set the
                // renderer's HDR10 flag from the resulting actual format.
                // Pushing m_sourceIsHDR10 here would create a one-iteration
                // mismatch where the renderer thinks the capture is P010
                // before the capture device has actually switched.
            }
        }

        // Capture-side HDR reconcile. Runs every iteration; fast no-op when
        // the device is already in the right format. When Alt+H flipped
        // m_config->hdrEnabled, or the HDR poller's drain above flipped
        // m_sourceIsHDR10 in response to a source transition (PS5
        // dashboard -> HDR game launch etc), this is where the actual
        // teardown + re-Open happens. Must run BEFORE the renderer-sync
        // block below so the renderer's swap-chain format swap follows the
        // capture format swap, not the other way around: otherwise P010
        // pixels would briefly render through the BGRA backbuffer (or vice
        // versa) for one iteration.
        //
        // Force-reopen path: the capture worker sets a flag when it exits
        // due to a fatal stream condition (MF_SOURCE_READERF_ERROR, a
        // media-type change underneath the reader, or repeated ReadSample
        // failures exceeding the in-loop retry budget: common on PS5 boot
        // logo / SDR<->HDR boundaries). When the flag is set the format
        // may match the target, but the IMFSourceReader is dead and the
        // worker thread has already exited; the full
        // Stop->Close->Open->Start cycle must run regardless.
        const bool forceReopen = m_captureDevice && m_captureDevice->ConsumeNeedsReopen();
        if (forceReopen) {
            AppLog(L"Capture worker signaled needs-reopen, forcing format reconcile");
        }
        ReconcileCaptureFormat(forceReopen);

        // HDR toggle sync: when the config flag flips, recreate the swap
        // chain in the new format. Same D2D-release / resize / D2D-recreate
        // dance as a window resize, because the backbuffer is being recreated.
        // If the renderer reports failure (display doesn't support HDR, etc)
        // the config flag is rolled back so the UI reflects reality.
        if (m_config && m_renderer && m_config->hdrEnabled != m_renderer->IsHDREnabled()) {
            const bool target = m_config->hdrEnabled;
            AppLog(target ? L"HDR: enabling..." : L"HDR: disabling...");
            if (m_overlay) m_overlay->OnResizeBegin();
            const bool ok = m_renderer->SetHDREnabled(target);
            if (m_overlay) m_overlay->OnResizeEnd();
            if (!ok && target) {
                AppLog(L"HDR: SetHDREnabled returned false, reverting config flag");
                m_config->hdrEnabled = false;
                // Surface the failure to the user: most users won't be
                // watching DebugView. The renderer's gate refuses when
                // Windows HDR isn't engaged for the active output, so
                // that's the overwhelmingly common reason this branch
                // fires. Keep the text short and tell them what to do.
                ShowToast(L"Windows HDR is not enabled. Press Win+Alt+B and try again.");
            } else {
                AppLog(target ? L"HDR: enabled" : L"HDR: disabled");
            }
        }

        auto captureStart = Clock::now();

        bool haveFreshFrame         = false;
        bool freshFrameIsRealSource = false;
        FrameBuffer::FrameData frame;
        // Null guard: ReconcileCaptureFormat resets m_frameBuffer and only
        // rebuilds it if Open() succeeds. If both reopen attempts fail
        // (transient driver failure during a PS5 HDMI handshake, etc.) the
        // buffer stays null until a future forced-reopen succeeds. Skip
        // the read this iteration; the no-signal debounce handles the UX.
        if (m_frameBuffer && m_frameBuffer->Read(frame)) {
            haveFreshFrame = true;

            // Elgato placeholder-frame detection.
            //
            // The card emits its own NO SIGNAL placeholder as a valid frame
            // stream during HDMI unplug / handshake gaps. Those frames need
            // to be suppressed so the existing 2.5 s no-signal debounce can
            // surface the NitLink no-signal page. The detector fingerprints
            // each frame (3x3 zone-luma signature, format-aware) and
            // declares "placeholder" only when the fingerprint matches a
            // baked-in Elgato signature for kRequiredConsecutiveMatches
            // frames in a row.
            //
            // Why this is safe against false positives on real static
            // content: a paused game / static menu / black scene transition
            // is content-shaped and will not match the Elgato placeholder's
            // specific zone pattern across all 9 zones. See
            // placeholder_detector.h for the fingerprint and matching rules.
            //
            // Until live fingerprints are baked into placeholder_detector.cpp
            // via the Ctrl+F5 hotkey, the detector is a no-op: every fresh
            // frame is treated as real source and the existing pipeline runs
            // unchanged.
            PlaceholderDetector::Fingerprint fp{};
            auto cls = PlaceholderDetector::FrameClassification::Real;
            if (m_placeholderDetector && m_captureDevice) {
                const auto subtype = m_captureDevice->GetOutputFormat().subtype;
                PlaceholderDetector::CaptureFormatKind fmt;
                if (IsEqualGUID(subtype, MFVideoFormat_P010)) {
                    fmt = PlaceholderDetector::CaptureFormatKind::P010;
                } else if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
                    fmt = PlaceholderDetector::CaptureFormatKind::NV12;
                } else {
                    fmt = PlaceholderDetector::CaptureFormatKind::BGRA;
                }
                fp = PlaceholderDetector::Compute(
                    frame.data, frame.size, frame.width, frame.height, fmt);

                // Debug hotkey: dump this frame's fingerprint and clear the
                // request. The user presses Ctrl+F5 while the Elgato
                // placeholder is on screen to capture the live signature,
                // then pastes the result into kKnownPlaceholders.
                if (m_dumpFingerprintRequested) {
                    m_dumpFingerprintRequested = false;
                    PlaceholderDetector::LogFingerprint(fp,
                        [](const std::wstring& m) {
                            OutputDebugStringW((L"[NitLink/Placeholder] " + m + L"\n").c_str());
                        });
                }

                cls = m_placeholderDetector->Process(fp, fmt);
                freshFrameIsRealSource =
                    (cls == PlaceholderDetector::FrameClassification::Real);
            } else {
                // No detector available: fall back to "every fresh frame
                // is real" so behavior never regresses versus pre-detector state.
                freshFrameIsRealSource = true;
            }

            // Three-way branch on the classifier:
            //
            //   Real                  : upload, bump grace timer, run differ.
            //   CandidatePlaceholder  : suppress upload + differ + grace
            //                           bump. Do NOT log: the streak may
            //                           still break on the next frame.
            //                           This prevents even a single placeholder
            //                           frame from polluting m_captureTexture
            //                           before confirmation is reached.
            //   ConfirmedPlaceholder  : same suppression as candidate, plus
            //                           log the transition once and let the
            //                           shorter placeholder-specific grace
            //                           inside ShouldShowNoSignal kick in.
            if (cls == PlaceholderDetector::FrameClassification::Real) {
                m_renderer->UpdateCaptureTexture(frame.data, frame.size, frame.width, frame.height);
                auto captureEnd = Clock::now();
                m_captureLatencyMs = std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();
                m_lastGoodFrameTime    = std::chrono::steady_clock::now();
                m_hasEverReceivedFrame = true;
                if (m_inPlaceholderState) {
                    AppLog(L"Signal: real source frames resumed");
                    m_inPlaceholderState = false;
                }
            } else if (cls == PlaceholderDetector::FrameClassification::ConfirmedPlaceholder) {
                if (!m_inPlaceholderState) {
                    AppLog(L"Signal: Elgato placeholder detected, holding last real frame, no upload");
                    m_inPlaceholderState = true;
                }
            }
            // CandidatePlaceholder: no upload, no grace bump, no log. Wait
            // and see whether the streak breaks (back to Real) or completes
            // (Confirmed).
        }
        // No fresh frame at all: existing no-fresh-frame debounce path
        // (ShouldShowNoSignal + m_lastGoodFrameTime) handles it unchanged.

        // Evaluate the no-signal debounce ONCE per iteration. ShouldShowNoSignal
        // owns the latched "reacquiring"/"lost"/"restored" log transitions, so
        // calling it multiple times in the same iteration would double-log
        // state changes. The render block below reads showNoSignalNow at
        // both the HDR and SDR no-signal trigger sites.
        const bool showNoSignalNow = ShouldShowNoSignal();

        // =====================================================================
        // VRR PRESENT PACING: differ-driven Present
        // =====================================================================
        // The Elgato 4K Pro delivers frames at constant 60Hz to Media Foundation
        // regardless of the source's actual framerate: frame duplication happens
        // at the HDMI signal level. So in v1.0 with naive present-every-frame,
        // the monitor's VRR sees a constant 60Hz Present rate even when the PS5
        // game is running at 30fps or has variable framerate.
        //
        // Fix: run the GPU frame differ HERE, before the render decision. If the
        // captured frame is a duplicate of the previous one (frame duplication at
        // the HDMI level), skip Present entirely. With Independent Flip +
        // ALLOW_TEARING active on the swap chain, the monitor's G-Sync/FreeSync
        // VRR will sync to the actual unique-frame rate instead of constant 60Hz.
        //
        // Verified working: LG C3 OLED Game Dashboard reports refresh rate
        // tracking the application's Present rate when NitLink is the focused
        // window.
        //
        // The differ runs on the raw capture SRV which was populated by
        // UpdateCaptureTexture above. The differ is hoisted here so its result
        // can gate the entire render+Present block.
        //
        // Fallback: if VRR pacing is disabled, render+present always (v1.0
        // behavior). Also: if differ isn't ready (first few frames) always
        // present. The differ is conservative: it defaults to "is new" until
        // it has previous-frame data to compare against.
        // =====================================================================
        bool isNewFrame = true;  // default to "present" for safety
        // freshFrameIsRealSource gate keeps the differ off Elgato placeholder
        // frames. Feeding the differ a stretch of placeholder luma would
        // let m_prevTex go stale relative to the next real frame: the first
        // post-recovery diff would either spike (false "new") or get
        // suppressed by the ring-buffer smoother depending on smoothing
        // state, both bad. Skipping the differ entirely keeps m_prevTex
        // anchored to the last real-source frame so recovery diffs against
        // a sensible baseline.
        if (haveFreshFrame && freshFrameIsRealSource
            && m_frameDiffer && m_renderer->GetRawCaptureSRV()) {
            m_frameDiffer->Process(m_renderer->GetContext(),
                                     m_renderer->GetRawCaptureSRV());
            isNewFrame = m_frameDiffer->WasPreviousFrameNew();
            if (isNewFrame) m_uniqueFrameCount++;
        }

        // FPS sampling and differ diagnostic: fire every iteration (NOT
        // inside the render block). Otherwise when VRR pacing skips render+
        // Present, the FPS counter would freeze on stale values and the
        // periodic log wouldn't print, hiding differ behavior.
        {
            auto now = Clock::now();
            auto elapsed = std::chrono::duration<double>(now - lastFpsUpdate).count();
            if (elapsed >= 1.0) {
                // Null guard: m_frameBuffer can be null between a failed
                // reopen and a later successful one. Treat as "no new frames
                // written this window" so m_currentFps naturally reads 0.
                uint64_t framesWritten = m_frameBuffer
                    ? m_frameBuffer->GetFramesWritten()
                    : lastFramesWritten;
                uint64_t deltaFrames   = framesWritten - lastFramesWritten;
                m_currentFps           = static_cast<uint32_t>(deltaFrames / elapsed);
                lastFramesWritten      = framesWritten;

                uint64_t deltaUnique   = m_uniqueFrameCount - lastUniqueFrameCount;
                m_currentContentFps    = static_cast<uint32_t>(deltaUnique / elapsed);
                lastUniqueFrameCount   = m_uniqueFrameCount;

                lastFpsUpdate          = now;

                if (m_settingsVisible) PushSettingsState();
            }

            // VRR pacing diagnostic, every 2 seconds. Reports exactly what
            // the differ is observing and whether classification is stable.
            if (m_frameDiffer) {
                static auto lastDiffLog = Clock::now();
                if (std::chrono::duration<double>(now - lastDiffLog).count() >= 2.0) {
                    // dropped = total frames the producer (capture worker)
                    // overran because the renderer hadn't picked up the
                    // previous fresh frame yet. Monotonic since FrameBuffer
                    // construction (rebuilt on every reconcile), so it
                    // resets to 0 across format swaps. A non-zero delta
                    // between consecutive 2-second log lines means frames
                    // are actively being dropped right now: useful when
                    // chasing transition-time anomalies.
                    const uint64_t dropped =
                        m_frameBuffer ? m_frameBuffer->GetFramesDropped() : 0;
                    std::wstringstream ss;
                    ss << L"VRR pacing: lastDiff=" << m_frameDiffer->GetLastDiffValue()
                       << L" threshold=" << m_frameDiffer->GetThreshold()
                       << L" contentFps=" << m_currentContentFps
                       << L" hdmiFps=" << m_currentFps
                       << L" dropped=" << dropped
                       << L" totalSkipped=" << m_skippedFrameCount
                       << L" consecutive=" << m_consecutiveSkips
                       << L" thisFrameNew=" << (isNewFrame ? L"Y" : L"N")
                       << L" haveFresh=" << (haveFreshFrame ? L"Y" : L"N");
                    AppLog(ss.str());
                    lastDiffLog = now;
                }
            }
        }

        // Decision: should this iteration render+Present?
        //
        // VRR pacing OFF (legacy): always render every loop iteration. This
        //   is the v1.0 behavior. Present rate ends up near the desktop
        //   refresh rate via the waitable swap chain.
        //
        // VRR pacing ON: render only when there's a new capture frame AND
        //   the differ says it's unique content. This makes the Present
        //   rate track the actual game framerate, and the monitor's VRR
        //   syncs to it.
        //
        //   Safety floor: if too many consecutive frames have been skipped
        //   (static content like a paused menu, or differ falsely flagging
        //   real frames as duplicates), force a Present every kMaxSkipFrames
        //   iterations to keep DWM happy. At ~16ms capture cadence, kMax=15
        //   means a Present at minimum every ~250ms (~4Hz), which is well
        //   within the VRR window's minimum and well above DWM's "this
        //   window is unresponsive" threshold.
        const bool vrrPacingActive = m_config && m_config->vrrPresentPacing;
        constexpr uint32_t kMaxConsecutiveSkips = 15;
        bool shouldRender;
        if (!vrrPacingActive) {
            shouldRender = true;
        } else if (!haveFreshFrame) {
            // No new frame from the capture worker this iteration. The
            // capture worker delivers at the HDMI cadence (~60 Hz on a
            // healthy source, one frame every ~16.7 ms) while the render
            // loop polls at a much higher rate (sub-millisecond between
            // iterations during the skip path), so a large majority of
            // renderer iterations naturally have no fresh frame even
            // during smooth 60 fps playback.
            //
            // The VRR pacing design intent is to collapse Present rate
            // down to the SOURCE'S UNIQUE-FRAME RATE so monitor VRR
            // follows real game framerate. The signal that tells us
            // about unique-frame rate is the FrameDiffer's isNewFrame
            // classification on a fresh frame, NOT "did the renderer
            // get a fresh frame this poll cycle." A no-fresh-frame
            // iteration carries zero information about whether the
            // content is new or duplicate; it just means the renderer
            // outran the producer this tick.
            //
            // Skipping Present on these iterations starves the swap
            // chain and produces visible stutter on non-VRR monitors
            // during legitimate low-motion content (game intros, slow
            // fades, splash screens), because the kMaxConsecutiveSkips
            // safety floor drops Present rate into the single digits.
            // That was the symptom the user saw with 1-5 fps dips
            // during game intros.
            //
            // Render anyway. Leave m_consecutiveSkips untouched: this
            // iteration is neither a real duplicate (so it should not
            // burn the skip budget) nor a real new frame (so it should
            // not reset the budget either). The budget is fed only by
            // genuine fresh+duplicate iterations below.
            shouldRender = true;
        } else if (isNewFrame) {
            shouldRender = true;
            m_consecutiveSkips = 0;
        } else if (m_consecutiveSkips >= kMaxConsecutiveSkips) {
            shouldRender = true;
            m_consecutiveSkips = 0;
        } else {
            shouldRender = false;
        }

        if (!shouldRender) {
            // Duplicate frame detected, VRR pacing on: skip the entire
            // render+Present block. The monitor will hold the previous frame
            // an extra cycle, which is exactly what VRR is designed for.
            //
            // Small sleep to not spin the CPU. Tuned to be short enough that
            // the wake happens well before the next capture frame arrives (capture
            // worker delivers at ~16.6ms intervals for 60Hz HDMI), but long
            // enough to drop CPU usage meaningfully.
            m_skippedFrameCount++;
            m_consecutiveSkips++;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // Small sleep to avoid 100% CPU spin when running uncapped.
        // 1ms is short enough to be imperceptible but stops the render loop
        // from hammering at 3000+ fps doing nothing useful.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        auto renderStart = Clock::now();

        // HDR path renders the capture directly into the HDR10 PQ
        // backbuffer. D2D can't target R10G10B10A2 HDR10 backbuffers, so
        // overlay D2D content paints into a BGRA8 offscreen and gets
        // composited via CompositeUI in this branch.
        const bool hdrActive = m_renderer && m_renderer->IsHDREnabled();

        // When the settings overlay is visible, render a solid black
        // frame underneath the WebView2 child every iteration. This is
        // simpler and more reliable than freezing the last rendered
        // frame (which depended on flip-discard NOT discarding it,
        // unreliable, and any swap chain recreation like toggling HDR
        // wiped the stored frame, leaving a flicker). Solid black is
        // what real game pause menus do; the WebView2 child renders on
        // top normally and the swap chain keeps presenting clean black
        // pixels behind it.
        //
        // Important: BeginFrame/EndFrame still run because the swap
        // chain MUST keep presenting; on a bare Sleep, DWM eventually
        // marks the window as unresponsive and the WebView2 child can
        // get its compositing context torn down.
        if (m_settingsVisible) {
            m_renderer->BeginFrame();   // clears to (0,0,0,1): solid black
            m_renderer->EndFrame();     // presents the black frame
            continue;                    // skip the rest of the pipeline
        }

        if (hdrActive) {
            // HDR path bypasses NIS (the NIS compute shader is wired for the
            // SDR pipeline today). Make sure DrawCaptureFrame writes to the
            // backbuffer, not the post-input intermediate. If the user
            // toggled NIS on while in SDR and then turned HDR on, the
            // renderer would otherwise still have post-input enabled from
            // the previous SDR iteration, and DrawCaptureFrame would paint
            // into the unused intermediate, leaving the backbuffer black.
            m_renderer->SetPostInputEnabled(false);

            m_renderer->BeginFrame();
            m_renderer->DrawCaptureFrame();

            // HDR color-fidelity diagnostic overlay (Ctrl+F4). Draws known
            // scRGB reference patches over the bottom of the captured frame
            // for visual A/B against a PS5-direct-to-TV signal showing the
            // same patches. Patches use raw scRGB values that bypass the
            // capture pipeline entirely: any color shift between the local
            // overlay and the native render is measurable pipeline bias.
            if (m_renderer->IsHDRDiagModeOn()) {
                m_renderer->DrawHDRDiagnostics();
            }

            // No-signal screen takes precedence over the HUD overlay: when
            // there's no HDMI input, the branded screen is drawn instead of
            // the (meaningless) FPS/latency HUD. This also sidesteps the
            // HDR offscreen-sharing complication: only one of these draws
            // into the BGRA8 offscreen per frame.
            //
            // showNoSignalNow is the debounced decision from
            // ShouldShowNoSignal(): true only after the grace period has
            // elapsed without a fresh frame. During the grace period
            // DrawNoSignal is skipped entirely: DrawCaptureFrame above
            // already repainted the last good frame from m_captureTexture,
            // which is exactly the "keep showing the last good capture"
            // behavior wanted for PS5 boot logo / source-switch transitions.
            //
            // Null guard mirrors the run-loop Read site above: if a failed
            // reopen left m_frameBuffer null, report no signal. The
            // user-visible no-signal page is still gated by
            // showNoSignalNow, so this only affects the HUD stats path.
            const bool signalActive = m_frameBuffer && m_frameBuffer->IsSignalActive();

            if (showNoSignalNow && m_overlay) {
                m_overlay->DrawNoSignal(m_renderer->GetWindowWidth(),
                                          m_renderer->GetWindowHeight());
                if (m_overlay->IsUsingOffscreen()) {
                    m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
                }
            }
            // PiP gate: the HUD panel is a fixed 280x156 px overlay, which
            // dominates a 480x270 PiP window and looks broken. Suppress it
            // entirely while PiP is active for rc1: the panel still renders
            // normally when PiP is off.
            else if (m_showOverlay && m_overlay && !m_isPiP) {
                Overlay::Stats stats{};
                stats.captureLatencyMs = m_captureLatencyMs;
                stats.renderLatencyMs  = m_renderLatencyMs;
                // Content fps comes from the frame differ (detects unique
                // frames). When the scene is static the differ correctly
                // reports 0, but a "0 fps" reading is misleading because
                // the game IS still running. Fall back to the HDMI signal
                // rate in that case so the overlay never lies about it.
                stats.fps = (m_frameDiffer && m_currentContentFps > 0)
                              ? m_currentContentFps
                              : m_currentFps;
                stats.captureWidth     = m_captureDevice->GetOutputFormat().width;
                stats.captureHeight    = m_captureDevice->GetOutputFormat().height;
                stats.deviceName       = m_captureDevice->GetDeviceName();
                stats.signalActive     = signalActive;
                // Pipeline feature flags shown as the HUD's bottom strip so
                // users can see which features are on without opening F1.
                stats.hdrActive        = true; // inside the HDR branch
                stats.nisActive        = m_nisUpscaler && m_config && m_config->nisEnabled;
                stats.colorExpansion   = m_config && m_config->colorExpansion;
                m_overlay->Render(stats);

                // Composite the offscreen onto the HDR backbuffer. The
                // overlay only generates an offscreen SRV in HDR mode;
                // in SDR it paints the backbuffer directly, no composite
                // needed.
                if (m_overlay->IsUsingOffscreen()) {
                    m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
                }
            }

            // Transient toast (separate D2D pass on top of everything else).
            // Fades over the last 500 ms so it doesn't pop out.
            if (m_overlay && !m_toastText.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (now < m_toastExpiry) {
                    const auto remaining = std::chrono::duration<float>(m_toastExpiry - now).count();
                    const float alpha = (remaining < 0.5f) ? (remaining / 0.5f) : 1.0f;
                    m_overlay->DrawToast(
                        m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                        m_toastText.c_str(), alpha);
                    if (m_overlay->IsUsingOffscreen()) {
                        m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
                    }
                } else {
                    m_toastText.clear();
                }
            }

            m_renderer->EndFrame();
        }

        if (!hdrActive) {
        // NIS upscaling requires the renderer to write through the post-input
        // intermediate (clean RGBA8 capture-resolution texture independent
        // of NV12/BGRA source format). When NIS is off, DrawCaptureFrame
        // writes straight to the backbuffer for minimum latency.
        const bool nisActive = m_nisUpscaler && m_config && m_config->nisEnabled;
        m_renderer->SetPostInputEnabled(nisActive);

        m_renderer->BeginFrame();
        m_renderer->DrawCaptureFrame();

        if (nisActive && m_renderer->GetCaptureOutputSRV()) {
            const uint32_t inW  = m_renderer->GetCaptureOutputWidth();
            const uint32_t inH  = m_renderer->GetCaptureOutputHeight();
            uint32_t outW = inW;
            uint32_t outH = inH;

            // Compute upscale target dimensions. NIS only supports 1.0x
            // to 2.0x scale ratio (its NVScalerUpdateConfig rejects
            // anything beyond), so clamp aggressively.
            switch (m_config->nisScaleMode) {
                case 0: // 1.5x
                    outW = (uint32_t)(inW * 1.5f);
                    outH = (uint32_t)(inH * 1.5f);
                    break;
                case 1: // 2x
                    outW = inW * 2;
                    outH = inH * 2;
                    break;
                case 2: // Match window, aspect-preserving, clamped to <= 2x
                default: {
                    // Find the largest scale factor that:
                    //   - fits in the window
                    //   - preserves source aspect ratio
                    //   - is at most 2.0 (NIS's upper limit)
                    // The composite pass will letterbox the result inside
                    // the window, but NIS itself produces aspect-correct
                    // output -- preventing the vertical squish that would
                    // appear if windowW/windowH were used naively.
                    const uint32_t winW = m_renderer->GetWindowWidth();
                    const uint32_t winH = m_renderer->GetWindowHeight();
                    if (winW == 0 || winH == 0 || inW == 0 || inH == 0) {
                        outW = inW * 2; outH = inH * 2;
                    } else {
                        float scaleW = (float)winW / (float)inW;
                        float scaleH = (float)winH / (float)inH;
                        float scale  = std::min(scaleW, scaleH);
                        if (scale < 1.0f) scale = 1.0f;
                        if (scale > 2.0f) scale = 2.0f;
                        outW = (uint32_t)(inW * scale);
                        outH = (uint32_t)(inH * scale);
                    }
                    break;
                }
            }

            if (m_nisUpscaler->Configure(m_renderer->GetContext(),
                                          inW, inH, outW, outH,
                                          m_config->nisSharpness))
            {
                m_nisUpscaler->Dispatch(m_renderer->GetContext(),
                                         m_renderer->GetCaptureOutputSRV());

                m_renderer->CompositeUpscaledTexture(
                    m_nisUpscaler->GetOutputSRV(),
                    m_nisUpscaler->GetOutputWidth(),
                    m_nisUpscaler->GetOutputHeight());
            }
        }
        
        // No-signal screen: when the capture card reports no HDMI input,
        // override its hardware "NO SIGNAL · elgato" placeholder with the
        // NitLink brand identity. Takes precedence over the HUD overlay: a
        // FPS/latency HUD with no signal is just zeros, not useful.
        //
        // showNoSignalNow comes from the debounce in ShouldShowNoSignal();
        // see comment in the HDR branch above. During the grace period
        // DrawNoSignal is skipped and the backbuffer keeps DrawCaptureFrame's
        // last-good-frame output.
        //
        // Null guard: see HDR branch above. Same rationale.
        const bool signalActive = m_frameBuffer && m_frameBuffer->IsSignalActive();
        if (showNoSignalNow && m_overlay) {
            m_overlay->DrawNoSignal(m_renderer->GetWindowWidth(),
                                      m_renderer->GetWindowHeight());
        }
        // Draw overlay if enabled (only when a real signal is present).
        // PiP gate: same rationale as the HDR branch: the 280x156 HUD
        // dominates a 480x270 PiP window. Suppress while PiP is active.
        else if (m_showOverlay && !m_isPiP) {
            Overlay::Stats stats{};
            stats.captureLatencyMs = m_captureLatencyMs;
            stats.renderLatencyMs  = m_renderLatencyMs;
            // Prefer the real game framerate (from FrameDiffer) over the
            // HDMI signal rate. They diverge for sub-60fps games: HDMI
            // duplicates frames at the signal level, so a 30fps game still
            // produces 60 captured frames/sec, and only the differ knows
            // the real number.
            //
            // BUT: when the scene is static (menu screens, paused game,
            // looking at a wall), the differ correctly reports 0 unique
            // frames, and displaying "0 fps" to the user is misleading
            // because the game IS still running. Fall back to the HDMI
            // signal rate in that case.
            stats.fps = (m_frameDiffer && m_currentContentFps > 0)
                          ? m_currentContentFps
                          : m_currentFps;
            stats.captureWidth     = m_captureDevice->GetOutputFormat().width;
            stats.captureHeight    = m_captureDevice->GetOutputFormat().height;
            stats.deviceName       = m_captureDevice->GetDeviceName();
            stats.signalActive     = signalActive;
            // Pipeline feature flags shown as the HUD's bottom strip so
            // users can see which features are on without opening F1.
            stats.hdrActive        = false; // inside the SDR branch
            stats.nisActive        = m_nisUpscaler && m_config && m_config->nisEnabled;
            stats.colorExpansion   = m_config && m_config->colorExpansion;
            m_overlay->Render(stats);
        }

        // Transient toast: see matching block in the HDR branch above.
        // In SDR the D2D target is the backbuffer directly, so no
        // CompositeUI is needed.
        if (m_overlay && !m_toastText.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now < m_toastExpiry) {
                const auto remaining = std::chrono::duration<float>(m_toastExpiry - now).count();
                const float alpha = (remaining < 0.5f) ? (remaining / 0.5f) : 1.0f;
                m_overlay->DrawToast(
                    m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                    m_toastText.c_str(), alpha);
            } else {
                m_toastText.clear();
            }
        }

        m_renderer->EndFrame();
        } // end if (!hdrActive)
        
        auto renderEnd = Clock::now();
        m_renderLatencyMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();

        // Screenshot dispatch -- Ctrl+S sets the flag, the work happens here on
        // the render thread (DX11 context is not thread-safe).
        if (m_screenshotRequested.exchange(false)) {
            TakeScreenshot();
        }
    }
}

void Application::Shutdown()
{
    m_running = false;

    // Stop the HDR source poller FIRST. Its worker thread can be mid-call
    // to ReadElgatoHDRSource, which opens a DirectShow graph against the
    // same Elgato device. Joining it here ensures no orphaned graph
    // outlives the capture device teardown below.
    if (m_hdrPoller) {
        m_hdrPoller->Stop();
        m_hdrPoller.reset();
    }

    // Disconnect Discord RPC FIRST so its worker thread can join cleanly
    // before anything else is torn down. The destructor would do this too
    // but being explicit avoids any chance of the worker thread racing
    // against the cleanup path.
    if (m_discord) {
        m_discord->Disconnect();
        m_discord.reset();
    }

    if (m_audioRouter) {
        m_audioRouter->Shutdown();
        m_audioRouter.reset();
    }

    // Stop capture FIRST and explicitly close the device. The Elgato driver
    // can hold the device handle indefinitely if the process exits without
    // calling Shutdown() on the IMFMediaSource, which leads to "no signal"
    // on subsequent runs until USB is reconnected or the system reboots.
    if (m_captureDevice) {
        m_captureDevice->StopCapture();
        m_captureDevice->Close();
        m_captureDevice.reset();
    }

    // Save config before the window handle is lost
    if (m_config && m_window) {
        auto [w, h] = m_window->GetClientSize();
        if (w >= 200 && h >= 200) { // Don't save zero-sized state from a closing window
            m_config->windowWidth = w;
            m_config->windowHeight = h;
        }
        m_config->Save("nitlink.json");
    }

    // Tear down GPU resources before the window goes away
    if (m_webviewSettings) { m_webviewSettings->Shutdown(); m_webviewSettings.reset(); }
    if (m_overlay)       { m_overlay->Shutdown();       m_overlay.reset(); }
    if (m_nisUpscaler)   { m_nisUpscaler->Shutdown();   m_nisUpscaler.reset(); }
    if (m_frameDiffer)   { m_frameDiffer->Shutdown();   m_frameDiffer.reset(); }
    if (m_renderer) m_renderer.reset();
    if (m_window)   m_window.reset();
}

void Application::ToggleFullscreen()
{
    // rc1 safety: refuse fullscreen toggle while PiP is active. PiP runs
    // the main window as WS_EX_LAYERED + WS_EX_TOPMOST with an alpha-keyed
    // layered surface; if SetFullscreen swaps styles out from under those
    // attributes, DWM ends up with a fullscreen-ish window that still
    // carries the layered alpha state, visible as a transparent /
    // ghosted main window. The user must drop PiP (Alt+P) first, then
    // toggle fullscreen. Logged once per attempt so a quick test in
    // DebugView shows why the hotkey did nothing.
    if (m_isPiP) {
        if (!m_pipFullscreenBlockedLogged) {
            AppLog(L"[NitLink/PiP] fullscreen toggle ignored while PiP is active");
            m_pipFullscreenBlockedLogged = true;
        }
        return;
    }
    m_pipFullscreenBlockedLogged = false;

    m_isFullscreen = !m_isFullscreen;
    m_window->SetFullscreen(m_isFullscreen);

    // Note: the WebView2 settings overlay (when visible) is a child of
    // main, so it follows fullscreen automatically. The resize handler
    // calls webviewSettings->Resize() on the new client dimensions.
}

void Application::TogglePiP()
{
    // Note: an earlier rc1 build refused this toggle when HDR output was
    // active because layered-window alpha over an HDR10 PQ backbuffer
    // looked broken. After the SetPiP exit-style fixes and the Alt+Enter
    // guard landed, PiP is stable enough to allow HDR PiP again:
    // the colours may still need work in a future post-rc1 SDR-in-HDR
    // PiP path, but the window-state behaviour is sound.

    m_isPiP = !m_isPiP;

    if (m_isPiP) {
        m_isFullscreen = false;
        m_window->SetFullscreen(false);
        // Hand the saved (or hotkey-updated) preferred top-left to Window
        // so it places PiP where the user last had it. -1 / -1 = default
        // bottom-right corner.
        m_window->SetPreferredPiPPosition(m_config->pipX, m_config->pipY);
        m_window->SetPiP(true, m_config->pipWidth, m_config->pipHeight, m_config->pipOpacity);
        // Sync back the actual position SetPiP landed on (in case the
        // preferred coord was clamped) so persistence reflects reality.
        int32_t actualX = 0, actualY = 0;
        if (m_window->GetPiPPosition(actualX, actualY)) {
            m_config->pipX = actualX;
            m_config->pipY = actualY;
        }
    } else {
        m_window->SetPiP(false, 0, 0, 1.0f);
    }
}

void Application::ToggleOverlay()
{
    m_showOverlay = !m_showOverlay;
}

void Application::ShowToast(const std::wstring& text,
                              std::chrono::milliseconds duration)
{
    m_toastText   = text;
    m_toastExpiry = std::chrono::steady_clock::now() + duration;
}

void Application::ToggleSettings()
{
    m_settingsVisible = !m_settingsVisible;
    if (m_webviewSettings) {
        m_webviewSettings->Show(m_settingsVisible);
        // Push current state every time the menu opens so toggles reflect
        // the truth: important for things that can change outside the menu
        // (Alt+H for HDR, Ctrl+G for game cycle).
        if (m_settingsVisible) PushSettingsState();
    }
}

bool Application::ShouldShowNoSignal()
{
    using namespace std::chrono;
    using Clock = steady_clock;

    // Reacquire grace: how long to keep showing the last good frame after
    // the capture pipeline stops delivering frames. 2.5s comfortably
    // covers PS5 boot logo, source switch, and SDR<->HDR handshakes (all
    // empirically <2s) without making true signal loss feel unresponsive.
    constexpr auto kReacquireGrace   = milliseconds(2500);
    // Startup grace: shorter so a user who launches the app with no
    // source connected isn't staring at a black window for 2.5s.
    constexpr auto kStartupGrace     = milliseconds(1500);
    // Placeholder-confirmed grace: tighter still. Once the detector has
    // confirmed an Elgato no-signal placeholder (5 consecutive fingerprint
    // matches, ~83 ms), there's no point waiting the full 2.5s: by
    // definition the source is gone and the frames arriving are Elgato's
    // own image, not the upstream. 1s is short enough to feel responsive
    // on a real unplug, long enough to absorb the rare case where a brief
    // HDMI handshake state happens to fingerprint as a placeholder for a
    // fraction of a second before the real source resumes.
    constexpr auto kPlaceholderGrace = milliseconds(1000);
    // Reacquire-log debounce: minimum elapsed-without-a-real-frame before
    // surfacing the "Signal: reacquiring" transition. The render path
    // already paints the last good capture texture during this window
    // without any change, so there is nothing user-visible to surface;
    // logging the transition before this threshold just produces noise
    // on legitimate transient gaps (renderer iterating faster than the
    // capture worker, brief MF reader stalls between frames, short
    // format-reconcile windows). Once elapsed crosses this threshold,
    // the transition logs once and the latch holds until elapsed drops
    // back below the threshold or grace expires.
    constexpr auto kReacquireDebounce = milliseconds(250);

    const bool placeholderConfirmed = m_placeholderDetector
        && m_placeholderDetector->IsCurrentlyPlaceholder();

    // Decision-of-record: elapsed time since the last frame the run loop
    // accepted as a real source frame (UpdateCaptureTexture called and
    // m_lastGoodFrameTime bumped at the same site).
    //
    // FrameBuffer::IsSignalActive() is deliberately not consulted here.
    // That predicate is content-based: it flips false after 30 consecutive
    // identical-hash frames in the producer (about 500 ms at 60 Hz
    // capture, because the same 256 sampled bytes hash equal on bit-
    // static content). Legitimate low-motion playback like intro logos,
    // slow fades, paused menus, and splash screens routinely toggles that
    // predicate false even while real frames keep flowing and being
    // classified as Real downstream. Using it as a fast path here would
    // surface a paired "reacquiring" / "reacquired" log every ~500 ms
    // hash cycle of static content.
    //
    // Signal loss is decided from two positive / negative signals
    // instead. Positive: the placeholder detector (format-tagged
    // fingerprints plus the temporal-stability gate) confirms the
    // Elgato NO SIGNAL output. Negative: m_lastGoodFrameTime fails to
    // advance for longer than the active grace window.
    const auto now     = Clock::now();
    const auto elapsed = now - m_lastGoodFrameTime;
    const auto grace   = !m_hasEverReceivedFrame
        ? kStartupGrace
        : (placeholderConfirmed ? kPlaceholderGrace : kReacquireGrace);

    // Active / recovering: elapsed below the reacquire debounce. The
    // capture pipeline has produced a Real frame within the last
    // kReacquireDebounce, so by every meaningful definition the signal
    // is healthy. Clear the latches; the transition log fires only when
    // a "reacquiring" or "lost" latch was held coming into this call.
    if (elapsed < kReacquireDebounce) {
        if (m_signalLostLogged) {
            AppLog(L"Signal: restored (was lost)");
        } else if (m_signalReacquiringLogged) {
            AppLog(L"Signal: reacquired");
        }
        m_signalLostLogged        = false;
        m_signalReacquiringLogged = false;
        return false;
    }

    // Past the reacquire debounce but still within grace. Render side
    // keeps painting the last good capture texture; log the transition
    // once so DebugView shows the cause (startup window, placeholder
    // confirmed, or ordinary no-fresh-frame stall).
    if (elapsed < grace) {
        if (!m_signalReacquiringLogged && !m_signalLostLogged) {
            const wchar_t* reason = !m_hasEverReceivedFrame
                ? L"startup"
                : (placeholderConfirmed ? L"placeholder" : L"no-fresh-frame");
            const auto ms = duration_cast<milliseconds>(elapsed).count();
            std::wstringstream ss;
            ss << L"Signal: reacquiring (reason=" << reason
               << L", elapsed=" << ms
               << L"ms, grace=" << duration_cast<milliseconds>(grace).count()
               << L"ms, holding last good frame)";
            AppLog(ss.str());
            m_signalReacquiringLogged = true;
        }
        return false;
    }

    // Grace exhausted: commit to the no-signal page.
    if (!m_signalLostLogged) {
        const wchar_t* reason = !m_hasEverReceivedFrame
            ? L"startup"
            : (placeholderConfirmed ? L"placeholder" : L"no-fresh-frame");
        const auto ms = duration_cast<milliseconds>(elapsed).count();
        std::wstringstream ss;
        ss << L"Signal: lost (reason=" << reason
           << L", elapsed=" << ms
           << L"ms, exceeded " << duration_cast<milliseconds>(grace).count()
           << L"ms grace), showing no-signal screen";
        AppLog(ss.str());
        m_signalLostLogged        = true;
        m_signalReacquiringLogged = false;
    }
    return true;
}

bool Application::ReconcileCaptureFormat(bool force)
{
    // Same decision logic as Initialize: P010 when the user wants HDR AND
    // either the source was detected as HDR10 OR detection isn't available
    // (the user's preference is trusted in that case: typically the 4K S).
    // Keeping the formula in one place would be nicer, but pulling it into
    // a helper for two call sites (Initialize + here) isn't worth the extra
    // indirection yet.
    if (!m_captureDevice || !m_config) return false;

    // Forced-reopen path: refresh the source's HDR state synchronously
    // before computing wantP010 below. The capture worker only flags
    // needs-reopen on a fatal stream condition (MF media-type change,
    // reader error, or repeated ReadSample failures); the most common
    // trigger is the source itself transitioning HDR<->SDR (PS5 HDR
    // setting flipped, game launched with a different HDR mode, etc).
    // The periodic HDRSourcePoller will eventually catch up via its
    // AcceptUpdate drain in the main loop, but on a polling cadence of
    // ~1s the drain often hasn't fired yet by the time the capture
    // worker dies and forces us in here. Using the stale cached
    // m_sourceIsHDR10 in that window makes wantP010 below resolve to
    // the OLD format, the device re-opens as P010 against a now-SDR
    // source, and the next frames upload as green garbage (SDR-shaped
    // bytes interpreted as P010 luma+chroma). A synchronous re-read of
    // the Elgato InfoFrame property here closes that window for the
    // 4K Pro path; the 4K S path has no detection property and falls
    // through to the user's hdrEnabled preference as before.
    // Gate Elgato-specific calls in this function on whether the
    // current device is an Elgato. For non-Elgato sources, the
    // IKsPropertySet HDR InfoFrame property is unavailable and the
    // 4K S HID protocol does not apply. m_hdrDetectionAvailable is
    // already false for non-Elgato devices (set by Initialize and
    // SwitchCaptureDevice), so the InfoFrame re-read below was
    // already effectively gated; this makes the gating explicit and
    // keeps the structure consistent with the other call sites.
    const bool isElgato = IsElgatoDevice(m_currentDeviceInfo.name);

    if (force && isElgato && m_hdrDetectionAvailable) {
        HDRSourceInfo srcInfo = ReadElgatoHDRSource(m_currentDeviceInfo.name);
        if (srcInfo.propertyAccessible && srcInfo.isHDR10 != m_sourceIsHDR10) {
            AppLog(srcInfo.isHDR10
                ? L"Reconcile force-reopen: source InfoFrame re-read says HDR10 (was SDR)"
                : L"Reconcile force-reopen: source InfoFrame re-read says SDR (was HDR10)");
            m_sourceIsHDR10 = srcInfo.isHDR10;
        }
    }

    // Capture format selection: P010 whenever the source is HDR10, regardless
    // of the user's HDR-rendering preference. When the user has HDR rendering
    // off but the source is HDR10, the P010 shader path does an in-shader
    // PQ -> linear -> BT.709 -> Reinhard -> sRGB tonemap to the SDR backbuffer.
    //
    // Why not switch to NV12 when the user wants SDR output? Two reasons:
    //   1) The Elgato hardware tonemap toggle (IKsPropertySet "TonemapEnable")
    //      reliably turns OFF at init time but does NOT reliably turn back ON
    //      mid-session on this firmware: Set() returns S_OK but the card
    //      keeps passing raw HDR10. So NV12 + raw HDR10 codes interpreted as
    //      sRGB BT.709 gives a green cast (the v1.0.0 bug that drove this design).
    //   2) Tonemapping in shader gives 10-bit precision through the
    //      conversion (P010 source) and full control over the operator,
    //      which is better than the Elgato's baked-in tonemap anyway.
    //
    // For non-HDR sources the NV12 path still applies because P010 would
    // waste bandwidth (24MB/frame vs 12MB/frame at 4K).
    //
    // When HDR detection is unavailable (4K S), fall back to trusting the
    // user's hdrEnabled flag, same as Initialize.
    //
    // The expression is also gated on isElgato. The P010 path is only
    // validated for Elgato hardware; for non-Elgato sources (webcams,
    // third-party capture cards), force SDR regardless of hdrEnabled or
    // any cached HDR detection state. See the equivalent gate in
    // Initialize for the full rationale.
    const bool userWantsHDR = m_config->hdrEnabled;
    const bool wantP010     = isElgato &&
                              (m_sourceIsHDR10 ||
                               (userWantsHDR && !m_hdrDetectionAvailable));
    const bool isP010       = m_captureDevice->IsP010Requested();

    if (wantP010 == isP010 && !force) {
        // Device is already in the format the current state dictates.
        // The renderer-side flag updates (m_hdrEnabled, sourceIsHDR10) flow
        // through the cbuffer every frame, so the shader picks up the
        // SDR-from-HDR branch without needing a capture-format swap.
        // Cheap no-op: this method runs every reconcile trigger.
        return false;
    }

    if (force && wantP010 == isP010) {
        AppLog(wantP010
            ? L"Reconcile: forced reopen, staying on P010 HDR10"
            : L"Reconcile: forced reopen, staying on SDR");
    } else {
        AppLog(wantP010
            ? L"Reconcile: capture format change, SDR -> P010 HDR10"
            : L"Reconcile: capture format change, P010 HDR10 -> SDR");
    }

    // Tear down. StopCapture joins the worker so once it returns no more
    // frames will land in m_frameBuffer; Close releases the source reader
    // and the IMFMediaSource. m_captureDevice itself is NOT destroyed:
    // its FrameCallback and ownership stays stable.
    m_captureDevice->StopCapture();
    m_captureDevice->Close();

    // The frame buffer's capacity and stride are fixed at construction;
    // P010 needs 2x the bytes-per-row of NV12, so the buffer is rebuilt
    // after the new negotiated format is known. Release the old one
    // first so the capture worker can't possibly write into a buffer
    // about to be discarded (StopCapture already joined the worker,
    // this is belt-and-suspenders).
    m_frameBuffer.reset();

    // Disable the Elgato hardware tonemap before re-Open. All HDR<->SDR
    // conversion happens in the shaders now, so raw HDR10 codes from
    // the card are required whenever the source is HDR10. Gated on the
    // current device being an Elgato: webcams and third-party capture
    // cards do not implement this property and the call would only
    // generate log noise. (4K S HID tonemap is sent further down,
    // after the wantP010 decision is known and right before Open;
    // sending unconditional OFF here would leave the card in HDR-
    // passthrough mode when reconciling to an SDR/NV12 capture, which
    // makes the SDR picture wash out.)
    if (isElgato) {
        SetElgatoTonemap(m_currentDeviceInfo.name, false);
    } else {
        AppLog(L"Reconcile: current device is not Elgato; skipping Elgato-specific HDR controls");
    }

    // Re-enumerate first to handle the edge case where the user replugged
    // the card mid-session (the cached symbolic link would still resolve
    // but the device index can shift). Fall back to the cached DeviceInfo
    // if enumeration comes up empty for some transient reason.
    DeviceInfo deviceToOpen = m_currentDeviceInfo;
    {
        auto devices = DeviceEnumerator::FindCaptureDevices();
        if (!devices.empty()) {
            // First try to keep the device that was open before the
            // reconcile (handles the replug index-shift case where the
            // physical device is back but at a different MF index).
            bool matched = false;
            for (const auto& d : devices) {
                if (d.name == m_currentDeviceInfo.name) {
                    deviceToOpen = d;
                    matched = true;
                    break;
                }
            }
            // Fallback path: the previously-open device is gone from
            // the enumeration. Route through PickPreferredDevice so the
            // user's preferred_device config still wins and the
            // Elgato-bias still applies, matching Initialize's
            // selection logic.
            if (!matched) {
                int idx = DeviceEnumerator::PickPreferredDevice(
                    devices, m_config ? m_config->preferredDevice : std::wstring{});
                if (idx < 0 || static_cast<size_t>(idx) >= devices.size()) idx = 0;
                deviceToOpen = devices[idx];
            }
        } else {
            AppLog(L"Reconcile: device enumeration returned empty, using cached DeviceInfo");
        }
    }

    m_captureDevice->RequestP010(wantP010);

    // Pair the 4K S HID tonemap state with the upcoming capture format
    // (same rationale as Initialize). On Alt+H toggling from HDR to SDR
    // this re-sends ON so the card stops passing raw HDR10 through and
    // starts delivering clean SDR for the NV12 capture; on toggling
    // back to HDR it re-sends OFF. Same Elgato gate as above: the HID
    // protocol is 4K S-specific (VID 0x0FD9), so non-Elgato sources
    // skip the call.
    if (isElgato && Set4KSTonemap(/*enableTonemap=*/ !wantP010)) {
        AppLog(wantP010
            ? L"Reconcile: 4K S HID tonemap OFF sent for raw HDR/P010"
            : L"Reconcile: 4K S HID tonemap ON sent for SDR/NV12");
    }

    bool opened = m_captureDevice->Open(deviceToOpen);
    if (!opened && wantP010) {
        // P010 negotiation failed at the MF level. Same fallback as
        // Initialize: retry without P010 to get the SDR pipeline back.
        // Roll the user's HDR config flag back so the UI reflects what
        // actually happened (and so the next reconcile doesn't immediately
        // try P010 again on the next iteration).
        AppLog(L"Reconcile: P010 Open failed, retrying with BGRA");
        m_captureDevice->RequestP010(false);
        // Same retry-fallback HID flip as Initialize: tonemap was
        // sent OFF for the P010 attempt, that failed, so the pipeline
        // is falling back to SDR capture; flip the card's tonemap
        // state to ON so the NV12 frames come out clean. Elgato-gated
        // like the call above.
        if (isElgato && Set4KSTonemap(/*enableTonemap=*/ true)) {
            AppLog(L"Reconcile: 4K S HID tonemap ON sent for SDR retry");
        }
        opened = m_captureDevice->Open(deviceToOpen);
        if (opened) {
            AppLog(L"Reconcile: rolled back hdrEnabled, P010 unavailable for this source");
            m_config->hdrEnabled = false;
        }
    }

    if (!opened) {
        // Both attempts failed. Capture device is closed; frames will
        // stop arriving and the no-signal screen will take over. Surface
        // the failure so the user at least sees something in the log.
        AppLog(L"Reconcile: CaptureDevice::Open FAILED after format swap, capture is stopped");
        m_currentDeviceInfo = deviceToOpen;
        return false;
    }

    // Successful open: rebuild the frame buffer at the new format, refresh
    // the renderer's row-order and range hints (driver flips between paths
    // can change either), and restart the capture worker.
    auto format = m_captureDevice->GetOutputFormat();
    m_frameBuffer = std::make_unique<FrameBuffer>(format.width, format.height, format.stride);

    // Reset the frame differ's per-stream state. Its m_prevTex still holds
    // luma from the previous capture format's frames, and its smoothing ring
    // buffer holds pre-reconcile votes; without this clear, the first
    // post-reconcile diff is against stale data and a sticky "duplicate"
    // classification can persist across the swap, leaving contentFps wedged
    // at 0 even when fresh content is arriving from the new capture session.
    if (m_frameDiffer) m_frameDiffer->Reset();

    // Reset the placeholder detector for the same reason: a streak of
    // matching frames in the prior format would otherwise carry over, and
    // the same Elgato placeholder encodes to different zone luma in NV12
    // vs P010 vs BGRA, so any in-flight match streak is invalid post-swap.
    if (m_placeholderDetector) m_placeholderDetector->Reset();
    m_inPlaceholderState = false;

    if (m_renderer) {
        m_renderer->SetSourceRowOrder(format.topDown);
        m_renderer->SetSourceFullRange(format.fullRange);
        // Push the shader's HDR-source flag based on the ACTUAL negotiated
        // capture format, not on m_sourceIsHDR10. See the long-form comment
        // in Initialize() for why these are different questions on the 4K S.
        //
        // Critical: do NOT write to m_sourceIsHDR10 here. That variable is
        // consumed at the top of this function (and elsewhere) to decide
        // what format to negotiate; setting it to "true whenever capture
        // is P010" would create a feedback loop on the 4K S: every
        // reconcile would see "in P010, so source must be HDR10, so
        // stay in P010", and Alt+H off would no-op instead of switching
        // back to NV12.
        const bool captureIsP010 = IsEqualGUID(format.subtype, MFVideoFormat_P010);
        m_renderer->SetSourceIsHDR10(captureIsP010);

        // Same subtype-to-enum routing as Initialize. Critical to do this BEFORE
        // StartCapture so the new capture worker thread can never deliver a
        // frame to UpdateCaptureTexture with a stale m_sourceFormat from the
        // previous format's session.
        DX11Renderer::CaptureFormatKind rkind;
        if (IsEqualGUID(format.subtype, MFVideoFormat_P010)) {
            rkind = DX11Renderer::CaptureFormatKind::P010;
        } else if (IsEqualGUID(format.subtype, MFVideoFormat_NV12)) {
            rkind = DX11Renderer::CaptureFormatKind::NV12;
        } else {
            rkind = DX11Renderer::CaptureFormatKind::BGRA;
        }
        m_renderer->SetSourceFormat(rkind);
    }

    m_captureDevice->StartCapture([this](const uint8_t* data, uint32_t size, int64_t timestamp) {
        if (m_frameBuffer) m_frameBuffer->Write(data, size, timestamp);
    });

    m_currentDeviceInfo = deviceToOpen;
    AppLog(L"Reconcile: capture restarted in new format");
    return true;
}

bool Application::SwitchCaptureDevice(const std::wstring& deviceName)
{
    if (!m_captureDevice || !m_config) return false;

    // Resolve the user-provided name against the live enumeration.
    auto devices = DeviceEnumerator::FindCaptureDevices();
    if (devices.empty()) {
        AppLog(L"SwitchCaptureDevice: enumeration returned no devices");
        return false;
    }

    // Strict resolver: exact name match, then substring, then fail.
    // Unlike PickPreferredDevice's startup ladder, this does NOT fall
    // back to Elgato-bias or to devices[0] when the requested name is
    // not present. An explicit user switch must open the requested
    // device or none; silently swapping in a different device than the
    // user asked for would be a worse failure mode than returning false
    // and leaving the current device active.
    int idx = DeviceEnumerator::FindDeviceByName(devices, deviceName);
    if (idx < 0) {
        std::wstringstream ss;
        ss << L"SwitchCaptureDevice: no device matched '" << deviceName
           << L"' (enumeration size=" << devices.size()
           << L"); keeping current device";
        AppLog(ss.str());
        return false;
    }
    const DeviceInfo& newDevice = devices[idx];

    // Same-device case: skip the tear-down + reconcile, but still update
    // and persist the saved preference so an explicit user confirmation
    // sticks across the next launch.
    if (newDevice.name == m_currentDeviceInfo.name) {
        if (m_config->preferredDevice != deviceName) {
            m_config->preferredDevice = deviceName;
            m_config->Save("nitlink.json");
        }
        return true;
    }

    // Snapshot rollback state. Capture mutable members about to change
    // so a failed reconcile on the new device can restore the previous
    // working pipeline. Without this, an Open() failure on the new
    // device would leave the user staring at a black window.
    const DeviceInfo previousDevice = m_currentDeviceInfo;
    const bool previousSourceIsHDR10 = m_sourceIsHDR10;
    const bool previousHdrDetectionAvailable = m_hdrDetectionAvailable;
    const std::wstring previousPreferredDevice = m_config->preferredDevice;

    {
        std::wstringstream ss;
        ss << L"SwitchCaptureDevice: '" << previousDevice.name
           << L"' -> '" << newDevice.name << L"'";
        AppLog(ss.str());
    }

    // Apply the new selection. ReconcileCaptureFormat keys off
    // m_currentDeviceInfo.name when deciding which enumerated device to
    // open, so updating the cache here is what redirects the reconcile
    // to the new device instead of re-opening the previous one.
    m_currentDeviceInfo = newDevice;
    m_config->preferredDevice = deviceName;

    // Refresh HDR detection state for the new device. The InfoFrame
    // read is an Elgato-specific property call; skip it for non-Elgato
    // devices and clear the detection flags so the reconcile's
    // wantP010 decision below falls through to the user's hdrEnabled
    // preference, same as the 4K S path does at startup.
    if (IsElgatoDevice(newDevice.name)) {
        HDRSourceInfo srcInfo = ReadElgatoHDRSource(newDevice.name);
        m_hdrDetectionAvailable = srcInfo.propertyAccessible;
        m_sourceIsHDR10 = srcInfo.propertyAccessible ? srcInfo.isHDR10 : false;
    } else {
        m_hdrDetectionAvailable = false;
        m_sourceIsHDR10 = false;
        AppLog(L"SwitchCaptureDevice: selected device is not Elgato; skipping Elgato-specific HDR controls");
    }

    // Hand off to Reconcile for the actual transition work: tear down
    // the current capture, run the Elgato property + 4K S HID tonemap
    // calls for the new device, negotiate the format (with P010 -> NV12
    // fallback when needed), rebuild the framebuffer, reset the frame
    // differ and placeholder detector, sync the renderer's row order
    // and HDR10 flag, and restart the capture worker.
    if (!ReconcileCaptureFormat(/*force=*/ true)) {
        std::wstringstream ss;
        ss << L"SwitchCaptureDevice: reconcile failed on '" << newDevice.name
           << L"', rolling back to '" << previousDevice.name << L"'";
        AppLog(ss.str());

        // Restore the previous state and reconcile back onto it. If the
        // rollback reconcile also fails (the previous device was
        // unplugged between the snapshot and now, for example), the
        // existing no-signal UI takes over and the user sees a clear
        // failure rather than a half-open pipeline.
        m_currentDeviceInfo = previousDevice;
        m_sourceIsHDR10 = previousSourceIsHDR10;
        m_hdrDetectionAvailable = previousHdrDetectionAvailable;
        m_config->preferredDevice = previousPreferredDevice;
        if (!ReconcileCaptureFormat(/*force=*/ true)) {
            AppLog(L"SwitchCaptureDevice: rollback reconcile also failed; capture pipeline is down");
        }
        return false;
    }

    // Persist the new preference so the next launch opens this device
    // by default through Initialize's PickPreferredDevice path.
    m_config->Save("nitlink.json");
    return true;
}

void Application::PushSettingsState()
{
    if (!m_webviewSettings || !m_config) return;

    // Build a JSON state blob and push to JS via WebView2 PostWebMessageAsJson.
    // Schema must match what nitlink-menu.html's applyState() expects.
    // Keep manual (no JSON lib) since it's small and structured.
    std::wstringstream js;
    js << L"{\"state\":{";
    js << L"\"hdrEnabled\":"        << (m_config->hdrEnabled        ? L"true" : L"false") << L",";
    js << L"\"colorExpansion\":"    << (m_config->colorExpansion    ? L"true" : L"false") << L",";
    js << L"\"nisEnabled\":"        << (m_config->nisEnabled        ? L"true" : L"false") << L",";
    js << L"\"vrrPresentPacing\":"  << (m_config->vrrPresentPacing  ? L"true" : L"false") << L",";
    js << L"\"audioMuted\":"        << (m_config->audioMuted        ? L"true" : L"false") << L",";
    js << L"\"volume\":"            << m_config->audioVolume        << L",";
    js << L"\"scalerName\":\"Catmull-Rom\",";

    // Header meta line: real capture resolution, last measured fps,
    // and end-to-end latency. These come from the running pipeline.
    {
        uint32_t w = 0, h = 0;
        if (m_captureDevice) {
            auto fmt = m_captureDevice->GetOutputFormat();
            w = fmt.width;
            h = fmt.height;
        }
        std::wstringstream res;
        if (w && h) {
            res << w << L"×" << h << L" · " << m_currentContentFps << L" fps";
        } else {
            res << L"no signal";
        }
        js << L"\"resolutionText\":\"" << res.str() << L"\",";

        // Link type. The Elgato capture cards NitLink targets are HDMI-
        // input devices, so the link is always HDMI by hardware contract.
        // The field exists as a string rather than a constant so a later
        // EDID-derived link-type readout can drop in without a schema
        // change on the JS side.
        js << L"\"linkText\":\"HDMI\",";

        const double e2e = m_captureLatencyMs + m_renderLatencyMs;
        std::wstringstream lat;
        lat << static_cast<int>(e2e + 0.5) << L" ms";
        js << L"\"latencyText\":\"" << lat.str() << L"\",";
    }

    // ===== Game selector state =====
    //
    // currentGameId: the selected game's id, or "" if none. JS reads this
    // to highlight the current selection in the dropdown.
    //
    // gameList: array of { id, title } for every entry in the built-in
    // catalog. JS uses it to populate the search dropdown.
    {
        std::wstring wcur;
        wcur.reserve(m_config->currentGameId.size());
        for (char c : m_config->currentGameId) wcur.push_back(static_cast<wchar_t>(c));
        js << L"\"currentGameId\":\"" << wcur << L"\",";

        js << L"\"gameList\":[";
        const auto& games = GetGameDatabase();
        for (size_t i = 0; i < games.size(); ++i) {
            const auto& g = games[i];
            // ASCII-safe widening of id; titles may need escaping for the
            // few that contain apostrophes or quotes.
            std::wstring wid;
            wid.reserve(g.id.size());
            for (char c : g.id) wid.push_back(static_cast<wchar_t>(c));

            std::wstring wtitle;
            wtitle.reserve(g.title.size());
            for (char c : g.title) {
                // Escape \ and " (apostrophes don't need escaping in JSON).
                if (c == '\\') { wtitle.push_back(L'\\'); wtitle.push_back(L'\\'); }
                else if (c == '"') { wtitle.push_back(L'\\'); wtitle.push_back(L'"'); }
                else wtitle.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }

            js << L"{\"id\":\"" << wid << L"\",\"title\":\"" << wtitle << L"\"}";
            if (i + 1 < games.size()) js << L",";
        }
        js << L"]";
    }

    // ===== Capture-device picker state =====
    //
    // captureDevices: array of friendly-name strings produced by a
    // fresh Media Foundation enumeration. The order matches MF
    // enumeration order; the UI consumer can display it directly.
    // Each name is run through JsonEscapeWide so non-ASCII Unicode
    // and the rare quote/backslash do not break the JSON.
    //
    // activeDevice: friendly name of the device the capture pipeline
    // currently has open (mirrors m_currentDeviceInfo.name). The UI
    // uses this to mark which entry in captureDevices is currently
    // active. May differ from preferredDevice when the saved
    // preference was unplugged and Initialize fell through to a
    // different device via PickPreferredDevice.
    //
    // preferredDevice: the user's saved preference from Config. The
    // UI can show this distinctly from activeDevice when the two
    // differ ("you asked for X, currently showing Y because X is
    // not connected"). Empty when the user has never explicitly
    // picked a device.
    {
        js << L",\"captureDevices\":[";
        auto devices = DeviceEnumerator::FindCaptureDevices();
        for (size_t i = 0; i < devices.size(); ++i) {
            js << L"\"" << JsonEscapeWide(devices[i].name) << L"\"";
            if (i + 1 < devices.size()) js << L",";
        }
        js << L"]";

        js << L",\"activeDevice\":\""
           << JsonEscapeWide(m_currentDeviceInfo.name) << L"\"";
        js << L",\"preferredDevice\":\""
           << JsonEscapeWide(m_config->preferredDevice) << L"\"";
    }

    js << L"}}";

    m_webviewSettings->PostMessage(js.str());
}

void Application::ApplyGameSettings(const std::string& gameId)
{
    if (!m_config || gameId.empty()) return;

    // If the user has saved settings for this game, load them into the live
    // config. Otherwise, seed a new entry with the *current* settings so the
    // user has a starting point to tweak from.
    auto it = m_config->gameSettings.find(gameId);
    if (it != m_config->gameSettings.end()) {
        const auto& gs = it->second;
        m_config->nisEnabled     = gs.nisEnabled;
        m_config->colorExpansion = gs.colorExpansion;

        // Notify the renderer about the pipeline change. Color expansion
        // feeds into the SDR shader so push it now; NIS is checked per
        // frame by the render loop and needs no explicit kick.
        if (m_renderer) {
            m_renderer->SetColorExpansion(m_config->colorExpansion);
        }

        std::wstring wid;
        for (char c : gameId) wid.push_back(static_cast<wchar_t>(c));
        AppLog(L"Applied saved settings for game: " + wid);
    } else {
        // First time selecting this game: record current state as its
        // starting profile. User can tweak and Save to commit.
        GameSettings gs;
        gs.nisEnabled     = m_config->nisEnabled;
        gs.colorExpansion = m_config->colorExpansion;
        m_config->gameSettings[gameId] = gs;
    }
}

void Application::UpdateDiscordForCurrentGame()
{
    if (!m_discord || !m_discord->IsConnected() || !m_config) return;

    if (m_config->currentGameId.empty()) {
        // No game: back to idle presence
        m_discord->SetActivity(
            L"In NitLink",
            L"PS5 Capture Viewer",
            std::chrono::system_clock::now(),
            "nitlink-logo",
            L"NitLink"
        );
        return;
    }

    // Look up the human-readable title from the catalog. If a currentGameId
    // is somehow not in the catalog (manual edit of config?), fall back to
    // the id itself as the display string.
    const auto* entry = FindGameById(m_config->currentGameId);
    std::wstring title;
    if (entry) {
        for (char c : entry->title) title.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    } else {
        for (char c : m_config->currentGameId) title.push_back(static_cast<wchar_t>(c));
    }

    // Use the game id as the Discord art asset key. The user must have
    // uploaded an asset with this exact name to their Discord Developer
    // Portal for the image to render. If no asset exists, Discord falls
    // back to no image: the text still shows fine.
    m_discord->SetActivity(
        title,                                      // "Spider-Man 2"
        L"Playing on PS5",                          // state line under title
        std::chrono::system_clock::now(),           // restart elapsed timer
        m_config->currentGameId,                    // large image asset key
        title                                       // tooltip when hovering image
    );
}

void Application::TakeScreenshot()
{
    AppLog(L"TakeScreenshot: ENTRY");
    OutputDebugStringW(L"[NitLink/App] TakeScreenshot ENTRY\n");

    // Save screenshots under the user's Pictures folder in a NitLink subdir
    PWSTR picturesPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &picturesPath))) {
        AppLog(L"Screenshot: failed to find Pictures folder");
        return;
    }

    std::wstring folder = std::wstring(picturesPath) + L"\\NitLink";
    CoTaskMemFree(picturesPath);

    CreateDirectoryW(folder.c_str(), nullptr);

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_s(&tm_buf, &time_t);

    std::wstring safeTitle = L"nitlink";

    wchar_t timestamp[64];
    wcsftime(timestamp, 64, L"%Y%m%d_%H%M%S", &tm_buf);

    std::wstring fullPath = folder + L"\\" + safeTitle + L"_" + timestamp + L".png";

    AppLog(L"Screenshot: calling SaveScreenshot for " + fullPath);
    bool ok = m_renderer->SaveScreenshot(fullPath);
    if (ok) {
        AppLog(L"Screenshot saved: " + fullPath);
    } else {
        AppLog(L"Screenshot failed");
    }
}

void Application::UpdateTaskbarIcon(const std::wstring& iconPath)
{
    m_window->SetIcon(iconPath);
}

} // namespace NitLink
