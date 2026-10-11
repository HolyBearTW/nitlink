#include "app/hdmi_source_display.h"
#include "app/localization.h"
#include "discord/discord_rpc.h"
#include "discord/pipe_transport.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace NitLink {
struct DiscordRPCTestAccess {
    static void SetOpener(DiscordRPC& rpc, std::function<HANDLE()> opener) {
        rpc.m_pipeOpener = std::move(opener);
    }
};
}

using namespace NitLink;
namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    Check(length > 0, "valid UTF-16 test expectation");
    std::string result(length, '\0');
    Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr) == length,
        "UTF-8 test expectation conversion");
    return result;
}

struct Pipe {
    HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    Pipe() {
        static unsigned sequence = 0;
        const auto name = L"\\\\.\\pipe\\NitLink-presence-test-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
        server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, nullptr);
        Check(server != INVALID_HANDLE_VALUE, "create isolated presence pipe");
        client = OpenRpcPipe(name.c_str());
        Check(client != INVALID_HANDLE_VALUE, "open isolated presence pipe");
        Check(ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED,
            "connect isolated presence pipe");
        const char ready[] = "{}";
        const uint32_t header[] = {1, 2};
        DWORD written = 0;
        Check(WriteFile(server, header, sizeof(header), &written, nullptr) && written == sizeof(header),
            "write handshake reply header");
        Check(WriteFile(server, ready, 2, &written, nullptr) && written == 2, "write handshake reply");
    }
    ~Pipe() {
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    }
    std::string Receive(uint32_t opcode) {
        const auto deadline = GetTickCount64() + 8000;
        const auto wait = [&](DWORD size) {
            DWORD available = 0;
            while (GetTickCount64() < deadline) {
                if (PeekNamedPipe(server, nullptr, 0, nullptr, &available, nullptr) && available >= size)
                    return;
                Sleep(5);
            }
            throw std::runtime_error("bounded presence frame wait");
        };
        wait(8);
        uint32_t header[2]{};
        DWORD got = 0;
        Check(ReadFile(server, header, sizeof(header), &got, nullptr) && got == sizeof(header) &&
            header[0] == opcode && header[1] <= 65536, "presence frame header");
        wait(header[1]);
        std::string payload(header[1], '\0');
        Check(ReadFile(server, payload.data(), header[1], &got, nullptr) && got == header[1],
            "presence frame body");
        return payload;
    }
};

void CheckField(const std::string& payload, const char* key, std::wstring_view expected) {
    const auto field = std::string("\"") + key + "\"";
    if (expected.empty()) Check(payload.find(field) == std::string::npos, "unavailable field is omitted");
    else Check(payload.find(field + ":\"" + Utf8(expected) + "\"") != std::string::npos,
        "wire JSON has the exact expected activity field");
}

struct Fixture {
    std::wstring_view source, signal, range;
    HdmiSourceVrrState vrr;
    std::wstring_view expectedSignal;
    bool shortSource;
};

