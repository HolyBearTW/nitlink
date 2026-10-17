#include "app/WebViewSettings.h"
#include "app/settings_message.h"
#include "app/settings_state_json.h"
#include "app/game_service.h"
#include "app/hdmi_source.h"
#include "app/webview_lifecycle.h"
#include "app/webview_policy.h"

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>

using Microsoft::WRL::Callback;

struct WebViewSettingsTestAccess {
    static void QueueFinalSliders(WebViewSettings& host) {
        const auto now = GetTickCount64();
        while (host.m_messageBudget.Accept(now)) {}
        host.m_pendingSliders.Store(0, LR"({"action":"setVolume","value":0.234})");
        host.m_pendingSliders.Store(1, LR"({"action":"setPiPOpacity","value":0.567})");
    }
    static bool DeadlineUnarmed(const WebViewSettings& host) { return host.m_startTime == 0; }
    static void ExpireReadyDeadline(WebViewSettings& host) {
        host.m_startTime = GetTickCount64() - NitLink::kSettingsReadyTimeoutMs;
    }
    static unsigned Restarts(const WebViewSettings& host) { return host.m_restartAttempts; }
    static ICoreWebView2* View(WebViewSettings& host) { return host.m_webview.get(); }
    static ICoreWebView2Controller* Controller(WebViewSettings& host) { return host.m_controller.get(); }
};

namespace {
void Check(bool result, const char* label) {
    if (!result) throw std::runtime_error(label);
    std::cout << "PASS " << label << '\n';
}
NitLink::WebViewCoreSettingsState RuntimeNativeStateFixture() {
    NitLink::WebViewCoreSettingsState state;
    state.languagePreference = L"zh-TW";
    state.locale = L"zh-TW";
    state.holyBearUpdate.current = L"1.2.4-holybear";
    state.holyBearUpdate.latest = L"v1.2.5-holybear";
    state.holyBearUpdate.lastCheckUtc = 1791730000;
    state.holyBearUpdate.automatic = true;
    state.holyBearUpdate.status = L"available";
    state.holyBearUpdate.hasNewer = true;
    state.holyBearUpdate.unread = true;
    state.holyBearUpdate.url =
        L"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.5-holybear";
    state.manualHdmiSource = L"switch2";
    state.effectiveHdmiSource = L"Switch 2";
    for (const auto& [value, label] : NitLink::kManualHdmiSources) {
        state.manualHdmiSourceOptions.push_back({
            std::wstring(value.begin(), value.end()), std::wstring(label)});
    }
    state.preventSleep = true;
    state.noSignalMode = L"default";
    state.noSignalImage = L"C:\\test\\no-signal.png";
    state.noSignalImageAvailable = true;
    state.noSignalFit = L"contain";
    state.noSignalDimImage = true;
    state.captureDevices = {L"AVerMedia \"test\" device", L"Live Gamer ULTRA S GC553Pro"};
    state.activeDevice = L"Live Gamer ULTRA S GC553Pro";
    state.preferredDevice = state.activeDevice;
    return state;
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
std::wstring Script(ICoreWebView2* view, const wchar_t* script) {
    struct Result { bool done = false; HRESULT hr = E_PENDING; std::wstring value; };
    const auto result = std::make_shared<Result>();
    Hr(view->ExecuteScript(script, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
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

void PushManualState(WebViewSettings& host, std::string_view manual, std::wstring_view label) {
    std::wstringstream state;
    state << L"{\"state\":{\"manualHdmiSource\":\""
          << std::wstring(manual.begin(), manual.end())
          << L"\",\"manualHdmiSourceCustom\":\"\",\"effectiveHdmiSource\":\"" << label
          << L"\",\"manualHdmiSourceOptions\":[";
    bool first = true;
    for (const auto& [value, name] : NitLink::kManualHdmiSources) {
        if (!first) state << L",";
        first = false;
        state << L"{\"value\":\"" << std::wstring(value.begin(), value.end())
              << L"\",\"label\":\"" << name << L"\"}";
    }
    state << L"]}}";
    host.PostMessage(state.str());
}

void OtherEditorLocalesTest(WebViewSettings& host, ICoreWebView2* view,
                            std::vector<std::wstring>& messages) {
    // These fixed fixture strings need no escaping. Use the common resolver so
    // native effective identity and the actual WebView summary agree.
    const auto pushOther = [&](const wchar_t* locale, std::wstring_view custom,
                               std::wstring_view detected = L"Detected PS5") {
        const auto effective = NitLink::GetEffectiveHdmiSourceLabel("other", detected, {}, custom);
        std::wstringstream state;
        state << L"{\"state\":{\"locale\":\"" << locale
              << L"\",\"languagePreference\":\"" << locale
              << L"\",\"manualHdmiSource\":\"other\",\"manualHdmiSourceCustom\":\"" << custom
              << L"\",\"effectiveHdmiSource\":\"" << effective << L"\"}}";
        host.PostMessage(state.str());
    };
    const auto firstMessage = messages.size();
    for (const auto locale : {L"en-US", L"zh-TW", L"zh-CN", L"de-DE"}) {
        pushOther(locale, L"Steam Deck");
        const auto ready = std::wstring(L"document.documentElement.lang === '") + locale +
            L"' && document.getElementById('meta-hdmi-source').textContent === 'Steam Deck'";
        Check(PumpUntil([&] { return Script(view, ready.c_str()) == L"true"; }),
              "saved custom source reaches each locale");
        Check(Script(view, LR"JS((() => {
            const strings = window.NitLinkLocales[document.documentElement.lang];
            const translated = (selector, key) => document.querySelector(selector).textContent === strings[key];
            return translated('#hdmi-source-trigger option[value=other]', 'hdmiSource.otherOption') &&
                translated('#hdmi-source-trigger option[value=auto]', 'value.auto') &&
                translated('#hdmi-source-custom-row label', 'hdmiSource.customNameShort') &&
                translated('#hdmi-source-custom-editor label', 'hdmiSource.customName') &&
                translated('#hdmi-source-custom-save', 'hdmiSource.save') &&
                translated('#hdmi-source-custom-cancel', 'hdmiSource.cancel') &&
                translated('#hdmi-source-custom-error', 'hdmiSource.invalidCustom');
        })())JS") == L"true", "Other, Auto, custom name, Save, Cancel and error strings use the active locale");

        auto count = messages.size();
        Check(Script(view, LR"JS((() => {
            const input = document.getElementById('hdmi-source-custom-input');
            document.getElementById('hdmi-source-custom-edit').click();
            const saved = input.value === 'Steam Deck' && document.activeElement === input;
            input.value = 'unsaved draft';
            document.getElementById('hdmi-source-custom-cancel').click();
            const cancelled = document.getElementById('hdmi-source-custom-editor').hidden &&
                document.activeElement === document.getElementById('hdmi-source-trigger') &&
                document.getElementById('meta-hdmi-source').textContent === 'Steam Deck';
            document.getElementById('hdmi-source-custom-edit').click();
            return saved && cancelled && input.value === 'Steam Deck';
        })())JS") == L"true" && messages.size() == count,
            "Cancel posts no draft, retains the saved source and restores the editor from saved state");
        Script(view, L"document.getElementById('hdmi-source-custom-input').value='x'.repeat(65); document.getElementById('hdmi-source-custom-save').click()");
        Check(Script(view, LR"JS(!document.getElementById('hdmi-source-custom-error').hidden &&
            !document.getElementById('hdmi-source-custom-editor').hidden &&
            document.getElementById('hdmi-source-custom-error').textContent ===
                window.NitLinkLocales[document.documentElement.lang]['hdmiSource.invalidCustom'])JS") == L"true" &&
            messages.size() == count, "invalid edits show the localized error without changing saved identity");
        Script(view, L"document.getElementById('hdmi-source-custom-cancel').click()");

        pushOther(locale, L"");
        Check(PumpUntil([&] {
            return Script(view, L"document.getElementById('meta-hdmi-source').textContent === 'Detected PS5'") == L"true";
        }), "unset Other summary uses the effective Auto identity in every locale");
        count = messages.size();
        Check(Script(view, LR"JS((() => {
            document.getElementById('hdmi-source-custom-edit').click();
            document.getElementById('hdmi-source-custom-input').value = 'unsaved source';
            document.getElementById('hdmi-source-custom-cancel').click();
            return document.getElementById('meta-hdmi-source').textContent === 'Detected PS5' &&
                document.getElementById('hdmi-source-custom-editor').hidden;
        })())JS") == L"true" && messages.size() == count,
            "cancelling an unset Other draft preserves the effective Auto source without posting");
        for (const auto blank : {L"''", L"' \\u3000\\u00a0 '"}) {
            count = messages.size();
            const auto save = std::wstring(L"document.getElementById('hdmi-source-custom-edit').click(); ") +
                L"document.getElementById('hdmi-source-custom-input').value=" + blank +
                L"; document.getElementById('hdmi-source-custom-save').click()";
            Script(view, save.c_str());
            Check(PumpUntil([&] { return messages.size() == count + 1; }),
                  "blank and Unicode-whitespace saves post exactly one native update");
            const auto cleared = NitLink::ParseSettingsMessage(messages.back());
            Check(cleared && cleared->action == L"setManualHdmiSourceCustom" && cleared->text.empty(),
                  "blank and Unicode-whitespace custom names normalize to an empty saved value");
            pushOther(locale, blank == std::wstring_view(L"''") ? L"" : L" \u3000\u00a0 ");
            const auto restored = std::wstring(L"(() => { document.getElementById('hdmi-source-custom-edit').click(); ") +
                L"const matches = document.getElementById('hdmi-source-custom-input').value === " + blank +
                L"; document.getElementById('hdmi-source-custom-cancel').click(); return matches && " +
                L"document.getElementById('meta-hdmi-source').textContent === 'Detected PS5'; })()";
            Check(PumpUntil([&] {
                return Script(view, restored.c_str()) == L"true";
            }), "blank and Unicode-whitespace native state uses the Auto fallback without Other filler");
        }

        pushOther(locale, L"", L"");
        Check(PumpUntil([&] {
            return Script(view, LR"JS(document.getElementById('meta-hdmi-source').textContent ===
                window.NitLinkLocales[document.documentElement.lang]['meta.none'] &&
                document.getElementById('hdmi-source-trigger').title ===
                    window.NitLinkLocales[document.documentElement.lang]['meta.hdmiSourceInfo'])JS") == L"true";
        }), "unknown Other source shows the localized none marker without inventing identity");

        count = messages.size();
        Script(view, L"document.getElementById('hdmi-source-custom-edit').click(); document.getElementById('hdmi-source-custom-input').value='熊'; document.getElementById('hdmi-source-custom-save').click()");
        Check(PumpUntil([&] { return messages.size() == count + 1; }),
              "one Unicode character is accepted by the local custom editor");
        const auto single = NitLink::ParseSettingsMessage(messages.back());
        Check(single && single->action == L"setManualHdmiSourceCustom" && single->text == L"熊",
              "native custom-source validation preserves a one-character local name");
        pushOther(locale, L"熊");
        Check(PumpUntil([&] {
            return Script(view, L"document.getElementById('meta-hdmi-source').textContent === '熊'") == L"true";
        }), "one-character native identity is displayed intact in every locale");
    }
    for (size_t i = firstMessage; i < messages.size(); ++i) {
        const auto action = NitLink::ParseSettingsMessage(messages[i]);
        Check(action && action->action == L"setManualHdmiSourceCustom",
              "custom editor emits only identity updates, never capture or HDR actions");
    }
}

