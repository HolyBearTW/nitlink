#pragma once
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <functional>

// WebViewSettings: host a WebView2 control as a CHILD of the main window,
// occupying the full client rect when visible. When the menu opens, the
// render loop skips painting the swap chain and WebView2 owns the client
// area alone (PS5 Control Center-style: opening settings pauses the
// game view).
class WebViewSettings {
public:
    using MessageCallback = std::function<void(const std::wstring&)>;

    // Initialize attaches WebView2 to `parent` (the main app HWND).
    // Width/height are the initial parent client size; the controller
    // bounds track parent size automatically via Resize().
    bool Initialize(HWND parent, int width, int height);

    // Show/hide the overlay. Setting visible=true makes the WebView2 fill
    // the entire parent client area and become input-active. Setting
    // visible=false hides it immediately and returns focus to the parent.
    void Show(bool visible);
    bool IsVisible() const { return m_visible; }

    // Resize the WebView2 to match parent client dimensions (read via
    // GetClientRect). Called from the application loop on window resize.
    void Resize();

    void NavigateToFile(const std::wstring& path);
    void NavigateToString(const std::wstring& html);
    void PostMessage(const std::wstring& json);
    void SetMessageHandler(MessageCallback cb) { m_onMessage = cb; }
    void Shutdown();

private:
    HWND m_parent  = nullptr;
    bool m_visible = false;

    wil::com_ptr<ICoreWebView2Controller> m_controller;
    wil::com_ptr<ICoreWebView2> m_webview;
    // If NavigateToFile is called before WebView2 finishes its async
    // init (m_webview still null), the URI is saved here and the
    // controller-ready callback replays it. Cleared once consumed.
    std::wstring m_pendingNavigateUri;
    // Token from add_WebMessageReceived so that Shutdown() can unregister
    // cleanly. value==0 means "not yet registered."
    EventRegistrationToken m_messageToken = {};
    MessageCallback m_onMessage;
};
