#include "elgato_hdr_control.h"

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>

#include <atlbase.h>   // CComPtr
#include <cwchar>      // wcsstr, wcslen
#include <sstream>
#include <iomanip>     // setw, setfill for hex dump
#include <vector>      // Detect4KXSourceMode: exact-sized XU read buffers
#include <chrono>      // Source4KXPoller: poll cadence timing

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ksuser.lib")   // KS interface support for the 4K X XU probe

namespace NitLink {

namespace {

// Elgato Game Capture SDK custom property set GUID.
// Documented in github.com/elgatosf/capture-device-support (MK.2 SDK).
// Property 722 = HDR tonemapping toggle. Value 0 = OFF, value 1 = ON.
constexpr GUID kElgatoCustomPropertySet =
    { 0xD1E5209F, 0x68FD, 0x4529, { 0xBE, 0xE0, 0x5E, 0x7A, 0x1F, 0x47, 0x92, 0x26 } };

// Elgato 4K X UVC Extension Unit #4 GUID (from 13bm/elgato4k-linux). The X
// does NOT implement the Pro's custom set above (it returns E_PROP_SET_
// UNSUPPORTED); its vendor controls live on this XU instead, with a trigger
// (selector 2) + payload (selector 1) two-packet protocol on VideoControl.
constexpr GUID k4KXExtensionUnit =
    { 0x961073C7, 0x49F7, 0x44F2, { 0xAB, 0x42, 0xE9, 0x40, 0x40, 0x59, 0x40, 0xC2 } };

// Well-known KS interface IIDs, defined locally to sidestep the DEFINE_GUIDEX /
// INITGUID linkage dance in ks.h. Standard Windows SDK values.
constexpr GUID kIID_IKsTopologyInfo =
    { 0x720D4AC0, 0x7533, 0x11D0, { 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 } };
constexpr GUID kIID_IKsControl =
    { 0x28F54685, 0x06FD, 0x11D2, { 0xB2, 0x7A, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };

// 4K X XU control selectors (13bm): trigger announces the payload length,
// value carries the command/response.
constexpr ULONG kXuSelTrigger = 0x02;
constexpr ULONG kXuSelValue   = 0x01;

// Minimal local vtable declarations for the two KS proxy interfaces used here.
// ksproxy.h only forward-declares IKsTopologyInfo in this toolchain, so the
// layouts are declared here (method order matches the Windows SDK exactly)
// and bound through the explicit IIDs above via QueryInterface. Only the methods
// actually called are listed; trailing SDK methods are omitted (never called,
// so their vtable slots are irrelevant). KSP_NODE / KSPROPERTY come from ks.h.
struct IKsTopologyInfoLocal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_NumCategories(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Category(DWORD, GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NumConnections(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ConnectionInfo(DWORD, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NodeName(DWORD, WCHAR*, DWORD, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NumNodes(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NodeType(DWORD, GUID*) = 0;
};
struct IKsControlLocal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY, ULONG, void*, ULONG, ULONG*) = 0;
};

constexpr DWORD kPropertyTonemapToggle = 722;

// HDR Dynamic Range InfoFrame readout split across two property IDs.
// Each returns 16 bytes; concatenated they form the 32-byte CEA-861
// "Dynamic Range and Mastering" InfoFrame the upstream source sent
// the Elgato over HDMI. Byte[4] of the packet is the EOTF field.
constexpr DWORD kPropertyHDRPacketLow  = 720;  // bytes [0..15]
constexpr DWORD kPropertyHDRPacketHigh = 721;  // bytes [16..31]

// EOTF values per CEA-861 / SMPTE ST.2086:
//   0x00 = Traditional gamma: SDR
//   0x01 = Traditional gamma: Dolby legacy HDR
//   0x02 = SMPTE ST.2084: HDR10 (this is the PS5's HDR mode)
//   0x03 = Hybrid Log-Gamma: HLG (broadcast HDR, rare for games)
constexpr uint8_t kEotfSDR    = 0x00;
constexpr uint8_t kEotfHDR10  = 0x02;
constexpr uint8_t kEotfHLG    = 0x03;

// Helper: log a message via OutputDebugString in the standard NitLink
// format. This file intentionally avoids a project-wide logger because
// it is self-contained and may run before any such logger exists in
// some startup paths.
void Log(const std::wstring& msg) {
    std::wstringstream ss;
    ss << L"[NitLink/ElgatoHDR] " << msg << L"\n";
    OutputDebugStringW(ss.str().c_str());
}

// Format a GUID as the canonical {8-4-4-4-12} string for the 4K X scan report.
std::wstring GuidToWString(const GUID& g) {
    wchar_t buf[48];
    swprintf(buf, 48,
        L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, static_cast<unsigned>(g.Data2), static_cast<unsigned>(g.Data3),
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

// Case-insensitive substring match for wide strings. Returns true if
// 'needle' appears anywhere in 'haystack', ignoring case.
bool ContainsCaseInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    // Lowercase both via CharLowerBuffW for proper Windows-locale handling
    std::wstring h = haystack;
    std::wstring n = needle;
    CharLowerBuffW(h.data(), static_cast<DWORD>(h.size()));
    CharLowerBuffW(n.data(), static_cast<DWORD>(n.size()));
    return h.find(n) != std::wstring::npos;
}

// Walks the DirectShow video-input device enumerator and tries to find a
// filter whose FriendlyName contains 'deviceName' (case-insensitive).
// Returns the IBaseFilter on success, nullptr on failure.
//
// `quiet` suppresses the routine "Matched device: ..." log line on the
// success path. Failure paths (no devices, CoCreateInstance fail) still
// log because those indicate a real problem worth seeing. Callers that
// poll the HDR source state once per second should pass quiet=true.
CComPtr<IBaseFilter> FindDeviceFilter(const std::wstring& deviceName, bool quiet = false) {
    CComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) {
        Log(L"CoCreateInstance(SystemDeviceEnum) failed");
        return nullptr;
    }

    CComPtr<IEnumMoniker> monEnum;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monEnum, 0);
    if (hr != S_OK || !monEnum) {
        // S_FALSE = no devices in this category; not an error per se.
        Log(L"No video input devices enumerated");
        return nullptr;
    }

    CComPtr<IMoniker> mon;
    ULONG fetched = 0;
    while (monEnum->Next(1, &mon, &fetched) == S_OK) {
        CComPtr<IPropertyBag> pb;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&pb)))) {
            VARIANT v;
            VariantInit(&v);
            // IPropertyBag::Read populates v on success regardless of the
            // returned variant type, so VariantClear must run unconditionally --
            // gating it on vt == VT_BSTR leaks any non-BSTR payload. friendlyName
            // is a copy, so it stays valid after the clear.
            std::wstring friendlyName;
            if (SUCCEEDED(pb->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR && v.bstrVal) {
                friendlyName = v.bstrVal;
            }
            VariantClear(&v);

            if (ContainsCaseInsensitive(friendlyName, deviceName)) {
                CComPtr<IBaseFilter> filter;
                if (SUCCEEDED(mon->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter)))) {
                    if (!quiet) {
                        std::wstringstream ss;
                        ss << L"Matched device: " << friendlyName;
                        Log(ss.str());
                    }
                    return filter;
                }
            }
        }
        mon.Release();
    }

