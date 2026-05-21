#include "hotkey_manager.h"

namespace NitLink {

HotkeyManager::HotkeyManager(HWND hwnd) : m_hwnd(hwnd) {}
HotkeyManager::~HotkeyManager() = default;

void HotkeyManager::Register(const std::string& name, std::vector<int> keys, Callback callback)
{
    Hotkey hk;
    hk.name     = name;
    hk.keys     = std::move(keys);
    hk.callback = std::move(callback);
    m_hotkeys.push_back(std::move(hk));
}

void HotkeyManager::Unregister(const std::string& name)
{
    m_hotkeys.erase(
        std::remove_if(m_hotkeys.begin(), m_hotkeys.end(),
            [&](const Hotkey& h) { return h.name == name; }),
        m_hotkeys.end()
    );
}

void HotkeyManager::Poll()
{
    // Only fire hotkeys when the app is in the foreground. With child-window
    // WebView2, focus on the menu still resolves to main's HWND, so a single
    // foreground check is sufficient.
    HWND fg = GetForegroundWindow();
    if (fg != m_hwnd) return;

    // Don't fire hotkeys that conflict with text-input contexts inside
    // the WebView2 popup. When the popup has focus only the small set of
    // hotkeys that are safe to intercept (toggle, HDR, etc) are allowed.
    // For now the registered hotkeys are assumed non-conflicting
    // (F1, Alt+H, Ctrl+Shift+G: none are bare letters).

    for (auto& hk : m_hotkeys) {
        bool allPressed = true;
        for (int key : hk.keys) {
            if (!(GetAsyncKeyState(key) & 0x8000)) {
                allPressed = false;
                break;
            }
        }

        if (allPressed && !hk.wasPressed) {
            // Key combo just pressed (edge trigger, not held)
            hk.callback();
        }
        hk.wasPressed = allPressed;
    }
}

} // namespace NitLink
