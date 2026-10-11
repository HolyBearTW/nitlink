#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace NitLink {

struct RefreshRateRational {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;
};

// Zero components describe an unspecified/invalid rate, not measured 0 Hz.
// Convert before division so fractional rates never undergo integer division.
inline std::optional<double> RationalRefreshHz(RefreshRateRational rate)
{
    if (rate.numerator == 0 || rate.denominator == 0) return std::nullopt;
    return static_cast<double>(rate.numerator) / static_cast<double>(rate.denominator);
}

enum class DisplayRefreshStatus {
    Exact, NoMonitor, QueryFailed, SourceNameUnavailable, NoMatchingSource,
    AmbiguousSource, TargetUnavailable, InvalidTargetMode, InvalidRational, MonitorChanged
};

enum class DisplayRefreshOrigin { None, PhysicalSignal, ActivePath };

// Bound trusted display metadata before it reaches duration conversion. The
// rational converter itself remains generic; extreme ratios are not credible
// active desktop refresh rates and must trigger the explicit unknown fallback.
inline constexpr double kMinimumReliableDisplayRefreshHz = 1.0;
inline constexpr double kMaximumReliableDisplayRefreshHz = 10000.0;

struct DisplayRefreshPath {
    std::wstring gdiDeviceName;
    RefreshRateRational pathRate;
    RefreshRateRational signalRate;
    bool signalModeValid = false;
    bool boosted = false;
    bool active = true;
    bool targetAvailable = true;
};

struct DisplayRefreshRate {
    double hz = 0.0;
    RefreshRateRational rational;
    DisplayRefreshStatus status = DisplayRefreshStatus::NoMatchingSource;
    DisplayRefreshOrigin origin = DisplayRefreshOrigin::None;
};

// GDI display names are ASCII identifiers such as \\.\DISPLAY1. Fold ASCII
// explicitly so matching does not depend on the process locale.
inline bool SameGdiDeviceName(std::wstring_view left, std::wstring_view right)
{
    if (left.empty() || left.size() != right.size()) return false;
    const auto fold = [](wchar_t c) {
        return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c;
    };
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (fold(left[i]) != fold(right[i])) return false;
    }
    return true;
}

// A source GDI name can feed multiple physical targets in clone mode. A GDI
// monitor alone cannot identify one of those panels, even if their rates agree.
// Reject ambiguity instead of borrowing the first path's physical ceiling.
inline DisplayRefreshRate ResolveDisplayRefresh(
    std::wstring_view gdiDeviceName, std::span<const DisplayRefreshPath> paths)
{
    const DisplayRefreshPath* match = nullptr;
    for (const auto& path : paths) {
        if (!path.active || !SameGdiDeviceName(gdiDeviceName, path.gdiDeviceName)) continue;
        if (match) return {0.0, {}, DisplayRefreshStatus::AmbiguousSource};
        match = &path;
    }
    if (!match) return {};
    if (!match->targetAvailable) return {0.0, {}, DisplayRefreshStatus::TargetUnavailable};

    // In Windows 11 dynamic refresh mode, pathRate is the virtual rate.
    // A boosted path therefore needs its physical target mode; otherwise its
    // virtual rate must not be presented as a physical monitor ceiling.
    if (!match->signalModeValid && match->boosted) {
        return {0.0, {}, DisplayRefreshStatus::InvalidTargetMode};
    }
    const auto rational = match->signalModeValid ? match->signalRate : match->pathRate;
    const auto hz = RationalRefreshHz(rational);
    if (!hz || *hz < kMinimumReliableDisplayRefreshHz || *hz > kMaximumReliableDisplayRefreshHz) {
        return {0.0, {}, DisplayRefreshStatus::InvalidRational};
    }
    return {*hz, rational, DisplayRefreshStatus::Exact,
            match->signalModeValid ? DisplayRefreshOrigin::PhysicalSignal : DisplayRefreshOrigin::ActivePath};
}

inline const wchar_t* DisplayRefreshStatusName(DisplayRefreshStatus status)
{
    switch (status) {
    case DisplayRefreshStatus::Exact: return L"exact rational";
    case DisplayRefreshStatus::NoMonitor: return L"window monitor unavailable";
    case DisplayRefreshStatus::QueryFailed: return L"DisplayConfig query failed";
    case DisplayRefreshStatus::SourceNameUnavailable: return L"DisplayConfig source GDI name unavailable";
    case DisplayRefreshStatus::NoMatchingSource: return L"no matching active GDI source";
    case DisplayRefreshStatus::AmbiguousSource: return L"ambiguous cloned GDI source";
    case DisplayRefreshStatus::TargetUnavailable: return L"matching physical target unavailable";
    case DisplayRefreshStatus::InvalidTargetMode: return L"physical target mode unavailable";
    case DisplayRefreshStatus::InvalidRational: return L"invalid refresh rational";
    case DisplayRefreshStatus::MonitorChanged: return L"window monitor changed during query";
    }
    return L"unknown";
}

inline const wchar_t* DisplayRefreshOriginName(DisplayRefreshOrigin origin)
{
    switch (origin) {
    case DisplayRefreshOrigin::PhysicalSignal: return L"target video signal";
    case DisplayRefreshOrigin::ActivePath: return L"active path rational";
    case DisplayRefreshOrigin::None: return L"none";
    }
    return L"none";
}

} // namespace NitLink
