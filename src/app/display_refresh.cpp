#include "display_refresh.h"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace NitLink {
namespace {

bool SameAdapter(LUID left, LUID right)
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

LONG QueryActivePaths(const DisplayRefreshApi& api, UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                      std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    // Display topology can change between sizing and query. Retry with freshly
    // sized buffers, with a bound so a changing desktop cannot stall playback.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        LONG error = api.getBufferSizes(flags, &pathCount, &modeCount);
        if (error != ERROR_SUCCESS) return error;
        if (pathCount == 0 || modeCount == 0) return ERROR_NOT_FOUND;
        paths.resize(pathCount);
        modes.resize(modeCount);
        error = api.queryDisplayConfig(flags, &pathCount, paths.data(),
                                   &modeCount, modes.data(), nullptr);
        if (error == ERROR_INSUFFICIENT_BUFFER) continue;
        if (error != ERROR_SUCCESS) return error;
        paths.resize(pathCount);
        modes.resize(modeCount);
        return ERROR_SUCCESS;
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

bool WindowMonitorInfo(const WindowMonitorApi& api, HWND window, HMONITOR& monitor, MONITORINFOEXW& info)
{
    // An invalid HWND would make DEFAULTTONEAREST select the primary monitor.
    // Do not let an absent window become a successful query of another panel.
    if (!window || !api.isWindow(window)) return false;
    monitor = api.monitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    info = {};
    info.cbSize = sizeof(info);
    return monitor && api.getMonitorInfo(monitor, &info);
}

} // namespace

DisplayRefreshPath DescribeDisplayRefreshPath(
    const DISPLAYCONFIG_PATH_INFO& path,
    std::span<const DISPLAYCONFIG_MODE_INFO> modes,
    std::wstring gdiDeviceName)
{
    DisplayRefreshPath result;
    result.gdiDeviceName = std::move(gdiDeviceName);
    result.pathRate = {path.targetInfo.refreshRate.Numerator, path.targetInfo.refreshRate.Denominator};
    result.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
    result.targetAvailable = path.targetInfo.targetAvailable != FALSE;
    result.boosted = (path.flags & DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE) != 0;
    const bool virtualMode = (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0;
    const UINT32 index = virtualMode ? path.targetInfo.targetModeInfoIdx : path.targetInfo.modeInfoIdx;
    const UINT32 invalid = virtualMode ? DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID
                                       : DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    if (index != invalid && index < modes.size()) {
        const auto& mode = modes[index];
        // Index range alone is insufficient: never read the union of a source
        // mode, another adapter or another physical target as a refresh rate.
        if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET &&
            mode.id == path.targetInfo.id && SameAdapter(mode.adapterId, path.targetInfo.adapterId)) {
            result.signalModeValid = true;
            const auto& rate = mode.targetMode.targetVideoSignalInfo.vSyncFreq;
            result.signalRate = {rate.Numerator, rate.Denominator};
        }
    }
    return result;
}

DisplayRefreshQuery QueryDisplayRefreshForGdiDevice(std::wstring gdiDeviceName, const DisplayRefreshApi& api)
{
    DisplayRefreshQuery result;
    result.gdiDeviceName = std::move(gdiDeviceName);
    if (result.gdiDeviceName.empty()) {
        result.refresh.status = DisplayRefreshStatus::NoMatchingSource;
        return result;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE | QDC_VIRTUAL_REFRESH_RATE_AWARE;
    result.apiError = QueryActivePaths(api, flags, paths, modes);
    if (result.apiError == ERROR_INVALID_PARAMETER) {
        // Windows 10 lacks virtual-refresh awareness; Windows 7/8 also lack
        // virtual-mode awareness. Only unsupported flags trigger a downgrade.
        flags &= ~QDC_VIRTUAL_REFRESH_RATE_AWARE;
        result.apiError = QueryActivePaths(api, flags, paths, modes);
        if (result.apiError == ERROR_INVALID_PARAMETER) {
            result.apiError = QueryActivePaths(api, QDC_ONLY_ACTIVE_PATHS, paths, modes);
        }
    }
    if (result.apiError != ERROR_SUCCESS) {
        result.refresh.status = DisplayRefreshStatus::QueryFailed;
        return result;
    }

    std::vector<DisplayRefreshPath> candidates;
    candidates.reserve(paths.size());
    for (const auto& path : paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.sourceInfo.adapterId;
        name.header.id = path.sourceInfo.id;
        result.apiError = api.getDeviceInfo(&name.header);
        if (result.apiError != ERROR_SUCCESS) {
            // An incomplete source-name table cannot establish unique matching.
            result.refresh.status = DisplayRefreshStatus::SourceNameUnavailable;
            return result;
        }
        const auto nameEnd = std::find(std::begin(name.viewGdiDeviceName),
                                       std::end(name.viewGdiDeviceName), L'\0');
        if (nameEnd == std::end(name.viewGdiDeviceName) || nameEnd == std::begin(name.viewGdiDeviceName)) {
            result.apiError = ERROR_INVALID_DATA;
            result.refresh.status = DisplayRefreshStatus::SourceNameUnavailable;
            return result;
        }
        candidates.push_back(DescribeDisplayRefreshPath(path, modes,
            std::wstring(std::begin(name.viewGdiDeviceName), nameEnd)));
    }
    result.refresh = ResolveDisplayRefresh(result.gdiDeviceName, candidates);
    return result;
}

DisplayRefreshQuery QueryWindowDisplayRefresh(HWND window,
    const DisplayRefreshApi& api, const WindowMonitorApi& monitorApi)
{
    DisplayRefreshQuery result;
    // A window can move to another panel while CCD is taking its snapshot.
    // Retry the whole identity/snapshot pair, with no per-frame retry polling.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        HMONITOR monitor = nullptr;
        MONITORINFOEXW info{};
        if (!WindowMonitorInfo(monitorApi, window, monitor, info)) {
            result = {};
            result.refresh.status = DisplayRefreshStatus::NoMonitor;
            return result;
        }
        result = QueryDisplayRefreshForGdiDevice(info.szDevice, api);
        result.monitor = monitor;

        HMONITOR currentMonitor = nullptr;
        MONITORINFOEXW currentInfo{};
        const bool currentAvailable = WindowMonitorInfo(monitorApi, window, currentMonitor, currentInfo);
        if (currentAvailable && currentMonitor == result.monitor &&
            SameGdiDeviceName(currentInfo.szDevice, result.gdiDeviceName)) return result;
        result.refresh = {0.0, {}, DisplayRefreshStatus::MonitorChanged};
        // On exhaustion, cache the most recent identity rather than the stale
        // panel queried earlier. Its rate stays unknown until a future event.
        result.monitor = currentAvailable ? currentMonitor : nullptr;
        result.gdiDeviceName = currentAvailable ? currentInfo.szDevice : L"";
    }
    return result;
}

} // namespace NitLink
