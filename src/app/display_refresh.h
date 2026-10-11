#pragma once

#include <windows.h>
#include "display_refresh_policy.h"

namespace NitLink {

struct DisplayRefreshQuery {
    DisplayRefreshRate refresh;
    HMONITOR monitor = nullptr;
    std::wstring gdiDeviceName;
    LONG apiError = ERROR_SUCCESS;
};

struct DisplayRefreshApi {
    decltype(&GetDisplayConfigBufferSizes) getBufferSizes = &GetDisplayConfigBufferSizes;
    decltype(&QueryDisplayConfig) queryDisplayConfig = &QueryDisplayConfig;
    decltype(&DisplayConfigGetDeviceInfo) getDeviceInfo = &DisplayConfigGetDeviceInfo;
};

struct WindowMonitorApi {
    decltype(&IsWindow) isWindow = &IsWindow;
    decltype(&MonitorFromWindow) monitorFromWindow = &MonitorFromWindow;
    decltype(&GetMonitorInfoW) getMonitorInfo = &GetMonitorInfoW;
};

// Pure conversion of one queried path and its same-snapshot mode table. The
// source GDI name is supplied by DisplayConfigGetDeviceInfo(GET_SOURCE_NAME).
DisplayRefreshPath DescribeDisplayRefreshPath(
    const DISPLAYCONFIG_PATH_INFO& path,
    std::span<const DISPLAYCONFIG_MODE_INFO> modes,
    std::wstring gdiDeviceName);

// Injectable DisplayConfig boundary: tests exercise flag compatibility,
// topology retries and failed lookups without changing the desktop or using a
// real monitor. The window wrapper supplies the current monitor's GDI name.
DisplayRefreshQuery QueryDisplayRefreshForGdiDevice(
    std::wstring gdiDeviceName, const DisplayRefreshApi& api = {});

// Called on initialization, monitor/display changes and settings changes,
// never on every frame. Query failure leaves hz=0 with an explicit status.
DisplayRefreshQuery QueryWindowDisplayRefresh(HWND window,
    const DisplayRefreshApi& api = {}, const WindowMonitorApi& monitorApi = {});

} // namespace NitLink