void GameResultBridgeTest(WebViewSettings& host, ICoreWebView2* view,
    std::vector<std::wstring>& messages) {
    const auto firstMessage = messages.size();
    uint32_t request = 0;
    Script(view, LR"JS((() => {
        window.__gameBridgeEvents = [];
        window.__originalGameEvent = window._gameEvent;
        window._gameEvent = event => { __gameBridgeEvents.push(event); __originalGameEvent(event); };
        document.querySelector('[data-tab="pane-games"]').click();
        document.getElementById('game-query').value = '薩爾達';
        document.getElementById('game-search').click();
        return true;
    })())JS");
    Check(PumpUntil([&] {
        for (size_t i = firstMessage; i < messages.size(); ++i) {
            const auto message = NitLink::ParseSettingsMessage(messages[i]);
            if (message && message->action == L"searchGames") { request = message->request; return true; }
        }
        return false;
    }), "game search reaches the native bridge with its request ID");
    NitLink::Games::Result result;
    result.operation = NitLink::Games::Operation::Search;
    result.request = request; result.locale = L"zh-TW";
    result.reply.status = L"ok";
    result.reply.games.push_back({5303117, 0, L"The Legend of Zelda: Tears of the Kingdom",
        L"薩爾達傳說 王國之淚", L"fixture"});
    Check(host.PostMessage(NitLink::SerializeGameResult(result)), "production search JSON is accepted by WebView2");
    Check(PumpUntil([&] { return Script(view, LR"JS(
        document.querySelector('.game-result-title')?.textContent === '薩爾達傳說 王國之淚' &&
        document.getElementById('game-search-status').getAttribute('aria-busy') === 'false'
    )JS") == L"true"; }), "successful native search renders the game and clears the timeout watchdog");
    result.operation = NitLink::Games::Operation::Covers;
    result.reply.page = 1; result.reply.hasMore = true;
    Check(host.PostMessage(NitLink::SerializeGameResult(result)), "production covers JSON is accepted by WebView2");
    result.operation = NitLink::Games::Operation::Image;
    NitLink::Games::Cover cover; cover.id = 123; cover.provider = L"steamgriddb"; cover.shape = L"square";
    result.reply.covers.push_back(cover);
    Check(host.PostMessage(NitLink::SerializeGameResult(result)), "production default-image JSON is accepted by WebView2");
    Check(PumpUntil([&] { return Script(view, LR"JS(
        __gameBridgeEvents.some(event => event.kind === 'covers' && event.page === 1 && event.hasMore === true) &&
        __gameBridgeEvents.some(event => event.kind === 'image' && event.coverIdSelected === 123 &&
            event.coverShape === 'square' && event.coverIsOfficial === false && event.page === 1)
    )JS") == L"true"; }), "covers and default-image metadata preserve JSON separators and native field types");
    const auto beforeCoverRequest=messages.size();
    Script(view, LR"JS((() => {
        document.querySelector('[data-tab="pane-games"]').click();
        window._updateGameState({id:5303117,key:'sgdb-5303117',name:'The Legend of Zelda: Tears of the Kingdom',
            displayName:'薩爾達傳說 王國之淚',locale:'zh-TW',artworkPlatform:'switch',coverId:0,
            manualCover:false,finalImageKey:'',dataMode:'real'});
        return true;
    })())JS");
    uint32_t coverRequest=0;
    Check(PumpUntil([&] {
        for(size_t i=beforeCoverRequest;i<messages.size();++i){
            const auto message=NitLink::ParseSettingsMessage(messages[i]);
            if(message&&message->action==L"getGameCovers"){coverRequest=message->request;return true;}
        }
        return false;
    }), "current-game state requests its cover gallery through the native bridge");
    NitLink::Games::Result artworkResult;
    artworkResult.operation=NitLink::Games::Operation::Covers;artworkResult.request=coverRequest;
    artworkResult.gameId=5303117;artworkResult.locale=L"zh-TW";artworkResult.shape=L"all";
    artworkResult.reply.status=L"ok";
    const auto addCommunity=[&](uint32_t id,std::wstring provider,std::wstring language,bool verified){
        NitLink::Games::Cover item;item.id=id;item.provider=std::move(provider);item.sourceLabel=L"Community artwork";
        item.shape=L"square";item.language=std::move(language);item.languageVerified=verified;
        if(verified)item.languageSource=L"https://artwork.example/source";
        artworkResult.reply.covers.push_back(std::move(item));
    };
    addCommunity(9001,L"steamgriddb",L"",false);
    addCommunity(9002,L"community-catalogue",L"zh-TW",false);
    addCommunity(9003,L"community-catalogue",L"zh-CN",true);
    NitLink::Games::Cover taiwanCover;taiwanCover.id=9004;taiwanCover.provider=L"nintendo-tw";
    taiwanCover.sourceLabel=L"Official Nintendo Taiwan";taiwanCover.author=L"台灣任天堂";
    taiwanCover.region=L"TW";taiwanCover.isOfficial=true;taiwanCover.kind=L"keyArt";
    taiwanCover.shape=L"landscape";taiwanCover.language=L"zh-TW";taiwanCover.languageVerified=true;
    taiwanCover.languageSource=L"https://www.nintendo.com/tw/zelda/totk/index.html";
    artworkResult.reply.covers.push_back(taiwanCover);
    Check(host.PostMessage(NitLink::SerializeGameResult(artworkResult)), "gallery with sourced and default artwork languages is accepted");
    Check(PumpUntil([&] { return Script(view,L"document.querySelectorAll('#game-gallery .game-cover-info').length === 4") == L"true"; }),
          "gallery renders community and Nintendo Taiwan cover details controls");
    const auto openDetails=[&](uint32_t coverId){
        const auto script=L"document.getElementById('game-cover-details').open&&document.getElementById('game-cover-details').close();"
            L"document.querySelector('#game-gallery [data-cover-id=\""+std::to_wstring(coverId)+
            L"\"]').closest('.game-cover-tile').querySelector('.game-cover-info').click(); true";
        return Script(view,script.c_str());
    };
    const auto detailValue=[&](std::wstring_view label){
        const auto script=L"(()=>{const row=[...document.querySelectorAll('#game-cover-detail-fields dt')].find(term=>term.textContent==='"+
            std::wstring(label)+L"');return row?.nextElementSibling.textContent||''})()";
        return Script(view,script.c_str());
    };
    openDetails(9001);const auto defaultSgdbLanguage=detailValue(L"語言");
    openDetails(9002);const auto defaultCommunityLanguage=detailValue(L"語言");
    openDetails(9003);const auto sourcedCommunityLanguage=detailValue(L"語言");
    openDetails(9004);
    const auto taiwanMetadata=detailValue(L"作者")+L"|"+detailValue(L"語言")+L"|"+detailValue(L"素材類型");
    if(defaultSgdbLanguage!=L"\"英文\""||defaultCommunityLanguage!=L"\"英文\""||
       sourcedCommunityLanguage!=L"\"簡中\""||taiwanMetadata!=L"\"台灣任天堂\"|\"繁體中文\"|\"官方主視覺\"")
        std::wcerr<<L"Artwork detail metadata observed: sgdb="<<defaultSgdbLanguage<<L" community="
            <<defaultCommunityLanguage<<L" sourced="<<sourcedCommunityLanguage<<L" Taiwan="<<taiwanMetadata<<L'\n';
    Check(defaultSgdbLanguage==L"\"英文\""&&defaultCommunityLanguage==L"\"英文\""&&
          sourcedCommunityLanguage==L"\"簡中\""&&taiwanMetadata==L"\"台灣任天堂\"|\"繁體中文\"|\"官方主視覺\"",
          "community artwork defaults to English unless a source verifies its language; Taiwan official metadata stays localized and uses key-art type");
    Script(view, LR"JS((() => {
        if(document.getElementById('game-cover-details').open)document.getElementById('game-cover-details').close();
        window._updateGameState({id:0,key:'',name:'',displayName:'',locale:'zh-TW',artworkPlatform:'unknown',coverId:0,
            manualCover:false,finalImageKey:'',dataMode:'real'});
        window._gameEvent = window.__originalGameEvent;
        document.getElementById('game-query').value = '';
        document.getElementById('game-query').dispatchEvent(new Event('input'));
        document.querySelector('[data-tab="pane-video"]').click();
        return true;
    })())JS");
}

void ManualSourcePickerTest(WebViewSettings& host, HWND window, ICoreWebView2* view,
                            std::vector<std::wstring>& messages) {
    // Keep the controller visible inside the hidden fixture so WebView2 does
    // not suspend viewport updates while checking responsive layout.
    host.Show(true);
    PushManualState(host, "switch2", L"Switch 2");
    Check(PumpUntil([&] {
        return Script(view, L"document.getElementById('hdmi-source-trigger').value === 'switch2' && document.getElementById('hdmi-source-trigger').options.length === 9") == L"true";
    }), "manual picker receives the shared native options and effective identity");
    const auto beforeOpen = messages.size();
    Script(view, L"document.getElementById('source-trigger').click()");
    Check(PumpUntil([&] { return messages.size() == beforeOpen + 1; }) &&
          NitLink::ParseSettingsMessage(messages.back())->action == L"getDeviceList",
          "original source menu keeps its explicit device refresh on opening");
    const auto manualLayout=Script(view, LR"JS((() => {
        const source = document.getElementById('source-popover');
        source.querySelector('.source-format-details').open = true;
        const picker = document.getElementById('hdmi-source-picker');
        const resolution = source.querySelector('.source-format-details > .source-popover-select-row');
        const strings=window.NitLinkLocales[document.documentElement.lang];
        return JSON.stringify({locale:document.documentElement.lang,open:source.dataset.open==='true',
            headingMatches:picker.previousElementSibling.textContent===strings['game.hdmiSection'],
            pickerBottom:picker.getBoundingClientRect().bottom,resolutionTop:resolution.getBoundingClientRect().top,
            resolutionFollows:picker.getBoundingClientRect().bottom<=resolution.getBoundingClientRect().top,
            resolutionMatches:resolution.querySelector('label').textContent===strings['source.resolution'],
            formatRows:source.querySelectorAll('.source-format-details > .source-popover-select-row').length,
            autoMatches:document.querySelector('#hdmi-source-trigger option[value=auto]').textContent===strings['value.auto'],
            otherMatches:document.querySelector('#hdmi-source-trigger option[value=other]').textContent===strings['hdmiSource.otherOption'],
            selected:document.getElementById('hdmi-source-trigger').selectedOptions[0]?.value});
    })())JS");
    const bool manualLayoutPass=Script(view, LR"JS((() => {
        const source = document.getElementById('source-popover');
        const strings = window.NitLinkLocales[document.documentElement.lang];
        const picker = document.getElementById('hdmi-source-picker');
        const resolution = source.querySelector('.source-format-details > .source-popover-select-row');
        return source.dataset.open === 'true' && picker.previousElementSibling.textContent === strings['game.hdmiSection'] &&
            picker.getBoundingClientRect().bottom <= resolution.getBoundingClientRect().top &&
            resolution.querySelector('label').textContent === strings['source.resolution'] &&
            source.querySelectorAll('.source-format-details > .source-popover-select-row').length === 3 &&
            document.querySelector('#hdmi-source-trigger option[value=auto]').textContent === strings['value.auto'] &&
            document.querySelector('#hdmi-source-trigger option[value=other]').textContent === strings['hdmiSource.otherOption'] &&
            document.getElementById('hdmi-source-trigger').selectedOptions[0].value === 'switch2';
    })())JS") == L"true";
    if(!manualLayoutPass)std::wcerr<<L"Manual source picker observations: "<<manualLayout<<L'\n';
    Check(manualLayoutPass, "HDMI identity precedes the separate expandable resolution, framerate and format region");
    Check(Script(view, LR"JS((() => {
        const manual = document.getElementById('hdmi-source-trigger');
        const resolution = document.querySelector('#source-popover .source-format-details > .source-popover-select-row select');
        const same = (a, b, properties) => properties.every(property => getComputedStyle(a)[property] === getComputedStyle(b)[property]);
        return manual.tagName === 'SELECT' && resolution.tagName === 'SELECT' &&
            same(manual, resolution, ['backgroundColor','color','fontFamily','fontSize','fontWeight','lineHeight','borderTopColor','borderTopWidth','borderRadius','paddingTop','paddingBottom','paddingLeft','paddingRight','height','appearance']) &&
            same(manual.parentElement, resolution.parentElement, ['gridTemplateColumns','gap','paddingTop','paddingBottom','paddingLeft','paddingRight']) &&
            same(document.getElementById('hdmi-source-label'), resolution.previousElementSibling, ['color','fontSize','fontWeight']) &&
            Math.abs(manual.getBoundingClientRect().left - resolution.getBoundingClientRect().left) < 1 &&
            Math.abs(manual.getBoundingClientRect().width - resolution.getBoundingClientRect().width) < 1;
    })())JS") == L"true", "actual WebView computed style, borders, spacing and column alignment match the existing selects");
    Check(Script(view, LR"JS((() => {
        const manual = document.getElementById('hdmi-source-trigger');
        const resolution = document.querySelector('#source-popover .source-format-details > .source-popover-select-row select');
        resolution.focus();
        const focusedBorder = getComputedStyle(resolution).borderTopColor;
        manual.focus();
        return getComputedStyle(manual).borderTopColor === focusedBorder;
    })())JS") == L"true", "HDMI select shares the existing focus border style");
    Script(view, LR"JS(window.__captureLabels = JSON.stringify(['meta-source','meta-resolution','meta-format','pacing-val','toggle-hdr'].map(id => {
        const el = document.getElementById(id); return [el.textContent, el.className];
    })))JS");
    const auto firstMessage = messages.size();
    for (const auto& [value, name] : NitLink::kManualHdmiSources) {
        const auto count = messages.size();
        const auto click = L"document.getElementById('hdmi-source-trigger').value='" +
            std::wstring(value.begin(), value.end()) + L"'; document.getElementById('hdmi-source-trigger').dispatchEvent(new Event('change', {bubbles:true}))";
        Script(view, click.c_str());
        Check(PumpUntil([&] { return messages.size() > count; }), "identity selection reaches native bridge");
        const auto parsed = NitLink::ParseSettingsMessage(messages.back());
        Check(parsed && parsed->action == L"setManualHdmiSource" &&
              parsed->text == std::wstring(value.begin(), value.end()), "identity payload uses canonical value");
        PushManualState(host, value, name);
        const auto selected = L"document.getElementById('hdmi-source-trigger').selectedOptions[0].value === '" +
            std::wstring(value.begin(), value.end()) + L"'";
        Check(PumpUntil([&] { return Script(view, selected.c_str()) == L"true"; }), "native selection updates check state");
        if (value == "other") Script(view, L"document.getElementById('hdmi-source-custom-cancel').click()");
    }
    Check(messages.size() == firstMessage + NitLink::kManualHdmiSources.size(),
          "identity picker never enumerates devices or changes capture format");
    Check(Script(view, LR"JS(JSON.stringify(['meta-source','meta-resolution','meta-format','pacing-val','toggle-hdr'].map(id => {
        const el = document.getElementById(id); return [el.textContent, el.className];
    })) === window.__captureLabels)JS") == L"true", "identity leaves live signal, format, HDR and pacing controls unchanged");

    Script(view, L"document.getElementById('hdmi-source-custom-edit').click(); document.getElementById('hdmi-source-custom-input').value='unsaved draft'");
    Check(Script(view, L"document.getElementById('hdmi-source-custom-input').click(); document.getElementById('source-popover').dataset.open === 'true'") == L"true",
          "nested custom input does not toggle its containing source menu");
    host.PostMessage(LR"({"state":{"manualHdmiSource":"other","manualHdmiSourceCustom":"Steam Deck","effectiveHdmiSource":"Steam Deck"}})");
    Check(PumpUntil([&] { return Script(view, L"document.getElementById('meta-hdmi-source').textContent === 'Steam Deck'") == L"true"; }), "custom effective name reaches picker");
    Check(Script(view, L"document.getElementById('hdmi-source-custom-input').value === 'unsaved draft' && !document.getElementById('hdmi-source-custom-editor').hidden") == L"true",
          "periodic native state preserves a custom edit in progress");
    auto count = messages.size();
    Script(view, L"document.getElementById('hdmi-source-custom-input').value='x'.repeat(65); document.getElementById('hdmi-source-custom-save').click()");
    Check(Script(view, L"!document.getElementById('hdmi-source-custom-error').hidden && !document.getElementById('hdmi-source-custom-editor').hidden") == L"true" && messages.size() == count,
          "overlength custom edit remains open without posting");
    Script(view, L"document.getElementById('hdmi-source-custom-input').value='Deck\\u0085PC'; document.getElementById('hdmi-source-custom-save').click()");
    Check(messages.size() == count, "custom controls cannot cross the UI bridge");
    Script(view, LR"JS(document.getElementById('hdmi-source-custom-input').value='  Deck "<b>& PC</b>\\  '; document.getElementById('hdmi-source-custom-save').click())JS");
    Check(PumpUntil([&] { return messages.size() > count; }), "custom edit save posts through real serializer");
    const auto custom = NitLink::ParseSettingsMessage(messages.back());
    Check(custom && custom->action == L"setManualHdmiSourceCustom" && custom->text == L"Deck \"<b>& PC</b>\\",
          "custom bridge safely preserves quotes, markup and backslash while trimming");
    host.PostMessage(LR"({"state":{"manualHdmiSource":"other","manualHdmiSourceCustom":"Deck \"<b>& PC</b>\\","effectiveHdmiSource":"Deck \"<b>& PC</b>\\"}})");
    Check(PumpUntil([&] { return Script(view, LR"JS(document.getElementById('meta-hdmi-source').textContent === 'Deck "<b>& PC</b>\\' && !document.getElementById('meta-hdmi-source').children.length)JS") == L"true"; }),
          "custom name renders as literal text, never HTML");
    OtherEditorLocalesTest(host, view, messages);
    host.PostMessage(LR"({"state":{"locale":"zh-TW","languagePreference":"zh-TW","manualHdmiSource":"auto","manualHdmiSourceCustom":"Steam Deck","effectiveHdmiSource":"Detected PS5"}})");
    Check(PumpUntil([&] { return Script(view, L"document.getElementById('hdmi-source-trigger').selectedOptions[0].textContent === '自動' && document.getElementById('hdmi-source-trigger').title.includes('Detected PS5') && document.getElementById('hdmi-source-custom-row').hidden") == L"true"; }),
          "Auto retains detector identity in tooltip and ignores preserved custom text");
    count = messages.size();
    Script(view, L"chrome.webview.postMessage({action:'setManualHdmiSource',value:'<invalid>'}); chrome.webview.postMessage({action:'setManualHdmiSourceCustom',value:'Deck\\nPC'}); chrome.webview.postMessage({action:'setManualHdmiSourceCustom',value:'x'.repeat(65)})");
    Check(PumpUntil([&] { return messages.size() == count + 3; }), "invalid fixtures reach native validation boundary");
    // This host records raw transport messages. Application validates with
    // ParseSettingsMessage before dispatching any config or capture action.
    for (size_t i = count; i < messages.size(); ++i)
        Check(!NitLink::ParseSettingsMessage(messages[i]), "native parser rejects invalid WebView identity and custom values");
    host.PostMessage(LR"({"state":{"manualHdmiSource":"other","manualHdmiSourceCustom":"Steam Deck","effectiveHdmiSource":"Steam Deck"}})");
    Check(PumpUntil([&] { return Script(view, L"!document.getElementById('hdmi-source-custom-row').hidden && document.getElementById('meta-hdmi-source').textContent === 'Steam Deck'") == L"true"; }),
          "existing Other custom identity remains visible and editable after reopening settings");
    Check(Script(view, LR"JS((() => {
        const trigger = document.getElementById('hdmi-source-trigger');
        document.getElementById('hdmi-source-custom-edit').click();
        const editing = document.activeElement === document.getElementById('hdmi-source-custom-input') && document.activeElement.value === 'Steam Deck';
        document.activeElement.dispatchEvent(new KeyboardEvent('keydown', {key:'Escape', bubbles:true}));
        return editing && document.activeElement === trigger && document.getElementById('hdmi-source-custom-editor').hidden &&
            document.getElementById('source-popover').dataset.open === 'true';
    })())JS") == L"true", "Escape cancels custom editing while retaining original source menu and select focus");
    Check(Script(view, L"document.getElementById('hdmi-source-custom-edit').click(); document.querySelector('main').click(); document.getElementById('source-popover').dataset.open === 'false' && document.getElementById('hdmi-source-custom-editor').hidden") == L"true",
          "outside click closes the source menu and its custom editor");

    PushManualState(host, "auto", L"Detected PS5");
    Check(PumpUntil([&] { return Script(view, L"document.getElementById('hdmi-source-trigger').value === 'auto'") == L"true"; }), "native Auto state reaches select");
    Script(view, L"document.getElementById('source-trigger').click()");
    Check(Script(view, L"document.getElementById('source-popover').dataset.open === 'true' && document.getElementById('hdmi-source-trigger').value === 'auto' && document.querySelectorAll('#source-popover .source-format-details > .source-popover-select-row').length === 3") == L"true",
          "reopening source menu retains identity listeners and native selection beside the format cascade");

    SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    host.Resize();
    Check(PumpUntil([&] { return Script(view, LR"JS((() => {
        const source = document.getElementById('source-popover').getBoundingClientRect();
        const manual = document.getElementById('hdmi-source-picker').getBoundingClientRect();
        const resolution = document.querySelector('#source-popover .source-format-details > .source-popover-select-row').getBoundingClientRect();
        return window.innerWidth > 1000 && manual.top >= source.top && manual.bottom <= resolution.top && manual.right <= source.right;
    })())JS") == L"true"; }), "desktop identity stays below manual format heading and above resolution");
    SetWindowPos(window, nullptr, 0, 0, 440, 650, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    host.Resize();
    Check(PumpUntil([&] { return Script(view, LR"JS((() => {
        const source = document.getElementById('source-popover').getBoundingClientRect();
        const manual = document.getElementById('hdmi-source-picker').getBoundingClientRect();
        const resolution = document.querySelector('#source-popover .source-format-details > .source-popover-select-row').getBoundingClientRect();
        return window.innerWidth < 600 && manual.top >= source.top && manual.bottom <= resolution.top && manual.right <= window.innerWidth;
    })())JS") == L"true"; }), "compact identity stays below manual format heading and above resolution without overflow");
    Script(view, L"document.querySelector('main').click()");
    SetWindowPos(window, nullptr, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    host.Resize();
    host.Show(false);
}
}

int main(int argc, char** argv) {
    const bool expectMissingAssets = argc == 2 && std::string(argv[1]) == "--expect-missing-assets";
    const bool expectReadyTimeout = argc == 2 && std::string(argv[1]) == "--expect-ready-timeout";
    // This standalone host has no capture or renderer code. A disposable profile
    // and software-only browser flags keep the viewer's profile and GPU untouched.
    const auto profile = std::filesystem::temp_directory_path() /
        (L"NitLink-WebView-test-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", profile.c_str());
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        L"--disable-gpu --disable-gpu-compositing --disable-background-networking");
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    HWND window = CreateWindowExW(0, L"STATIC", L"NitLink settings test", WS_OVERLAPPEDWINDOW,
        0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int result = 0;
    try {
        Check(window != nullptr, "hidden test window");
        if (expectMissingAssets || expectReadyTimeout) {
            WebViewSettings incomplete;
            std::wstring failure;
            unsigned failures = 0;
            incomplete.SetFailureHandler([&](const std::wstring& reason) { failure = reason; ++failures; });
            Check(incomplete.Initialize(window, 800, 600), "incomplete-package initialization dispatched");
            incomplete.NavigateToMenu();
            incomplete.Show(true);
            Check(!incomplete.IsVisible(), "starting page remains hidden until trusted ready");
            Check(WebViewSettingsTestAccess::DeadlineUnarmed(incomplete),
                  "initialization does not start a timer before pumping");
            Check(PumpUntil([&] {
                incomplete.CheckHealth();
                auto* current = WebViewSettingsTestAccess::View(incomplete);
                if (expectReadyTimeout && current) {
                    wil::unique_cotaskmem_string source;
                    if (SUCCEEDED(current->get_Source(&source)) && source &&
                        NitLink::WebViewPolicy::IsTrustedDocument(source.get()) &&
                        Script(current, L"document.readyState") == L"\"complete\"") {
                        WebViewSettingsTestAccess::ExpireReadyDeadline(incomplete);
                        incomplete.CheckHealth();
                    }
                }
                return !failure.empty() && !incomplete.IsStarting();
            }, 20000), "failed startup stops after bounded automatic retries");
            Check(failures == 3 && WebViewSettingsTestAccess::Restarts(incomplete) == 2,
                  "initial attempt plus exactly two restarts");
            Check(failure == (expectReadyTimeout ? L"toast.settingsTimeout" : L"toast.settingsFiles") &&
                  !incomplete.IsReady() && !incomplete.IsStarting(), "failure identifies localized repair reason");
            incomplete.Show(true);
            Check(!incomplete.IsVisible(), "failed package never opens an empty panel");
            incomplete.Shutdown();
        } else {
        std::vector<std::wstring> messages;
        std::vector<std::wstring> javascriptExceptions;
        auto nativeState = RuntimeNativeStateFixture();
        unsigned noSignalModeActions = 0;
        unsigned nativeStatePushes = 0;
        bool nativeStatePostAccepted = false;
        std::wstring startupFailure;
        WebViewSettings host;
        Check(host.Initialize(window, 800, 600), "host initialization dispatched");
        Check(WebViewSettingsTestAccess::DeadlineUnarmed(host), "healthy startup timer waits for message pumping");
        host.SetFailureHandler([&](const std::wstring& reason) { startupFailure = reason; });
        host.SetMessageHandler([&](const std::wstring& json) {
            messages.push_back(json);
            const auto message = NitLink::ParseSettingsMessage(json);
            if (!message) return;
            if (message->action == L"javascriptException") {
                javascriptExceptions.push_back(message->text);
            } else if (message->action == L"ready" || message->action == L"cycleNoSignalMode") {
                if (message->action == L"cycleNoSignalMode") {
                    nativeState.noSignalMode = NitLink::NextNoSignalMode(nativeState.noSignalMode);
                    ++noSignalModeActions;
                }
                ++nativeStatePushes;
                nativeStatePostAccepted = host.PostMessage(
                    NitLink::SerializeWebViewCoreSettingsState(nativeState));
            }
        });
        host.NavigateToMenu();
        const bool readyViaBridge = PumpUntil([&] {
            host.CheckHealth();
            return !messages.empty() || (!startupFailure.empty() && !host.IsStarting());
        }, 50000);
        if (!readyViaBridge || messages.empty()) {
            std::wcerr << L"WebView2 packaged-menu startup failure: "
                       << (startupFailure.empty() ? L"ready message timed out" : startupFailure) << L'\n';
            if (auto* diagnosticView=WebViewSettingsTestAccess::View(host)) {
                std::wcerr << L"Packaged page diagnostics: " << Script(diagnosticView, LR"JS(JSON.stringify({
                    readyState:document.readyState, menu:!!document.getElementById('game-search'),
                    scripts:[...document.scripts].map(script=>({src:script.src,loaded:script.dataset.loaded||''})),
                    resources:performance.getEntriesByType('resource').filter(item=>/\.js/.test(item.name))
                        .map(item=>({url:item.name,status:item.responseStatus||0,duration:item.duration}))
                }))JS") << L'\n';
            }
            if(!javascriptExceptions.empty())for(const auto& exception:javascriptExceptions)
                std::wcerr << L"JavaScript exception: " << exception << L'\n';
        }
        Check(readyViaBridge && !messages.empty(), "packaged menu ready via real bridge");
        Check(NitLink::ParseSettingsMessage(messages.front()).has_value(), "ready payload schema");
        Check(host.IsReady(), "native host accepts ready state");
        Check(nativeStatePushes == 1 && nativeStatePostAccepted,
              "production settings serializer accepted by WebView2 PostWebMessageAsJson");
        auto* view = WebViewSettingsTestAccess::View(host);
        Check(view != nullptr, "secured WebView available");
        Check(Script(view, L"document.getElementById('game-built-in') === null && "
            L"!!document.getElementById('game-query') && !!document.getElementById('game-search') && "
            L"window.NitLinkLocales['zh-TW']['game.kind.keyArt'] === '官方主視覺' && "
            L"window.NitLinkLocales['zh-TW']['game.coverLanguage.zh-TW'] === '繁體中文'") == L"true",
            "built-in selector is removed; search and Taiwan artwork metadata strings remain");
        auto view2 = wil::com_ptr<ICoreWebView2>(view).try_query<ICoreWebView2_2>();
        if (view2) {
            wil::com_ptr<ICoreWebView2Environment> env;
            Hr(view2->get_Environment(&env));
            wil::unique_cotaskmem_string version;
            Hr(env->get_BrowserVersionString(&version));
            std::wcout << L"Runtime " << version.get() << L'\n';
        }
        wil::com_ptr<ICoreWebView2Settings> settings;
        Hr(view->get_Settings(&settings));
        GameResultBridgeTest(host, view, messages);
        BOOL enabled = TRUE;
        Hr(settings->get_AreHostObjectsAllowed(&enabled)); Check(!enabled, "host objects disabled");
        Hr(settings->get_AreDevToolsEnabled(&enabled)); Check(!enabled, "developer tools disabled");
        Hr(settings->get_AreDefaultScriptDialogsEnabled(&enabled)); Check(!enabled, "dialogs disabled");
        auto controller4 = wil::com_ptr<ICoreWebView2Controller>(WebViewSettingsTestAccess::Controller(host))
            .try_query<ICoreWebView2Controller4>();
        Hr(controller4->get_AllowExternalDrop(&enabled)); Check(!enabled, "external drop disabled");
        Script(view, LR"JS((() => {
            window.__lastNativeState = null;
            window.chrome.webview.addEventListener('message', event => {
                if (event.data && event.data.state) window.__lastNativeState = event.data.state;
            });
            return true;
        })())JS");
        Check(host.PostMessage(NitLink::SerializeWebViewCoreSettingsState(nativeState)),
              "production serializer JSON is accepted for typed-field verification");
        Check(PumpUntil([&] { return Script(view, LR"JS((() => {
            const state = window.__lastNativeState;
            return !!state && state.holyBearUpdate.hasNewer === true &&
                state.holyBearUpdate.current === '1.2.4-holybear' &&
                state.holyBearUpdate.latest === 'v1.2.5-holybear' &&
                state.holyBearUpdate.unread === true &&
                state.holyBearUpdate.lastCheckUtc === 1791730000 &&
                state.preventSleep === true && Array.isArray(state.captureDevices) &&
                state.captureDevices.length === 2 &&
                state.activeDevice === 'Live Gamer ULTRA S GC553Pro' &&
                Array.isArray(state.manualHdmiSourceOptions) &&
                state.manualHdmiSourceOptions.length === 9 &&
                state.manualHdmiSourceOptions.some(option =>
                    option.value === 'switch2' && option.label === 'Switch 2');
        })())JS") == L"true"; }),
              "production state arrives as valid JSON with exact boolean, number and collection values");
        Check(PumpUntil([&] { return Script(view, LR"JS((() => {
            const update = document.getElementById('holy-bear-update-status');
            const link = document.getElementById('holy-bear-release-link');
            const badge = document.getElementById('holy-bear-update-badge');
            const lastCheck = document.getElementById('holy-bear-last-check');
            return document.documentElement.lang === 'zh-TW' &&
                update.textContent === window.NitLinkLocales['zh-TW']['update.available'].replace('{version}', '1.2.5-holybear') &&
                !update.hidden && link.href.endsWith('/v1.2.5-holybear') && !link.hidden && !badge.hidden &&
                lastCheck.textContent === new Date(1791730000 * 1000).toLocaleString('zh-TW') &&
                document.getElementById('toggle-prevent-sleep').classList.contains('on') &&
                document.getElementById('meta-source').textContent === 'Live Gamer ULTRA S GC553Pro' &&
                document.getElementById('hdmi-source-trigger').options.length === 9 &&
                document.getElementById('hdmi-source-trigger').value === 'switch2' &&
                document.getElementById('meta-hdmi-source').textContent === 'Switch 2' &&
                document.getElementById('no-signal-image-row').hidden;
        })())JS") == L"true"; }),
              "production state JSON applies update flags, sleep, capture source, HDMI options and No Signal defaults");
        const size_t devicesInMenu = messages.size();
        Script(view, L"document.getElementById('source-trigger').click()");
        Check(PumpUntil([&] { return Script(view, LR"JS((() => {
            const items = [...document.querySelectorAll('#source-popover .source-popover-item')];
            return document.getElementById('source-popover').dataset.open === 'true' &&
                items.length === 2 && items.some(item => item.textContent === 'AVerMedia "test" device') &&
                items.some(item => item.textContent === 'Live Gamer ULTRA S GC553Pro' && item.dataset.active === 'true');
        })())JS") == L"true"; }),
              "production captureDevices and activeDevice populate the native-backed source picker");
        Check(PumpUntil([&] {
            return std::any_of(messages.begin() + static_cast<std::ptrdiff_t>(devicesInMenu), messages.end(),
                               [](const auto& json) {
                                   const auto parsed = NitLink::ParseSettingsMessage(json);
                                   return parsed && parsed->action == L"getDeviceList";
                               });
        }), "source picker request crosses the real WebView2 bridge");
        Script(view, L"document.querySelector('main').click()");
        Check(Script(view, L"document.getElementById('no-signal-image-row').hidden") == L"true",
              "No Signal image controls start hidden in default mode");
        Script(view, L"document.getElementById('no-signal-screen-row').click()");
        Check(PumpUntil([&] {
            return noSignalModeActions == 1 && Script(view, LR"JS((() => {
                return !document.getElementById('no-signal-image-row').hidden &&
                    !document.getElementById('no-signal-fit-row').hidden &&
                    !document.getElementById('no-signal-dim-row').hidden &&
                    document.getElementById('no-signal-image-value').textContent === 'no-signal.png';
            })())JS") == L"true";
        }), "No Signal click reaches native handler and image settings return through WebView2");
        Check(nativeState.noSignalMode == L"image" && nativeStatePostAccepted,
              "native No Signal mode advances and production state is accepted");
        Script(view, L"document.getElementById('no-signal-screen-row').click()");
        Check(PumpUntil([&] {
            return noSignalModeActions == 2 && Script(view,
                L"document.getElementById('no-signal-image-row').hidden") == L"true";
        }), "second No Signal click restores default mode through state round trip");
        Check(nativeState.noSignalMode == L"default" && nativeStatePushes == 3,
              "two native No Signal transitions produce three state pushes including ready");

        Script(view, LR"JS((() => {
            window.__originalGetElementById = document.getElementById.bind(document);
            document.getElementById = id => {
                if (id === 'holy-bear-update-status') throw new Error('simulated applyState exception');
                return window.__originalGetElementById(id);
            };
            return true;
        })())JS");
        Check(host.PostMessage(NitLink::SerializeWebViewCoreSettingsState(nativeState)),
              "exception fixture state reaches real WebView2");
        Check(PumpUntil([&] { return !javascriptExceptions.empty(); }),
              "JavaScript applyState exception reports through native bridge");
        Check(javascriptExceptions.back().find(L"simulated applyState exception") != std::wstring::npos,
              "JavaScript exception detail is preserved for native diagnostics");
        Script(view, L"document.getElementById = window.__originalGetElementById");
        Check(host.PostMessage(NitLink::SerializeWebViewCoreSettingsState(nativeState)),
              "normal state recovers after JavaScript exception fixture");
        Check(Script(view, LR"JS((() => {
            window.__violations = [];
            document.addEventListener('securitypolicyviolation', e => __violations.push(e.violatedDirective));
            window.__injected = false;
            const s = document.createElement('script'); s.textContent = 'window.__injected=true'; document.body.append(s);
            const f = document.createElement('iframe'); f.srcdoc = '<script>parent.__injected=true<\/script>'; document.body.append(f);
            const remoteFrame = document.createElement('iframe'); remoteFrame.src = 'https://blocked.invalid/frame'; document.body.append(remoteFrame);
            fetch('https://blocked.invalid/').catch(() => {});
            try { new Worker('assets/menu/menu.js'); } catch (_) {}
            return document.querySelector('h1').textContent.length > 0;
        })())JS") == L"true", "localized UI loaded");
        PumpUntil([] { return false; }, 300);
        std::wcout << L"CSP observations " << Script(view, L"JSON.stringify({injected:window.__injected,violations:window.__violations})") << L'\n';
        Check(Script(view, L"!window.__injected && __violations.some(x=>x.startsWith('script-src')) && __violations.includes('frame-src') && __violations.includes('connect-src') && __violations.includes('worker-src')") == L"true",
              "CSP blocks inline injection, frames, connections and workers");
        host.PostMessage(LR"({"state":{"locale":"zh-CN","languagePreference":"zh-CN"}})");
        for (const auto* locale : {L"en-US", L"zh-TW", L"zh-CN", L"de-DE"}) {
            const std::wstring state = std::wstring(L"{\"state\":{\"locale\":\"") + locale +
                L"\",\"holyBearUpdate\":{\"current\":\"1.2.4-holybear\",\"latest\":\"v1.2.5-holybear\","
                L"\"lastCheckUtc\":1791730000,\"automatic\":false,\"busy\":false,\"status\":\"available\","
                L"\"hasNewer\":true,\"unread\":true,\"url\":\"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.5-holybear\"}}}";
            host.PostMessage(state);
            Check(PumpUntil([&] { return Script(view, L"document.getElementById('holy-bear-update-status').textContent === window.NitLinkLocales[document.documentElement.lang]['update.available'].replace('{version}', '1.2.5-holybear') && document.getElementById('holy-bear-release-link').href.endsWith('/v1.2.5-holybear') && !document.getElementById('holy-bear-update-badge').hidden && !document.getElementById('holy-bear-latest-version') && document.getElementById('holy-bear-last-check').closest('.about-update') !== null") == L"true"; }),
                  "HolyBear update notification localizes without network or HTML injection");
        }
        host.PostMessage(LR"({"state":{"locale":"zh-CN","languagePreference":"zh-CN"}})");
        Check(PumpUntil([&] { return Script(view, L"document.documentElement.lang === 'zh-CN' && document.querySelector('[data-i18n=\"nav.video\"]').textContent === window.NitLinkLocales['zh-CN']['nav.video']") == L"true"; }),
              "Simplified Chinese resource loads through the native resource allowlist");
        host.PostMessage(LR"({"state":{"locale":"zh-TW","languagePreference":"zh-TW","audioVolume":0.75}})");
        Check(PumpUntil([&] { return Script(view, L"document.documentElement.lang") == L"\"zh-TW\""; }),
              "native state reaches trusted page");
        for (const auto* locale : {L"en-US", L"zh-TW", L"de-DE"}) {
            const std::wstring prefix = std::wstring(L"{\"state\":{\"locale\":\"") + locale +
                L"\",\"negotiatedWidth\":1920,\"negotiatedHeight\":1080,";
            host.PostMessage((prefix + L"\"negotiatedFps\":120,\"negotiatedFpsNumerator\":120,"
                              L"\"negotiatedFpsDenominator\":1}}").c_str());
            Check(PumpUntil([&] { return Script(view,
                L"document.getElementById('meta-resolution').textContent === '1920x1080 / 120 FPS'") == L"true"; }),
                "negotiated integer rate displays in both languages");
            host.PostMessage((prefix + L"\"negotiatedFps\":119,\"negotiatedFpsNumerator\":120000,"
                              L"\"negotiatedFpsDenominator\":1001}}").c_str());
            Check(PumpUntil([&] { return Script(view,
                L"document.getElementById('meta-resolution').textContent === '1920x1080 / 119.88 FPS'") == L"true"; }),
                "negotiated fractional rate displays without integer truncation");
            host.PostMessage((prefix + L"\"negotiatedFps\":0,\"negotiatedFpsNumerator\":0,"
                              L"\"negotiatedFpsDenominator\":1}}").c_str());
            Check(PumpUntil([&] { return Script(view,
                L"document.getElementById('meta-resolution').textContent === '1920x1080 / -- FPS'") == L"true"; }),
                "missing negotiated rate remains unknown while resolution stays visible");
        }
        ManualSourcePickerTest(host, window, view, messages);
        host.PostMessage(LR"({"state":{"noSignalMode":"image","noSignalImage":"C:\\photo.png","noSignalImageAvailable":true}})");
        Check(PumpUntil([&] { return Script(view, L"document.getElementById('no-signal-mode-val').textContent === window.NitLinkLocales['zh-TW']['value.customImage']") == L"true"; }),
              "custom image label available");
        Script(view, L"window.__neutralImageLabel = document.getElementById('no-signal-mode-val').textContent");
        host.PostMessage(LR"({"state":{"noSignalMode":"image","noSignalImage":"","noSignalImageAvailable":false,"configWarning":"Settings will not be saved."}})");
        Check(PumpUntil([&] {
            return Script(view, L"document.getElementById('no-signal-mode-val').textContent === window.__neutralImageLabel") == L"true";
        }), "unchosen custom image keeps neutral localized label");
        Check(Script(view, L"document.getElementById('config-warning').textContent === 'Settings will not be saved.' && document.getElementById('config-warning').getBoundingClientRect().height > 0") == L"true",
              "config warning remains visibly in settings");
        Check(Script(view, L"document.getElementById('no-signal-mode-val').textContent === window.NitLinkLocales['zh-TW']['value.customImage']") == L"true",
              "applied empty-image state has no unavailable label");
        host.PostMessage(LR"({"state":{"configWarning":""}})");
        Check(PumpUntil([&] { return Script(view, L"document.getElementById('config-warning').hidden") == L"true"; }),
              "resolved config warning clears");
        const size_t count = messages.size();
        Script(view, L"document.querySelector('[data-action=toggleHDR]').click()");
        Check(PumpUntil([&] { return messages.size() > count; }), "menu control posts through native bridge");
        Check(NitLink::ParseSettingsMessage(messages.back()).has_value(), "menu control payload schema");

        const size_t beforeAuto = messages.size();
        Script(view, L"chrome.webview.postMessage({action:'setCaptureFormatOverride',value:{width:0,height:0,fps:0,fpsNumerator:0,fpsDenominator:1,format:''}})");
        Check(PumpUntil([&] { return messages.size() > beforeAuto; }), "real WebView serializer delivers Auto payload");
        const auto automatic = NitLink::ParseSettingsMessage(messages.back());
        Check(automatic && automatic->action == L"setCaptureFormatOverride" &&
              automatic->format.isFullAuto() && automatic->format.fpsDenominator == 1,
              "real Auto payload preserves zero numerator and unit denominator");

        const size_t beforeUntrusted = messages.size();
        Script(view, LR"JS(
            window.__nativeProbe = false;
            chrome.webview.addEventListener('message', e => { if (e.data.probe) __nativeProbe = true; });
            history.replaceState(null, '', '#untrusted');
        )JS");
        // History notifications reach the host asynchronously; this remains the
        // same loaded document until the host observes the changed source.
        Check(PumpUntil([&] {
            wil::unique_cotaskmem_string source;
            return SUCCEEDED(view->get_Source(&source)) && source &&
                std::wstring_view(source.get()).ends_with(L"#untrusted");
        }), "host observes changed source");
        Script(view, L"chrome.webview.postMessage({action:'toggleHDR'})");
        host.PostMessage(LR"({"probe":true})");
        PumpUntil([] { return false; }, 100);
        Check(messages.size() == beforeUntrusted, "bridge rejects changed document source");
        Check(Script(view, L"window.__nativeProbe") == L"false", "state withheld from changed document");
        Script(view, L"history.replaceState(null, '', '/nitlink-menu.html')");
        Check(PumpUntil([&] {
            wil::unique_cotaskmem_string source;
            return SUCCEEDED(view->get_Source(&source)) && source &&
                NitLink::WebViewPolicy::IsTrustedDocument(source.get());
        }), "host observes restored source");
        host.PostMessage(LR"({"probe":true})");
        Check(PumpUntil([&] { return Script(view, L"window.__nativeProbe") == L"true"; }), "state resumes only at exact document");

        auto deniedResource = std::make_shared<bool>(false);
        EventRegistrationToken resourceToken{};
        Hr(view2->add_WebResourceResponseReceived(Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>(
            [deniedResource](ICoreWebView2*, ICoreWebView2WebResourceResponseReceivedEventArgs* args) -> HRESULT {
                wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                wil::unique_cotaskmem_string uri;
                wil::com_ptr<ICoreWebView2WebResourceResponseView> response;
                int status = 0;
                if (SUCCEEDED(args->get_Request(&request)) && SUCCEEDED(request->get_Uri(&uri)) && uri &&
                    std::wstring_view(uri.get()) == L"https://nitlink.invalid/nitlink.json" &&
                    SUCCEEDED(args->get_Response(&response)) && SUCCEEDED(response->get_StatusCode(&status)))
                    *deniedResource = status == 403;
                return S_OK;
            }).Get(), &resourceToken));
        Script(view, L"const probe = new Image(); probe.src='https://nitlink.invalid/nitlink.json'; document.body.append(probe)");
        Check(PumpUntil([&] { return *deniedResource; }), "unlisted local resource receives native 403");
        Hr(view2->remove_WebResourceResponseReceived(resourceToken));

        auto navigationCancelled = std::make_shared<bool>(false);
        EventRegistrationToken navToken{};
        Hr(view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [navigationCancelled](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                BOOL cancel = FALSE;
                const HRESULT hr = args->get_Cancel(&cancel);
                *navigationCancelled = SUCCEEDED(hr) && cancel;
                return hr;
            }).Get(), &navToken));
        for (auto uri : {L"file:///C:/NitLink-security-fixture/blocked.html", L"https://blocked.invalid/",
             L"https://nitlink.invalid/locales/en-US.js", L"https://nitlink.invalid/nitlink-menu.html?x",
             L"data:text/html,blocked", L"about:blank"}) {
            *navigationCancelled = false;
            Hr(view->Navigate(uri));
            Check(PumpUntil([&] { return *navigationCancelled; }), "host cancels untrusted navigation");
        }
        Hr(view->remove_NavigationStarting(navToken));
        auto popupHandled = std::make_shared<bool>(false);
        EventRegistrationToken popupToken{};
        Hr(view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [popupHandled](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                BOOL handled = FALSE;
                const HRESULT hr = args->get_Handled(&handled);
                *popupHandled = SUCCEEDED(hr) && handled;
                return hr;
            }).Get(), &popupToken));
        Script(view, L"window.open('https://blocked.invalid/')");
        Check(PumpUntil([&] { return *popupHandled; }), "unlisted popup handled without opening a browser");
        Hr(view->remove_NewWindowRequested(popupToken));
        auto permissionDenied = std::make_shared<bool>(false);
        EventRegistrationToken permissionToken{};
        Hr(view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [permissionDenied](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PERMISSION_STATE state = COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
                const HRESULT hr = args->get_State(&state);
                *permissionDenied = SUCCEEDED(hr) && state == COREWEBVIEW2_PERMISSION_STATE_DENY;
                return hr;
            }).Get(), &permissionToken));
        Script(view, L"window.__permission='pending'; Notification.requestPermission().then(result=>window.__permission=result)");
        Check(PumpUntil([&] { return Script(view, L"window.__permission") == L"\"denied\""; }), "permission denied");
        Check(*permissionDenied, "native permission handler denies the request");
        Hr(view->remove_PermissionRequested(permissionToken));
        const size_t beforeBurst = messages.size();
        Script(view, L"for(let i=0;i<2000;i++) chrome.webview.postMessage({action:'ready'})");
        PumpUntil([] { return false; }, 300);
        Check(messages.size() > beforeBurst && messages.size() - beforeBurst < 2000,
              "native bridge limits a message burst");
        Script(view, L"for(let i=0;i<2000;i++) { chrome.webview.postMessage({action:'setVolume',value:i/2000}); chrome.webview.postMessage({action:'setPiPOpacity',value:0.1+0.9*i/2000}); } chrome.webview.postMessage({action:'setVolume',value:0.321}); chrome.webview.postMessage({action:'setPiPOpacity',value:0.654})");
        Check(PumpUntil([&] {
            host.DispatchPendingMessages();
            double volume = -1, opacity = -1;
            for (const auto& json : messages) {
                const auto message = NitLink::ParseSettingsMessage(json);
                if (message && message->action == L"setVolume") volume = message->number;
                if (message && message->action == L"setPiPOpacity") opacity = message->number;
            }
            return volume == 0.321 && opacity == 0.654;
        }), "both final slider values survive an exhausted message budget");
        WebViewSettingsTestAccess::QueueFinalSliders(host);
        const auto beforeClose = messages.size();
        host.DispatchPendingMessages(/*closing=*/true);
        double closingVolume = -1, closingOpacity = -1;
        for (size_t i = beforeClose; i < messages.size(); ++i) {
            const auto parsed = NitLink::ParseSettingsMessage(messages[i]);
            if (parsed && parsed->action == L"setVolume") closingVolume = parsed->number;
            if (parsed && parsed->action == L"setPiPOpacity") closingOpacity = parsed->number;
        }
        Check(messages.size() == beforeClose + 2 && closingVolume == 0.234 && closingOpacity == 0.567,
              "native close commits only the two pending final slider values");
        std::wstring failure;
        host.SetFailureHandler([&](const std::wstring& reason) { failure = reason; });
        const size_t messagesBeforeCrash = messages.size();
        UINT32 browserPid = 0;
        Hr(view->get_BrowserProcessId(&browserPid));
        Check(browserPid != 0 && browserPid != GetCurrentProcessId(), "isolated test browser PID");
        HANDLE browser = OpenProcess(PROCESS_TERMINATE, FALSE, browserPid);
        Check(browser != nullptr, "test browser process handle");
        const bool terminated = TerminateProcess(browser, 1) != FALSE;
        CloseHandle(browser);
        Check(terminated && PumpUntil([&] { return !failure.empty(); }), "real browser process failure reaches host");
        Check(!host.IsReady() && !host.IsVisible() && failure == L"toast.settingsProcessFailed" &&
              failure == host.InitializationError(), "failed host reports localized reason and closes panel");
        host.Show(true);
        Check(!host.IsVisible(), "restarting host stays hidden");
        Check(PumpUntil([&] { host.CheckHealth(); return host.IsReady(); }),
              "browser crash automatically restarts the secured host");
        auto* recovered = WebViewSettingsTestAccess::View(host);
        Check(recovered && host.InitializationError().empty(),
              "restarted host has no initialization error");
        wil::unique_cotaskmem_string recoveredSource;
        Check(SUCCEEDED(recovered->get_Source(&recoveredSource)) && recoveredSource &&
              NitLink::WebViewPolicy::IsTrustedDocument(recoveredSource.get()),
              "restarted host loads the trusted packaged menu URI");
        Check(PumpUntil([&] {
            return Script(recovered,
                L"document.readyState === 'complete' && !!document.querySelector('h1') && "
                L"!!document.getElementById('pane-about') && "
                L"!!(window.NitLinkLocales && window.NitLinkLocales['en-US'] && "
                L"window.NitLinkLocales['zh-TW'] && window.NitLinkLocales['zh-CN'] && window.NitLinkLocales['de-DE']) && "
                L"!!(window.chrome && window.chrome.webview && window.chrome.webview.postMessage)") == L"true";
        }), "restarted packaged DOM, locale resources and JavaScript bridge finish loading");
        const std::wstring readyLocale = Script(recovered, L"document.documentElement.lang");
        Check(readyLocale == L"\"en-US\"" || readyLocale == L"\"zh-TW\"" || readyLocale == L"\"zh-CN\"" || readyLocale == L"\"de-DE\"",
              "restarted menu begins in a supported system locale");
        bool restartedReadyMessage = false;
        for (size_t i = messagesBeforeCrash; i < messages.size(); ++i) {
            const auto message = NitLink::ParseSettingsMessage(messages[i]);
            if (message && message->action == L"ready") {
                restartedReadyMessage = true;
                break;
            }
        }
        Check(restartedReadyMessage, "restarted menu sends a fresh ready message through the native bridge");

        // The application responds to every ready message with its persisted
        // state. Mirror that native response here and verify all supported
        // preferences are applied to the recovered document.
        for (const auto* locale : {L"zh-TW", L"en-US", L"zh-CN", L"de-DE"}) {
            const std::wstring state = std::wstring(L"{\"state\":{\"locale\":\"") + locale +
                L"\",\"languagePreference\":\"" + locale + L"\"}}";
            host.PostMessage(state);
            const std::wstring localizedProbe =
                std::wstring(L"document.documentElement.lang === '") + locale +
                L"' && document.querySelector('[data-i18n=\"nav.video\"]').textContent === "
                L"window.NitLinkLocales['" + locale + L"']['nav.video']";
            Check(PumpUntil([&] { return Script(recovered, localizedProbe.c_str()) == L"true"; }),
                  locale == std::wstring_view(L"zh-TW") ? "restart bridge applies Traditional Chinese" :
                  locale == std::wstring_view(L"en-US") ? "restart bridge applies English" :
                  locale == std::wstring_view(L"zh-CN") ? "restart bridge applies Simplified Chinese" :
                                                           "restart bridge applies German");
        }
        wil::com_ptr<ICoreWebView2Settings> recoveredSettings;
        Hr(recovered->get_Settings(&recoveredSettings));
        Hr(recoveredSettings->get_AreHostObjectsAllowed(&enabled));
        Check(!enabled, "restart reapplies host-object security restriction");
        Hr(recoveredSettings->get_AreDevToolsEnabled(&enabled));
        Check(!enabled, "restart reapplies developer-tools restriction");
        Hr(recoveredSettings->get_AreDefaultScriptDialogsEnabled(&enabled));
        Check(!enabled, "restart reapplies script-dialog restriction");
        auto recoveredController4 = wil::com_ptr<ICoreWebView2Controller>(
            WebViewSettingsTestAccess::Controller(host)).try_query<ICoreWebView2Controller4>();
        Hr(recoveredController4->get_AllowExternalDrop(&enabled));
        Check(!enabled, "restart reapplies external-drop restriction");
        Check(Script(recovered, LR"JS((() => {
            window.__restartedViolations = [];
            document.addEventListener('securitypolicyviolation', e => __restartedViolations.push(e.violatedDirective));
            window.__restartedInjection = false;
            const script = document.createElement('script'); script.textContent = 'window.__restartedInjection=true'; document.body.append(script);
            const frame = document.createElement('iframe'); frame.srcdoc = '<script>parent.__restartedInjection=true<\/script>'; document.body.append(frame);
            const remoteFrame = document.createElement('iframe'); remoteFrame.src = 'https://blocked.invalid/frame'; document.body.append(remoteFrame);
            fetch('https://blocked.invalid/').catch(() => {});
            try { new Worker('assets/menu/menu.js'); } catch (_) {}
            return true;
        })())JS") == L"true", "restarted document accepts the security-policy probe");
        const bool restartedCspEnforced = PumpUntil([&] { return Script(recovered,
            L"!window.__restartedInjection && __restartedViolations.some(x=>x.startsWith('script-src')) && "
            L"__restartedViolations.includes('frame-src') && __restartedViolations.includes('connect-src') && "
            L"__restartedViolations.includes('worker-src')") == L"true"; });
        if (!restartedCspEnforced)
            std::wcout << L"Restart CSP observations " << Script(recovered,
                L"JSON.stringify({injected:window.__restartedInjection,violations:window.__restartedViolations})") << L'\n';
        Check(restartedCspEnforced,
            "restart reapplies CSP against inline scripts, frames and network requests");
        bool blockedNavigationCancelled = false;
        EventRegistrationToken recoveredNavigationToken{};
        Hr(recovered->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [&blockedNavigationCancelled](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri &&
                    std::wstring_view(uri.get()).find(L"blocked.invalid/restart") != std::wstring_view::npos) {
                    BOOL cancelled = FALSE;
                    if (SUCCEEDED(args->get_Cancel(&cancelled))) blockedNavigationCancelled = cancelled != FALSE;
                }
                return S_OK;
            }).Get(), &recoveredNavigationToken));
        Hr(recovered->Navigate(L"https://blocked.invalid/restart"));
        Check(PumpUntil([&] { return blockedNavigationCancelled; }),
              "restart reapplies untrusted-navigation blocking");
        Hr(recovered->remove_NavigationStarting(recoveredNavigationToken));
        host.Shutdown();

        WebViewSettings navigationHost;
        std::wstring navigationFailure;
        navigationHost.SetFailureHandler([&](const std::wstring& reason) { navigationFailure = reason; });
        Check(navigationHost.Initialize(window, 800, 600), "navigation failure fixture starts");
        navigationHost.NavigateToMenu();
        Check(PumpUntil([&] { navigationHost.CheckHealth(); return navigationHost.IsReady(); }),
              "navigation fixture reaches trusted ready");
        auto* navigationView = WebViewSettingsTestAccess::View(navigationHost);
        EventRegistrationToken cancelToken{};
        Hr(navigationView->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                return args->put_Cancel(TRUE);
            }).Get(), &cancelToken));
        navigationHost.NavigateToMenu();
        Check(PumpUntil([&] { return !navigationFailure.empty(); }) &&
              navigationFailure == L"toast.settingsNavigation" && !navigationHost.IsReady(),
              "failed trusted navigation closes panel and reports reason");
        Check(PumpUntil([&] { navigationHost.CheckHealth(); return navigationHost.IsReady(); }),
              "failed trusted navigation automatically recovers");
        navigationHost.Shutdown();
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        result = 1;
    }
    if (window) DestroyWindow(window);
    CoUninitialize();
    std::wcout << L"Disposable profile: " << profile.c_str() << L'\n';
    return result;
}
