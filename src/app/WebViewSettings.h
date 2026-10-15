#pragma once
#include "message_budget.h"
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <functional>
#include <memory>
#include <map>
#include <vector>
#include <array>

// WebViewSettings: host a WebView2 control as a CHILD of the main window,
// occupying the full client rect when visible. When the menu opens, the
// render loop skips painting the swap chain and WebView2 owns the client
// area alone (PS5 Control Center-style: opening settings pauses the
// game view).
class WebViewSettings {
public:
    using MessageCallback = std::function<void(const std::wstring&)>;
    WebViewSettings() = default;
    ~WebViewSettings() { Shutdown(); }
    WebViewSettings(const WebViewSettings&) = delete;
    WebViewSettings& operator=(const WebViewSettings&) = delete;

    // Initialize attaches WebView2 to `parent` (the main app HWND).
    // Width/height are the initial parent client size; the controller
    // bounds track parent size automatically via Resize().
    bool Initialize(HWND parent, int width, int height);

    // Show/hide the overlay. Setting visible=true makes the WebView2 fill
    // the entire parent client area and become input-active. Setting
    // visible=false hides it immediately and returns focus to the parent.
    void Show(bool visible);
    bool IsVisible() const { return m_visible; }
    bool IsReady() const { return m_ready; }
    bool IsStarting() const { return m_starting; }
    void CheckHealth();
    // Returns the localization key for the current failure, or an empty string.
    const std::wstring& InitializationError() const { return m_initializationError; }
    void SetFailureHandler(MessageCallback cb) { m_onFailure = std::move(cb); }

    // Resize the WebView2 to match parent client dimensions (read via
    // GetClientRect). Called from the application loop on window resize.
    void Resize();

    // Panel placement. Right and Left size the control as a strip of
    // widthDip device-independent pixels beside the picture; Full covers
    // the whole client area. Takes effect on the next Show or Resize, or
    // immediately while the panel is visible.
    enum class Dock { Full, Right, Left };
    void SetDock(Dock dock, int widthDip);
    Dock GetDock() const { return m_dock; }

    // Lets the page draw translucent surfaces over the picture. Keeps the
    // opaque default when the installed runtime predates the interface.
    void SetTransparentBackground(bool on);

    void NavigateToMenu();
    void PostMessage(const std::wstring& json);
    void DispatchPendingMessages(bool closing = false);
    void SetMessageHandler(MessageCallback cb) { m_onMessage = cb; }
    void Shutdown();

private:
    friend struct WebViewSettingsTestAccess;
    HRESULT ConfigureSecurity(ICoreWebView2Environment* environment);
    HRESULT LoadResources();
    void Fail(const std::wstring& reason, HRESULT hr);
    bool Start(HWND parent);
    bool m_ready = false;
    bool m_starting = false;
    uint64_t m_startTime = 0;
    uint64_t m_restartAt = 0;
    unsigned m_restartAttempts = 0;
    UINT64 m_menuNavigationId = 0;
    std::wstring m_initializationError;
    MessageCallback m_onFailure;
    bool IsTrustedDocument() const;
    std::map<std::wstring, std::vector<BYTE>> m_resources;
    // Host methods and WebView callbacks run on the UI thread. Invalidating
    // this token prevents callbacks from an old initialization using this.
    std::shared_ptr<int> m_callbackLifetime;
    HWND m_parent  = nullptr;
    bool m_visible = false;
    void HideBrowserSurfaces();
    void ShowBrowserSurfaces();
    void RefreshComposition();
    std::vector<HWND> m_hiddenSurfaces;
    Dock m_dock = Dock::Full;
    int  m_widthDip = 420;
    bool m_transparent = false;

    // Bounds for the current dock mode in parent client coordinates.
    RECT ComputeBounds() const;
    void ApplyBackground();

    wil::com_ptr<ICoreWebView2Controller> m_controller;
    wil::com_ptr<ICoreWebView2> m_webview;
    bool m_pendingNavigate = false;
    // Token from add_WebMessageReceived so that Shutdown() can unregister
    // cleanly. value==0 means "not yet registered."
    EventRegistrationToken m_messageToken = {};
    MessageCallback m_onMessage;
    NitLink::MessageBudget m_messageBudget;
    NitLink::PendingSliderMessages m_pendingSliders;
    uint64_t m_lastBudgetLog = 0;
    uint64_t m_budgetDrops = 0, m_sliderReplacements = 0, m_sliderDeferred = 0;
    void ReceiveMessage(const std::wstring& json);
    void LogBudgetStats(bool force = false);
};
