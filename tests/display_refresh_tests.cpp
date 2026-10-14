#include "app/display_refresh.h"
#include "app/present_cap_policy.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace NitLink;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

DISPLAYCONFIG_PATH_INFO Path(UINT32 source, UINT32 target, UINT32 modeIndex = 0)
{
    DISPLAYCONFIG_PATH_INFO path{};
    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
    path.sourceInfo.adapterId = {7, 8};
    path.sourceInfo.id = source;
    path.targetInfo.adapterId = {9, 10};
    path.targetInfo.id = target;
    path.targetInfo.modeInfoIdx = modeIndex;
    path.targetInfo.targetAvailable = TRUE;
    path.targetInfo.refreshRate = {119998, 1000};
    return path;
}

DISPLAYCONFIG_MODE_INFO Mode(const DISPLAYCONFIG_PATH_INFO& path)
{
    DISPLAYCONFIG_MODE_INFO mode{};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    mode.id = path.targetInfo.id;
    mode.adapterId = path.targetInfo.adapterId;
    mode.targetMode.targetVideoSignalInfo.vSyncFreq = path.targetInfo.refreshRate;
    return mode;
}

struct Scenario {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    std::vector<UINT32> sizedFlags;
    LONG sizingError = ERROR_SUCCESS;
    LONG queryError = ERROR_SUCCESS;
    LONG sourceError = ERROR_SUCCESS;
    bool rejectVirtualRefresh = false;
    bool rejectVirtualMode = false;
    bool rejectFlagsAtQuery = false;
    unsigned insufficientQueries = 0;
    unsigned queryCalls = 0;
    unsigned sourceCalls = 0;
    bool emptySourceName = false;
    bool unterminatedSourceName = false;
};

Scenario scenario;

LONG WINAPI Sizing(UINT32 flags, UINT32* paths, UINT32* modes)
{
    scenario.sizedFlags.push_back(flags);
    if (!scenario.rejectFlagsAtQuery &&
        ((scenario.rejectVirtualRefresh && (flags & QDC_VIRTUAL_REFRESH_RATE_AWARE)) ||
         (scenario.rejectVirtualMode && (flags & QDC_VIRTUAL_MODE_AWARE)))) return ERROR_INVALID_PARAMETER;
    if (scenario.sizingError != ERROR_SUCCESS) return scenario.sizingError;
    *paths = static_cast<UINT32>(scenario.paths.size());
    *modes = static_cast<UINT32>(scenario.modes.size());
    return ERROR_SUCCESS;
}

LONG WINAPI Query(UINT32 flags, UINT32* pathCount, DISPLAYCONFIG_PATH_INFO* paths,
                  UINT32* modeCount, DISPLAYCONFIG_MODE_INFO* modes, DISPLAYCONFIG_TOPOLOGY_ID* topology)
{
    ++scenario.queryCalls;
    Require((flags & QDC_ONLY_ACTIVE_PATHS) != 0 && topology == nullptr,
            "query uses live active paths instead of persistent topology or first-display selection");
    if (scenario.rejectFlagsAtQuery &&
        ((scenario.rejectVirtualRefresh && (flags & QDC_VIRTUAL_REFRESH_RATE_AWARE)) ||
         (scenario.rejectVirtualMode && (flags & QDC_VIRTUAL_MODE_AWARE)))) return ERROR_INVALID_PARAMETER;
    if (scenario.insufficientQueries != 0) {
        --scenario.insufficientQueries;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (scenario.queryError != ERROR_SUCCESS) return scenario.queryError;
    Require(*pathCount >= scenario.paths.size() && *modeCount >= scenario.modes.size(),
            "query uses newly sized path and mode buffers");
    std::copy(scenario.paths.begin(), scenario.paths.end(), paths);
    std::copy(scenario.modes.begin(), scenario.modes.end(), modes);
    *pathCount = static_cast<UINT32>(scenario.paths.size());
    *modeCount = static_cast<UINT32>(scenario.modes.size());
    return ERROR_SUCCESS;
}

LONG WINAPI Source(DISPLAYCONFIG_DEVICE_INFO_HEADER* header)
{
    ++scenario.sourceCalls;
    Require(header->type == DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME &&
            header->size == sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME) &&
            header->adapterId.LowPart == 7 && header->adapterId.HighPart == 8,
            "source GDI lookup uses the source adapter identity and the proper device-info packet");
    if (scenario.sourceError != ERROR_SUCCESS) return scenario.sourceError;
    auto* name = reinterpret_cast<DISPLAYCONFIG_SOURCE_DEVICE_NAME*>(header);
    if (scenario.unterminatedSourceName) {
        std::fill(std::begin(name->viewGdiDeviceName), std::end(name->viewGdiDeviceName), L'X');
    } else if (!scenario.emptySourceName) {
        const auto text = header->id == 1 ? L"\\\\.\\DISPLAY1" : L"\\\\.\\DISPLAY2";
        wcscpy_s(name->viewGdiDeviceName, text);
    }
    return ERROR_SUCCESS;
}

