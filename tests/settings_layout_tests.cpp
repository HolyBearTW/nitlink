#include "app/WebViewSettings.h"
#include "app/settings_message.h"

#include <shlwapi.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Microsoft::WRL::Callback;

struct WebViewSettingsTestAccess {
    static ICoreWebView2* View(WebViewSettings& host) { return host.m_webview.get(); }
    static ICoreWebView2Controller* Controller(WebViewSettings& host) { return host.m_controller.get(); }
};

namespace {
void Check(bool result, const char* label) {
    if (!result) throw std::runtime_error(label);
    std::cout << "PASS " << label << '\n';
}
void Hr(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("WebView API failed: " + std::to_string(hr));
}
bool PumpUntil(const std::function<bool()>& predicate, int milliseconds = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (predicate()) return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
std::wstring Script(ICoreWebView2* view, const std::wstring& script) {
    struct Result { bool done = false; HRESULT hr = E_PENDING; std::wstring value; };
    const auto result = std::make_shared<Result>();
    Hr(view->ExecuteScript(script.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [result](HRESULT hr, LPCWSTR json) -> HRESULT {
            result->hr = hr;
            if (json) result->value = json;
            result->done = true;
            return S_OK;
        }).Get()));
    if (!PumpUntil([&] { return result->done; })) throw std::runtime_error("script timed out");
    Hr(result->hr);
    return result->value;
}

// These are test values, not observations from capture hardware. Every native
// state push carries the same settings; only panelSide and locale vary.
void PushState(WebViewSettings& host, const wchar_t* side, const wchar_t* locale = L"en-US") {
    host.PostMessage(std::wstring(LR"({"state":{"hdrEnabled":true,"colorExpansion":true,
        "colorExpansionAvailable":true,"nisEnabled":true,"vsync":false,
        "lowLatency":true,"preventSleep":true,"presentPacing":"unique",
        "audioMuted":false,"volume":0.37,"pipOpacity":0.64,"aspectRatio":"16:9",
        "noSignalMode":"image","noSignalImage":"C:\\layout-test.png",
        "noSignalImageAvailable":true,"noSignalFit":"contain","noSignalDimImage":true,
        "configWarning":"","hdrAutoDetectSupported":true,
        "negotiatedWidth":1920,"negotiatedHeight":1080,"negotiatedFps":60,
        "negotiatedFormat":"NV12","linkText":"HDMI","latencyText":"--",
        "captureDevices":["Layout test device"],"activeDevice":"Layout test device",
        "availableFormats":[{"width":1920,"height":1080,"fps":60,"format":"NV12"}],
        "captureFormatOverride":{"width":0,"height":0,"fps":0,
            "fpsNumerator":0,"fpsDenominator":1,"format":""},
        "languagePreference":"system","panelSide":")") + side +
        L"\",\"locale\":\"" + locale + L"\"}}");
    auto* view = WebViewSettingsTestAccess::View(host);
    const std::wstring expected = side == std::wstring_view(L"Full") ? L"full"
        : side == std::wstring_view(L"Left") ? L"left" : L"right";
    Check(PumpUntil([&] {
        return Script(view, L"document.documentElement.dataset.side === '" + expected +
            L"' && document.documentElement.lang === '" + locale + L"'") == L"true";
    }), "native presentation state applied");
    PumpUntil([] { return false; }, 180);
}

void Resize(WebViewSettings& host, HWND window, int width, int height,
            WebViewSettings::Dock dock, int dockWidth = 420) {
    RECT outer{0, 0, width, height};
    Check(AdjustWindowRectEx(&outer, WS_POPUP, FALSE, 0) != FALSE, "client size calculated");
    Check(SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "test window resized");
    host.SetDock(dock, dockWidth);
    host.Show(true);
    host.Resize();
    auto* view = WebViewSettingsTestAccess::View(host);
    const int cssWidth = dock == WebViewSettings::Dock::Full ? width : dockWidth;
    const bool sized = PumpUntil([&] {
        return Script(view, L"innerWidth === " + std::to_wstring(cssWidth) +
            L" && innerHeight === " + std::to_wstring(height)) == L"true";
    });
    if (!sized) {
        RECT client{}, bounds{};
        GetClientRect(window, &client);
        Hr(WebViewSettingsTestAccess::Controller(host)->get_Bounds(&bounds));
        std::wcout << L"Client " << client.right << L'x' << client.bottom << L"; bounds "
            << bounds.left << L',' << bounds.top << L',' << bounds.right << L',' << bounds.bottom
            << L"; viewport " << Script(view, L"[innerWidth,innerHeight]") << L'\n';
    }
    Check(sized, "native bounds produce expected CSS viewport");
}

const wchar_t* SharedValues = LR"JS(JSON.stringify({
    toggles: [...document.querySelectorAll('.toggle')].map(e => [e.id, e.className]),
    values: ['aspect-val', 'pacing-val', 'slider-volume-val', 'slider-pip-opacity-val',
        'no-signal-mode-val', 'no-signal-fit-val', 'no-signal-dim-val', 'meta-source',
        'meta-resolution', 'meta-format', 'meta-link', 'meta-latency'].map(id =>
        [id, document.getElementById(id).textContent]),
    selects: [...document.querySelectorAll('select, input')].map(e => [e.id, e.value]),
    activePane: document.querySelector('.pane[data-active="true"]').id,
    advancedOpen: document.querySelector('.advanced').open,
    sourceOpen: document.getElementById('source-popover').dataset.open,
    actions: [...document.querySelectorAll('[data-action]')].map(e => e.dataset.action)
}))JS";

