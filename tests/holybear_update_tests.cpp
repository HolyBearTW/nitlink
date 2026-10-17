#include "app/update_checker.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>
using namespace NitLink::HolyBear;
int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    // Explicit post-publication probe; CTest always uses offline mock responses.
    if (argc == 2 && std::string_view(argv[1]) == "--live-release") {
        UpdateChecker live;
        if (!live.Start()) return 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (live.Busy() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto result = live.TakeResult();
        if (!result) return 1;
        std::wcout << L"Live HolyBear Release: " << StatusKey(result->status) << L"; " << result->tag << L'\n';
        return result->status == UpdateStatus::Current && result->tag == L"v" + std::wstring(Version) ? 0 : 1;
    }
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) ++failures; std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; };
    check(ParseVersion(L"v1.2.4-holybear") == ParseVersion(L"1.2.4.0-holybear"), "three/four part equality");
    check(ParseVersion(L"v1.2.4.1-holybear") > ParseVersion(L"1.2.4-holybear"), "1.2.4.1 update from published 1.2.4");
    check(ParseVersion(L"v1.2.4.2-holybear") > ParseVersion(L"v1.2.4.1-holybear"), "numeric revision");
    check(ParseVersion(L"v1.2.4.10-holybear") > ParseVersion(L"v1.2.4.2-holybear"), "numeric, not lexical order");
    check(ParseVersion(L"v1.2.5-holybear") > ParseVersion(L"v1.2.4.2-holybear"), "patch exceeds revision");
    for (auto bad : {L"1.2.4", L"v0.1.2", L"v1.2.4-holybear-beta", L"v1.2-holybear", L"v1.02.4-holybear",
            L"v1.2.4.1.1-holybear", L"v65536.2.4-holybear", L"v1.2.4.-holybear", L"v1.2.4-holybear/evil"})
        check(!ParseVersion(bad), "invalid/other edition version");
    auto response = [](std::string tag) {
        return HttpReply{200, 0, "{\"tag_name\":\"" + tag + "\",\"draft\":false,\"prerelease\":false,\"html_url\":\"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/" + tag + "\",\"assets\":[],\"body\":null}"};
    };
    std::string currentTag = "v";
    for (auto c : Version) currentTag.push_back(static_cast<char>(c));
    check(ClassifyReply(response(currentTag)).status == UpdateStatus::Current, "equal current revision");
    check(ClassifyReply(response("v1.2.5-holybear"), L"1.2.4.1-holybear").status == UpdateStatus::Available,
        "published 1.2.4.1 upgrades to 1.2.5");
    check(ClassifyReply(response("v1.2.4.1-holybear"), L"1.2.4-holybear").status == UpdateStatus::Available,
        "published 1.2.4 recognizes new revision tag");
    const auto revision = ClassifyReply(response("v1.2.4.1-holybear"), L"1.2.4-holybear");
    check(revision.tag == L"v1.2.4.1-holybear" &&
        std::wstring(ReleasePrefix) + revision.tag == L"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.4.1-holybear" &&
        IsReleaseUrl(std::wstring(ReleasePrefix) + revision.tag), "revision update retains the exact official v-tag URL");
    const std::pair<const char*, const char*> invalidRevisionFields[] = {
        {"\"draft\":false", "\"draft\":true"},
        {"\"draft\":false", "\"draft\":\"false\""},
        {"\"draft\":false", "\"draft\":0"},
        {"\"prerelease\":false", "\"prerelease\":true"},
        {"\"prerelease\":false", "\"prerelease\":null"},
        {"\"tag_name\":\"v1.2.4.1-holybear\"", "\"tag_name\":42"},
        {"\"tag_name\":\"v1.2.4.1-holybear\"", "\"tag_name\":null"},
        {"\"tag_name\":\"v1.2.4.1-holybear\"", "\"tag_name\":\"1.2.4.1-holybear\""},
        {"\"tag_name\":\"v1.2.4.1-holybear\"", "\"tag_name\":\"v1.2.4.1-holybear-beta\""},
        {"\"tag_name\":\"v1.2.4.1-holybear\"", "\"tag_name\":\"v1.2.4.2-holybear\""},
        {"\"html_url\":\"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.4.1-holybear\"", "\"html_url\":false"},
        {"\"html_url\":\"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.4.1-holybear\"", "\"html_url\":42"},
        {"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.4.1-holybear", "https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/1.2.4.1-holybear"},
        {"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.4.1-holybear", "https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.4.1-holybear?redirect=evil"},
        {"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.4.1-holybear", "https://github.com/CN115/nitlink/releases/tag/v1.2.4.1-holybear"}
    };
    for (const auto& [field, replacement] : invalidRevisionFields) {
        auto reply = response("v1.2.4.1-holybear");
        reply.body.replace(reply.body.find(field), std::string_view(field).size(), replacement);
        check(ClassifyReply(reply, L"1.2.4-holybear").status == UpdateStatus::Invalid,
            "revision update rejects nonformal, mismatched or incorrectly typed release data");
    }
    check(ClassifyReply(response("v1.2.6-holybear")).status == UpdateStatus::Available, "newer version");
    check(ClassifyReply(response("v1.2.3-holybear")).status == UpdateStatus::Current, "local newer version");
    check(ClassifyReply(response("v1.2.4.2-holybear"), L"1.2.4.1-holybear").status == UpdateStatus::Available, "revision API release");
    auto invalid = response("v1.2.5-holybear");
    auto pos = invalid.body.find("\"prerelease\":false"); invalid.body.replace(pos, 18, "\"prerelease\":true");
    check(ClassifyReply(invalid).status == UpdateStatus::Invalid, "prerelease rejected");
    for (auto json : {"{", "{}", "{\"tag_name\":42}", "{\"tag_name\":\"v1.2.5-holybear\",\"draft\":false,\"prerelease\":\"false\"}"})
        check(ClassifyReply({200, 0, json}).status == UpdateStatus::Invalid, "malformed or missing typed JSON fields");
    invalid = response("v1.2.5-holybear"); pos = invalid.body.find("HolyBearTW"); invalid.body.replace(pos, 10, "nitlink-dev");
    check(ClassifyReply(invalid).status == UpdateStatus::Invalid, "other repository rejected");
    check(ClassifyReply({200, 0, std::string(MaxResponseBytes + 1, 'x')}).status == UpdateStatus::Invalid, "response size bound");
    check(ClassifyReply({0, ERROR_WINHTTP_TIMEOUT, {}}).status == UpdateStatus::Offline, "timeout nonfatal");
    check(ClassifyReply({0, ERROR_WINHTTP_CANNOT_CONNECT, {}}).status == UpdateStatus::Offline, "offline nonfatal");
    check(ClassifyReply({403, 0, "{}", 0}).status == UpdateStatus::RateLimited, "403 with zero remaining is rate-limited");
    check(ClassifyReply({403, 0, "{}", 28}).status == UpdateStatus::ApiUnavailable, "403 with remaining quota is not called rate-limited");
    check(ClassifyReply({403, 0, "{}"}).status == UpdateStatus::ApiUnavailable, "403 without quota evidence is not called rate-limited");
    check(ClassifyReply({429, 0, "{}"}).status == UpdateStatus::RateLimited, "429 temporarily rate-limited");
    check(ClassifyReply({404, 0, "{}"}).status == UpdateStatus::NotFound, "404 means no release endpoint found");
    for (auto code : {500u, 502u, 503u, 599u})
        check(ClassifyReply({code, 0, "{}"}).status == UpdateStatus::ServerUnavailable, "5xx server unavailable");
    for (auto code : {301u, 302u, 400u, 401u, 418u})
        check(ClassifyReply({code, 0, "{}"}).status == UpdateStatus::ApiUnavailable, "other HTTP response nonfatal");
    check(ParseRateLimitRemaining("0") == 0, "rate limit header parses zero");
    check(ParseRateLimitRemaining(" 28\r\n") == 28, "rate limit header trims HTTP whitespace");
    for (auto value : {"", "-1", "28, 30", "4294967296", "1x"})
        check(!ParseRateLimitRemaining(value), "malformed rate limit header rejected");
    check(std::wstring_view(StatusKey(UpdateStatus::RateLimited)) == L"rateLimited" &&
        std::wstring_view(StatusKey(UpdateStatus::NotFound)) == L"notFound" &&
        std::wstring_view(StatusKey(UpdateStatus::ServerUnavailable)) == L"serverUnavailable",
        "HTTP categories map to localized UI states");
    check(!AutoCheckDue(false, false, 200000, 0), "automatic default off");
    check(AutoCheckDue(true, false, 200000, 0), "first enabled check");
    check(!AutoCheckDue(true, true, 200000, 0), "busy request deduplication");
    check(!AutoCheckDue(true, false, 186399, 100000), "under 24 hours");
    check(AutoCheckDue(true, false, 186400, 100000), "exact 24 hours");
    check(!AutoCheckDue(false, false, 300000, 100000), "disabled even when overdue");
    check(!AutoCheckDue(true, false, 1, UINT64_MAX), "clock rollback and no unsigned underflow");
    check(ReleaseApiPath == L"/repos/HolyBearTW/nitlink-holybear-edition/releases/latest", "fixed canonical Releases API endpoint");
    check(IsReleaseUrl(L"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.4.2-holybear"), "validated release destination");
    for (auto bad : {L"https://github.com/HolyBearTW/nitlink/releases/tag/v1.2.5-holybear",
            L"https://github.com/nitlink-dev/nitlink/releases/tag/v1.2.5-holybear", L"https://github.com/CN115/nitlink/releases/tag/v1.2.5-holybear",
            L"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.5-holybear?redirect=evil",
            L"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.5-holybear#fragment",
            L"https://github.com/HolyBearTW/nitlink-holybear-edition/releases/tag/v1.2.5-holybear/extra",
            L"https://github.com@evil/nitlink", L"file:///run.exe"})
        check(!IsReleaseUrl(bad), "unsafe release link rejected");
    std::atomic<unsigned> requests{0}; std::atomic<bool> release{false};
    UpdateChecker worker([&](std::stop_token stop) {
        ++requests;
        while (!release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return response("v1.2.6-holybear");
    });
    check(worker.Start(), "background start");
    check(!worker.Start(), "repeated manual request ignored while busy");
    worker.Cancel();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (worker.Busy() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto result = worker.TakeResult();
    check(!worker.Busy() && result && result->status == UpdateStatus::Cancelled && requests == 1, "disable cancels in-flight result");
    release = true; check(worker.Start(), "manual check remains usable after auto disabled");
    while (worker.Busy() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    result = worker.TakeResult();
    check(result && result->status == UpdateStatus::Available && requests == 2, "mock transport completion");
    check(!worker.TakeResult(), "one completion, no repeated notification");
    std::cout << checks - failures << '/' << checks << " HolyBear update checks passed (mock responses; no network)\n";
    return failures ? 1 : 0;
}
