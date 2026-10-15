// WebViewSettings: child-window WebView2 overlay.
//
// Attaches to the main window's HWND directly. Show(true) puts WebView2
// at full client-rect size and makes it visible; Show(false) hides it.
// No popup, no tracking, no separate window class: the fullscreen-when-
// open UX matches what console settings menus do.
//
// The application is responsible for skipping its render-loop EndFrame
// while the menu is visible, otherwise the swap chain repaints over the
// WebView2 child every frame. That's done in Application::Run.

#include "WebViewSettings.h"
#include "webview_policy.h"
#include "settings_message.h"
#include "webview_lifecycle.h"
#include <wil/result.h>
#include <shlwapi.h>
#include <filesystem>
#include <fstream>

#include <wrl.h>
#include <WebView2.h>
#include <wil/com.h>
#include <Shlobj.h>
#include <shellapi.h>   // ShellExecuteW: for opening external links in default browser
#include <sstream>
#include <string>
#include <wchar.h>
#include <windows.h>

using namespace Microsoft::WRL;

namespace {
void WVLog(const std::wstring& msg) {
    std::wstring line = L"[NitLink/WebView2] " + msg + L"\n";
    OutputDebugStringW(line.c_str());
}
void WVLogHr(const std::wstring& prefix, HRESULT hr) {
    std::wstringstream ss;
    ss << prefix << L" hr=0x" << std::hex << hr;
    WVLog(ss.str());
}
} // namespace

HRESULT WebViewSettings::LoadResources() {
    // Fixed paths relative to the executable avoid CWD substitution and URI decoding.
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return E_FAIL;
    module.resize(length);
    const auto folder = std::filesystem::path(module).parent_path();
    m_resources.clear();
    for (const auto& resource : NitLink::WebViewPolicy::Resources) {
        std::ifstream input(folder / resource.path, std::ios::binary | std::ios::ate);
        if (!input) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        const auto size = input.tellg();
        if (size <= 0 || size > 4 * 1024 * 1024) return E_FAIL;
        std::vector<BYTE> bytes(static_cast<size_t>(size));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) return E_FAIL;
        m_resources.emplace(resource.uri, std::move(bytes));
    }
    return S_OK;
}