const wchar_t* CompactSnapshot = LR"JS(JSON.stringify(
    [...document.querySelectorAll(`.rail, .rail-brand, .rail-item, .content, .topbar,
        .metabar, .meta, main, .pane, .band, .row, .row-name, .row-right, .row-chip,
        .row-value, .toggle, .slider-wrap, .slider, .language-select, .about-copy,
        .source-popover, #config-warning`)].map(e => {
        const r = e.getBoundingClientRect(), s = getComputedStyle(e);
        return {id: e.id, class: e.className, action: e.dataset.action,
            rect: [r.x, r.y, r.width, r.height],
            style: Array.from(s).map(k => [k, s.getPropertyValue(k)])};
    })
))JS";

void WriteText(const std::filesystem::path& path, const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    Check(size > 0, "snapshot encodes as UTF-8");
    std::string bytes(size, '\0');
    Check(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        bytes.data(), size, nullptr, nullptr) == size, "snapshot encoding completes");
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    Check(output.good(), "layout snapshot saved");
}

void Capture(ICoreWebView2* view, const std::filesystem::path& path) {
    wil::com_ptr<IStream> stream;
    Hr(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
        FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream));
    struct Result { bool done = false; HRESULT hr = E_PENDING; };
    const auto result = std::make_shared<Result>();
    Hr(view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([result](HRESULT hr) -> HRESULT {
            result->hr = hr;
            result->done = true;
            return S_OK;
        }).Get()));
    Check(PumpUntil([&] { return result->done; }), "native WebView preview completes");
    Hr(result->hr);
    Hr(stream->Commit(STGC_DEFAULT));
}

void PushAboutUpdate(WebViewSettings& host, const wchar_t* side, const wchar_t* locale,
    const wchar_t* status, const wchar_t* latest, bool hasNewer, bool automatic, bool busy,
    bool unread, const wchar_t* url) {
    PushState(host, side, locale);
    const auto timestamp = std::wstring_view(status) == L"never" ? 0 : 1791691200;
    std::wstring message = L"{\"state\":{\"holyBearUpdate\":{\"current\":\"1.2.4-holybear\",\"latest\":\"";
    message += latest;
    message += L"\",\"lastCheckUtc\":" + std::to_wstring(timestamp);
    message += L",\"automatic\":";
    message += automatic ? L"true" : L"false";
    message += L",\"busy\":";
    message += busy ? L"true" : L"false";
    message += L",\"status\":\"";
    message += status;
    message += L"\",\"hasNewer\":";
    message += hasNewer ? L"true" : L"false";
    message += L",\"unread\":";
    message += unread ? L"true" : L"false";
    message += L",\"url\":\"";
    message += url;
    message += L"\"}}}";
    host.PostMessage(message);
    auto* view = WebViewSettingsTestAccess::View(host);
    std::wstring ready = L"(() => { const node=document.querySelector('#holy-bear-update-status'); "
        L"let expected=window.NitLinkLocales['";
    ready += locale;
    ready += L"']['update.";
    ready += status;
    ready += L"'];";
    if (std::wstring_view(status) == L"available")
        ready += L"expected=expected.replace('{version}','" +
            std::wstring(latest).substr(1, std::wstring(latest).size() - 10) + L"');";
    ready += L"return document.documentElement.lang==='";
    ready += locale;
    ready += L"' && node.textContent===expected && node.hidden===";
    ready += std::wstring_view(status) == L"never" ? L"true" : L"false";
    ready += L" && document.querySelector('#toggle-holy-bear-auto-update').getAttribute('aria-checked')==='";
    ready += automatic ? L"true" : L"false";
    ready += L"'; })()";
    Check(PumpUntil([&] {
        return Script(view, ready) == L"true";
    }), "mock update state reaches About UI");
}

void SelectAbout(ICoreWebView2* view, bool allowLegacyCapture = false) {
    Script(view, L"document.querySelector('.rail-item[data-tab=\"pane-about\"]').click()");
    Check(PumpUntil([&] {
        return Script(view, allowLegacyCapture
            ? L"document.querySelector('#pane-about').dataset.active === 'true'"
            : L"document.documentElement.dataset.activePane === 'pane-about' && "
              L"document.querySelector('#pane-about').dataset.active === 'true'") == L"true";
    }), "About navigation updates the active pane");
}

