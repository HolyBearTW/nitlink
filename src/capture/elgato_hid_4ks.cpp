#include "elgato_hid_4ks.h"

#include <windows.h>
#include <setupapi.h>

// hidsdi.h depends on hidpi.h's HIDP types; both ship with the Windows SDK
// under shared/. Some older toolchains need explicit inclusion order;
// the SDK header itself pulls in hidpi.h transitively on modern setups.
extern "C" {
#include <hidsdi.h>
}

#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdint>

namespace NitLink {

namespace {

// Elgato 4K S USB IDs. The card enumerates as one of two PIDs depending
// on USB host speed: 0x00af when negotiated USB 3 (5 / 10 Gbps), 0x00ae
// when forced to USB 2. The HID vendor collection lives on interface 7
// in both cases. Both PIDs accept the same tonemap command.
constexpr USHORT kElgato4KSVID       = 0x0fd9;
constexpr USHORT kElgato4KSPIDUsb3   = 0x00af;
constexpr USHORT kElgato4KSPIDUsb2   = 0x00ae;

// Protocol constants for the 4K S vendor HID Output Report.
// See header file for attribution.
constexpr BYTE   k4KSHidReportId           = 0x06;
constexpr BYTE   k4KSHidHeader[5]          = { 0x06, 0x06, 0x06, 0x55, 0x02 };
constexpr BYTE   k4KSHidSubcommandToneMap  = 0x0a;
constexpr BYTE   k4KSToneMapOff            = 0x00; // raw HDR10 passthrough
constexpr BYTE   k4KSToneMapOn             = 0x01; // internal HDR-to-SDR
constexpr size_t k4KSHidPacketSize         = 255;

void Log(const std::wstring& msg)
{
    OutputDebugStringW((L"[NitLink/Elgato4KS-HID] " + msg + L"\n").c_str());
}

std::wstring FormatHex32(DWORD value)
{
    std::wstringstream ss;
    ss << L"0x" << std::hex << std::uppercase
       << std::setw(8) << std::setfill(L'0') << value;
    return ss.str();
}

bool PathContainsCaseInsensitive(const std::wstring& path, const wchar_t* needle)
{
    std::wstring p = path;
    std::wstring n = needle;
    if (!p.empty()) CharLowerBuffW(p.data(), static_cast<DWORD>(p.size()));
    if (!n.empty()) CharLowerBuffW(n.data(), static_cast<DWORD>(n.size()));
    return p.find(n) != std::wstring::npos;
}

// Walk every HID device interface present on the system; for each, open
// just enough to query VendorID/ProductID via HidD_GetAttributes; collect
// 4K S matches; prefer the one whose device-instance path contains the
// "mi_07" substring (the vendor interface multi-interface designator).
//
// Multiple HID collections can present from the same composite USB device
// on Windows: the 4K S exposes its UVC video interface as one HID collection
// and its vendor command interface as another. We want the vendor one.
// Microsoft's enumerator embeds the USB interface index in the device path
// as "mi_07" (or "mi_00", "mi_03", etc) for composite USB devices, which
// is the reliable cross-version selector.
//
// Returns empty string when no 4K S is connected.
std::wstring FindElgato4KSHIDPath()
{
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        Log(L"SetupDiGetClassDevs(HID) failed, err=" + FormatHex32(GetLastError()));
        return L"";
    }

    std::wstring bestPath;
    bool         bestHasMi07 = false;
    int          totalMatches = 0;