    return nullptr;
}

} // namespace

bool SetElgatoTonemap(const std::wstring& deviceName, bool enable) {
    // Initialize COM in apartment-threaded mode here just to be safe.
    // CoInitializeEx is reference-counted and idempotent: if the caller
    // already initialized COM (which Application::Initialize does very
    // early), this is a no-op that pairs cleanly with the matching
    // CoUninitialize below.
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwnedHere = SUCCEEDED(comHr) && comHr != S_FALSE;

    bool success = false;

    // Scope all COM-pointer work so destructors fire before CoUninitialize.
    {
        CComPtr<IBaseFilter> filter = FindDeviceFilter(deviceName);
        if (!filter) {
            Log(L"No matching capture device found, skipping tonemap toggle");
        } else {
            CComPtr<IKsPropertySet> ps;
            HRESULT hr = filter->QueryInterface(IID_PPV_ARGS(&ps));
            if (FAILED(hr) || !ps) {
                Log(L"Device does not expose IKsPropertySet (non-Elgato or older firmware)");
            } else {
                // Confirm the property is supported and writable before sending.
                // This prevents log spam on capture cards that share part of the
                // GUID space but don't implement Elgato's custom properties.
                // Diagnostic probe: log QuerySupported's raw HRESULT and the
                // support-flag bitmask, then ALWAYS attempt Set() regardless
                // of what QuerySupported reported. Some Elgato firmware
                // variants (notably the 4K S USB) appear to fail
                // QuerySupported but may still honor Set(). The previous
                // early-return on QuerySupported failure was a defensive log
                // -spam reduction, not a hard guarantee the write would fail;
                // we want the actual Set() HRESULT before concluding the
                // tonemap-control protocol is unavailable on a given card.
                DWORD supportFlags = 0;
                HRESULT qsHr = ps->QuerySupported(kElgatoCustomPropertySet,
                                                  kPropertyTonemapToggle, &supportFlags);
                {
                    std::wstringstream ss;
                    ss << L"QuerySupported(tonemap toggle) hr=0x"
                       << std::hex << static_cast<unsigned long>(qsHr)
                       << L" supportFlags=0x" << supportFlags;
                    Log(ss.str());
                }

                DWORD payload = enable ? 1u : 0u;
                HRESULT setHr = ps->Set(kElgatoCustomPropertySet, kPropertyTonemapToggle,
                                        nullptr, 0, &payload, sizeof(payload));
                {
                    std::wstringstream ss;
                    ss << L"Set(tonemap=" << (enable ? L"ON" : L"OFF")
                       << L") hr=0x" << std::hex << static_cast<unsigned long>(setHr);
                    Log(ss.str());
                }

                if (SUCCEEDED(setHr)) {
                    if (enable) {
                        Log(L"Hardware tonemap ENABLED (card will tonemap HDR10 to SDR internally)");
                    } else {
                        Log(L"Hardware tonemap DISABLED (card will pass raw HDR10)");
                    }
                    success = true;
                } else if (FAILED(qsHr)) {
                    Log(L"Both QuerySupported and Set failed: property not supported on this hardware");
                } else if (!(supportFlags & KSPROPERTY_SUPPORT_SET)) {
                    Log(L"Property advertised GET but rejected Set (older firmware or read-only on this device)");
                } else {
                    Log(L"Property advertised SET-capable but Set still failed (driver inconsistency)");
                }
            }
        }
    }

    if (comOwnedHere) {
        CoUninitialize();
    }
    return success;
}