void CaptureLayouts(WebViewSettings& host, HWND window, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    auto* view = WebViewSettingsTestAccess::View(host);
    for (const auto size : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        const auto name = std::to_wstring(size.first) + L"x" + std::to_wstring(size.second);
        for (const auto side : {L"Full", L"Left", L"Right"}) {
            const auto dock = side == std::wstring_view(L"Full") ? WebViewSettings::Dock::Full
                : side == std::wstring_view(L"Left") ? WebViewSettings::Dock::Left : WebViewSettings::Dock::Right;
            Resize(host, window, size.first, size.second, dock);
            PushState(host, side);
            Script(view, L"document.querySelector('.rail-item[data-tab=\"pane-video\"]').click()");
            PumpUntil([] { return false; }, 180);
            Capture(view, folder / (name + L"-" + side + L"-test-host.png"));
            if (dock == WebViewSettings::Dock::Full) {
                WriteText(folder / (name + L"-Full-geometry.json"), Script(view, LR"JS(JSON.stringify(
                    [...document.querySelectorAll('.content, .topbar, .topbar h1, .topbar-actions, .metabar, main')]
                        .map(e => ({name: e.className || e.tagName, rect: e.getBoundingClientRect().toJSON(),
                            rows: getComputedStyle(e).gridTemplateRows, area: getComputedStyle(e).gridArea}))
                ))JS"));
            }
            if (dock != WebViewSettings::Dock::Full) {
                for (const auto pane : {L"pane-video", L"pane-audio", L"pane-shortcuts", L"pane-about"}) {
                    Script(view, L"document.querySelector('.rail-item[data-tab=\"" + std::wstring(pane) + L"\"]').click()");
                    PumpUntil([] { return false; }, 180);
                    WriteText(folder / (name + L"-" + side + L"-" + pane + L".json"), Script(view, CompactSnapshot));
                }
            }
        }
    }
    auto captureAbout = [&](const wchar_t* name, const wchar_t* side, const wchar_t* locale,
                            const wchar_t* status, const wchar_t* latest, bool newer,
                            const wchar_t* url, int width, int height, WebViewSettings::Dock dock) {
        Resize(host, window, width, height, dock);
        PushAboutUpdate(host, side, locale, status, latest, newer, false, false, newer, url);
        SelectAbout(view, true);
        Script(view, L"{ const details=document.querySelector('.about-details'); if(details) details.open=false; }");
        PumpUntil([] { return false; }, 120);
        Capture(view, folder / name);
    };
    captureAbout(L"about-docked-zh-TW-current.png", L"Right", L"zh-TW", L"current",
        L"v1.2.4-holybear", false, L"", 1920, 1080, WebViewSettings::Dock::Right);
    Script(view, L"document.querySelector('.about-details').open = true");
    PumpUntil([] { return false; }, 120);
    Capture(view, folder / L"about-docked-zh-TW-details.png");
    captureAbout(L"about-docked-en-US-available.png", L"Right", L"en-US", L"available",
        L"v1.2.5-holybear", true,
        L"https://github.com/HolyBearTW/nitlink/releases/tag/v1.2.5-holybear",
        1920, 1080, WebViewSettings::Dock::Right);
    captureAbout(L"about-docked-zh-TW-api-unavailable.png", L"Right", L"zh-TW", L"apiUnavailable",
        L"v1.2.4-holybear", false, L"", 1920, 1080, WebViewSettings::Dock::Right);
    captureAbout(L"about-full-1920x1080-zh-TW.png", L"Full", L"zh-TW", L"current",
        L"v1.2.4-holybear", false, L"", 1920, 1080, WebViewSettings::Dock::Full);
    captureAbout(L"about-full-2560x1440-zh-TW.png", L"Full", L"zh-TW", L"current",
        L"v1.2.4-holybear", false, L"", 2560, 1440, WebViewSettings::Dock::Full);
    WriteText(folder / L"README.txt", L"Native WebView2 test-host renders of packaged NitLink settings.\n"
        L"Signal and settings are a fixed test fixture, not live capture observations.\n"
        L"Full PNGs have 1920x1080 and 2560x1440 viewports. Docked PNGs are 420px strips.\n"
        L"About PNGs exercise the localized current, available, API-error and disclosure states.\n"
        L"These do not replace screenshots from the NitLink application on real hardware.\n");
}

