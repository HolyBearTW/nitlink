#pragma once

#include <string>
#include <utility>
#include <vector>

namespace NitLink {

template <typename Enumerator>
void RefreshCaptureDeviceNamesIfRequested(
    bool refresh,
    std::vector<std::wstring>& cachedNames,
    Enumerator&& enumerateNames)
{
    if (refresh) {
        cachedNames = std::forward<Enumerator>(enumerateNames)();
    }
}

} // namespace NitLink
