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

#include <wrl.h>
#include <WebView2.h>
#include <wil/com.h>
#include <Shlobj.h>
#include <shellapi.h>   // ShellExecuteW: for opening external links in default browser
#include <sstream>
#include <string>
#include <wchar.h>      // _wcsnicmp: case-insensitive URI scheme allow-list checks
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

bool WebViewSettings::Initialize(HWND parent, int width, int height) {
    m_parent = parent;

    // WebView2 user-data folder under %LOCALAPPDATA%\NitLink\WebView2.
    // Using a dedicated path silences the runtime warning about default
    // location and isolates this app's cookies/storage from other
    // WebView2 hosts on the system.
    wchar_t localAppData[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData);
    std::wstring userDataFolder = std::wstring(localAppData) + L"\\NitLink\\WebView2";

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(envHr) || !env) {
                    WVLogHr(L"environment creation failed", envHr);
                    return envHr;
                }
                HRESULT hr2 = env->CreateCoreWebView2Controller(m_parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT ctlHr, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(ctlHr) || !controller) {
                                WVLogHr(L"controller creation failed", ctlHr);
                                return ctlHr;
                            }
                            m_controller = controller;
                            controller->get_CoreWebView2(&m_webview);

                            // Size to current parent client area.
                            RECT bounds;
                            GetClientRect(m_parent, &bounds);
                            m_controller->put_Bounds(bounds);

                            // Start HIDDEN; Show(true) flips it on when F1 pressed.
                            m_controller->put_IsVisible(FALSE);

                            // Message handler. JS posts an object, so the
                            // handler uses get_WebMessageAsJson (not
                            // TryGetWebMessageAsString, which would E_INVALIDARG
                            // for non-string payloads).
                            m_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*,
                                            ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string json;
                                        HRESULT hr = args->get_WebMessageAsJson(&json);
                                        if (SUCCEEDED(hr) && json && m_onMessage) {
                                            m_onMessage(json.get());
                                        }
                                        return S_OK;
                                    }).Get(), &m_messageToken);

                            // Navigation allow-list. The only legitimate
                            // top-level navigation is to the app's own local
                            // file:// content (nitlink-menu.html). Cancel
                            // anything else (http(s), about:, javascript:, a
                            // planted path) so a compromised or redirected
                            // page can't drive the top frame somewhere hostile.
                            // Real external links go through NewWindowRequested
                            // below and open in the user's browser instead.
                            // (NavigateToString, if ever wired up, lands on
                            // about:blank and would need "about:" added here.)
                            EventRegistrationToken navToken{};
                            m_webview->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2*,
                                        ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string uri;
                                        if (SUCCEEDED(args->get_Uri(&uri)) && uri &&
                                            _wcsnicmp(uri.get(), L"file:", 5) != 0) {
                                            args->put_Cancel(TRUE);
                                            WVLog(L"NavigationStarting: blocked "
                                                  + std::wstring(uri.get()));
                                        }
                                        return S_OK;
                                    }).Get(), &navToken);

                            // Intercept target="_blank" / window.open() / external link
                            // clicks so they open in the user's default browser instead
                            // of spawning a stripped-down child WebView2 window (which
                            // would look broken). Required for Ko-fi, feedback form,
                            // GitHub repo, etc.: anything that should leave the app.
                            EventRegistrationToken newWindowToken{};
                            m_webview->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*,
                                        ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string uri;
                                        if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                                            // Only hand http(s) links to the shell.
                                            // NewWindowRequested can fire for any scheme
                                            // (file:, javascript:, custom protocol handlers
                                            // that launch local programs); ShellExecute
                                            // "open" on those could run an arbitrary
                                            // registered handler, so restrict to web links.
                                            if (_wcsnicmp(uri.get(), L"http:", 5) == 0 ||
                                                _wcsnicmp(uri.get(), L"https:", 6) == 0) {
                                                ShellExecuteW(nullptr, L"open", uri.get(),
                                                              nullptr, nullptr, SW_SHOWNORMAL);
                                            } else {
                                                WVLog(L"NewWindowRequested: blocked non-web "
                                                      L"scheme: " + std::wstring(uri.get()));
                                            }
                                        }
                                        // Tell WebView2 the event was handled here: don't
                                        // open a child window.
                                        args->put_Handled(TRUE);
                                        return S_OK;
                                    }).Get(), &newWindowToken);

                            // Drain any pending navigation that was queued
                            // before async init completed.
                            if (!m_pendingNavigateUri.empty()) {
                                std::wstring uri = m_pendingNavigateUri;
                                m_pendingNavigateUri.clear();
                                WVLog(L"Replaying pending navigation: " + uri);
                                m_webview->Navigate(uri.c_str());
                            }

                            WVLog(L"WebView2 ready (child of main)");
                            return S_OK;
                        }).Get());
                if (FAILED(hr2)) WVLogHr(L"controller dispatch failed", hr2);
                return hr2;
            }).Get());

    if (FAILED(hr)) {
        WVLogHr(L"environment dispatch failed", hr);
        return false;
    }
    return true;
}