HRESULT WebViewSettings::ConfigureSecurity(ICoreWebView2Environment* environment) {
    using namespace NitLink::WebViewPolicy;
    const std::weak_ptr<int> lifetime = m_callbackLifetime;
    wil::com_ptr<ICoreWebView2Settings> settings;
    RETURN_IF_FAILED(m_webview->get_Settings(&settings));
    RETURN_IF_FAILED(settings->put_IsScriptEnabled(TRUE));
    RETURN_IF_FAILED(settings->put_IsWebMessageEnabled(TRUE));
    RETURN_IF_FAILED(settings->put_AreHostObjectsAllowed(FALSE));
    RETURN_IF_FAILED(settings->put_AreDefaultScriptDialogsEnabled(FALSE));
    RETURN_IF_FAILED(settings->put_AreDefaultContextMenusEnabled(FALSE));
    RETURN_IF_FAILED(settings->put_AreDevToolsEnabled(FALSE));
    RETURN_IF_FAILED(settings->put_IsStatusBarEnabled(FALSE));
    RETURN_IF_FAILED(settings->put_IsZoomControlEnabled(FALSE));
    RETURN_IF_FAILED(settings->put_IsBuiltInErrorPageEnabled(FALSE));
    const auto settings3 = settings.try_query<ICoreWebView2Settings3>();
    const auto settings4 = settings.try_query<ICoreWebView2Settings4>();
    const auto controller4 = m_controller.try_query<ICoreWebView2Controller4>();
    const auto webview4 = m_webview.try_query<ICoreWebView2_4>();
    const auto webview22 = m_webview.try_query<ICoreWebView2_22>();
    if (!settings3 || !settings4 || !controller4 || !webview4 || !webview22) return E_NOINTERFACE;
    RETURN_IF_FAILED(settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE));
    RETURN_IF_FAILED(settings4->put_IsPasswordAutosaveEnabled(FALSE));
    RETURN_IF_FAILED(settings4->put_IsGeneralAutofillEnabled(FALSE));
    RETURN_IF_FAILED(controller4->put_AllowExternalDrop(FALSE));

    EventRegistrationToken token{};
    RETURN_IF_FAILED(m_webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this, lifetime](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string uri;
                BOOL redirected = TRUE;
                const bool allowed = !lifetime.expired() && SUCCEEDED(args->get_Uri(&uri)) && uri &&
                    SUCCEEDED(args->get_IsRedirected(&redirected)) && !redirected &&
                    NitLink::WebViewPolicy::IsTrustedDocument(uri.get());
                if (allowed && FAILED(args->get_NavigationId(&m_menuNavigationId)))
                    return args->put_Cancel(TRUE);
                return args->put_Cancel(allowed ? FALSE : TRUE);
            }).Get(), &token));
    RETURN_IF_FAILED(m_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this, lifetime](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (lifetime.expired()) return S_OK;
                UINT64 navigationId = 0;
                BOOL success = FALSE;
                if (FAILED(args->get_NavigationId(&navigationId)) || navigationId != m_menuNavigationId)
                    return S_OK;
                if (FAILED(args->get_IsSuccess(&success)) || !success) {
                    COREWEBVIEW2_WEB_ERROR_STATUS status{};
                    args->get_WebErrorStatus(&status);
                    WVLog(L"menu NavigationCompleted failed, status=" + std::to_wstring(status));
                    Fail(L"toast.settingsNavigation", E_FAIL);
                }
                return S_OK;
            }).Get(), &token));
    RETURN_IF_FAILED(m_webview->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this, lifetime](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                if (lifetime.expired()) return S_OK;
                COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                if (FAILED(args->get_ProcessFailedKind(&kind))) return S_OK;
                WVLog(L"WebView process event, kind=" + std::to_wstring(kind));
                if (NitLink::SettingsProcessNeedsRestart(kind))
                    Fail(L"toast.settingsProcessFailed", E_FAIL);
                return S_OK;
            }).Get(), &token));
    RETURN_IF_FAILED(m_webview->add_FrameNavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                return args->put_Cancel(TRUE);
            }).Get(), &token));
    RETURN_IF_FAILED(m_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this, lifetime](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (lifetime.expired()) return S_OK;
                wil::unique_cotaskmem_string source, current, json;
                if (FAILED(args->get_Source(&source)) || !source ||
                    FAILED(m_webview->get_Source(&current)) || !current ||
                    !CanReceiveMessage(source.get(), current.get())) return S_OK;
                RETURN_IF_FAILED(args->get_WebMessageAsJson(&json));
                if (json && wcsnlen_s(json.get(), 8193) <= 8192)
                    ReceiveMessage(json.get());
                return S_OK;
            }).Get(), &m_messageToken));
    RETURN_IF_FAILED(m_webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this, lifetime](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                RETURN_IF_FAILED(args->put_Handled(TRUE));
                if (lifetime.expired()) return S_OK;
                wil::unique_cotaskmem_string uri, current;
                BOOL userInitiated = FALSE;
                if (FAILED(args->get_Uri(&uri)) || !uri ||
                    FAILED(args->get_IsUserInitiated(&userInitiated)) ||
                    FAILED(m_webview->get_Source(&current)) || !current ||
                    !CanOpenExternal(current.get(), uri.get(), userInitiated != FALSE)) return S_OK;
                const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(m_parent, L"open",
                    uri.get(), nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32) WVLog(L"external link could not be opened");
                return S_OK;
            }).Get(), &token));
    RETURN_IF_FAILED(m_webview->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            }).Get(), &token));
    RETURN_IF_FAILED(webview4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                RETURN_IF_FAILED(args->put_Cancel(TRUE));
                return args->put_Handled(TRUE);
            }).Get(), &token));

    // Every request receives a local response, including denied URLs. No virtual
    // folder mapping exposes the rest of the installation to the renderer.
    RETURN_IF_FAILED(m_webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this, lifetime, env = ComPtr<ICoreWebView2Environment>(environment)](
                ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (lifetime.expired()) return E_ABORT;
                wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                wil::unique_cotaskmem_string uri, method;
                COREWEBVIEW2_WEB_RESOURCE_CONTEXT context = COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL;
                const Resource* resource = nullptr;
                if (SUCCEEDED(args->get_Request(&request)) && request &&
                    SUCCEEDED(request->get_Uri(&uri)) && uri &&
                    SUCCEEDED(request->get_Method(&method)) && method &&
                    std::wstring_view(method.get()) == L"GET" &&
                    SUCCEEDED(args->get_ResourceContext(&context))) resource = FindResource(uri.get());
                if (resource) {
                    const auto expected = resource->kind == ResourceKind::Document ? COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT :
                        resource->kind == ResourceKind::Script ? COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT :
                        resource->kind == ResourceKind::Font ? COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT : COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE;
                    if (context != expected) resource = nullptr;
                }
                wil::com_ptr<IStream> stream;
                std::wstring headers = L"Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
                if (resource) {
                    const auto it = m_resources.find(std::wstring(resource->uri));
                    if (it == m_resources.end()) resource = nullptr;
                    else {
                        stream.attach(SHCreateMemStream(it->second.data(), static_cast<UINT>(it->second.size())));
                        if (!stream) {
                            Fail(L"toast.settingsMemory", E_OUTOFMEMORY);
                            return E_OUTOFMEMORY;
                        }
                        headers += L"Content-Type: " + std::wstring(resource->contentType) + L"\r\n";
                        headers += L"Content-Security-Policy: " + std::wstring(ContentSecurityPolicy) + L"\r\n";
                    }
                }
                wil::com_ptr<ICoreWebView2WebResourceResponse> response;
                HRESULT hr = env->CreateWebResourceResponse(stream.get(), resource ? 200 : 403,
                    resource ? L"OK" : L"Forbidden", headers.c_str(), &response);
                if (SUCCEEDED(hr)) hr = args->put_Response(response.get());
                if (FAILED(hr)) {
                    Fail(L"toast.settingsResources", hr);
                }
                return hr;
            }).Get(), &token));
    RETURN_IF_FAILED(webview22->AddWebResourceRequestedFilterWithRequestSourceKinds(L"*",
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL));
    return S_OK;
}