bool DisableElgatoTonemap(const std::wstring& deviceName) {
    return SetElgatoTonemap(deviceName, false);
}

bool EnableElgatoTonemap(const std::wstring& deviceName) {
    return SetElgatoTonemap(deviceName, true);
}

HDRSourceInfo ReadElgatoHDRSource(const std::wstring& deviceName, bool quiet) {
    HDRSourceInfo info{};

    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwnedHere = SUCCEEDED(comHr) && comHr != S_FALSE;

    {
        CComPtr<IBaseFilter> filter = FindDeviceFilter(deviceName, quiet);
        if (!filter) {
            if (!quiet) Log(L"ReadHDRSource: no matching device, treating as SDR");
        } else {
            CComPtr<IKsPropertySet> ps;
            HRESULT hr = filter->QueryInterface(IID_PPV_ARGS(&ps));
            if (FAILED(hr) || !ps) {
                if (!quiet) Log(L"ReadHDRSource: device does not expose IKsPropertySet");
            } else {
                // Confirm both halves of the packet are readable before issuing Get.
                // The MK.2 SDK splits the InfoFrame across two property IDs;
                // some cards only implement one half. Both halves are required.
                DWORD supportLow = 0, supportHigh = 0;
                HRESULT hrLow  = ps->QuerySupported(kElgatoCustomPropertySet,
                                                   kPropertyHDRPacketLow, &supportLow);
                HRESULT hrHigh = ps->QuerySupported(kElgatoCustomPropertySet,
                                                   kPropertyHDRPacketHigh, &supportHigh);
                if (!quiet) {
                    std::wstringstream ss;
                    ss << L"ReadHDRSource: QuerySupported(720) hr=0x"
                       << std::hex << static_cast<unsigned long>(hrLow)
                       << L" flags=0x" << supportLow
                       << L"; QuerySupported(721) hr=0x"
                       << static_cast<unsigned long>(hrHigh)
                       << L" flags=0x" << supportHigh;
                    Log(ss.str());
                }
                const bool bothGet = SUCCEEDED(hrLow)  && (supportLow  & KSPROPERTY_SUPPORT_GET) &&
                                     SUCCEEDED(hrHigh) && (supportHigh & KSPROPERTY_SUPPORT_GET);
                if (!bothGet) {
                    if (!quiet) Log(L"ReadHDRSource: device exposes neither / only one half of HDR packet");
                } else {
                    // Read both halves into a 32-byte buffer.
                    uint8_t packet[32] = {0};
                    DWORD bytesReturned = 0;

                    hr = ps->Get(kElgatoCustomPropertySet, kPropertyHDRPacketLow,
                                 nullptr, 0, &packet[0], 16, &bytesReturned);
                    if (!quiet || FAILED(hr) || bytesReturned < 16) {
                        std::wstringstream ss;
                        ss << L"ReadHDRSource: Get(720) hr=0x"
                           << std::hex << static_cast<unsigned long>(hr)
                           << std::dec << L" bytes=" << bytesReturned;
                        Log(ss.str());
                    }
                    if (FAILED(hr) || bytesReturned < 16) {
                        // Always log Get failures: they're unusual even from a
                        // polling caller and worth surfacing if they happen.
                        Log(L"ReadHDRSource: low-half Get() failed");
                    } else {
                        bytesReturned = 0;
                        hr = ps->Get(kElgatoCustomPropertySet, kPropertyHDRPacketHigh,
                                     nullptr, 0, &packet[16], 16, &bytesReturned);
                        if (!quiet || FAILED(hr) || bytesReturned < 16) {
                            std::wstringstream ss;
                            ss << L"ReadHDRSource: Get(721) hr=0x"
                               << std::hex << static_cast<unsigned long>(hr)
                               << std::dec << L" bytes=" << bytesReturned;
                            Log(ss.str());
                        }
                        if (FAILED(hr) || bytesReturned < 16) {
                            Log(L"ReadHDRSource: high-half Get() failed");
                        } else {
                            // Decode the EOTF byte. The packet structure starts
                            // with a 3-byte InfoFrame header (type/version/len)
                            // and a 1-byte checksum, so the EOTF lands at byte[4].
                            info.propertyAccessible = true;
                            info.rawEotf = packet[4];
                            info.isHDR10 = (packet[4] == kEotfHDR10);
                            info.isHLG   = (packet[4] == kEotfHLG);

                            // Verbose diagnostics: full packet hex dump + decoded
                            // EOTF line. Suppressed when the caller is polling
                            // (would emit four log lines per second forever) but
                            // kept for one-shot callers like Initialize and the
                            // standalone elgato_hdr_test: the hex is useful when
                            // diagnosing weird HDMI sources.
                            if (!quiet) {
                                {
                                    std::wstringstream hx;
                                    hx << L"HDR packet [00..15]:";
                                    for (int i = 0; i < 16; i++) {
                                        hx << L" " << std::hex << std::setw(2)
                                           << std::setfill(L'0') << static_cast<int>(packet[i]);
                                    }
                                    Log(hx.str());
                                }
                                {
                                    std::wstringstream hx;
                                    hx << L"HDR packet [16..31]:";
                                    for (int i = 16; i < 32; i++) {
                                        hx << L" " << std::hex << std::setw(2)
                                           << std::setfill(L'0') << static_cast<int>(packet[i]);
                                    }
                                    Log(hx.str());
                                }

                                std::wstringstream ss;
                                ss << L"HDR InfoFrame read: EOTF=0x"
                                   << std::hex << static_cast<int>(packet[4])
                                   << std::dec;
                                if (info.isHDR10)      ss << L" (ST.2084 / HDR10), auto-detect: HDR10";
                                else if (info.isHLG)   ss << L" (HLG), treated as SDR for v1 (no HLG path yet)";
                                else if (packet[4] == kEotfSDR) ss << L" (SDR), auto-detect: SDR";
                                else                   ss << L" (unknown), auto-detect: SDR fallback";
                                Log(ss.str());
                            }
                        }
                    }
                }
            }
        }
    }

    if (comOwnedHere) {
        CoUninitialize();
    }
    return info;
}

