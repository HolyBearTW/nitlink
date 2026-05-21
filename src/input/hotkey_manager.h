#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace NitLink {

class HotkeyManager {
public:
    using Callback = std::function<void()>;

    HotkeyManager(HWND hwnd);
    ~HotkeyManager();

    void Register(const std::string& name, std::vector<int> keys, Callback callback);
    void Unregister(const std::string& name);
    void Poll();

private:
    struct Hotkey {
        std::string      name;
        std::vector<int> keys;
        Callback         callback;
        bool             wasPressed = false;
    };

    HWND m_hwnd;
    std::vector<Hotkey> m_hotkeys;
};

} // namespace NitLink