void WebViewSettings::Fail(const std::wstring& reason, HRESULT hr) {
    WVLogHr(reason, hr);
    Shutdown();
    m_initializationError = reason;
    if (m_restartAttempts < NitLink::kSettingsRestartLimit) {
        ++m_restartAttempts;
        m_starting = true;
        m_restartAt = GetTickCount64() + 1000;
        WVLog(L"settings restart scheduled, attempt=" + std::to_wstring(m_restartAttempts));
    }
    if (m_onFailure) m_onFailure(reason);
}

bool WebViewSettings::Initialize(HWND parent, int /*width*/, int /*height*/) {
    m_restartAttempts = 0;
    return Start(parent);
}

bool WebViewSettings::Start(HWND parent) {
    Shutdown();
    m_initializationError.clear();
    m_starting = true;
    m_startTime = 0;
    m_callbackLifetime = std::make_shared<int>(0);
    const std::weak_ptr<int> lifetime = m_callbackLifetime;
    m_parent = parent;

    // WebView2 user-data folder under %LOCALAPPDATA%\NitLink\WebView2.
    // Using a dedicated path silences the runtime warning about default
    // location and isolates this app's cookies/storage from other
    // WebView2 hosts on the system.
    wil::unique_cotaskmem_string localAppData;
    const HRESULT folderHr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (FAILED(folderHr)) {
        Fail(L"toast.settingsProfile", folderHr);
        return false;
    }
    std::wstring userDataFolder = std::wstring(localAppData.get()) + L"\\NitLink\\WebView2";

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, lifetime](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT {
                if (lifetime.expired()) return S_OK;
                if (FAILED(envHr) || !env) {
                    Fail(L"toast.settingsRuntime", envHr);
                    return envHr;
                }
                HRESULT hr2 = env->CreateCoreWebView2Controller(m_parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, lifetime, environment = ComPtr<ICoreWebView2Environment>(env)](HRESULT ctlHr, ICoreWebView2Controller* controller) -> HRESULT {
                            if (lifetime.expired()) {
                                if (controller) controller->Close();
                                return S_OK;
                            }
                            if (FAILED(ctlHr) || !controller) {
                                Fail(L"toast.settingsStartFailed", ctlHr);
                                return ctlHr;
                            }
                            m_controller = controller;
                            HRESULT setupHr = controller->get_CoreWebView2(&m_webview);
                            if (FAILED(setupHr)) {
                                Fail(L"toast.settingsStartFailed", setupHr);
                                return setupHr;
                            }
                            setupHr = LoadResources();
                            if (FAILED(setupHr)) {
                                Fail(L"toast.settingsFiles", setupHr);
                                return setupHr;
                            }
                            setupHr = ConfigureSecurity(environment.Get());
                            if (FAILED(setupHr)) {
                                Fail(L"toast.settingsSecurity", setupHr);
                                return setupHr;
                            }

                            // Size to current parent client area.
                            m_controller->put_Bounds(ComputeBounds());
                            ApplyBackground();

                            // The page must acknowledge readiness before any panel opens.
                            m_controller->put_IsVisible(FALSE);

                            if (m_pendingNavigate) {
                                m_pendingNavigate = false;
                                NavigateToMenu();
                            }

                            WVLog(L"WebView2 controller secured; waiting for menu ready");
                            return S_OK;
                        }).Get());
                if (FAILED(hr2)) Fail(L"toast.settingsStartFailed", hr2);
                return hr2;
            }).Get());

    if (FAILED(hr)) {
        Fail(L"toast.settingsRuntime", hr);
        return false;
    }
    return true;
}