const DisplayRefreshApi mockApi{&Sizing, &Query, &Source};

std::vector<std::uintptr_t> monitorSequence;
std::size_t monitorRead = 0;

BOOL WINAPI ValidWindow(HWND) { return TRUE; }

HMONITOR WINAPI WindowMonitor(HWND, DWORD flags)
{
    Require(flags == MONITOR_DEFAULTTONEAREST && monitorRead < monitorSequence.size(),
            "window lookup uses the nearest current monitor and bounded snapshot reads");
    return reinterpret_cast<HMONITOR>(monitorSequence[monitorRead++]);
}

BOOL WINAPI MonitorInfo(HMONITOR monitor, LPMONITORINFO rawInfo)
{
    Require(rawInfo->cbSize == sizeof(MONITORINFOEXW), "monitor identity uses the extended GDI-name packet");
    auto* info = reinterpret_cast<MONITORINFOEXW*>(rawInfo);
    const auto name = reinterpret_cast<std::uintptr_t>(monitor) == 1 ? L"\\\\.\\DISPLAY1" : L"\\\\.\\DISPLAY2";
    wcscpy_s(info->szDevice, name);
    return TRUE;
}

const WindowMonitorApi mockMonitorApi{&ValidWindow, &WindowMonitor, &MonitorInfo};

void ResetScenario()
{
    scenario = {};
    scenario.paths.push_back(Path(1, 11));
    scenario.modes.push_back(Mode(scenario.paths.front()));
}

void CheckRate(DisplayRefreshRate result, double expected)
{
    Require(result.status == DisplayRefreshStatus::Exact && std::abs(result.hz - expected) < 1.0e-10,
            "successful refresh conversion preserves the full rational quotient");
}
}

