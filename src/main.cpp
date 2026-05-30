/*
 * NitLink: Console games as native PC apps.
 *
 * Lightweight capture card viewer with shader pipeline,
 * image upscaling, and per-game presets.
 *
 * MIT License, https://github.com/nitlink-dev/nitlink
 */

#include "app/application.h"
#include <windows.h>
#include <shellscalingapi.h>
#pragma comment(lib, "shcore.lib")

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    // Harden the DLL search path before anything else runs. This drops the
    // current working directory (and other unsafe locations) from the default
    // search order so a DLL planted next to wherever the app was launched
    // can't hijack a dependency loaded later, notably WebView2Loader.dll,
    // pulled in when the settings overlay initializes. LOAD_LIBRARY_SEARCH_
    // DEFAULT_DIRS keeps System32 and the executable's own directory, which is
    // where NitLink's real dependencies live. Best-effort: nothing to do if it
    // fails, the app still runs (just without the hardening).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    // Opt into per-monitor DPI awareness BEFORE creating any windows.
    // Without this, Windows lies about pixel sizes when DPI scaling is
    // not 100%, which produces a blurry image because rendering happens at
    // a lower resolution than the actual screen.
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    // COM init: single-threaded apartment.
    //
    // Earlier builds used COINIT_MULTITHREADED (MTA) because Media Foundation,
    // DXGI, and WASAPI all work fine in MTA. WebView2 requires STA on the
    // thread that creates the environment/controller: calling
    // CreateCoreWebView2EnvironmentWithOptions from an MTA thread returns
    // RPC_E_CHANGED_MODE (0x80010106) and the async callback never fires.
    //
    // STA is the path of least surprise for a Windows app with a UI thread.
    // MF and WASAPI marshal cross-apartment internally and keep working.
    // Background threads owned by NitLink that call back into COM need
    // their own CoInitializeEx.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to initialize COM runtime.", L"NitLink", MB_ICONERROR);
        return 1;
    }

    // Start Media Foundation
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to initialize Media Foundation.", L"NitLink", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    {
        NitLink::Application app;
        
        if (!app.Initialize(hInstance, nCmdShow)) {
            MessageBoxW(nullptr, L"Failed to initialize NitLink.\nCheck that a capture card is connected.", 
                        L"NitLink", MB_ICONERROR);
            MFShutdown();
            CoUninitialize();
            return 1;
        }

        app.Run();
    }

    MFShutdown();
    CoUninitialize();
    return 0;
}