void WebViewSettings::Show(bool visible) {
    m_visible = visible && m_ready;
    if (!m_controller) return;
    visible = m_visible;
    if (visible) {
        // Resize to current parent client bounds in case the parent grew
        // since last show: DXGI swap chain may have resized but the
        // controller wouldn't have been told.
        m_controller->put_Bounds(ComputeBounds());
        ShowBrowserSurfaces();
        m_controller->put_IsVisible(TRUE);
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        WVLog(L"Show(true): overlay visible");
    } else {
        m_controller->put_IsVisible(FALSE);
        // Collapse the hidden control to an empty rectangle. The browser's
        // child window belongs to another process, so its hide reaches the
        // desktop compositor later than our own frames do; in a window on the
        // desktop that could leave the panel's last picture standing over
        // the video until the mouse disturbed it. An empty window has no
        // picture to leave behind. Show(true) restores the bounds first.
        m_controller->put_Bounds(RECT{0, 0, 0, 0});
        HideBrowserSurfaces();
        RefreshComposition();
        // Return keyboard focus to the parent so hotkeys + key events
        // (Esc, F1, etc.) continue to be processed by the main window.
        SetFocus(m_parent);
        WVLog(L"Show(false): overlay hidden");
    }
}

void WebViewSettings::Resize() {
    // A hidden control stays collapsed; Show(true) sizes it to the window.
    if (!m_controller || !m_visible) return;
    m_controller->put_Bounds(ComputeBounds());
}

// The browser draws the panel into a layered child window owned by its GPU
// process ("Intermediate D3D Window"). Hiding the control hides the browser's
// own windows, but that drawing surface keeps its visible style, and on the
// desktop the compositor could keep showing its last picture until the main
// window moved. Hide it from here as well, and show it again before the
// control returns. SWP_ASYNCWINDOWPOS posts the request to its owner, so this
// thread never waits on the browser process.
namespace {
constexpr UINT kSurfaceFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
bool IsBrowserSurface(HWND hwnd) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Intermediate D3D Window") == 0;
}
}