// ===========================================================================
// Current source mode readout for the Elgato 4K Pro.
//
// Reads property 208 (source fps) and property 210 (source resolution
// packed as height-LE16 + width-LE16) from the Elgato custom property
// set. Both populate only after the capture filter has been opened.
// ===========================================================================
Source4KProMode Detect4KProSourceMode(const std::wstring& deviceName) {
    Source4KProMode result;

    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwnedHere = SUCCEEDED(comHr) && comHr != S_FALSE;

    {
        CComPtr<IBaseFilter> filter = FindDeviceFilter(deviceName, /*quiet=*/ true);
        if (filter) {
            CComPtr<IKsPropertySet> ps;
            HRESULT hr = filter->QueryInterface(IID_PPV_ARGS(&ps));
            if (SUCCEEDED(hr) && ps) {
                DWORD fpsBuf = 0;
                DWORD modeBuf = 0;
                DWORD returned = 0;

                HRESULT hrFps = ps->Get(
                    kElgatoCustomPropertySet, 208,
                    nullptr, 0, &fpsBuf, sizeof(fpsBuf), &returned);

                HRESULT hrMode = ps->Get(
                    kElgatoCustomPropertySet, 210,
                    nullptr, 0, &modeBuf, sizeof(modeBuf), &returned);

                if (SUCCEEDED(hrFps) && SUCCEEDED(hrMode) &&
                    fpsBuf > 0 && modeBuf != 0) {
                    // Property 210 packs height in the low 16 bits and
                    // width in the high 16 bits, both little-endian.
                    // Example at 4K: 70 08 00 0f -> height=0x0870=2160,
                    // width=0x0F00=3840.
                    result.detected = true;
                    result.fps      = fpsBuf;
                    result.height   = static_cast<uint32_t>(modeBuf & 0xFFFF);
                    result.width    = static_cast<uint32_t>((modeBuf >> 16) & 0xFFFF);

                    std::wstringstream ss;
                    ss << L"Detect4KProSourceMode: source is "
                       << result.width << L"x" << result.height
                       << L" @ " << result.fps << L"fps";
                    Log(ss.str());
                }
            }
        }
    }

    if (comOwnedHere) {
        CoUninitialize();
    }
    return result;
}