void TestLayouts(WebViewSettings& host, HWND window, std::vector<std::wstring>& messages) {
    auto* view = WebViewSettingsTestAccess::View(host);
    Resize(host, window, 1920, 1080, WebViewSettings::Dock::Right);
    PushState(host, L"Right");
    Script(view, LR"JS(
        document.querySelector('.advanced').open = true;
        document.querySelector('#source-trigger').click();
        document.querySelector('#source-trigger').click();
        window.__controls = [...document.querySelectorAll('[id], [data-action]')]
            .filter(e => !e.closest('#source-popover'));
    )JS");
    const auto values = Script(view, SharedValues);
    const auto compactRight = Script(view, CompactSnapshot);
    Resize(host, window, 1920, 1080, WebViewSettings::Dock::Left);
    PushState(host, L"Left");
    const auto compactLeft = Script(view, CompactSnapshot);
    const size_t count = messages.size();
    for (const auto size : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        Resize(host, window, size.first, size.second, WebViewSettings::Dock::Full);
        PushState(host, L"Full");
        Check(Script(view, LR"JS((() => {
            const meta = document.querySelector('.metabar').getBoundingClientRect();
            const main = document.querySelector('main').getBoundingClientRect();
            const pane = document.querySelector('#pane-video').getBoundingClientRect();
            const groups = [...document.querySelectorAll('#pane-video > .settings-group')];
            return meta.right === main.left && meta.top === main.top && main.right === innerWidth &&
                pane.width === main.width && groups.length === 6 &&
                groups[0].getBoundingClientRect().left < groups[1].getBoundingClientRect().left &&
                getComputedStyle(document.querySelector('.content')).display === 'grid' &&
                document.querySelector('.topbar h1').getBoundingClientRect().top >= 0;
        })())JS") == L"true", "Full uses left signal column and the entire main area with two settings columns");
        const auto observed = Script(view, SharedValues);
        if (observed != values) std::wcout << L"Expected " << values << L"\nObserved " << observed << L'\n';
        Check(observed == values, "Full retains settings, actions, active pane and advanced disclosure");
        Check(Script(view, LR"JS(
            [...document.querySelectorAll(`[data-action=toggleLowLatency], [data-action=cyclePresentPacing],
                [data-action=toggleNIS], [data-action=toggleHDR]`)].every(row => {
                const group = row.querySelector('.row-right'), text = group.firstElementChild,
                    control = group.lastElementChild;
                return getComputedStyle(group).justifyContent === 'flex-end' &&
                    getComputedStyle(text).textAlign === 'right' &&
                    control.getBoundingClientRect().right === group.getBoundingClientRect().right &&
                    control.getBoundingClientRect().left - text.getBoundingClientRect().right === 12;
            }) && document.querySelector('#slider-pip-opacity').getBoundingClientRect().width >= 150
        )JS") == L"true", "Full control text stays next to its toggle or chevron with a 12px gap and sliders keep their width");
    }
    for (const auto side : {L"Left", L"Right"}) {
        Resize(host, window, 1920, 1080, side == std::wstring_view(L"Left")
            ? WebViewSettings::Dock::Left : WebViewSettings::Dock::Right);
        PushState(host, side);
        Check(Script(view, LR"JS(getComputedStyle(document.querySelector('.content')).display === 'flex' &&
            getComputedStyle(document.querySelector('.rail')).height === '48px' &&
            getComputedStyle(document.querySelector('.rail')).flexDirection === 'row' &&
            getComputedStyle(document.querySelector('.metabar')).display === 'grid' &&
            getComputedStyle(document.querySelector('.row')).paddingLeft === '16px' &&
            getComputedStyle(document.querySelector('#pane-video')).display === 'block')JS") == L"true",
            "Left and Right retain the compact navigation, metabar, row spacing and pane layout");
        Check(Script(view, CompactSnapshot) == (side == std::wstring_view(L"Left") ? compactLeft : compactRight),
            "Full round trip leaves every compact control's geometry and computed styles unchanged");
        Check(Script(view, SharedValues) == values, "docked modes retain the same settings and actions");
    }
    Check(messages.size() == count, "presentation changes emit no settings actions");
    Check(Script(view, L"__controls.every(e => e.isConnected) && new Set([...document.querySelectorAll('[id]')].map(e => e.id)).size === document.querySelectorAll('[id]').length") == L"true",
        "presentation changes retain the original controls without duplicate IDs");

    Resize(host, window, 1920, 1080, WebViewSettings::Dock::Full);
    PushState(host, L"Full");
    Script(view, L"document.querySelector('#source-trigger').click()");
    std::wcout << L"Source popover integration geometry: " << Script(view, LR"JS((() => {
        const p = document.querySelector('#source-popover'), r = p.getBoundingClientRect();
        return JSON.stringify({open:p.dataset.open,viewport:[innerWidth,innerHeight],
            bounds:[r.left,r.top,r.right,r.bottom],selects:[...p.querySelectorAll('select')]
                .map(e=>({id:e.id,label:e.previousElementSibling?.textContent,
                    capture:e.parentElement.parentElement===p}))});
    })())JS") << L'\n';
    Check(Script(view, LR"JS((() => {
        const p = document.querySelector('#source-popover'), r = p.getBoundingClientRect();
        const capture = [...p.querySelectorAll(':scope > .source-popover-select-row > select')];
        const identity = p.querySelector('#hdmi-source-picker #hdmi-source-trigger');
        const labels = ['source.resolution', 'source.framerate', 'source.format'];
        const strings = window.NitLinkLocales[document.documentElement.lang];
        return p.dataset.open === 'true' && r.width > 0 && r.left >= 0 && r.right <= innerWidth &&
            r.top >= 0 && r.bottom <= innerHeight && p.querySelectorAll('select').length === 4 &&
            identity?.tagName === 'SELECT' && identity.getBoundingClientRect().width > 0 &&
            capture.length === 3 && capture.every((e, i) =>
                e.previousElementSibling.textContent === strings[labels[i]] &&
                e.getBoundingClientRect().width > 0);
    })())JS") == L"true", "source picker and format controls remain reachable in the signal sidebar");
    Script(view, L"document.querySelector('#source-trigger').click()");

    for (const int width : {640, 900, 1200}) {
        for (const auto side : {L"Left", L"Right"}) {
            Resize(host, window, 1920, 1080, side == std::wstring_view(L"Left")
                ? WebViewSettings::Dock::Left : WebViewSettings::Dock::Right, width);
            PushState(host, side);
            Check(Script(view, L"getComputedStyle(document.querySelector('.content')).display === 'flex' && getComputedStyle(document.querySelector('.settings-group')).display === 'contents'") == L"true",
                "wider docked panels never opt into the Full presentation");
        }
    }

    for (const auto size : {std::pair{640, 480}, std::pair{1024, 576}, std::pair{1280, 720}}) {
        Resize(host, window, size.first, size.second, WebViewSettings::Dock::Full);
        PushState(host, L"Full", L"zh-TW");
        Check(Script(view, LR"JS((() => {
            const main = document.querySelector('main'), r = main.getBoundingClientRect();
            return r.width > 0 && r.height > 0 && main.scrollWidth <= main.clientWidth &&
                document.documentElement.scrollWidth === innerWidth;
        })())JS") == L"true", "smaller Full windows keep localized settings scrollable without horizontal overflow");
    }

    for (const auto side : {L"Full", L"Left", L"Right"}) {
        const auto dock = side == std::wstring_view(L"Full") ? WebViewSettings::Dock::Full
            : side == std::wstring_view(L"Left") ? WebViewSettings::Dock::Left : WebViewSettings::Dock::Right;
        Resize(host, window, 1920, 1080, dock);
        PushState(host, side);
        const size_t before = messages.size();
        Script(view, L"document.querySelector('[data-action=toggleHDR]').click()");
        Check(PumpUntil([&] { return messages.size() == before + 1; }), "shared toggle posts exactly one action");
        const auto toggle = NitLink::ParseSettingsMessage(messages.back());
        Check(toggle && toggle->action == L"toggleHDR", "identical HDR action crosses the native bridge in every presentation");
        Script(view, L"document.querySelector('#slider-pip-opacity').value = '64'; document.querySelector('#slider-pip-opacity').dispatchEvent(new Event('input'))");
        Check(PumpUntil([&] { return messages.size() == before + 2; }), "shared slider posts exactly one action");
        const auto slider = NitLink::ParseSettingsMessage(messages.back());
        Check(slider && slider->action == L"setPiPOpacity" && slider->number == 0.64,
            "identical slider value crosses the native bridge in every presentation");
        Script(view, L"document.querySelector('.rail-item[data-tab=\"pane-audio\"]').click()");
        PushState(host, side, L"zh-TW");
        Check(Script(view, L"document.querySelector('#pane-audio').dataset.active === 'true' && document.querySelector('#slider-volume-val').textContent === '37'") == L"true",
            "navigation and localized state use the same hidden and visible controls");
    }
}