void WebViewSettings::HideBrowserSurfaces() {
    m_hiddenSurfaces.clear();
    EnumChildWindows(m_parent, [](HWND hwnd, LPARAM param) -> BOOL {
        if (IsBrowserSurface(hwnd) && (GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE)) {
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, kSurfaceFlags | SWP_HIDEWINDOW);
            reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&m_hiddenSurfaces));
    if (!m_hiddenSurfaces.empty())
        WVLog(L"Show(false): browser surfaces hidden: " + std::to_wstring(m_hiddenSurfaces.size()));
}

// On the desktop the compositor could keep the panel's last picture on screen
// after everything above was hidden, until the main window moved. Moving the
// window one pixel and back in the same instant is batched into one frame by
// the compositor and makes it recompose the window. A window covering its
// monitor (fullscreen) never showed the leftover and is left alone.
void WebViewSettings::RefreshComposition() {
    RECT rc{};
    if (!GetWindowRect(m_parent, &rc)) return;
    MONITORINFO mi{sizeof(mi)};
    HMONITOR monitor = MonitorFromWindow(m_parent, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &mi) && EqualRect(&rc, &mi.rcMonitor)) return;
    constexpr UINT flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    SetWindowPos(m_parent, nullptr, rc.left + 1, rc.top, 0, 0, flags);
    SetWindowPos(m_parent, nullptr, rc.left, rc.top, 0, 0, flags);
}

void WebViewSettings::ShowBrowserSurfaces() {
    for (HWND hwnd : m_hiddenSurfaces)
        if (IsWindow(hwnd)) SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, kSurfaceFlags | SWP_SHOWWINDOW);
    m_hiddenSurfaces.clear();
}

RECT WebViewSettings::ComputeBounds() const {
    RECT rc{};
    GetClientRect(m_parent, &rc);
    if (m_dock == Dock::Full) return rc;
    // The strip width follows the monitor scale so 420 DIP reads the same
    // on a 4K display at 150% as on a 1080p display at 100%.
    UINT dpi = GetDpiForWindow(m_parent);
    if (dpi == 0) dpi = 96;
    LONG width = MulDiv(m_widthDip, static_cast<int>(dpi), 96);
    const LONG client = rc.right - rc.left;
    if (width > client) width = client;
    if (m_dock == Dock::Right) rc.left  = rc.right - width;
    else                       rc.right = rc.left + width;
    return rc;
}

void WebViewSettings::SetDock(Dock dock, int widthDip) {
    if (dock == m_dock && widthDip == m_widthDip) return;
    m_dock = dock;
    m_widthDip = widthDip;
    if (m_controller && m_visible) m_controller->put_Bounds(ComputeBounds());
    WVLog(std::wstring(L"dock: ") +
          (dock == Dock::Full ? L"full" : dock == Dock::Right ? L"right" : L"left"));
}

void WebViewSettings::SetTransparentBackground(bool on) {
    if (on == m_transparent) return;
    m_transparent = on;
    ApplyBackground();
}

void WebViewSettings::ApplyBackground() {
    if (!m_controller) return;
    auto controller2 = m_controller.try_query<ICoreWebView2Controller2>();
    if (!controller2) {
        WVLog(L"DefaultBackgroundColor unavailable on this runtime, panel stays opaque");
        return;
    }
    // Alpha 0 lets the page's own translucent surfaces show the picture
    // behind the control. The color channels only matter while opaque.
    COREWEBVIEW2_COLOR color{};
    color.A = m_transparent ? 0 : 255;
    color.R = 0x20;
    color.G = 0x20;
    color.B = 0x20;
    controller2->put_DefaultBackgroundColor(color);
}

void WebViewSettings::NavigateToMenu() {
    if (!m_webview) {
        m_pendingNavigate = true;
        return;
    }
    const HRESULT hr = m_webview->Navigate(NitLink::WebViewPolicy::MenuUri);
    if (FAILED(hr)) Fail(L"toast.settingsNavigation", hr);
}