    SP_DEVICE_INTERFACE_DATA ifaceData{};
    ifaceData.cbSize = sizeof(ifaceData);

    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifaceData);
         ++i) {

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &required, nullptr);
        if (required == 0) continue;

        std::vector<BYTE> buf(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail,
                                               required, nullptr, nullptr)) {
            continue;
        }

        const std::wstring path = detail->DevicePath;

        // Open with zero requested access: enough to call HidD_GetAttributes
        // without needing exclusive ownership. Share flags allow other
        // applications (e.g. Elgato Studio) to keep the device open.
        HANDLE h = CreateFileW(path.c_str(),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        BOOL ok = HidD_GetAttributes(h, &attrs);
        CloseHandle(h);

        if (!ok) continue;
        if (attrs.VendorID  != kElgato4KSVID)       continue;
        if (attrs.ProductID != kElgato4KSPIDUsb3 &&
            attrs.ProductID != kElgato4KSPIDUsb2)    continue;

        const bool hasMi07 = PathContainsCaseInsensitive(path, L"mi_07");
        ++totalMatches;

        {
            std::wstringstream ss;
            ss << L"Candidate match #" << totalMatches
               << L": VID=0x" << std::hex << attrs.VendorID
               << L" PID=0x" << attrs.ProductID
               << L" mi_07=" << (hasMi07 ? L"yes" : L"no")
               << L" path=" << path;
            Log(ss.str());
        }

        // Prefer the mi_07 candidate if visible. Otherwise keep the first
        // match as a fallback; on some driver versions the multi-interface
        // designator may not appear in the device interface path.
        if (bestPath.empty()) {
            bestPath    = path;
            bestHasMi07 = hasMi07;
        } else if (hasMi07 && !bestHasMi07) {
            bestPath    = path;
            bestHasMi07 = true;
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (bestPath.empty()) {
        Log(L"No Elgato 4K S HID device matched VID 0x0FD9 + PID 0x00AE/0x00AF "
            L"(this is the silent / expected path when running on a 4K Pro "
            L"or with no 4K S connected).");
    } else {
        std::wstringstream ss;
        ss << L"Selected HID path (mi_07=" << (bestHasMi07 ? L"yes" : L"no")
           << L"): " << bestPath;
        Log(ss.str());
    }
    return bestPath;
}

} // anonymous namespace


bool Set4KSTonemap(bool enableTonemap)
{
    const std::wstring path = FindElgato4KSHIDPath();
    if (path.empty()) {
        return false;
    }

    HANDLE h = CreateFileW(path.c_str(),
                            GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr,
                            OPEN_EXISTING,
                            0, // no FILE_FLAG_OVERLAPPED: synchronous write is fine
                            nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"CreateFile(GENERIC_WRITE) on selected HID path failed, err=" +
            FormatHex32(GetLastError()));
        return false;
    }

    // Build the 255-byte HID Output Report. Byte 0 is the HID Report ID
    // (Windows requires the report ID as the first byte of the buffer
    // for HidD_SetOutputReport / WriteFile on HID devices). Bytes 1-4
    // complete the 5-byte header. Byte 5 is the tone-map sub-command.
    // Byte 6 is the tone-map value: OFF for raw HDR10 passthrough, ON
    // for the card's internal HDR-to-SDR conversion. Bytes 7..254 are
    // zero-padded via the initializer.
    BYTE report[k4KSHidPacketSize] = {};
    report[0] = k4KSHidReportId;
    report[1] = k4KSHidHeader[1];
    report[2] = k4KSHidHeader[2];
    report[3] = k4KSHidHeader[3];
    report[4] = k4KSHidHeader[4];
    report[5] = k4KSHidSubcommandToneMap;
    report[6] = enableTonemap ? k4KSToneMapOn
                              : k4KSToneMapOff;

    bool sent = false;

    // First try: HidD_SetOutputReport. This is the standard Windows API
    // for sending a Set_Report request over the control endpoint and
    // works for most vendor HID collections.
    const wchar_t* stateName = enableTonemap ? L"ON  (0x01)" : L"OFF (0x00)";

    if (HidD_SetOutputReport(h, report, static_cast<ULONG>(sizeof(report)))) {
        std::wstringstream ss;
        ss << L"HidD_SetOutputReport succeeded: 4K S HDR tone-map "
           << stateName << L" sent.";
        Log(ss.str());
        sent = true;
    } else {
        const DWORD err1 = GetLastError();
        Log(L"HidD_SetOutputReport failed, err=" + FormatHex32(err1) +
            L"; falling back to WriteFile (interrupt-OUT-style delivery).");

        // Fallback: WriteFile. Some HID drivers route OUT reports through
        // the interrupt OUT pipe rather than the control endpoint; for
        // those, WriteFile is the right path. Same 255-byte buffer.
        DWORD written = 0;
        BOOL  ok      = WriteFile(h, report, static_cast<DWORD>(sizeof(report)),
                                   &written, nullptr);
        const DWORD err2 = ok ? 0u : GetLastError();
        if (ok && written == sizeof(report)) {
            std::wstringstream ss;
            ss << L"WriteFile succeeded (" << written
               << L" bytes): 4K S HDR tone-map " << stateName << L" sent.";
            Log(ss.str());
            sent = true;
        } else {
            std::wstringstream ss;
            ss << L"WriteFile also failed, err=" << FormatHex32(err2)
               << L" (wrote " << written << L"/" << sizeof(report)
               << L" bytes). Neither write path delivered the HID command.";
            Log(ss.str());
        }
    }

    CloseHandle(h);
    return sent;
}

} // namespace NitLink
