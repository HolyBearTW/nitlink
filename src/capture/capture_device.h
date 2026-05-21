#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace NitLink {

struct CaptureFormat {
    uint32_t width  = 3840;
    uint32_t height = 2160;
    uint32_t fps    = 60;
    uint32_t stride = 0; // bytes per row
    GUID     subtype{};  // MF media subtype (NV12, YUY2, RGB32, etc.)
    // Row order of the captured frames. Media Foundation defaults to
    // bottom-up for legacy RGB formats (GDI convention) but some drivers
    // (e.g. Elgato 4K S) deliver top-down RGB32 instead. Determined by
    // querying MF_MT_DEFAULT_STRIDE: negative stride = bottom-up,
    // positive (or absent) = top-down. The renderer uses this to choose
    // whether to apply the V-flip in the BGRA shader path. Planar formats
    // like P010/NV12 are always top-down regardless of this flag.
    bool     topDown = true;
    // Pixel value range of the captured frames. Most HDMI sources (PS5,
    // Xbox) send LIMITED range (16-235): that's the HDMI spec default.
    // Some drivers convert the YUV input to FULL range (0-255) before
    // handing it off; the Elgato 4K S does this on its NV12 output
    // path. Determined by querying MF_MT_VIDEO_NOMINAL_RANGE on the
    // negotiated media type. The shaders use this to decide whether to
    // apply the limited-to-full range expansion. Getting this wrong crushes
    // blacks (treating full as limited) or washes out the image
    // (treating limited as full).
    bool     fullRange = false;
};

struct DeviceInfo {
    std::wstring name;
    std::wstring symbolicLink;
    uint32_t     index = 0;
};

// Callback: raw frame data, size in bytes, presentation timestamp (100ns units)
using FrameCallback = std::function<void(const uint8_t*, uint32_t, int64_t)>;

class CaptureDevice {
public:
    CaptureDevice();
    ~CaptureDevice();

    bool Open(const DeviceInfo& device);
    void Close();

    bool StartCapture(FrameCallback callback);
    void StopCapture();

    // Request the P010 (10-bit BT.2020 PQ) output format on next Open().
    // Must be called BEFORE Open(): changing it later has no effect on the
    // already-opened reader. Pass true only when:
    //   1. The source is detected as HDR10 (see elgato_hdr_control.h)
    //   2. The user has the HDR toggle enabled
    //   3. The device exposes P010 (confirmed on 4K Pro; older Elgato
    //      firmware may not)
    // If P010 negotiation fails inside Open(), Open() returns false; the
    // caller should retry with this flag false to get the SDR pipeline.
    void RequestP010(bool want) { m_requestP010 = want; }
    bool IsP010Requested() const { return m_requestP010; }

    // True if the capture worker observed a fatal stream condition since the
    // last call: MF_SOURCE_READERF_ERROR (reader permanently failed),
    // MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED / _NATIVEMEDIATYPECHANGED
    // (the source renegotiated format underneath the reader: common across
    // PS5 SDR<->HDR transitions and after HDMI signal recovery), or repeated
    // ReadSample failures that exceeded the in-loop retry budget. The
    // application's run loop polls this and routes through ReconcileCaptureFormat
    // with force=true to perform a clean teardown + re-Open. Test-and-clear
    // semantics: each call returns the current state and resets it to false.
    bool ConsumeNeedsReopen() { return m_needsReopen.exchange(false); }

    CaptureFormat GetOutputFormat() const { return m_format; }
    std::wstring  GetDeviceName()   const { return m_deviceName; }
    bool          IsCapturing()     const { return m_capturing; }

    // Log every native media type this device exposes via Media Foundation.
    // Pure diagnostic: does NOT change the pipeline. Used to discover
    // whether the device offers high-bit-depth formats like P010 (10-bit
    // BT.2020 PQ, needed for real HDR10 passthrough).
    //
    // Walks the IMFStreamDescriptor for the selected video stream, iterates
    // every media type the handler exposes, and writes them to the
    // OutputDebugString channel under the [NitLink/Formats] tag.
    //
    // Safe to call after Open() succeeds. Returns true if at least one
    // format was enumerated (useful info logged), false on COM failures.
    bool LogAvailableFormats() const;

private:
    void CaptureLoop();
    bool NegotiateFormat(IMFMediaSource* source);

    ComPtr<IMFMediaSource>  m_source;
    ComPtr<IMFSourceReader> m_reader;

    CaptureFormat   m_format;
    std::wstring    m_deviceName;
    FrameCallback   m_callback;
    
    std::thread       m_captureThread;
    std::atomic<bool> m_capturing{false};

    // Set via RequestP010() before Open(). When true, Open() will try
    // MFVideoFormat_P010 first (and ONLY P010: no fallback within HDR
    // path, the caller decides whether to retry without P010).
    bool m_requestP010 = false;

    // Set by CaptureLoop on fatal stream conditions; consumed by the
    // application's run loop via ConsumeNeedsReopen(). When set, the worker
    // thread has already exited and the application is expected to force a
    // reconcile to bring capture back up.
    std::atomic<bool> m_needsReopen{false};
};

} // namespace NitLink