bool WebViewSettings::IsTrustedDocument() const {
    wil::unique_cotaskmem_string source;
    return m_webview && SUCCEEDED(m_webview->get_Source(&source)) && source &&
        NitLink::WebViewPolicy::IsTrustedDocument(source.get());
}

void WebViewSettings::PostMessage(const std::wstring& json) {
    if (!IsTrustedDocument()) return;
    const HRESULT hr = m_webview->PostWebMessageAsJson(json.c_str());
    if (FAILED(hr)) WVLogHr(L"settings state message failed", hr);
}

void WebViewSettings::LogBudgetStats(bool force) {
    if (!m_budgetDrops && !m_sliderReplacements && !m_sliderDeferred) return;
    const auto now = GetTickCount64();
    if (force || !m_lastBudgetLog || now - m_lastBudgetLog >= 1000) {
        WVLog(L"message budget: non-slider drops=" + std::to_wstring(m_budgetDrops) +
              L", superseded slider values=" + std::to_wstring(m_sliderReplacements) +
              L", deferred latest values=" + std::to_wstring(m_sliderDeferred));
        m_lastBudgetLog = now;
        m_budgetDrops = m_sliderReplacements = m_sliderDeferred = 0;
    }
}

void WebViewSettings::ReceiveMessage(const std::wstring& json) {
    const auto parsed = NitLink::ParseSettingsMessage(json);
    if (parsed && parsed->action == L"ready" && m_starting) {
        m_starting = false;
        m_ready = true;
        WVLog(L"trusted settings page ready");
        if (m_onMessage) m_onMessage(json);
        return;
    }
    if (!m_ready) return;
    if (parsed && (parsed->action == L"setVolume" || parsed->action == L"setPiPOpacity")) {
        const size_t index = parsed->action == L"setVolume" ? 0 : 1;
        const bool replaced = m_pendingSliders.Store(index, json);
        if (replaced) ++m_sliderReplacements;
        DispatchPendingMessages();
        if (!replaced && m_pendingSliders.Has(index)) ++m_sliderDeferred;
        LogBudgetStats();
        return;
    }
    DispatchPendingMessages();
    if (m_messageBudget.Accept(GetTickCount64())) {
        if (m_onMessage) m_onMessage(json);
    } else {
        ++m_budgetDrops;
        LogBudgetStats();
    }
}

void WebViewSettings::CheckHealth() {
    LogBudgetStats();
    if (!m_starting) return;
    const auto now = GetTickCount64();
    if (m_restartAt) {
        if (now >= m_restartAt && Start(m_parent)) NavigateToMenu();
        return;
    }
    // Called only after the application has pumped queued messages.
    if (!m_startTime) m_startTime = now;
    else if (now - m_startTime >= NitLink::kSettingsReadyTimeoutMs)
        Fail(L"toast.settingsTimeout", HRESULT_FROM_WIN32(WAIT_TIMEOUT));
}

void WebViewSettings::DispatchPendingMessages(bool closing) {
    if (m_pendingSliders.Empty()) return;
    if (!IsTrustedDocument() || !m_onMessage) {
        m_pendingSliders.Clear();
        return;
    }
    // Only native teardown may drain these two validated values without budget.
    m_pendingSliders.Drain(m_messageBudget, GetTickCount64(), closing, m_onMessage);
}

void WebViewSettings::Shutdown() {
    LogBudgetStats(/*force=*/true);
    m_ready = false;
    m_starting = false;
    m_startTime = m_restartAt = 0;
    m_menuNavigationId = 0;
    m_visible = false;
    m_callbackLifetime.reset();
    m_messageBudget = {};
    m_pendingSliders = {};
    m_lastBudgetLog = m_budgetDrops = m_sliderReplacements = m_sliderDeferred = 0;
    if (m_webview && m_messageToken.value != 0) {
        m_webview->remove_WebMessageReceived(m_messageToken);
        m_messageToken = {};
    }
    if (m_controller) {
        m_controller->Close();
        m_controller = nullptr;
    }
    m_webview = nullptr;
    m_resources.clear();
    m_pendingNavigate = false;
}