void TestAbout(WebViewSettings& host, HWND window, std::vector<std::wstring>& messages) {
    auto* view = WebViewSettingsTestAccess::View(host);
    Resize(host, window, 1920, 1080, WebViewSettings::Dock::Right);
    Script(view, L"document.querySelector('.rail-item[data-tab=\"pane-video\"]').click()");
    PumpUntil([] { return false; }, 120);
    PushAboutUpdate(host, L"Right", L"zh-TW", L"current", L"v1.2.4-holybear",
        false, false, false, false, L"");
    const auto settingsBefore = Script(view, SharedValues);
    const size_t beforeAbout = messages.size();
    SelectAbout(view);
    Check(messages.size() == beforeAbout + 1 && messages.back().find(L"markHolyBearUpdateSeen") != std::wstring::npos,
        "opening About only acknowledges the current update notification");
    Check(Script(view, LR"JS((() => {
        const pane = document.querySelector('#pane-about'), text = pane.innerText;
        return document.documentElement.dataset.activePane === 'pane-about' &&
            getComputedStyle(document.querySelector('.metabar')).display === 'none' &&
            !document.querySelector('.about-details').open &&
            document.querySelector('#about-details-summary').getAttribute('aria-expanded') === 'false' &&
            document.querySelector('#about-details-summary').getAttribute('aria-controls') === 'about-details-body' &&
            document.querySelector('#about-details-summary').tabIndex >= 0 &&
            document.querySelector('#holy-bear-product-title').textContent === 'NitLink 1.2.4 HolyBear Edition' &&
            (text.match(/1\.2\.4/g) || []).length === 1 &&
            document.querySelector('#holy-bear-update-status').textContent === window.NitLinkLocales['zh-TW']['update.current'] &&
            document.querySelector('#holy-bear-update-status').hidden === false &&
            document.querySelector('#holy-bear-release-link').hidden &&
            document.querySelector('#holy-bear-update-status').textContent.indexOf('未知') < 0 &&
            !!document.querySelector('.about-links a[href="https://tally.so/r/EkZjNr"]') &&
            !!document.querySelector('.about-links a[href="https://ko-fi.com/klosed89"]');
    })())JS") == L"true", "collapsed About has one dynamic version, concise update state, quick links and accessible disclosure");
    Check(Script(view, LR"JS((() => {
        const main = document.querySelector('main');
        return document.documentElement.scrollWidth <= innerWidth && main.scrollWidth <= main.clientWidth &&
            document.querySelector('#pane-about').scrollWidth <= document.querySelector('#pane-about').clientWidth;
    })())JS") == L"true", "420px docked About has no horizontal overflow");

    const size_t sizeBeforeCheck = messages.size();
    Script(view, L"document.querySelector('#holy-bear-check-update').click()");
    Check(PumpUntil([&] { return messages.size() == sizeBeforeCheck + 1; }), "manual update click posts a native action");
    const auto autoCheck = NitLink::ParseSettingsMessage(messages.back());
    Check(autoCheck && autoCheck->action == L"checkHolyBearUpdate" &&
        Script(view, LR"JS(document.querySelector('#holy-bear-update-status').textContent ===
            window.NitLinkLocales[document.documentElement.lang]['update.checking'] &&
            document.querySelector('#holy-bear-check-update').disabled)JS") == L"true",
        "manual button retains its action and shows a busy state");

    PushAboutUpdate(host, L"Right", L"zh-TW", L"current", L"v1.2.4-holybear",
        false, false, false, false, L"");
    const size_t sizeBeforeToggle = messages.size();
    Script(view, L"document.querySelector('#toggle-holy-bear-auto-update').click()");
    Check(PumpUntil([&] { return messages.size() == sizeBeforeToggle + 1; }), "automatic check switch posts one native action");
    auto automatic = NitLink::ParseSettingsMessage(messages.back());
    Check(automatic && automatic->action == L"setHolyBearAutoUpdateCheck" && automatic->number == 1 &&
        Script(view, L"document.querySelector('#toggle-holy-bear-auto-update').getAttribute('aria-checked') === 'true'") == L"true",
        "automatic check enables with an accessible switch state");
    PushAboutUpdate(host, L"Right", L"zh-TW", L"current", L"v1.2.4-holybear",
        false, true, false, false, L"");
    Check(Script(view, L"document.querySelector('#toggle-holy-bear-auto-update').classList.contains('on') && "
        L"document.querySelector('#toggle-holy-bear-auto-update').getAttribute('aria-checked') === 'true'") == L"true",
        "automatic check setting is restored from native state");
    const size_t sizeBeforeDisable = messages.size();
    Script(view, L"document.querySelector('#toggle-holy-bear-auto-update').click()");
    Check(PumpUntil([&] { return messages.size() == sizeBeforeDisable + 1; }), "automatic check can be turned off");
    automatic = NitLink::ParseSettingsMessage(messages.back());
    Check(automatic && automatic->action == L"setHolyBearAutoUpdateCheck" && automatic->number == 0,
        "turning automatic checks off persists the false setting");
    PushAboutUpdate(host, L"Right", L"zh-TW", L"current", L"v1.2.4-holybear",
        false, false, false, false, L"");

    const std::array<std::pair<const wchar_t*, const wchar_t*>, 9> statuses{{
        {L"checking", L"checking"}, {L"current", L"current"}, {L"offline", L"offline"},
        {L"rateLimited", L"rateLimited"}, {L"notFound", L"notFound"},
        {L"serverUnavailable", L"serverUnavailable"}, {L"apiUnavailable", L"apiUnavailable"},
        {L"invalid", L"invalid"}, {L"never", L"never"}}};
    for (const auto& [status, key] : statuses) {
        const auto latest = std::wstring_view(status) == L"never" ? L"" : L"v1.2.4-holybear";
        PushAboutUpdate(host, L"Right", L"zh-TW", status, latest, false,
            true, std::wstring_view(status) == L"checking", false, L"");
        std::wstring expression = L"(() => { const node=document.querySelector('#holy-bear-update-status'); return node.hidden === ";
        expression += std::wstring_view(status) == L"never" ? L"true" : L"false";
        expression += L" && node.textContent === window.NitLinkLocales['zh-TW']['update.";
        expression += key;
        expression += L"']; })()";
        Check(Script(view, expression) == L"true", "localized update status renders without a duplicate unknown row");
    }

    const std::wstring validUrl = L"https://github.com/HolyBearTW/nitlink/releases/tag/v1.2.5-holybear";
    PushAboutUpdate(host, L"Right", L"zh-TW", L"available", L"v1.2.5-holybear",
        true, true, false, true, validUrl.c_str());
    Check(Script(view, LR"JS((() => {
        const link = document.querySelector('#holy-bear-release-link'), status = document.querySelector('#holy-bear-update-status');
        return status.textContent === window.NitLinkLocales['zh-TW']['update.available'].replace('{version}', '1.2.5') &&
            !link.hidden && link.href === 'https://github.com/HolyBearTW/nitlink/releases/tag/v1.2.5-holybear' &&
            document.querySelector('#holy-bear-update-badge').hidden === false;
    })())JS") == L"true", "newer release shows a localized message, verified link and unread state");
    for (const auto& badUrl : {L"https://github.com/nitlink-dev/nitlink/releases/tag/v1.2.5-holybear",
            L"https://github.com/HolyBearTW/nitlink/releases/tag/v1.2.5-holybear?next=evil"}) {
        PushAboutUpdate(host, L"Right", L"zh-TW", L"available", L"v1.2.5-holybear",
            true, true, false, true, badUrl);
        Check(Script(view, L"document.querySelector('#holy-bear-release-link').hidden") == L"true",
            "untrusted release URL is never shown");
    }
    PushAboutUpdate(host, L"Right", L"zh-TW", L"available", L"v1.2.5-holybear",
        false, true, false, false, validUrl.c_str());
    Check(Script(view, L"document.querySelector('#holy-bear-release-link').hidden") == L"true",
        "release link requires a native newer-version result");

    PushAboutUpdate(host, L"Right", L"zh-TW", L"available", L"v1.2.5-holybear",
        true, true, false, true, validUrl.c_str());
    Script(view, L"document.querySelector('.about-details > summary').click()");
    Check(PumpUntil([&] { return Script(view, L"document.querySelector('.about-details').open && "
        L"document.querySelector('#about-details-summary').getAttribute('aria-expanded') === 'true'") == L"true"; }),
        "technical disclosure expands and updates its ARIA state");
    Check(Script(view, LR"JS((() => {
        const text = document.querySelector('#about-details-body').innerText;
        return text.includes('1.2.4-holybear') && text.includes('v1.2.5-holybear') &&
            text.includes('CN_115 (@CN115)') && text.includes('HolyBear') &&
            !!document.querySelector('#about-details-body a[href="https://github.com/nitlink-dev/nitlink"]') &&
            !!document.querySelector('#about-details-body a[href="https://github.com/HolyBearTW/nitlink/blob/main/LICENSES.md"]');
    })())JS") == L"true", "expanded details retain build history, project sources and named contributor handles");
    Check(Script(view, LR"JS((() => {
        const main = document.querySelector('main');
        return document.documentElement.scrollWidth <= innerWidth && main.scrollWidth <= main.clientWidth &&
            document.querySelector('#pane-about').scrollWidth <= document.querySelector('#pane-about').clientWidth;
    })())JS") == L"true", "expanded 420px About remains horizontally scrollable without clipping");

    PushAboutUpdate(host, L"Right", L"zh-CN", L"available", L"v1.2.5-holybear",
        true, true, false, true, validUrl.c_str());
    Check(Script(view, LR"JS(document.documentElement.lang === 'zh-CN' &&
        document.querySelector('#about-details-summary').textContent.includes('技术信息与贡献者') &&
        document.querySelector('#holy-bear-check-update').textContent === '检查更新' &&
        document.querySelector('#holy-bear-update-status').textContent.includes('已发布'))JS") == L"true",
        "Simplified Chinese updates the About and update controls");
    PushAboutUpdate(host, L"Right", L"en-US", L"available", L"v1.2.5-holybear",
        true, true, false, true, validUrl.c_str());
    Check(Script(view, LR"JS(document.documentElement.lang === 'en-US' &&
        document.querySelector('#about-details-summary').textContent.includes('Technical information') &&
        document.querySelector('#holy-bear-check-update').textContent === 'Check for updates' &&
        document.querySelector('#holy-bear-update-status').textContent.includes('has been released'))JS") == L"true",
        "English updates About and update controls");

    for (const auto size : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        Resize(host, window, size.first, size.second, WebViewSettings::Dock::Full);
        PushAboutUpdate(host, L"Full", L"zh-TW", L"apiUnavailable", L"v1.2.4-holybear",
            false, true, false, false, L"");
        Script(view, L"document.querySelector('.about-details').open = false");
        Check(Script(view, LR"JS((() => {
            const content = document.querySelector('.content'), main = document.querySelector('main');
            const c = content.getBoundingClientRect(), m = main.getBoundingClientRect();
            return document.documentElement.dataset.side === 'full' &&
                getComputedStyle(document.querySelector('.metabar')).display === 'none' &&
                Math.abs(m.left - c.left) < 1 && Math.abs(m.width - c.width) < 1 &&
                main.scrollWidth <= main.clientWidth && document.documentElement.scrollWidth <= innerWidth &&
                !document.querySelector('.about-details').open;
        })())JS") == L"true", "Full About fills the released signal column without horizontal overflow");
        Script(view, L"document.querySelector('.about-details > summary').click()");
        Check(Script(view, L"document.querySelector('.about-details').open && "
            L"document.querySelector('#holy-bear-update-status').textContent === window.NitLinkLocales['zh-TW']['update.apiUnavailable']") == L"true",
            "Full details and API-error status render at both target viewport sizes");
    }

    Resize(host, window, 1920, 1080, WebViewSettings::Dock::Right);
    PushAboutUpdate(host, L"Right", L"zh-TW", L"current", L"v1.2.4-holybear",
        false, false, false, false, L"");
    const size_t sizeBeforeReturn = messages.size();
    Script(view, L"document.querySelector('.rail-item[data-tab=\"pane-video\"]').click()");
    Check(PumpUntil([&] { return Script(view, L"document.documentElement.dataset.activePane === 'pane-video' && "
        L"getComputedStyle(document.querySelector('.metabar')).display === 'grid'") == L"true"; }),
        "leaving About restores the shared signal strip");
    Check(messages.size() == sizeBeforeReturn && Script(view, SharedValues) == settingsBefore &&
        Script(view, L"document.querySelector('#hdmi-source-trigger').isConnected && "
            L"document.querySelector('#slider-volume').isConnected && "
            L"document.querySelector('#pane-shortcuts').isConnected") == L"true",
        "About navigation preserves settings, capture source controls, audio and shortcuts without reinitialization actions");
}
}

