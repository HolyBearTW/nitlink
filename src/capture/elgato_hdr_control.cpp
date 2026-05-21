#include "elgato_hdr_control.h"

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>

#include <atlbase.h>   // CComPtr
#include <cwchar>      // wcsstr, wcslen
#include <sstream>
#include <iomanip>     // setw, setfill for hex dump

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace NitLink {

namespace {

// Elgato Game Capture SDK custom property set GUID.
// Documented in github.com/elgatosf/capture-device-support (MK.2 SDK).
// Property 722 = HDR tonemapping toggle. Value 0 = OFF, value 1 = ON.
constexpr GUID kElgatoCustomPropertySet =
    { 0xD1E5209F, 0x68FD, 0x4529, { 0xBE, 0xE0, 0x5E, 0x7A, 0x1F, 0x47, 0x92, 0x26 } };

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
            if (SUCCEEDED(pb->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) {
                std::wstring friendlyName = v.bstrVal;
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
                    if (FAILED(hr) || bytesReturned < 16) {
                        // Always log Get failures: they're unusual even from a
                        // polling caller and worth surfacing if they happen.
                        Log(L"ReadHDRSource: low-half Get() failed");
                    } else {
                        hr = ps->Get(kElgatoCustomPropertySet, kPropertyHDRPacketHigh,
                                     nullptr, 0, &packet[16], 16, &bytesReturned);
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

} // namespace NitLink