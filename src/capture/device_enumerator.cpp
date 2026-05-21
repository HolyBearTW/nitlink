#include "device_enumerator.h"
#include <mfapi.h>
#include <mfidl.h>
#include <windows.h>
#include <debugapi.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>

namespace NitLink {

namespace {

void DebugLog(const std::wstring& msg)
{
    OutputDebugStringW((L"[NitLink/DeviceEnum] " + msg + L"\n").c_str());
}

// Lowercase a wide string in place using the Windows locale-aware helper.
// Matches the convention used elsewhere in the capture layer
// (elgato_hid_4ks.cpp's PathContainsCaseInsensitive) for consistency.
std::wstring ToLower(std::wstring s)
{
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// Single Media Foundation enumeration pass. Split out from the public
// FindCaptureDevices entry point so the wrapper can retry on a
// transiently empty result without duplicating the COM walk.
std::vector<DeviceInfo> EnumerateOnce()
{
    std::vector<DeviceInfo> result;

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) return result;

    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return result;

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
    if (FAILED(hr)) return result;

    for (UINT32 i = 0; i < count; i++) {
        DeviceInfo info;
        info.index = i;

        // Get friendly name
        WCHAR* name = nullptr;
        UINT32 nameLen = 0;
        hr = devices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen);
        if (SUCCEEDED(hr) && name) {
            info.name = name;
            CoTaskMemFree(name);
        }

        // Get symbolic link
        WCHAR* link = nullptr;
        UINT32 linkLen = 0;
        hr = devices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &linkLen);
        if (SUCCEEDED(hr) && link) {
            info.symbolicLink = link;
            CoTaskMemFree(link);
        }

        result.push_back(std::move(info));
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    return result;
}

} // anonymous namespace


std::vector<DeviceInfo> DeviceEnumerator::FindCaptureDevices()
{
    // Media Foundation can return an empty enumeration during brief
    // windows: a Device Manager disable/enable cycle on a neighboring
    // device, a capture-card driver service still spinning up after a
    // cold boot, or the device being briefly claimed and released by
    // another process. Retry a small number of times before treating
    // an empty result as final. Each attempt is cheap (a few COM calls
    // and a friendly-name read per device); three attempts at 500 ms
    // spacing covers the common transient windows without adding
    // noticeable startup latency in the steady state.
    constexpr int kMaxAttempts = 3;
    constexpr auto kRetryDelay = std::chrono::milliseconds(500);

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto result = EnumerateOnce();
        if (!result.empty() || attempt == kMaxAttempts - 1) {
            if (attempt > 0) {
                std::wstringstream ss;
                ss << L"Enumeration succeeded on attempt " << (attempt + 1)
                   << L" with " << result.size() << L" device(s)";
                DebugLog(ss.str());
            }
            return result;
        }
        DebugLog(L"Enumeration returned 0 devices, retrying");
        std::this_thread::sleep_for(kRetryDelay);
    }
    return {};
}

int DeviceEnumerator::PickPreferredDevice(const std::vector<DeviceInfo>& devices,
                                          const std::wstring& preferredName)
{
    if (devices.empty()) return -1;

    if (!preferredName.empty()) {
        const std::wstring needle = ToLower(preferredName);

        // Strategy 1: case-insensitive exact name match.
        for (size_t i = 0; i < devices.size(); ++i) {
            if (ToLower(devices[i].name) == needle) {
                return static_cast<int>(i);
            }
        }

        // Strategy 2: case-insensitive substring match. Allows a saved
        // short identifier like "Elgato 4K Pro" to match a longer
        // enumerated name when drivers append suffixes or instance
        // numbers.
        for (size_t i = 0; i < devices.size(); ++i) {
            if (ToLower(devices[i].name).find(needle) != std::wstring::npos) {
                return static_cast<int>(i);
            }
        }
    }

    // Strategy 3: first device whose name contains "Elgato". The common
    // failure mode without this bias is a laptop with both an integrated
    // webcam and a connected Elgato capture card, where the webcam tends
    // to win MF enumeration order and gets opened by default.
    {
        const std::wstring elgato = L"elgato";
        for (size_t i = 0; i < devices.size(); ++i) {
            if (ToLower(devices[i].name).find(elgato) != std::wstring::npos) {
                return static_cast<int>(i);
            }
        }
    }

    // Strategy 4: fall back to the first device in MF enumeration order.
    return 0;
}

std::vector<DeviceInfo> DeviceEnumerator::FindCaptureCards()
{
    auto all = FindCaptureDevices();

    // Known capture card identifiers in device names
    static const std::wstring captureCardKeywords[] = {
        L"Elgato", L"AVerMedia", L"Magewell", L"Blackmagic",
        L"Game Capture", L"Live Gamer", L"4K", L"Cam Link",
        L"Razer Ripsaw", L"EVGA XR1", L"Startech",
    };

    // Known webcam identifiers to exclude
    static const std::wstring webcamKeywords[] = {
        L"Webcam", L"FaceTime", L"Integrated Camera", L"IR Camera",
        L"Windows Hello", L"Brio", L"C920", L"C922", L"C930",
        L"StreamCam", L"Logitech HD",
    };

    std::vector<DeviceInfo> filtered;
    for (auto& dev : all) {
        bool isWebcam = false;
        for (const auto& kw : webcamKeywords) {
            if (dev.name.find(kw) != std::wstring::npos) {
                isWebcam = true;
                break;
            }
        }
        if (!isWebcam) {
            filtered.push_back(std::move(dev));
        }
    }

    // If filtering removed everything, return all devices
    // (user might have an unusual capture card)
    return filtered.empty() ? all : filtered;
}

int DeviceEnumerator::FindDeviceByName(const std::vector<DeviceInfo>& devices,
                                       const std::wstring& name)
{
    if (devices.empty() || name.empty()) return -1;

    const std::wstring needle = ToLower(name);

    // Strategy 1: case-insensitive exact name match.
    for (size_t i = 0; i < devices.size(); ++i) {
        if (ToLower(devices[i].name) == needle) {
            return static_cast<int>(i);
        }
    }

    // Strategy 2: case-insensitive substring match. Handles the case
    // where the saved or user-supplied name is a short identifier and
    // the enumerated friendly name has extra suffixes such as a
    // driver-instance number.
    for (size_t i = 0; i < devices.size(); ++i) {
        if (ToLower(devices[i].name).find(needle) != std::wstring::npos) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

} // namespace NitLink
