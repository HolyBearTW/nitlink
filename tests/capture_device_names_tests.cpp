#include "app/capture_device_names.h"

#include <iostream>
#include <string>
#include <vector>

using NitLink::RefreshCaptureDeviceNamesIfRequested;

int main()
{
    std::vector<std::wstring> cachedNames{L"Startup Device"};
    int enumerationCalls = 0;

    RefreshCaptureDeviceNamesIfRequested(false, cachedNames, [&] {
        ++enumerationCalls;
        return std::vector<std::wstring>{L"Unexpected Device"};
    });
    if (enumerationCalls != 0 || cachedNames != std::vector<std::wstring>{L"Startup Device"}) {
        return 1;
    }

    RefreshCaptureDeviceNamesIfRequested(true, cachedNames, [&] {
        ++enumerationCalls;
        return std::vector<std::wstring>{L"Newly Connected Device"};
    });
    if (enumerationCalls != 1 ||
        cachedNames != std::vector<std::wstring>{L"Newly Connected Device"}) {
        return 2;
    }

    RefreshCaptureDeviceNamesIfRequested(false, cachedNames, [&] {
        ++enumerationCalls;
        return std::vector<std::wstring>{L"Stale Replacement"};
    });
    if (enumerationCalls != 1 ||
        cachedNames != std::vector<std::wstring>{L"Newly Connected Device"}) {
        return 3;
    }

    std::cout << "capture device name cache tests passed\n";
    return 0;
}
