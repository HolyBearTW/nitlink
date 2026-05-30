#include "elgato_hid_4ks.h"

#include <windows.h>
#include <setupapi.h>

extern "C" {
#include <hidsdi.h>
}

#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdio>

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
constexpr BYTE   k4KSToneMapOn             = 0x01; // on-card HDR-to-SDR
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
// and its vendor command interface as another. The vendor interface is
// the target for tonemap and source-detection writes. Microsoft's
// enumerator embeds the USB interface index in the device path
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
    // for the card's on-card HDR-to-SDR conversion. Bytes 7..254 are
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

// ===========================================================================
// HDMI source identifier readout.
//
// Reads sub_cmd 0x14 on the vendor query channel and decodes the HDMI
// Source Product Descriptor InfoFrame. The 4K S MCU stores the
// descriptor the connected source delivered over HDMI.
//
// Wire format: write 255-byte report with header
// 06 06 07 55 01 14 20 ..zeros.., read 2047 bytes from rid=0x05.
// Response layout: byte 0 = status, bytes 1+ = HDMI SPD InfoFrame.
// ===========================================================================

HdmiSourceInfo Detect4KSHdmiSource()
{
    HdmiSourceInfo info;
    info.detected = false;
    info.label    = L"";

    const std::wstring path = FindElgato4KSHIDPath();
    if (path.empty()) {
        // No 4K S present. Silent return matches Set4KSTonemap behavior
        // on non-4K-S devices.
        return info;
    }

    HANDLE h = CreateFileW(path.c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr,
                            OPEN_EXISTING,
                            0,
                            nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"Detect4KSHdmiSource: CreateFile failed, err=" +
            FormatHex32(GetLastError()));
        return info;
    }

    // Build the SET_REPORT query: sub_cmd 0x14 with param 0x20.
    BYTE writeBuf[255] = {};
    writeBuf[0] = 0x06; // rid
    writeBuf[1] = 0x06; // header constant
    writeBuf[2] = 0x07; // channel: query
    writeBuf[3] = 0x55; // magic: control plane
    writeBuf[4] = 0x01; // op: read
    writeBuf[5] = 0x14; // sub_cmd: HDMI source identifier
    writeBuf[6] = 0x20; // param: expected response payload length = 32 bytes

    if (!HidD_SetOutputReport(h, writeBuf, static_cast<ULONG>(sizeof(writeBuf)))) {
        Log(L"Detect4KSHdmiSource: HidD_SetOutputReport failed, err=" +
            FormatHex32(GetLastError()));
        CloseHandle(h);
        return info;
    }

    // Brief wait between SET and GET. Studio's wire-level delta is ~113us;
    // 50ms is comfortably above any plausible MCU response-queue latency
    // and well below anything that would feel like a startup hang.
    Sleep(50);

    // Read with ReportBufferLength=2047. NOT 2048. The MCU's declared
    // input report 0x05 wire length is exactly 2047; passing 2048
    // over-reads by one byte and triggers a STALL that surfaces as
    // ERROR_GEN_FAILURE (0x1F).
    BYTE readBuf[2048] = {};
    readBuf[0] = 0x05;

    if (!HidD_GetInputReport(h, readBuf, static_cast<ULONG>(2047))) {
        Log(L"Detect4KSHdmiSource: HidD_GetInputReport failed, err=" +
            FormatHex32(GetLastError()));
        CloseHandle(h);
        return info;
    }

    CloseHandle(h);

    // Log the raw response bytes so unknown sources can have markers
    // added later by inspection. First 32 bytes is the full payload
    // length for sub_cmd 0x14.
    {
        std::wstringstream ss;
        ss << L"Detect4KSHdmiSource: raw response[0..32] =";
        for (size_t b = 0; b < 32; ++b) {
            ss << L" " << std::hex << std::setw(2) << std::setfill(L'0')
               << static_cast<unsigned>(readBuf[b]);
        }
        Log(ss.str());
    }

    // The MCU status byte (readBuf[0]) must be 0x00 for the response to be
    // valid. On a non-success status the descriptor region holds error or
    // stale bytes, and matching markers against it can yield a false-positive
    // source identification, so bail out here (info.detected stays false).
    // The raw log above still captures the bytes for diagnosis.
    if (readBuf[0] != 0x00) {
        Log(L"Detect4KSHdmiSource: non-success MCU status byte, skipping decode");
        return info;
    }

    // Decode known markers.
    //
    // Observed layout for PS5:
    //   readBuf[0]    = 0x00              (status: success)
    //   readBuf[1..3] = 83 01 19          (descriptor header)
    //   readBuf[4..8] = "_SCEI"           (vendor marker, ASCII)
    //   readBuf[9..12] = 00 00 00 00      (padding)
    //   readBuf[13..15] = "PS5"           (product marker, ASCII)
    //   readBuf[16..28] = zero padding
    //   readBuf[29..32] = 08 00 00 01     (suffix)
    //
    // Match logic: compare 5 bytes of "_SCEI" at offset 4. If matched,
    // check 3 bytes at offset 13 for the specific PlayStation model.

    auto bytesEqual = [&](size_t offset, const BYTE* expected, size_t n) -> bool {
        for (size_t i = 0; i < n; ++i) {
            if (readBuf[offset + i] != expected[i]) return false;
        }
        return true;
    };

    static const BYTE kMarkerSCEI[] = { 0x5f, 0x53, 0x43, 0x45, 0x49 }; // "_SCEI"
    static const BYTE kMarkerPS5[]  = { 0x50, 0x53, 0x35 };             // "PS5"
    // (Add PS4 / PS3 / Xbox / etc. markers here as additional sources
    // get captured. Each console family writes its own SPD InfoFrame;
    // HDMI CEC Source ID specs describe the values to expect.)

    if (bytesEqual(4, kMarkerSCEI, sizeof(kMarkerSCEI))) {
        info.detected = true;
        if (bytesEqual(13, kMarkerPS5, sizeof(kMarkerPS5))) {
            info.label = L"PlayStation 5";
        } else {
            info.label = L"PlayStation (unknown model)";
        }
    }
    // else: no recognized marker. info.detected stays false; raw bytes
    // are in the log above so additional markers can be added.

    return info;
}