void WebViewSettings::Show(bool visible) {
    m_visible = visible;
    if (!m_controller) return;
    if (visible) {
        // Resize to current parent client bounds in case the parent grew
        // since last show: DXGI swap chain may have resized but the
        // controller wouldn't have been told.
        RECT rc;
        GetClientRect(m_parent, &rc);
        m_controller->put_Bounds(rc);
        m_controller->put_IsVisible(TRUE);
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        WVLog(L"Show(true): overlay visible");
    } else {
        m_controller->put_IsVisible(FALSE);
        // Return keyboard focus to the parent so hotkeys + key events
        // (Esc, F1, etc.) continue to be processed by the main window.
        SetFocus(m_parent);
        WVLog(L"Show(false): overlay hidden");
    }
}

void WebViewSettings::Resize() {
    if (!m_controller) return;
    RECT rc;
    GetClientRect(m_parent, &rc);
    m_controller->put_Bounds(rc);
}

void WebViewSettings::NavigateToFile(const std::wstring& path) {
    // Resolve relative names against the EXECUTABLE's directory, not the
    // process CWD. GetFullPathNameW anchors at the CWD, so a file planted in
    // whatever directory the app happens to be launched from could shadow the
    // real UI html. The bundled html ships next to the exe, so anchor there
    // deterministically. Absolute paths (drive-letter or UNC) are passed
    // through unchanged.
    const bool isAbsolute =
        (path.size() >= 2 && path[1] == L':') ||
        (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
                             (path[1] == L'\\' || path[1] == L'/'));

    std::wstring fullPath;
    if (isAbsolute) {
        fullPath = path;
    } else {
        wchar_t exePath[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring dir(exePath);
            size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) dir.resize(slash + 1);
            fullPath = dir + path;
        } else {
            // Fallback to the prior CWD-relative behavior only if the module
            // path is unreadable (should never happen in practice).
            wchar_t tmp[MAX_PATH] = {};
            GetFullPathNameW(path.c_str(), MAX_PATH, tmp, nullptr);
            fullPath = tmp;
        }
    }

    std::wstring uri = L"file:///";
    uri += fullPath;
    for (auto& c : uri) if (c == L'\\') c = L'/';

    if (!m_webview) {
        WVLog(L"NavigateToFile queued (WebView2 not ready): " + uri);
        m_pendingNavigateUri = uri;
        return;
    }
    m_webview->Navigate(uri.c_str());
}

void WebViewSettings::NavigateToString(const std::wstring& html) {
    if (!m_webview) return;
    m_webview->NavigateToString(html.c_str());
}

void WebViewSettings::PostMessage(const std::wstring& json) {
    if (!m_webview) return;
    m_webview->PostWebMessageAsJson(json.c_str());
}

void WebViewSettings::Shutdown() {
    if (m_webview && m_messageToken.value != 0) {
        m_webview->remove_WebMessageReceived(m_messageToken);
        m_messageToken = {};
    }
    if (m_controller) {
        m_controller->Close();
        m_controller = nullptr;
    }
    m_webview = nullptr;
}
