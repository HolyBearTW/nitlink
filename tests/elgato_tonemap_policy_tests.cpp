#include "capture/elgato_tonemap_policy.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace NitLink;

int main()
{
    try {
        auto require = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        std::vector<bool> commands;
        const auto send = [&](const std::wstring& name, bool enabled) {
            require(IsElgato4KX(name), "only selected 4K X reaches property sender");
            commands.push_back(enabled);
            return true;
        };
        for (const auto* name : {L"Elgato 4K X", L"elgato 4k x", L"ELGATO 4K X"}) {
            commands.clear();
            // Initialize: desired P010, successful Open actually delivers NV12.
            require(Apply4KXTonemapForCapture(name, true, send).value(), "P010 attempt sent");
            require(Apply4KXTonemapForCapture(name, false, send).value(), "actual NV12 sent");
            require(commands == std::vector<bool>({false, true}), "silent fallback enables SDR tone mapping");
            commands.clear();
            // Failed P010 attempt -> explicit SDR retry -> actual SDR readback.
            Apply4KXTonemapForCapture(name, true, send);
            Apply4KXTonemapForCapture(name, false, send);
            Apply4KXTonemapForCapture(name, false, send);
            require(commands == std::vector<bool>({false, true, true}), "retry retains clean SDR");
            commands.clear();
            // Reconcile/reopen: actual P010 -> NV12 -> actual P010.
            for (bool p010 : {true, true, false, false, true, true})
                Apply4KXTonemapForCapture(name, p010, send);
            require(commands == std::vector<bool>({false, false, true, true, false, false}),
                    "reopen follows actual P010 passthrough and SDR transitions");
            require(!Apply4KXTonemapForCapture(name, true,
                [](const auto&, bool) { return false; }).value(), "failed command remains unconfirmed");
        }
        commands.clear();
        for (const auto* name : {L"AVerMedia GC553Pro", L"Live Gamer ULTRA S GC553Pro",
                 L"Elgato 4K S", L"Elgato 4K Pro", L"Game Capture 4K60 Pro MK.2",
                 L"USB 4K X", L"Webcam", L""}) {
            for (bool p010 : {false, true})
                require(!Apply4KXTonemapForCapture(name, p010, send).has_value(),
                        "other devices never receive 4K X commands");
        }
        require(commands.empty(), "GC553Pro/4K S/4K Pro isolation");
        std::cout << "PASS 4K X identity, negotiated SDR/P010, retry/reopen, failure and device isolation (no hardware I/O)\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