int main()
{
    try {
        struct Sample { UINT32 numerator; UINT32 denominator; double hz; };
        for (const auto sample : {Sample{59940, 1000, 59.94}, {60000, 1001, 60000.0 / 1001.0},
                                  {60, 1, 60.0}, {119880, 1000, 119.88}, {119998, 1000, 119.998},
                                  {120, 1, 120.0}, {144, 1, 144.0}}) {
            const auto hz = RationalRefreshHz({sample.numerator, sample.denominator});
            Require(hz && std::abs(*hz - sample.hz) < 1.0e-10,
                    "fractional and integer rational rates are converted without flooring or rounding");
        }
        Require(!RationalRefreshHz({0, 0}) && !RationalRefreshHz({0, 1}) && !RationalRefreshHz({60, 0}),
                "unspecified numerator and zero denominator are unknown rather than measured 0Hz");
        const auto maximum = std::numeric_limits<UINT32>::max();
        Require(RationalRefreshHz({maximum, maximum}) == 1.0,
                "full-width numerator and denominator divide without integer overflow");
        Require(RationalRefreshHz({maximum, 1}) == static_cast<double>(maximum) &&
                RationalRefreshHz({1, maximum}) == 1.0 / static_cast<double>(maximum),
                "mathematical rational conversion remains generic even for implausible display metadata");

        const auto first = DisplayRefreshPath{L"\\\\.\\DISPLAY1", {144, 1}};
        const auto second = DisplayRefreshPath{L"\\\\.\\DISPLAY2", {119880, 1000}};
        for (const auto rate : {RefreshRateRational{maximum, 1}, {1, maximum}}) {
            const auto implausible = DisplayRefreshPath{first.gdiDeviceName, rate};
            const auto rejected = ResolveDisplayRefresh(first.gdiDeviceName, std::span(&implausible, 1));
            Require(rejected.hz == 0.0 && rejected.status == DisplayRefreshStatus::InvalidRational &&
                    ChoosePresentCap({0, rejected.hz, 60.0}).reason == PresentCapReason::UnknownDisplayFallback,
                    "extreme metadata cannot reach duration conversion or silently disable the fallback cap");
        }
        for (const auto rate : {RefreshRateRational{1, 1}, {10000, 1}}) {
            const auto boundary = DisplayRefreshPath{first.gdiDeviceName, rate};
            CheckRate(ResolveDisplayRefresh(first.gdiDeviceName, std::span(&boundary, 1)),
                      static_cast<double>(rate.numerator));
        }
        std::vector<DisplayRefreshPath> candidates{first, second};
        CheckRate(ResolveDisplayRefresh(L"\\\\.\\display2", candidates), 119.88);
        Require(ResolveDisplayRefresh(L"\\\\.\\DISPLAY3", candidates).status == DisplayRefreshStatus::NoMatchingSource &&
                ResolveDisplayRefresh(L"", candidates).hz == 0.0,
                "a missing source never inherits the first active monitor's refresh");
        candidates.push_back(second);
        Require(ResolveDisplayRefresh(L"\\\\.\\DISPLAY2", candidates).status == DisplayRefreshStatus::AmbiguousSource,
                "multiple cloned physical targets stay ambiguous even when their refresh rates agree");
        candidates.back().active = false;
        CheckRate(ResolveDisplayRefresh(L"\\\\.\\DISPLAY2", candidates), 119.88);
        candidates[1].targetAvailable = false;
        Require(ResolveDisplayRefresh(L"\\\\.\\DISPLAY2", candidates).status == DisplayRefreshStatus::TargetUnavailable,
                "an unavailable target cannot establish a reliable physical ceiling");

        auto path = Path(1, 11);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes{Mode(path)};
        auto described = DescribeDisplayRefreshPath(path, modes, first.gdiDeviceName);
        CheckRate(ResolveDisplayRefresh(first.gdiDeviceName, std::span(&described, 1)), 119.998);
        Require(described.signalModeValid, "a matching physical target mode is selected");
        auto pathOnly = path;
        pathOnly.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        described = DescribeDisplayRefreshPath(pathOnly, modes, first.gdiDeviceName);
        const auto pathFallback = ResolveDisplayRefresh(first.gdiDeviceName, std::span(&described, 1));
        CheckRate(pathFallback, 119.998);
        Require(pathFallback.origin == DisplayRefreshOrigin::ActivePath,
                "unboosted path rational fallback retains exact precision and records its different origin");
        for (unsigned invalid = 0; invalid < 5; ++invalid) {
            auto badPath = path;
            auto badModes = modes;
            if (invalid == 0) badPath.targetInfo.modeInfoIdx = 2;
            if (invalid == 1) badPath.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            if (invalid == 2) badModes[0].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
            if (invalid == 3) ++badModes[0].id;
            if (invalid == 4) ++badModes[0].adapterId.HighPart;
            badPath.flags |= DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE;
            badPath.targetInfo.refreshRate = {60, 1};
            const auto bad = DescribeDisplayRefreshPath(badPath, badModes, first.gdiDeviceName);
            Require(!bad.signalModeValid && ResolveDisplayRefresh(first.gdiDeviceName, std::span(&bad, 1)).status ==
                    DisplayRefreshStatus::InvalidTargetMode,
                    "bad physical mode identity/index cannot make a boosted virtual rate into a ceiling");
        }
        path.flags |= DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE | DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE;
        path.targetInfo.targetModeInfoIdx = 0;
        path.targetInfo.desktopModeInfoIdx = 1;
        path.targetInfo.refreshRate = {60, 1};
        described = DescribeDisplayRefreshPath(path, modes, first.gdiDeviceName);
        CheckRate(ResolveDisplayRefresh(first.gdiDeviceName, std::span(&described, 1)), 119.998);
        Require(described.signalModeValid, "virtual-aware paths decode targetModeInfoIdx rather than the packed legacy union");
        modes[0].targetMode.targetVideoSignalInfo.vSyncFreq = {0, 1};
        described = DescribeDisplayRefreshPath(path, modes, first.gdiDeviceName);
        Require(ResolveDisplayRefresh(first.gdiDeviceName, std::span(&described, 1)).status ==
                DisplayRefreshStatus::InvalidRational,
                "invalid physical rational is not disguised by a valid virtual rate");

        ResetScenario();
        scenario.paths.push_back(Path(2, 22, 1));
        scenario.paths.back().targetInfo.refreshRate = {59940, 1000};
        scenario.modes.push_back(Mode(scenario.paths.back()));
        CheckRate(QueryDisplayRefreshForGdiDevice(second.gdiDeviceName, mockApi).refresh, 59.94);
        Require(scenario.sourceCalls == 2, "multi-monitor query resolves source names instead of selecting first path");
        ResetScenario();
        scenario.rejectVirtualRefresh = true;
        CheckRate(QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi).refresh, 119.998);
        Require(scenario.sizedFlags.size() == 2 &&
                scenario.sizedFlags[0] == (QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE | QDC_VIRTUAL_REFRESH_RATE_AWARE) &&
                scenario.sizedFlags[1] == (QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE),
                "unsupported Windows11 flags downgrade explicitly to Windows10 virtual-mode-aware query");
        ResetScenario();
        scenario.rejectVirtualRefresh = scenario.rejectFlagsAtQuery = true;
        CheckRate(QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi).refresh, 119.998);
        Require(scenario.queryCalls == 2 && scenario.sizedFlags.size() == 2,
                "unsupported flag errors from QueryDisplayConfig also downgrade the next sizing/query pair");
        ResetScenario();
        scenario.rejectVirtualRefresh = scenario.rejectVirtualMode = true;
        CheckRate(QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi).refresh, 119.998);
        Require(scenario.sizedFlags.back() == QDC_ONLY_ACTIVE_PATHS && scenario.sizedFlags.size() == 3,
                "legacy flags downgrade only after invalid-parameter errors");
        ResetScenario();
        scenario.insufficientQueries = 1;
        CheckRate(QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi).refresh, 119.998);
        Require(scenario.queryCalls == 2 && scenario.sizedFlags.size() == 2,
                "topology change retries sizing instead of reusing a stale buffer");
        ResetScenario();
        scenario.insufficientQueries = 100;
        const auto exhausted = QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi);
        Require(exhausted.refresh.hz == 0.0 && exhausted.refresh.status == DisplayRefreshStatus::QueryFailed &&
                exhausted.apiError == ERROR_INSUFFICIENT_BUFFER && scenario.queryCalls == 3,
                "repeated topology changes fail explicitly after a bounded number of retries");
        for (const LONG error : {ERROR_ACCESS_DENIED, ERROR_NOT_SUPPORTED, ERROR_GEN_FAILURE}) {
            ResetScenario();
            scenario.sizingError = error;
            const auto result = QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi);
            Require(result.refresh.hz == 0.0 && result.refresh.status == DisplayRefreshStatus::QueryFailed &&
                    result.apiError == error && scenario.sizedFlags.size() == 1,
                    "non-flag failures cannot trigger weaker queries or guessed integer refresh");
        }
        ResetScenario();
        scenario.queryError = ERROR_ACCESS_DENIED;
        Require(QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi).refresh.status == DisplayRefreshStatus::QueryFailed,
                "a failed active-path query cannot produce a measured rate");
        ResetScenario();
        scenario.sourceError = ERROR_GEN_FAILURE;
        Require(QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi).refresh.status ==
                DisplayRefreshStatus::SourceNameUnavailable,
                "failed source identity rejects the partial source table");
        ResetScenario();
        scenario.paths.push_back(Path(1, 22, 1));
        scenario.modes.push_back(Mode(scenario.paths.back()));
        Require(QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi).refresh.status ==
                DisplayRefreshStatus::AmbiguousSource,
                "a cloned source returned by the real query boundary is never assumed to identify one physical panel");
        ResetScenario();
        scenario.paths.clear();
        const auto empty = QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi);
        Require(empty.refresh.hz == 0.0 && empty.refresh.status == DisplayRefreshStatus::QueryFailed &&
                empty.apiError == ERROR_NOT_FOUND && scenario.queryCalls == 0,
                "an empty active topology does not call the API with empty arrays or produce a rate");
        for (bool unterminated : {false, true}) {
            ResetScenario();
            scenario.emptySourceName = !unterminated;
            scenario.unterminatedSourceName = unterminated;
            Require(QueryDisplayRefreshForGdiDevice(first.gdiDeviceName, mockApi).refresh.status ==
                    DisplayRefreshStatus::SourceNameUnavailable,
                    "missing or unterminated source names cannot match another monitor");
        }
        Require(QueryWindowDisplayRefresh(nullptr).refresh.status == DisplayRefreshStatus::NoMonitor,
                "a missing HWND cannot accidentally query the primary monitor");
        ResetScenario();
        scenario.paths.push_back(Path(2, 22, 1));
        scenario.paths.back().targetInfo.refreshRate = {59940, 1000};
        scenario.modes.push_back(Mode(scenario.paths.back()));
        monitorSequence = {1, 2, 2, 2};
        monitorRead = 0;
        const auto moved = QueryWindowDisplayRefresh(reinterpret_cast<HWND>(1), mockApi, mockMonitorApi);
        CheckRate(moved.refresh, 59.94);
        Require(moved.monitor == reinterpret_cast<HMONITOR>(2) && scenario.queryCalls == 2 && monitorRead == 4,
                "a window moved during the first query retries and uses the new panel's physical rate");
        scenario.queryCalls = 0;
        monitorSequence = {1, 2, 1, 2, 1, 2};
        monitorRead = 0;
        const auto unstable = QueryWindowDisplayRefresh(reinterpret_cast<HWND>(1), mockApi, mockMonitorApi);
        Require(unstable.refresh.hz == 0.0 && unstable.refresh.status == DisplayRefreshStatus::MonitorChanged &&
                unstable.monitor == reinterpret_cast<HMONITOR>(2) &&
                SameGdiDeviceName(unstable.gdiDeviceName, second.gdiDeviceName) &&
                scenario.queryCalls == 3 && monitorRead == 6,
                "continuously moving identity fails after three snapshots and retains only the latest monitor identity");
        std::cout << "PASS rational refresh, multi-monitor identity, physical/virtual mode validation and DisplayConfig failures/retries\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
