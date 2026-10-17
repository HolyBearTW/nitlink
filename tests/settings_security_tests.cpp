#include "app/message_budget.h"
#include "app/webview_lifecycle.h"
#include "app/localization.h"
#include <cmath>
#include <vector>
#include "app/settings_message.h"
#include "app/webview_policy.h"

#include <iostream>
#include <random>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>

namespace NitLink {
struct LocalizationTestAccess {
    static std::wstring ResolveSystemLocale(const std::wstring& locale) {
        return Localization::ResolveSystemLocale(locale);
    }
};
}

namespace {
int failures = 0;
void Expect(bool value, const char* label) {
    if (!value) { std::cerr << "FAIL " << label << '\n'; ++failures; }
}

void CheckPackagedScripts() {
    wchar_t executable[32768]{};
    const auto length=GetModuleFileNameW(nullptr,executable,32768);
    Expect(length>0&&length<32768,"test executable path is bounded");
    if(!length||length>=32768)return;
    std::ifstream input(std::filesystem::path(executable).parent_path()/L"nitlink-menu.html",std::ios::binary);
    const std::string html{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    Expect(!html.empty(),"the actual packaged settings HTML is available");
    const std::regex scripts(R"html(<script\b[^>]*\bsrc="([^"]+)")html");
    size_t count=0;
    for(auto it=std::sregex_iterator(html.begin(),html.end(),scripts);it!=std::sregex_iterator();++it){
        std::wstring uri=L"https://nitlink.invalid/";
        for(const unsigned char c:(*it)[1].str())uri.push_back(static_cast<wchar_t>(c));
        const auto* resource=NitLink::WebViewPolicy::FindResource(uri);
        Expect(resource&&resource->kind==NitLink::WebViewPolicy::ResourceKind::Script,
            "every actual HTML script URL matches the exact native allowlist (F1 Ready contract)");
        ++count;
    }
    Expect(count>=7,"all four locales, preview guard, menu and artwork editor are checked");
    Expect(!NitLink::WebViewPolicy::FindResource(L"https://nitlink.invalid/assets/menu/menu.js?v=unexpected"),
        "unknown resource revisions remain denied");
}
}

int main() {
    CheckPackagedScripts();
    using NitLink::ParseSettingsMessage;
    for (const auto valid : {L"1", L"42", L"4294967295"}) {
        const auto request = ParseSettingsMessage(
            L"{\"action\":\"refreshAudioDiagnostics\",\"value\":" + std::wstring(valid) + L"}");
        Expect(request && request->number == std::stod(valid), "bounded audio refresh request preserved");
    }
    for (const auto invalid : {L"0", L"-1", L"1.5", L"4294967296", L"true", L"\"1\"", L"null", L"{}"})
        Expect(!ParseSettingsMessage(L"{\"action\":\"refreshAudioDiagnostics\",\"value\":" +
            std::wstring(invalid) + L"}"), "invalid audio refresh request rejected");
    Expect(!ParseSettingsMessage(LR"({"action":"refreshAudioDiagnostics"})"), "refresh requires a request ID");
    Expect(!ParseSettingsMessage(LR"({"action":"refreshAudioDiagnostics","value":1,"extra":true})"),
        "refresh rejects unknown fields");
    Expect(!ParseSettingsMessage(LR"({"action":"ready","value":1})"), "ready schema remains unchanged");
    for (auto action : {L"setAudioFifoMs", L"setAudioRenderMs"}) {
        const auto prefix = L"{\"action\":\"" + std::wstring(action) + L"\",\"value\":";
        for (auto valid : {L"3", L"12", L"100"})
            Expect(ParseSettingsMessage(prefix + valid + L"}").has_value(), "audio buffer valid integer");
        for (auto invalid : {L"2", L"101", L"3.5", L"true", L"\"12\"", L"null", L"1e99"})
            Expect(!ParseSettingsMessage(prefix + invalid + L"}"), "invalid audio buffer rejected");
    }
    const auto driftOn = ParseSettingsMessage(L"{\"action\":\"setAudioDrift\",\"value\":true}");
    const auto driftOff = ParseSettingsMessage(L"{\"action\":\"setAudioDrift\",\"value\":false}");
    Expect(driftOn && driftOn->number == 1 && driftOff && driftOff->number == 0, "drift boolean preserved");
    Expect(!ParseSettingsMessage(L"{\"action\":\"setAudioDrift\",\"value\":1}"), "drift rejects numeric toggle");

    const auto exclusiveOn = ParseSettingsMessage(L"{\"action\":\"setAudioExclusive\",\"value\":true}");
    const auto exclusiveOff = ParseSettingsMessage(L"{\"action\":\"setAudioExclusive\",\"value\":false}");
    Expect(exclusiveOn && exclusiveOn->number==1 && exclusiveOff && exclusiveOff->number==0,"exclusive boolean round trip");
    Expect(!ParseSettingsMessage(L"{\"action\":\"setAudioExclusive\",\"value\":1}"),"exclusive numeric toggle rejected");

    for (int kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
         kind <= COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED; ++kind)
        Expect(NitLink::SettingsProcessNeedsRestart(static_cast<COREWEBVIEW2_PROCESS_FAILED_KIND>(kind)) ==
               (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED ||
                kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED),
               "only browser and main renderer exits require host restart");
    auto& localization = NitLink::Localization::Instance();
    for (const auto* locale : {L"de", L"de-DE", L"de-AT", L"de-CH"})
        Expect(NitLink::LocalizationTestAccess::ResolveSystemLocale(locale) == L"de-DE",
               "German system locales resolve to de-DE");
    Expect(NitLink::LocalizationTestAccess::ResolveSystemLocale(L"fr-FR") == L"en-US",
           "unsupported system locales retain English fallback");
    for (const auto* key : {L"toast.configFolderNotWritable", L"toast.configLoadFailed",
         L"toast.configLoadFailedBackup", L"toast.configRecoveryRequired", L"toast.configRecoveryCopy", L"toast.configSaveFailed",
         L"toast.settingsProfile", L"toast.settingsRuntime", L"toast.settingsStartFailed",
         L"toast.settingsFiles", L"toast.settingsSecurity", L"toast.settingsMemory",
         L"toast.settingsResources", L"toast.settingsNavigation", L"toast.settingsProcessFailed",
         L"toast.settingsTimeout", L"toast.settingsStarting", L"toast.settingsRetry"}) {
        localization.SetPreference("en-US");
        const auto english = localization.Get(key);
        localization.SetPreference("zh-TW");
        const auto chinese = localization.Get(key);
        localization.SetPreference("zh-CN");
        const auto simplified = localization.Get(key);
        localization.SetPreference("de-DE");
        const auto german = localization.Get(key);
        Expect(english != key && chinese != key && simplified != key && german != key &&
               english != chinese && english != simplified && english != german,
               "all native locale tables define each settings/config warning");
    }
    localization.SetPreference("zh-CN");
    Expect(localization.PreferenceName() == "zh-CN" && localization.LocaleName() == L"zh-CN" &&
           std::wstring(localization.UiFontFamily()) == L"Microsoft YaHei UI",
           "Simplified Chinese selects native locale and font");
    localization.SetPreference("de-DE");
    Expect(localization.Preference() == NitLink::LanguagePreference::German &&
           localization.PreferenceName() == "de-DE" && localization.LocaleName() == L"de-DE" &&
           std::wstring(localization.UiFontFamily()) == L"Segoe UI",
           "German preference selects the German native table");
    localization.SetPreference("zh-CN");
    const auto simplifiedPlaceholder = localization.Format(L"toast.p010SelectionFallback",
           {{L"width", L"1920"}, {L"height", L"1080"}, {L"fps", L"60"}});
    if(simplifiedPlaceholder.find(L"1920×1080 @ 60")==std::wstring::npos)
        std::wcerr<<L"Simplified Chinese placeholder output: "<<simplifiedPlaceholder<<L'\n';
    Expect(simplifiedPlaceholder.find(L"1920×1080 @ 60") != std::wstring::npos,
           "Simplified Chinese native placeholders are substituted");
    localization.SetPreference("system");
    using namespace NitLink::WebViewPolicy;
    for (auto action : {L"ready", L"cycleNoSignalMode", L"chooseNoSignalImage",
         L"clearNoSignalImage", L"cycleNoSignalFit", L"cycleNoSignalDimImage",
         L"cyclePresentPacing", L"cycleAspect", L"cyclePanelSide", L"cycleScaler",
         L"clearGame", L"saveGameSettings", L"getDeviceList", L"refreshDevices",
         L"openScreenshotFolder"}) {
        const auto prefix = L"{\"action\":\"" + std::wstring(action) + L"\"";
        Expect(ParseSettingsMessage(prefix + L"}").has_value(), "action without value");
        Expect(!ParseSettingsMessage(prefix + L",\"value\":true}"), "unexpected value");
    }
    for (auto action : {L"toggleHDR", L"toggleColorExpansion", L"toggleNIS", L"toggleMute",
         L"toggleVSync", L"toggleLowLatency", L"togglePreventSleep"}) {
        const auto prefix = L"{\"action\":\"" + std::wstring(action) + L"\"";
        Expect(ParseSettingsMessage(prefix + L"}").has_value(), "native toggle");
        Expect(ParseSettingsMessage(prefix + L",\"value\":false}").has_value(), "menu toggle");
        Expect(!ParseSettingsMessage(prefix + L",\"value\":\"false\"}"), "string boolean");
    }
    const auto number = ParseSettingsMessage(L" { \"value\" : 7.5e-1, \"action\" : \"setVolume\" } \n");
    Expect(number && number->number == .75, "whitespace, order and JSON exponent");
    Expect(ParseSettingsMessage(LR"({"action":"setVolume","value":0})").has_value(), "volume minimum");
    Expect(ParseSettingsMessage(LR"({"action":"setPiPOpacity","value":0.1})").has_value(), "opacity minimum");
    Expect(ParseSettingsMessage(LR"({"action":"setPiPOpacity","value":1})").has_value(), "opacity maximum");
    for (auto invalid : {L"NaN", L"Infinity", L"-Infinity", L"1e999", L"1e-999", L"1.01",
         L"-0.01", L"01", L".5", L"1.", L"+1", L"0x1", L"1f", L"true", L"null", L"[]",
         L"{}", L"\"0.5\""}) {
        Expect(!ParseSettingsMessage(L"{\"action\":\"setVolume\",\"value\":" + std::wstring(invalid) + L"}"),
               "invalid numeric value");
    }
    for (auto invalid : {
        LR"({"nested":{"action":"toggleHDR"}})",
        LR"({"action":"ready","action":"toggleHDR"})",
        LR"({"action":"ready","\u0061ction":"toggleHDR"})",
        LR"({"action":"ready","unknown":0})",
        LR"({"action":"ready"}garbage)", LR"({"action":"ready",})",
        LR"([{"action":"ready"}])", LR"("{\"action\":\"ready\"}")",
        LR"({"action":"setPreferredDevice","value":"bad\x20"})",
        LR"({"action":"setPreferredDevice","value":"\uD800"})",
        LR"({"action":"setPreferredDevice","value":"\uDC00"})",
        LR"({"action":"setPreferredDevice","value":"\u0000"})",
        LR"({"action":"setPreferredDevice","value":"line\nbreak"})",
        LR"({"action":"setLanguage","value":"unknown"})",
        LR"({"action":"setNoSignalMode","value":"../file"})",
        LR"({"action":"setNoSignalFit","value":"fill"})",
        LR"({"action":"setGame","value":"arbitrary-config-key"})",
        LR"({"action":"openScreenshotFolder","value":"\\server\share\file.png"})",
        LR"({"action":"openScreenshotFolder","value":"C:\\other.png"})",
        LR"({"action":"notAnAction"})", LR"({"action":true})", L"", L"{}"}) {
        Expect(!ParseSettingsMessage(invalid), "malformed message or schema violation");
    }
    const auto device = ParseSettingsMessage(LR"({"action":"setPreferredDevice","value":"Elgato \"USB\" \\ 測試 \uD83D\uDE00"})");
    Expect(device && device->text == L"Elgato \"USB\" \\ 測試 \xD83D\xDE00", "escaped Unicode device name");
    for (auto valid : {
        LR"({"action":"setLanguage","value":"system"})", LR"({"action":"setLanguage","value":"en-US"})",
        LR"({"action":"setLanguage","value":"zh-CN"})",
        LR"({"action":"setLanguage","value":"zh-TW"})", LR"({"action":"setLanguage","value":"de-DE"})", LR"({"action":"setNoSignalMode","value":"image"})",
        LR"({"action":"setNoSignalMode","value":"default"})", LR"({"action":"setNoSignalFit","value":"contain"})",
        LR"({"action":"setNoSignalFit","value":"cover"})", LR"({"action":"setNoSignalFit","value":"stretch"})",
        LR"({"action":"setGame","value":"spider-man-2"})", LR"({"action":"setGame","value":""})"})
        Expect(ParseSettingsMessage(valid).has_value(), "allowed string value");

    const auto prefix = std::wstring(LR"({"action":"setCaptureFormatOverride","value":)");
    const auto format = ParseSettingsMessage(prefix + LR"({"width":1920,"height":1080,"fps":59,"fpsNumerator":60000,"fpsDenominator":1001,"format":"P010"}})");
    Expect(format && format->format.fps == 59 && format->format.fpsNumerator == 60000 &&
           format->format.fpsDenominator == 1001, "rational capture rate preserved");
    const auto integerRate = ParseSettingsMessage(prefix +
        LR"({"width":1920,"height":1080,"fps":60,"fpsNumerator":60,"fpsDenominator":1,"format":"NV12"}})");
    Expect(integerRate && integerRate->format.fps == 60 && integerRate->format.fpsNumerator == 60 &&
           integerRate->format.fpsDenominator == 1, "menu integer framerate rational payload");
    const auto legacy = ParseSettingsMessage(prefix + LR"({"width":0,"height":0,"fps":59,"format":""}})");
    Expect(legacy && legacy->format.fps == 59 && legacy->format.fpsNumerator == 0,
           "legacy integer rate remains unresolved");
    for (auto valid : {
        LR"({"width":0,"height":0,"fps":0,"fpsNumerator":0,"fpsDenominator":1,"format":""})",
        LR"({"width":3840,"height":2160,"fps":0,"fpsNumerator":0,"fpsDenominator":1,"format":"P010"})",
        LR"({"width":0,"height":0,"fps":0,"fpsNumerator":0,"fpsDenominator":1,"format":"NV12"})",
        LR"({"width":1920,"height":1080,"fps":0,"fpsNumerator":0,"fpsDenominator":1,"format":""})",
        LR"({"width":1920,"height":1080,"fps":0,"fpsNumerator":0,"fpsDenominator":1,"format":"BGRA"})"}) {
        const auto automatic = ParseSettingsMessage(prefix + valid + L"}");
        Expect(automatic && automatic->format.fps == 0 && automatic->format.fpsNumerator == 0 &&
               automatic->format.fpsDenominator == 1, "menu resolution/framerate Auto rational payload");
    }
    const auto legacyEcho = ParseSettingsMessage(prefix +
        LR"({"width":1920,"height":1080,"fps":59,"fpsNumerator":0,"fpsDenominator":1,"format":"NV12"}})");
    Expect(legacyEcho && legacyEcho->format.fps == 59 && legacyEcho->format.fpsNumerator == 0 &&
           legacyEcho->format.fpsDenominator == 1, "menu legacy-rate echo keeps zero numerator");
    for (auto bad : {
        LR"({"width":-1,"height":1080,"fps":60,"format":"NV12"})",
        LR"({"width":16385,"height":1080,"fps":60,"format":"NV12"})",
        LR"({"width":1920.5,"height":1080,"fps":60,"format":"NV12"})",
        LR"({"width":"1920","height":1080,"fps":60,"format":"NV12"})",
        LR"({"width":1920,"height":1080,"fps":1001,"format":"NV12"})",
        LR"({"width":1920,"height":1080,"fps":60,"format":"YUY2"})",
        LR"({"width":1920,"height":1080,"fps":60,"format":"NV12","extra":0})",
        LR"({"width":1920,"height":1080,"fps":60,"format":"NV12","fpsNumerator":60})",
        LR"({"width":1920,"height":1080,"fps":60,"format":"NV12","fpsDenominator":1})",
        LR"({"width":1920,"height":1080,"fps":60,"format":"NV12","fpsNumerator":60,"fpsDenominator":0})",
        LR"({"width":1920,"height":1080,"fps":60,"format":"NV12","fpsNumerator":1000000000,"fpsDenominator":1})",
        LR"({"width":1920,"height":1080,"fps":60,"format":"NV12","width":1})",
        LR"({"width":{"width":1920},"height":1080,"fps":60,"format":"NV12"})"})
        Expect(!ParseSettingsMessage(prefix + bad + L"}"), "invalid capture override");
    Expect(!ParseSettingsMessage(LR"({"action":"setCaptureFormatOverride","width":1920,"height":1080,"fps":60,"format":"NV12"})"), "override fields at wrong depth");
    Expect(!ParseSettingsMessage(std::wstring(8193, L' ')), "oversized input");
    Expect(!ParseSettingsMessage(std::wstring(4000, L'{')), "bounded nesting");
    std::mt19937 random(27);
    for (int i = 0; i < 20000; ++i) {
        std::wstring input(random() % 256, L' ');
        for (auto& c : input) c = static_cast<wchar_t>(random() % 65536);
        Expect(!ParseSettingsMessage(input), "random malformed UTF-16");
    }

    const std::vector<std::wstring> seeds = {
        LR"({"action":"ready"})", LR"({"action":"toggleHDR","value":true})",
        LR"({"action":"setCaptureFormatOverride","value":{"width":0,"height":0,"fps":0,"fpsNumerator":0,"fpsDenominator":1,"format":""}})",
        LR"({"action":"setVolume","value":0.75})", LR"({"action":"setPiPOpacity","value":1e-1})",
        LR"({"action":"setPreferredDevice","value":"Capture \u6e2c\u8a66 \uD83D\uDE00"})",
        LR"({"action":"setLanguage","value":"zh-TW"})",
        LR"({"action":"setCaptureFormatOverride","value":{"width":3840,"height":2160,"fps":59,"format":"P010","fpsNumerator":60000,"fpsDenominator":1001}})"};
    size_t acceptedMutations = 0;
    for (size_t i = 0; i < 100000; ++i) {
        std::wstring input = seeds[random() % seeds.size()];
        for (size_t j = 0, edits = 1 + random() % 8; j < edits; ++j) {
            const size_t offset = random() % (input.size() + 1);
            switch (random() % 4) {
                case 0: input.insert(offset, 1, static_cast<wchar_t>(random() % 65536)); break;
                case 1: input.erase(offset, random() % 12); break;
                case 2: input.insert(offset, input.substr(random() % input.size(), random() % 16)); break;
                case 3: input.insert(offset, LR"(\uD800)"); break;
            }
            if (input.empty()) input = seeds[0];
        }
        const auto parsed = ParseSettingsMessage(input);
        if (parsed) {
            ++acceptedMutations;
            Expect(parsed->action.size() <= 64 && parsed->text.size() <= 1024 &&
                std::isfinite(parsed->number) && parsed->number >= 0 && parsed->number <= 1 &&
                parsed->format.width <= 16384 && parsed->format.height <= 16384 &&
                parsed->format.fps <= 1000 && parsed->format.fpsDenominator > 0,
                "structurally mutated message preserves native bounds");
        }
    }
    Expect(acceptedMutations > 0, "mutation corpus reaches valid schema paths");
    NitLink::MessageBudget budget;
    for (int i = 0; i < 120; ++i) Expect(budget.Accept(100), "initial message burst");
    Expect(!budget.Accept(100), "burst exhausted");
    Expect(!budget.Accept(116) && budget.Accept(117), "60 Hz budget refill");
    Expect(!budget.Accept(0), "backwards timestamp cannot grant credit");
    Expect(budget.Accept(UINT64_MAX), "large elapsed time refill is bounded");
    NitLink::PendingSliderMessages pending;
    NitLink::MessageBudget slotsBudget;
    for (int i = 0; i < 120; ++i) slotsBudget.Accept(100);
    std::vector<std::wstring> delivered;
    auto collect = [&](const std::wstring& value) { delivered.push_back(value); };
    Expect(pending.Empty(), "pending slots start empty");
    pending.Store(0, L"volume old");
    Expect(pending.Store(0, L"volume latest"), "same slider replaces only its own value");
    pending.Store(1, L"opacity latest");
    pending.Drain(slotsBudget, 100, false, collect);
    Expect(delivered.empty() && !pending.Empty(), "exhausted budget retains both slots");
    pending.Drain(slotsBudget, 117, false, collect);
    pending.Store(0, L"volume arriving again");
    pending.Drain(slotsBudget, 134, false, collect);
    Expect(pending.Has(0) && !pending.Has(1), "volume refill cannot starve pending opacity");
    pending.Clear();
    Expect(delivered == std::vector<std::wstring>{L"volume latest", L"opacity latest"} &&
           pending.Empty(), "latest values drain fairly after refill");
    pending.Store(0, L"close volume"); pending.Store(1, L"close opacity");
    pending.Drain(slotsBudget, 134, true, collect);
    Expect(delivered.size() == 4 && pending.Empty(), "native closing drain is bounded to two slots");
    pending.Store(0, std::wstring(8193, L'x')); pending.Store(2, L"invalid slot");
    Expect(pending.Empty(), "slot indices and storage remain bounded");
    pending.Store(0, L"stale"); pending.Clear();
    Expect(pending.Empty(), "source/lifetime reset drops pending values");

    std::cout << "100000 structural mutations; " << acceptedMutations << " accepted within schema bounds\n";

    Expect(IsTrustedDocument(MenuUri), "exact menu document");
    Expect(CanReceiveMessage(MenuUri, MenuUri), "trusted source and current document");
    for (auto bad : {L"", L"about:blank", L"file:///C:/nitlink-menu.html", L"file://server/menu.html",
         L"https://nitlink.invalid/other.html", L"https://nitlink.invalid/nitlink-menu.html#x",
         L"https://nitlink.invalid/nitlink-menu.html?x", L"https://nitlink.invalid/nitlink-menu.html/",
         L"https://nitlink.invalid/%6eitlink-menu.html", L"https://nitlink.invalid/../nitlink-menu.html",
         L"https://nitlink.invalid:444/nitlink-menu.html", L"https://nitlink.invalid.evil/nitlink-menu.html",
         L"https://nitlink.invalid@other.invalid/nitlink-menu.html", L"javascript:alert(1)", L"data:text/html,test"}) {
        Expect(!IsTrustedDocument(bad), "untrusted document");
        Expect(!CanReceiveMessage(bad, MenuUri), "untrusted message source");
        Expect(!CanReceiveMessage(MenuUri, bad), "untrusted current document");
    }
    for (const auto& resource : Resources) Expect(FindResource(resource.uri) == &resource, "packaged resource");
    Expect(FindResource(L"https://nitlink.invalid/locales/de-DE.js") != nullptr,"German locale resource is allowlisted");
    Expect(FindResource(L"https://nitlink.invalid/assets/menu/artwork-editor.js?v=20261018-game-artwork-metadata2")!=nullptr,"fixed script revision maps to packaged editor");
    Expect(!FindResource(L"https://nitlink.invalid/assets/menu/artwork-editor.js?v=other"),"unapproved revision rejected");
    Expect(ParseSettingsMessage(LR"({"action":"renderGameArtwork","value":{"gameId":5267007,"coverId":4200000001,"mode":"crop","x":0.2,"y":0.8,"zoom":1.5}})").has_value(),"bounded artwork recipe bridge");
    Expect(!ParseSettingsMessage(LR"({"action":"renderGameArtwork","value":{"gameId":5267007,"coverId":4200000001,"mode":"crop","x":-1,"y":0.8,"zoom":1.5}})"),"negative crop rejected");
    Expect(!ParseSettingsMessage(LR"({"action":"renderGameArtwork","value":{"gameId":5267007,"coverId":4200000001,"mode":"fit","x":0.2,"y":0.8,"zoom":1,"url":"https://evil.test/image"}})"),"artwork editor cannot submit arbitrary URLs");
    Expect(ParseSettingsMessage(LR"({"action":"checkHolyBearUpdate"})").has_value(), "manual update check schema");
    Expect(ParseSettingsMessage(LR"({"action":"setHolyBearAutoUpdateCheck","value":false})").has_value(), "opt-out schema");
    Expect(!ParseSettingsMessage(LR"({"action":"setHolyBearAutoUpdateCheck","value":"true"})"), "update option requires boolean");
    Expect(!ParseSettingsMessage(LR"({"action":"checkHolyBearUpdate","value":"https://evil"})"), "caller cannot supply URL");
    Expect(CanOpenExternal(MenuUri, L"https://github.com/CN115", true), "contributor profile link");
    Expect(CanOpenExternal(MenuUri, L"https://github.com/Noka123", true), "German contributor profile link");
    Expect(CanOpenExternal(MenuUri, L"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.5-holybear", true), "validated HolyBear release link");
    Expect(!CanOpenExternal(MenuUri, L"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.5-holybear", false), "no automatic release popup");
    Expect(!CanOpenExternal(MenuUri, L"https://github.com/HolyBearTW/nitlink/releases/tag/v1.2.5-holybear", true), "old repository release link rejected after canonical rename");
    for (auto bad : {L"https://nitlink.invalid/nitlink.json", L"https://nitlink.invalid/src/main.cpp",
         L"https://nitlink.invalid/locales/../../nitlink.json", L"https://fonts.googleapis.com/",
         L"https://nitlink.invalid/assets/menu/menu.js?x", L"file:///C:/secret.txt"})
        Expect(!FindResource(bad), "unlisted resource");
    for (auto target : {L"https://tally.so/r/EkZjNr", L"https://ko-fi.com/klosed89", L"https://github.com/HolyBearTW"}) {
        Expect(CanOpenExternal(MenuUri, target, true), "user activated menu link");
        Expect(!CanOpenExternal(MenuUri, target, false), "script popup");
        Expect(!CanOpenExternal(L"about:blank", target, true), "link from untrusted document");
    }
    for (auto target : {L"file:///C:/run.exe", L"ms-settings:display", L"javascript:alert(1)",
         L"http://ko-fi.com/klosed89", L"https://ko-fi.com/klosed89?redirect=elsewhere",
         L"https://ko-fi.com@other.invalid/klosed89", L"https://other.invalid/"})
        Expect(!CanOpenExternal(MenuUri, target, true), "unlisted shell target");
    Expect(IsLocalScreenshotPath(L"C:\\Users\\測試\\Pictures\\NitLink\\nitlink_1.png"), "local screenshot");
    for (auto path : {L"", L"\\\\server\\share\\file.png", L"\\\\?\\C:\\file.png", L"C:file.png",
         L"C:\\file.png:stream", L"C:\\file\".png", L"C:\\x\\..\\file.png", L"C:\\file.exe"})
        Expect(!IsLocalScreenshotPath(path), "unsafe screenshot path");
    std::cout << (failures ? "FAIL" : "PASS") << " settings security policy and parser\n";
    return failures ? 1 : 0;
}
