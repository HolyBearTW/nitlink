#pragma once

#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>
#include <deque>

using Microsoft::WRL::ComPtr;

namespace NitLink {

class Overlay {
public:
    struct Stats {
        double       captureLatencyMs = 0;
        double       renderLatencyMs  = 0;
        // App ingest: real card-to-app delivery time, measured via
        // MFSampleExtension_DeviceTimestamp. Replaces the 16+8 baked
        // constants the older HUD formula added into the displayed
        // total. 0 means the device driver doesn't populate the
        // attribute (unavailable).
        double       appIngestMs      = 0;
        // Real per-frame GPU work in milliseconds, from D3D11 timestamp
        // queries bracketing the per-frame draws. 0 means the queries
        // are not yet populated (first few frames) or query creation
        // failed at startup.
        double       gpuMs            = 0;
        uint32_t     fps              = 0;   // game content frames per second
        uint32_t     captureWidth     = 0;
        uint32_t     captureHeight    = 0;
        std::wstring deviceName;
        bool         signalActive     = false; // false = capture card showing NO SIGNAL placeholder
        // Pipeline feature flags: drawn as a thin status strip at the
        // bottom of the overlay so users can see what's active without
        // opening F1. Active features show in accent color, inactive in
        // dim text.
        bool         hdrActive        = false;
        bool         nisActive        = false;
        bool         colorExpansion   = false;
    };

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    IDXGISwapChain1* swapChain, HWND hwnd);
    void Shutdown();

    // Call BEFORE the renderer resizes the swap chain -- releases the D2D bitmap
    // wrapping the backbuffer so the resize can proceed.
    void OnResizeBegin();

    // Call AFTER the renderer has resized the swap chain -- recreates the D2D
    // bitmap to wrap the new backbuffer.
    void OnResizeEnd();

    void Render(const Stats& stats);

    // Draw the branded "no signal" screen full-window. Called from the
    // app loop when the capture card reports no active HDMI signal:
    // overrides the Elgato hardware placeholder with NitLink's brand
    // identity. Like Render(), this uses the offscreen target in HDR
    // mode (caller must Composite afterward).
    //
    // windowW/windowH are the actual swap chain dimensions, used to
    // center the brand mark and size the type correctly.
    void DrawNoSignal(uint32_t windowW, uint32_t windowH);

    // Draw a small transient toast notification near the top of the window.
    // Single line, dark-glass card, 1px rule border, NitLink-blue accent
    // dot. Intended for short status messages that the user should see
    // briefly without breaking the capture view: currently used by the
    // HDR-not-engaged feedback path. `alpha` is in [0,1]; the caller fades
    // it as the toast expires so it doesn't pop out.
    void DrawToast(uint32_t windowW, uint32_t windowH,
                    const wchar_t* text, float alpha);

    // Accessors for other UI systems that need to draw onto the same swap
    // chain (e.g. the SettingsPanel). The returned pointers are valid as
    // long as the Overlay itself exists.
    ID2D1DeviceContext* GetD2DContext() const { return m_d2dContext.Get(); }
    IDWriteFactory*     GetDWriteFactory() const { return m_dwriteFactory.Get(); }

    // In HDR mode the overlay can't paint directly to the R10G10B10A2
    // HDR10 PQ backbuffer (D2D doesn't support HDR backbuffer formats).
    // Instead Render() draws into a BGRA8 offscreen texture, and the
    // renderer samples this SRV in a fullscreen pass that PQ-encodes for
    // the HDR10 chain (see DX11Renderer::CompositeUI for the conversion
    // details). GetOffscreenSRV() returns nullptr in SDR (where D2D paints
    // to the backbuffer directly and no compositor is needed).
    // IsUsingOffscreen() true == offscreen mode is active and the caller
    // should composite via DX11Renderer::CompositeUI.
    bool IsUsingOffscreen() const { return m_offscreenInUse; }
    ID3D11ShaderResourceView* GetOffscreenSRV() const { return m_offscreenSRV.Get(); }

private:
    bool CreateD2DResources();
    void ReleaseD2DResources();
    // True once the D3D device behind the Direct2D context is gone. Checked
    // at every draw entry point.
    bool DeviceIsLost();
    // Create a BGRA8 offscreen render target sized to the swap-chain
    // backbuffer, wrap it with D2D, and stash an SRV for compositing.
    // Used in HDR mode in place of the direct-to-backbuffer path.
    bool CreateOffscreenTarget(uint32_t width, uint32_t height);

    ID3D11Device*        m_device    = nullptr;
    ID3D11DeviceContext* m_context   = nullptr;
    IDXGISwapChain1*     m_swapChain = nullptr;
    HWND                 m_hwnd      = nullptr;

    // D2D / DirectWrite for text rendering directly onto the swap chain
    ComPtr<ID2D1Factory1>    m_d2dFactory;
    ComPtr<ID2D1Device>      m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext;
    ComPtr<ID2D1Bitmap1>     m_d2dTargetBitmap;
    // HDR offscreen path: when the swap chain backbuffer is R10G10B10A2
    // HDR10 (not BGRA8), it cannot be wrapped with D2D. Instead a BGRA8
    // staging texture is created, wrapped with D2D, and the renderer
    // composites it onto the HDR backbuffer via the UI pixel shader (see
    // DX11Renderer::CompositeUI).
    ComPtr<ID3D11Texture2D>          m_offscreenTex;
    ComPtr<ID3D11ShaderResourceView> m_offscreenSRV;
    bool                             m_offscreenInUse = false;
    // Set when Direct2D or the D3D device reports a lost device. Drawing
    // stops until the application rebuilds the overlay on a fresh device.
    // Direct2D is not robust to a removed device: recreating a target on
    // one can fault inside d2d1.dll instead of returning an error.
    bool                             m_deviceLost = false;
    uint32_t                         m_offscreenW = 0;
    uint32_t                         m_offscreenH = 0;
    ComPtr<IDWriteFactory>   m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormat;
    ComPtr<IDWriteTextFormat> m_smallTextFormat;
    ComPtr<ID2D1SolidColorBrush> m_brushText;
    ComPtr<ID2D1SolidColorBrush> m_brushAccent;
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushGood;
    ComPtr<ID2D1SolidColorBrush> m_brushWarn;
    // Red/critical state brush: used for FPS < 30, latency > 80ms, etc.
    // Defined inline in CreateD2DResources; not in the shared theme
    // (which only carries good/warn).
    ComPtr<ID2D1SolidColorBrush> m_brushCrit;
    // Dim text: for labels, units, branding. Subtler than m_brushText.
    ComPtr<ID2D1SolidColorBrush> m_brushDim;
    // Thicker fonts for the big primary numbers (FPS / Latency values).
    ComPtr<IDWriteTextFormat>    m_textFormatBig;
    ComPtr<IDWriteTextFormat>    m_textFormatLabel;
    ComPtr<IDWriteTextFormat>    m_textFormatUnit;

    bool m_initialized = false;

    // History for sparkline graphs
    std::deque<float> m_fpsHistory;
    std::deque<float> m_latencyHistory;
    static constexpr size_t kHistorySize = 60;
};

} // namespace NitLink