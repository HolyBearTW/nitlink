#pragma once

#include "capture_device.h"
#include <vector>
#include <string>

namespace NitLink {

class DeviceEnumerator {
public:
    // Find all connected video capture devices (capture cards, webcams, etc.)
    static std::vector<DeviceInfo> FindCaptureDevices();

    // Filter to just capture cards (heuristic: excludes known webcam vendors)
    static std::vector<DeviceInfo> FindCaptureCards();

    // Select the index of the device that best matches preferredName.
    // Strategy, first hit wins:
    //   1. Case-insensitive exact match on the device friendly name.
    //   2. Case-insensitive substring match (preferredName found inside
    //      the device friendly name, so a saved short identifier still
    //      matches an enumerated name with extra suffixes).
    //   3. First device whose name contains "Elgato" (bias against
    //      laptop webcams in the common laptop-plus-capture-card setup).
    //   4. Index 0 (MF enumeration order).
    // Returns -1 only when the input list is empty. Callers that have
    // already checked for empty can treat the return value as a valid
    // index into devices.
    //
    // Use this for startup selection and post-reopen fallback where a
    // best-effort match is required even if the saved preference no
    // longer matches a connected device.
    static int PickPreferredDevice(const std::vector<DeviceInfo>& devices,
                                   const std::wstring& preferredName);

    // Strict device lookup by friendly name. Strategy, first hit wins:
    //   1. Case-insensitive exact match.
    //   2. Case-insensitive substring match.
    //   3. No match: return -1.
    // Differs from PickPreferredDevice in that there is no Elgato bias
    // and no index-0 fallback. Used by explicit user device switches
    // where opening a different device than requested would be wrong:
    // if the named device is not present, the caller surfaces a
    // failure and keeps the current device.
    //
    // Returns -1 when devices is empty, when name is empty, or when no
    // device in the list matches name by either strategy.
    static int FindDeviceByName(const std::vector<DeviceInfo>& devices,
                                const std::wstring& name);
};

} // namespace NitLink