// ===========================================================================
// Direct HDR signal-state readout for the 4K S.
//
// Refreshes the MCU's HDR Metadata cache from current HDMI source state
// (SET sub_cmd 0x13), waits for the re-parse to complete, then reads
// sub_cmd 0x09. Byte 1 of the 33-byte response is the discriminator:
// 0x87 == HDR Static Metadata InfoFrame present, 0x00 == SDR.
//
// See the header for the full protocol description and the reason the
// SET 0x13 refresh trigger is mandatory before each read (the MCU
// caches the last-parsed InfoFrame indefinitely; without the refresh
// the read returns a stale value from the previous HDR session).
// ===========================================================================

HdrMetadataProbeResult Probe4KSHdrMetadata()
{
    HdrMetadataProbeResult result{};
    result.queryOk   = false;
    result.hdrActive = false;
    result.eotf      = 0x00;
    // result.raw is zero-initialized by the value-initializer above.

    const std::wstring path = FindElgato4KSHIDPath();
    if (path.empty()) {
        // No 4K S present. Silent return matches Set4KSTonemap and
        // Detect4KSHdmiSource behavior on non-4K-S devices.
        return result;
    }

    HANDLE h = CreateFileW(path.c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr,
                            OPEN_EXISTING,
                            0,
                            nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"Probe4KSHdrMetadata: CreateFile failed, err=" +
            FormatHex32(GetLastError()));
        return result;
    }

    // ---- Step 1: SET sub_cmd 0x13 (HDR Metadata refresh trigger) ----
    // Tells the MCU to re-parse the HDR Static Metadata InfoFrame from
    // the current HDMI source state into sub_cmd 0x09's register.
    // Without this, 0x09 returns the cached value from whenever the MCU
    // last parsed an InfoFrame. The cache persists indefinitely until
    // USB power-cycle or a fresh SET 0x13.
    BYTE writeBuf[255] = {};
    writeBuf[0] = 0x06; // rid
    writeBuf[1] = 0x06; // header constant
    writeBuf[2] = 0x06; // channel: set-state
    writeBuf[3] = 0x55; // magic: control plane
    writeBuf[4] = 0x02; // op: write
    writeBuf[5] = 0x13; // sub_cmd: refresh trigger
    writeBuf[6] = 0x01; // param: value to set

    if (!HidD_SetOutputReport(h, writeBuf, static_cast<ULONG>(sizeof(writeBuf)))) {
        Log(L"Probe4KSHdrMetadata: refresh-trigger SET failed, err=" +
            FormatHex32(GetLastError()));
        CloseHandle(h);
        return result;
    }
    Log(L"Probe4KSHdrMetadata: refresh trigger sent, waiting 1500ms for MCU re-parse...");

    // ---- Step 2: wait for MCU to re-parse the InfoFrame ----
    // Observed re-parse latency is ~1.3s on PS5 sources. 1500ms is the
    // comfortable headroom value; sub-second waits return transitional
    // or partial register contents.
    Sleep(1500);

    // ---- Step 3: QUERY sub_cmd 0x09 (HDR Metadata register) ----
    ZeroMemory(writeBuf, sizeof(writeBuf));
    writeBuf[0] = 0x06; // rid
    writeBuf[1] = 0x06; // header constant
    writeBuf[2] = 0x07; // channel: query
    writeBuf[3] = 0x55; // magic: control plane
    writeBuf[4] = 0x01; // op: read
    writeBuf[5] = 0x09; // sub_cmd: HDR Metadata register
    writeBuf[6] = 0x20; // param: expected payload length = 32 bytes
                       //        (response is 1 status byte + 32 data bytes = 33 total)

    if (!HidD_SetOutputReport(h, writeBuf, static_cast<ULONG>(sizeof(writeBuf)))) {
        Log(L"Probe4KSHdrMetadata: 0x09 QUERY SET failed, err=" +
            FormatHex32(GetLastError()));
        CloseHandle(h);
        return result;
    }

    Sleep(50);

    // ---- Step 4: GET_REPORT response ----
    // Buffer length 2047 NOT 2048: the MCU declares input report 0x05
    // as exactly 2047 bytes; over-reading by one byte triggers a STALL
    // surfacing as ERROR_GEN_FAILURE.
    BYTE readBuf[2048] = {};
    readBuf[0] = 0x05;

    if (!HidD_GetInputReport(h, readBuf, static_cast<ULONG>(2047))) {
        Log(L"Probe4KSHdrMetadata: GET_REPORT failed, err=" +
            FormatHex32(GetLastError()));
        CloseHandle(h);
        return result;
    }

    CloseHandle(h);
    result.queryOk = true;

    // Copy the 33-byte response into the result struct for caller
    // inspection. Response starts at readBuf[0] (no rid prefix) because
    // ReportBufferLength=2047 matches the on-wire report length exactly.
    for (size_t i = 0; i < 33; ++i) {
        result.raw[i] = readBuf[i];
    }

    // ---- Step 5: decode discriminator bytes ----
    // Byte 0 = MCU status (0x00 = success).
    // Byte 1 = InfoFrame type with extension flag:
    //          0x87 = CTA-861-G HDR Static Metadata InfoFrame (type 0x07
    //                 with the InfoFrame extension bit 0x80 set).
    //          0x00 = no InfoFrame parsed (source is SDR).
    // Byte 5 = EOTF byte from the InfoFrame payload (only meaningful
    //          when byte 1 == 0x87). 0x02 = ST2084 PQ, 0x03 = HLG.
    //
    // Require byte 0 == 0x00 (MCU success) BEFORE trusting byte 1. On a
    // non-success status the response may be stale or partial from an
    // incomplete re-parse, and a spurious 0x87 there would yield a false
    // HDR-active verdict. Mirrors the status-byte guard in Detect4KSHdmiSource.
    // The full 33-byte response is still logged below for diagnostics.
    if (readBuf[0] == 0x00 && readBuf[1] == 0x87) {
        result.hdrActive = true;
        result.eotf      = readBuf[5];
    }

    // Full 33-byte response log so unexpected sources or states can
    // have new decode paths added by inspection.
    {
        std::wstringstream ss;
        ss << L"Probe4KSHdrMetadata: response[0..32] =";
        for (size_t b = 0; b < 33; ++b) {
            ss << L" " << std::hex << std::setw(2) << std::setfill(L'0')
               << static_cast<unsigned>(readBuf[b]);
        }
        ss << L"  -> "
           << (result.hdrActive ? L"HDR ACTIVE" : L"SDR")
           << L" (byte[1]=0x"
           << std::hex << std::setw(2) << std::setfill(L'0')
           << static_cast<unsigned>(readBuf[1])
           << L", EOTF=0x"
           << std::setw(2) << std::setfill(L'0')
           << static_cast<unsigned>(result.eotf)
           << L")";
        Log(ss.str());
    }

    return result;
}


// ===========================================================================
// HDR-capability heuristic: source-ID maps to "user probably wants HDR".
// ===========================================================================

bool Is4KSHdmiSourceHdrCapable(const std::wstring& label)
{
    // Strings here must match Detect4KSHdmiSource()'s label outputs
    // exactly. When a new source marker is added there, add the matching
    // entry here if that source is HDR-capable by default.
    //
    // Notes per entry:
    //   PlayStation 5: outputs HDR10 PQ by default when connected to
    //     an HDR-capable display sink (NitLink presents a 4K HDR EDID).
    //   PlayStation (unknown model): covers PS4 Pro and other PlayStation
    //     variants that share the SCEI vendor marker; PS4 Pro IS
    //     HDR-capable, so defaulting to HDR is correct for that line.
    //
    // Additions when more markers land in Detect4KSHdmiSource:
    //   Xbox Series X / Series S, Xbox One X: all HDR10-capable.
    //   Nintendo Switch 2: HDR-capable per Nintendo announcements.
    //   (Switch 1 is NOT HDR-capable; don't add a generic "Nintendo"
    //   match without distinguishing Switch 1 vs 2.)
    return label == L"PlayStation 5"
        || label == L"PlayStation (unknown model)";
}

} // namespace NitLink