// ===========================================================================
// Current source mode for the Elgato 4K X (see header). Queries the UVC XU #4
// command mailbox via IKsControl on the DEV_SPECIFIC topology node: reg 0x37
// for timing (resolution + fps), 0x65 for the HDR DRM InfoFrame, 0x4b for the
// SPD source name. Request/response: each query WRITES to the XU command port
// (entity 0x04, sel 0x01/0x02) then reads the reply; writes never touch the
// HID or processing path, so it is safe. Register map is firmware-specific
// (see header).
// ===========================================================================
// Locate the 4K X XU node (KSNODETYPE_DEV_SPECIFIC) on an already-open capture
// filter and bind its IKsControl. Returns the node index, or -1 when the filter
// is not a 4K X or exposes no XU node; on success outCtrl holds the bound control
// interface. Split out so the poll thread can open the filter once and reuse it.
// Reopening a DirectShow filter every poll churned the device and destabilized
// USB enumeration; the Elgato app keeps one handle open for its whole session.
static LONG FindXuNode(IBaseFilter* filter, CComPtr<IKsControlLocal>& outCtrl) {
    if (!filter) return -1;
    CComPtr<IKsControlLocal>      ctrl;
    CComPtr<IKsTopologyInfoLocal> topo;
    if (FAILED(filter->QueryInterface(kIID_IKsControl,
                   reinterpret_cast<void**>(&ctrl))) || !ctrl ||
        FAILED(filter->QueryInterface(kIID_IKsTopologyInfo,
                   reinterpret_cast<void**>(&topo))) || !topo) {
        return -1;
    }
    // The XU is the KSNODETYPE_DEV_SPECIFIC topology node.
    static const GUID kDevSpecific =
        { 0x941C7AC0, 0xC559, 0x11D0, { 0x8A, 0x2B, 0x00, 0xA0, 0xC9, 0x25, 0x5A, 0xC1 } };
    DWORD numNodes = 0; topo->get_NumNodes(&numNodes);
    for (DWORD i = 0; i < numNodes; ++i) {
        GUID nt{};
        if (SUCCEEDED(topo->get_NodeType(i, &nt)) && IsEqualGUID(nt, kDevSpecific)) {
            outCtrl = ctrl;
            return static_cast<LONG>(i);
        }
    }
    return -1;
}