int main(int argc, char** argv) {
    const bool captureOnly = argc == 3 && std::string(argv[1]) == "--capture-layout";
    const auto profile = std::filesystem::temp_directory_path() /
        (L"NitLink-Settings-layout-test-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", profile.c_str());
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        L"--disable-gpu --disable-gpu-compositing --disable-background-networking");
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    HWND window = CreateWindowExW(0, L"STATIC", L"NitLink settings layout test", WS_POPUP,
        0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int result = 0;
    try {
        Check(window != nullptr, "isolated hidden test window created");
        WebViewSettings host;
        std::vector<std::wstring> messages;
        host.SetMessageHandler([&](const std::wstring& json) { messages.push_back(json); });
        Check(host.Initialize(window, 800, 600), "native settings host starts");
        host.NavigateToMenu();
        Check(PumpUntil([&] { return host.IsReady(); }, 15000), "packaged page is ready through the real native bridge");
        auto controller3 = wil::com_ptr<ICoreWebView2Controller>(WebViewSettingsTestAccess::Controller(host))
            .try_query<ICoreWebView2Controller3>();
        Check(controller3 != nullptr, "runtime supports deterministic raster scale");
        Hr(controller3->put_ShouldDetectMonitorScaleChanges(FALSE));
        Hr(controller3->put_RasterizationScale(1.0));
        Hr(controller3->put_ZoomFactor(1.0));
        auto* view = WebViewSettingsTestAccess::View(host);
        std::wcout << L"Initial page locale and browser languages: " <<
            Script(view, L"[document.documentElement.lang, navigator.languages]") << L'\n';
        Script(view, L"window.__fontsReady = false; document.fonts.ready.then(() => __fontsReady = true)");
        Check(PumpUntil([&] { return Script(view, L"window.__fontsReady") == L"true"; }), "bundled fonts loaded");
        if (captureOnly) CaptureLayouts(host, window, std::filesystem::path(argv[2]));
        else {
            TestLayouts(host, window, messages);
            TestAbout(host, window, messages);
        }
        host.Shutdown();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        result = 1;
    }
    if (window) DestroyWindow(window);
    CoUninitialize();
    std::wcout << L"Disposable profile: " << profile.c_str() << L'\n';
    return result;
}