void TestLocalizedWirePresence() {
    const auto signal = FormatHdmiSourceTiming({1920, 1080, 6000, true});
    using V = HdmiSourceVrrState;
    const Fixture fixtures[] = {
        {L"熊", signal, L"HDR", V::Enabled, L"1920x1080 @ 60Hz · HDR · VRR", true},
        {L"熊", {}, {}, V::Unknown, {}, true},
        {L"熊", signal, L"SDR", V::Disabled, L"1920x1080 @ 60Hz · SDR", true},
        {L"熊", signal, {}, V::Unknown, L"1920x1080 @ 60Hz", true},
        {L"熊", {}, L"HDR", V::Unknown, L"HDR", true},
        {L"熊", {}, {}, V::Enabled, L"VRR", true},
        {L"\xd83c\xdfae", {}, {}, V::Unknown, {}, true},
        {L"Switch 2", signal, L"HDR", V::Enabled, L"1920x1080 @ 60Hz · HDR · VRR", false},
        {L"Steam Deck", signal, L"SDR", V::Disabled, L"1920x1080 @ 60Hz · SDR", false},
        {L"", signal, L"HDR", V::Unknown, L"1920x1080 @ 60Hz · HDR", false},
        {L"　 ", {}, {}, V::Unknown, {}, false},
    };
    const std::pair<const char*, std::wstring_view> languages[] = {
        {"en-US", L"Source: "}, {"zh-TW", L"來源："}, {"zh-CN", L"来源："}
    };
    const auto started = std::chrono::system_clock::time_point{std::chrono::seconds(1700000000)};
    unsigned frames = 0;
    for (const auto& [language, expectedPrefix] : languages) {
        Localization::Instance().SetPreference(std::string(language));
        const auto prefix = Localization::Instance().Get(L"discord.sourcePrefix");
        Check(prefix == expectedPrefix, "native language selects the exact source prefix");
        for (const auto& fixture : fixtures) for (bool selectedGame : {false, true}) {
            const auto identity = TrimManualHdmiSourceCustom(fixture.source);
            const auto expectedSource = fixture.shortSource ? std::wstring(expectedPrefix) +
                std::wstring(identity) : std::wstring(identity);
            const auto details = selectedGame ? L"Spider-Man 2" : ComposeHdmiPresenceDetails(fixture.source, prefix);
            const auto state = selectedGame ? ComposeHdmiPresenceState(fixture.source, fixture.signal,
                fixture.range, fixture.vrr, prefix) : ComposeHdmiPresenceSignal(fixture.signal, fixture.range, fixture.vrr);
            const std::wstring expectedDetails = selectedGame ? L"Spider-Man 2" : expectedSource;
            std::wstring expectedState(fixture.expectedSignal);
            if (selectedGame) {
                if (expectedState.empty()) expectedState = expectedSource;
                else if (!identity.empty()) expectedState = std::wstring(identity) + L" · " + expectedState;
            }
            Check(details == expectedDetails && state == expectedState,
                "game/viewer composition matches the fixture without invented metadata");
            Check((details.empty() || HasMinimumDiscordActivityCharacters(details)) &&
                (state.empty() || HasMinimumDiscordActivityCharacters(state)), "composed fields meet Unicode minimum");

            Pipe pipe;
            DiscordRPC rpc;
            DiscordRPCTestAccess::SetOpener(rpc, [&] { return std::exchange(pipe.client, INVALID_HANDLE_VALUE); });
            Check(rpc.Connect("12345"), "isolated Discord handshake");
            pipe.Receive(0);
            rpc.SetActivity(details, state, started, selectedGame ? "spider-man-2" : "nitlink-logo",
                selectedGame ? L"Spider-Man 2" : L"NitLink");
            const auto payload = pipe.Receive(1);
            Check(payload.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos, "real RPC activity command");
            CheckField(payload, "details", expectedDetails);
            CheckField(payload, "state", expectedState);
            CheckField(payload, "large_text", selectedGame ? L"Spider-Man 2" : L"NitLink");
            Check(payload.find(selectedGame ? "\"large_image\":\"spider-man-2\"" :
                "\"large_image\":\"nitlink-logo\"") != std::string::npos &&
                payload.find("\"start\":1700000000") != std::string::npos, "game/viewer art and timestamp retained");
            if (!fixture.expectedSignal.empty()) {
                const auto metadata = Utf8(fixture.expectedSignal);
                const auto first = payload.find(metadata);
                Check(first != std::string::npos && payload.find(metadata, first + metadata.size()) == std::string::npos,
                    "actual serialized RPC contains signal metadata exactly once");
            }
            if (!selectedGame && fixture.source == L"熊" &&
                (fixture.expectedSignal.empty() || fixture.vrr == V::Enabled))
                std::cout << "Isolated " << language << " fixture RPC: " << payload << '\n';
            rpc.Disconnect();
            ++frames;
        }
    }
    std::cout << frames << " serialized activity fixtures passed across three languages and both presence modes\n";
}

void TestInvalidAndShortFields() {
    for (const auto prefix : {L"Source: ", L"來源：", L"来源："}) {
        Check(ComposeHdmiPresenceDetails(L"\xd800", prefix).empty() &&
            ComposeHdmiPresenceDetails(L"AB\xdc00", prefix).empty(), "prefix cannot rescue malformed UTF-16");
        Check(ComposeHdmiPresenceDetails(L"熊貓", prefix) == L"熊貓" &&
            ComposeHdmiPresenceDetails(L"\xd83c\xdfae熊", prefix) == L"\xd83c\xdfae熊",
            "two Unicode scalars stay unchanged without a prefix");
        Check(ComposeHdmiPresenceDetails(L"", prefix).empty() &&
            ComposeHdmiPresenceDetails(L"　 ", prefix).empty(), "blank identity never becomes a source prefix alone");
    }
    Check(!HasMinimumDiscordActivityCharacters(L"熊") &&
        !HasMinimumDiscordActivityCharacters(L"\xd83c\xdfae") &&
        HasMinimumDiscordActivityCharacters(L"熊貓"), "Unicode minimum counts scalars, not UTF-16 units");
}
}

int main() {
    try {
        TestInvalidAndShortFields();
        TestLocalizedWirePresence();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL Discord presence: " << error.what() << '\n';
        return 1;
    }
}