// Read the 4K X source mode over an ALREADY-OPEN XU handle. Issues the app's
// readiness poll (reg 0x67 via the a0 opcode) first, then the timing, HDR, and
// SPD register reads, paced, with every write confined to the XU command port
// (entity 0x04, sel 0x01/0x02). Opens and closes no device handle, so it runs
// repeatedly on one held connection without the reopen churn. Decoded from the
// app's request set: reg 0x37 -> 0x2A timing block (resolution + fps), reg 0x65
// -> 0x22 HDR DRM InfoFrame (byte4 == 0x87 when HDR), reg 0x4b -> 0x1E SPD
// InfoFrame (source product). Register map verified on firmware 250815, 241210,
// and 250210.
static bool ReadXuBlocks(IKsControlLocal* ctrl, LONG node, Source4KProMode& result) {
    if (!ctrl || node < 0) return false;

    auto np = [&](ULONG sel, ULONG flag, void* d, ULONG l, ULONG* r) -> HRESULT {
        KSP_NODE kp{};
        kp.Property.Set   = k4KXExtensionUnit;
        kp.Property.Id    = sel;
        kp.Property.Flags = flag | KSPROPERTY_TYPE_TOPOLOGY;
        kp.NodeId         = static_cast<ULONG>(node);
        return ctrl->KsProperty(reinterpret_cast<PKSPROPERTY>(&kp),
                                sizeof(kp), d, l, r);
    };
    auto rdU16 = [](const uint8_t* p) -> uint16_t {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    };

    // One request/response round against the XU mailbox: write the app's request
    // to the command port, poll the 2-byte header for the reply length, read the
    // reply at EXACT size. Fills 'out' on success.
    auto readBlock = [&](const uint8_t* reqBytes, int reqLen,
                         std::vector<uint8_t>& out) -> bool {
        ULONG    w = 0;
        uint16_t rl = static_cast<uint16_t>(reqLen);
        np(kXuSelTrigger, KSPROPERTY_TYPE_SET, &rl, sizeof(rl), &w);
        Sleep(3);
        np(kXuSelValue, KSPROPERTY_TYPE_SET,
           const_cast<uint8_t*>(reqBytes), static_cast<ULONG>(reqLen), &w);
        Sleep(3);
        uint16_t len = 0;
        for (int p = 0; p < 16 && len == 0; ++p) {
            uint8_t hdr[2] = {0}; ULONG hn = 0;
            if (SUCCEEDED(np(kXuSelTrigger, KSPROPERTY_TYPE_GET, hdr, sizeof(hdr), &hn))
                && hn >= 2) {
                len = rdU16(hdr);
            }
            if (len == 0) Sleep(4);
        }
        if (len == 0 || len > 256) return false;
        out.assign(len, 0); ULONG rn = 0;
        if (FAILED(np(kXuSelValue, KSPROPERTY_TYPE_GET, out.data(), len, &rn))
            || rn < 2) {
            return false;
        }
        out.resize(rn);
        return true;
    };

    // Readiness poll the app issues before each read cycle (reg 0x67). The reply
    // is not decoded; sending it mirrors the app's handshake so the card is in
    // the state it expects before the register reads.
    static const uint8_t kReqReady[9] =
        { 0xA0,0x06,0x00,0x00,0x67,0x00,0x00,0x00,0xF3 };
    std::vector<uint8_t> rdy;
    readBlock(kReqReady, 9, rdy);

    // Resolution + fps: request 0x37 -> 0x2A timing block.
    static const uint8_t kReqTiming[9] =
        { 0xA1,0x06,0x00,0x00,0x37,0x00,0x00,0x00,0x22 };
    std::vector<uint8_t> tb;
    if (readBlock(kReqTiming, 9, tb) && tb.size() >= 20 && tb[1] == 0x2A) {
        result.height   = rdU16(&tb[10]);
        result.width    = rdU16(&tb[12]);
        result.fps      = (rdU16(&tb[18]) + 50) / 100;  // Hz*100, rounded
        result.detected = (result.width != 0 && result.height != 0);
    }

    // HDR: request 0x65 -> 0x22 DRM InfoFrame. byte4 == 0x87 => HDR on.
    static const uint8_t kReqHdr[9] =
        { 0xA1,0x06,0x00,0x00,0x65,0x00,0x00,0x00,0xF4 };
    std::vector<uint8_t> hb;
    if (readBlock(kReqHdr, 9, hb) && hb.size() >= 5 && hb[1] == 0x22) {
        result.hdrActive = (hb[4] == 0x87);
    }

    // Source device: request 0x4b -> 0x1E SPD InfoFrame. The product description
    // is 16 bytes of ASCII at offset 15 (e.g. "PS5").
    static const uint8_t kReqSpd[9] =
        { 0xA1,0x06,0x00,0x00,0x4B,0x00,0x00,0x00,0x0E };
    std::vector<uint8_t> sb;
    if (readBlock(kReqSpd, 9, sb) && sb.size() >= 31 && sb[1] == 0x1E) {
        std::wstring name;
        for (int i = 0; i < 16; ++i) {
            uint8_t c = sb[15 + i];
            if (c == 0) break;
            if (c >= 32 && c < 127) name += static_cast<wchar_t>(c);
        }
        result.sourceName = name;
    }
    return result.detected;
}

