#include "app/config.h"
#include "renderer/window.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using NitLink::Config;
using NitLink::Window;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void ResizeClient(HWND window, uint32_t width, uint32_t height)
{
    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    Require(AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)),
                              FALSE, static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE))) != FALSE,
            "Window frame dimensions are available");
    Require(SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
            "Client resize succeeds");
}

enum class CloseState { Normal, Minimized, Maximized, Fullscreen, PiP, FullscreenRoundTrip, PiPRoundTrip };

static void CheckClose(CloseState state, const std::filesystem::path& configPath)
{
    constexpr std::pair<uint32_t, uint32_t> expected{1000, 600};
    const auto instance = GetModuleHandleW(nullptr);
    {
        Window window;
        Window::Desc desc;
        desc.width = 640;
        desc.height = 360;
        Require(window.Create(instance, desc), "Window creates");
        ResizeClient(window.GetHWND(), 800, 500);
        ResizeClient(window.GetHWND(), expected.first, expected.second);
        Require(window.GetClientSize() == expected, "Resize produces the requested client dimensions");

        switch (state) {
        case CloseState::Minimized:
            ShowWindow(window.GetHWND(), SW_MINIMIZE);
            Require(IsIconic(window.GetHWND()) != FALSE, "Window minimizes");
            break;
        case CloseState::Maximized:
            ShowWindow(window.GetHWND(), SW_MAXIMIZE);
            Require(IsZoomed(window.GetHWND()) != FALSE, "Window maximizes");
            break;
        case CloseState::Fullscreen:
        case CloseState::FullscreenRoundTrip:
            window.SetFullscreen(true);
            if (state == CloseState::FullscreenRoundTrip) window.SetFullscreen(false);
            break;
        case CloseState::PiP:
        case CloseState::PiPRoundTrip:
            window.SetPiP(true, 480, 270, 1.0f);
            if (state == CloseState::PiPRoundTrip) window.SetPiP(false, 0, 0, 1.0f);
            break;
        default:
            break;
        }

        const HWND handle = window.GetHWND();
        SendMessageW(handle, WM_CLOSE, 0, 0);
        Require(!IsWindow(handle), "Close destroys the native window before saving");
        Require(window.GetClientSize() == std::pair<uint32_t, uint32_t>{0, 0},
                "The destroyed window no longer supplies client dimensions");
        Require(window.GetWindowedClientSize() == expected, "The last normal client size survives close");
        Config config;
        const auto [width, height] = window.GetWindowedClientSize();
        config.windowWidth = width;
        config.windowHeight = height;
        Require(config.Save(configPath.string()), "Window dimensions save");
    }
    Require(UnregisterClassW(L"NitLinkWindowClass", instance) != FALSE, "Closed window class releases");

    Config loaded;
    Require(loaded.Load(configPath.string()), "Saved dimensions reload");
    {
        Window reopened;
        Window::Desc desc;
        desc.width = loaded.windowWidth;
        desc.height = loaded.windowHeight;
        Require(reopened.Create(instance, desc), "Window reopens from saved dimensions");
        Require(reopened.GetClientSize() == expected, "Reopened client dimensions match the last normal size");
        SendMessageW(reopened.GetHWND(), WM_CLOSE, 0, 0);
    }
    Require(UnregisterClassW(L"NitLinkWindowClass", instance) != FALSE, "Reopened window class releases");
    MSG message{};
    while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
}

int main()
{
    // Fullscreen and PiP transitions run on an inactive desktop so checks never cover the user's screen.
    // The test process retains the active desktop handle until process exit releases it.
    const std::wstring name = L"NitLinkWindowSizeTests-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) {
        std::cerr << "FAIL Could not create an isolated test desktop\n";
        return 1;
    }
    int result = 0;
    std::filesystem::path configPath;
    try {
        configPath = std::filesystem::temp_directory_path() / (name + L".ini");
        for (auto state : {CloseState::Normal, CloseState::Minimized, CloseState::Maximized,
                           CloseState::Fullscreen, CloseState::PiP,
                           CloseState::FullscreenRoundTrip, CloseState::PiPRoundTrip}) {
            CheckClose(state, configPath);
        }
        std::cout << "PASS Window size survives resize, close and reopen in all seven window states\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        result = 1;
    }
    std::error_code ignored;
    if (!configPath.empty()) std::filesystem::remove(configPath, ignored);
    return result;
}