Source4KProMode Detect4KXSourceMode(const std::wstring& deviceName) {
    Source4KProMode result;

    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwnedHere = SUCCEEDED(comHr) && comHr != S_FALSE;
    {
        CComPtr<IBaseFilter>     filter = FindDeviceFilter(deviceName, /*quiet=*/true);
        CComPtr<IKsControlLocal> ctrl;
        LONG node = FindXuNode(filter, ctrl);
        if (node >= 0) {
            ReadXuBlocks(ctrl, node, result);
        }
    }
    if (comOwnedHere) {
        CoUninitialize();
    }
    return result;
}

// ===========================================================================
// Source4KXPoller: background 4K X source-mode monitor (see header). Mirrors
// HDRSourcePoller's threading model: the worker is the sole writer to m_mode +
// m_hasUpdate, writes m_mode (under m_mutex) strictly before setting the flag,
// and the main thread test-and-clears via AcceptUpdate.
// ===========================================================================
void Source4KXPoller::Start(const std::wstring& deviceName, const Source4KProMode& initial) {
    if (m_running.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_mode = initial;
    }
    m_hasUpdate.store(false, std::memory_order_release);
    m_stop.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&Source4KXPoller::PollerThreadMain, this, deviceName);
}

void Source4KXPoller::Stop() {
    if (!m_running.load(std::memory_order_acquire) && !m_thread.joinable()) return;
    m_stop.store(true, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false, std::memory_order_release);
}

bool Source4KXPoller::AcceptUpdate(Source4KProMode* out) {
    if (!m_hasUpdate.exchange(false, std::memory_order_acq_rel)) return false;
    if (out) {
        std::lock_guard<std::mutex> lk(m_mutex);
        *out = m_mode;
    }
    return true;
}

void Source4KXPoller::PollerThreadMain(std::wstring deviceName) {
    using namespace std::chrono_literals;

    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwned = SUCCEEDED(comHr) && comHr != S_FALSE;

    // Open the XU connection ONCE and hold it for the thread lifetime. The prior
    // path reopened a DirectShow filter every poll; that churn destabilized the
    // card's USB enumeration. The Elgato app keeps one handle open for its whole
    // session and reads the same registers continuously, mirrored here.
    CComPtr<IBaseFilter>     filter = FindDeviceFilter(deviceName, /*quiet=*/true);
    CComPtr<IKsControlLocal> ctrl;
    LONG node = FindXuNode(filter, ctrl);

    // ~2.0s cadence, broken into 100ms chunks so Stop() returns promptly.
    constexpr int  kChunks = 20;
    constexpr auto kChunk  = 100ms;
    while (!m_stop.load(std::memory_order_acquire)) {
        if (node >= 0 && ctrl) {
            Source4KProMode m;
            if (ReadXuBlocks(ctrl, node, m) && m.detected) {
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    changed = m.width      != m_mode.width  ||
                              m.height     != m_mode.height ||
                              m.fps        != m_mode.fps    ||
                              m.hdrActive  != m_mode.hdrActive ||
                              m.sourceName != m_mode.sourceName;
                    if (changed) m_mode = m;
                }
                if (changed) m_hasUpdate.store(true, std::memory_order_release);
            }
        }
        for (int i = 0; i < kChunks && !m_stop.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(kChunk);
    }

    if (comOwned) {
        CoUninitialize();
    }
}

} // namespace NitLink
