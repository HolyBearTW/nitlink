#include "overlay.h"
#include "../ui/theme.h"
#include "../app/localization.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <debugapi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace NitLink {

static void OvLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/Overlay] " + msg + L"\n").c_str());
}

static std::wstring Tr(const wchar_t* key) {
    return Localization::Instance().Get(key);
}

bool Overlay::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                         IDXGISwapChain1* swapChain, HWND hwnd)
{
    m_device    = device;
    m_context   = context;
    m_swapChain = swapChain;
    m_hwnd      = hwnd;

    // D2D factory: single-threaded since it's only used from the render thread.
    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &opts, (void**)m_d2dFactory.GetAddressOf());
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"D2D1CreateFactory failed: 0x" << std::hex << hr;
        OvLog(ss.str());
        return false;
    }

    // Bridge the D3D11 device to D2D via DXGI -- requires D3D11_CREATE_DEVICE_BGRA_SUPPORT
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"QueryInterface(IDXGIDevice) failed: 0x" << std::hex << hr;
        OvLog(ss.str());
        return false;
    }

    hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"ID2D1Factory1::CreateDevice failed: 0x" << std::hex << hr
           << L" (likely missing D3D11_CREATE_DEVICE_BGRA_SUPPORT flag)";
        OvLog(ss.str());
        return false;
    }

    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"CreateDeviceContext failed: 0x" << std::hex << hr;
        OvLog(ss.str());
        return false;
    }

    // DirectWrite factory for text
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), (IUnknown**)m_dwriteFactory.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_dwriteFactory->CreateTextFormat(
        Localization::Instance().UiFontFamily(GetTheme().overlayFont), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        GetTheme().overlayFontSize, Localization::Instance().LocaleName().c_str(), &m_textFormat);
    if (FAILED(hr)) return false;

    // Footer text (GPU time): the panel's body size.
    hr = m_dwriteFactory->CreateTextFormat(
        Localization::Instance().UiFontFamily(GetTheme().fontFamily), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        11.5f, Localization::Instance().LocaleName().c_str(), &m_smallTextFormat);
    if (FAILED(hr)) return false;

    // Big primary number (frame rate, ingest): semibold, same family.
    hr = m_dwriteFactory->CreateTextFormat(
        Localization::Instance().UiFontFamily(GetTheme().fontFamily), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        26.0f, Localization::Instance().LocaleName().c_str(), &m_textFormatBig);
    if (FAILED(hr)) return false;

    // Uppercase band title and pipeline badges, like the panel's band titles.
    hr = m_dwriteFactory->CreateTextFormat(
        Localization::Instance().UiFontFamily(GetTheme().fontFamily), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        10.0f, Localization::Instance().LocaleName().c_str(), &m_textFormatLabel);
    if (FAILED(hr)) return false;

    // Metric labels and unit suffixes, like the panel's signal labels.
    hr = m_dwriteFactory->CreateTextFormat(
        Localization::Instance().UiFontFamily(GetTheme().fontFamily), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        11.0f, Localization::Instance().LocaleName().c_str(), &m_textFormatUnit);
    if (FAILED(hr)) return false;

    if (!CreateD2DResources()) {
        OvLog(L"CreateD2DResources failed during init");
        return false;
    }

    m_initialized = true;
    OvLog(L"Initialized successfully");
    return true;
}

bool Overlay::CreateD2DResources()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC bbDesc;
    backBuffer->GetDesc(&bbDesc);

    // HDR path: D2D cannot draw directly to the HDR swap-chain format
    // (R10G10B10A2 + DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, HDR10 PQ
    // in BT.2020 primaries). Render the UI into a BGRA8 offscreen texture
    // at the swap-chain dimensions, wrap THAT with D2D, and expose the
    // offscreen's SRV so the renderer's UI compositor shader can convert
    // the SDR-authored overlay into the HDR10 swap-chain path.
    if (bbDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        OvLog(L"CreateD2DResources: HDR backbuffer, using BGRA8 offscreen");
        if (m_d2dContext) m_d2dContext->SetTarget(nullptr);
        m_d2dTargetBitmap.Reset();

        if (!CreateOffscreenTarget(bbDesc.Width, bbDesc.Height)) {
            OvLog(L"CreateD2DResources: CreateOffscreenTarget failed");
            return false;
        }
        m_offscreenInUse = true;

        // Brushes are needed regardless of which target is used.
        const Theme& th = GetTheme();
        m_d2dContext->CreateSolidColorBrush(th.text,      &m_brushText);
        m_d2dContext->CreateSolidColorBrush(th.accent,    &m_brushAccent);
        m_d2dContext->CreateSolidColorBrush(th.overlayBg, &m_brushBg);
        m_d2dContext->CreateSolidColorBrush(th.good,      &m_brushGood);
        m_d2dContext->CreateSolidColorBrush(th.warn,      &m_brushWarn);
        // Critical and dim brushes: not yet in theme, hardcoded for now.
        m_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.878f, 0.376f, 0.376f, 1.0f), &m_brushCrit);
        m_d2dContext->CreateSolidColorBrush(th.textDim, &m_brushDim);
        m_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.706f, 0.706f, 0.706f, 1.0f), &m_brushInk2);
        return true;
    }

    // SDR path - create D2D target normally
    m_offscreenInUse = false;
    m_offscreenTex.Reset();
    m_offscreenSRV.Reset();

    ComPtr<IDXGISurface> dxgiBackBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer));
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96
    );

    hr = m_d2dContext->CreateBitmapFromDxgiSurface(
        dxgiBackBuffer.Get(), &bmpProps, &m_d2dTargetBitmap);
    if (FAILED(hr)) return false;

    m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());

    // Brushes come from the shared theme so colors stay centralized.
    const Theme& th = GetTheme();
    m_d2dContext->CreateSolidColorBrush(th.text,      &m_brushText);
    m_d2dContext->CreateSolidColorBrush(th.accent,    &m_brushAccent);
    m_d2dContext->CreateSolidColorBrush(th.overlayBg, &m_brushBg);
    m_d2dContext->CreateSolidColorBrush(th.good,      &m_brushGood);
    m_d2dContext->CreateSolidColorBrush(th.warn,      &m_brushWarn);
    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(0.878f, 0.376f, 0.376f, 1.0f), &m_brushCrit);
    m_d2dContext->CreateSolidColorBrush(th.textDim, &m_brushDim);
    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(0.706f, 0.706f, 0.706f, 1.0f), &m_brushInk2);

    return true;
}

bool Overlay::CreateOffscreenTarget(uint32_t width, uint32_t height)
{
    // Create a BGRA8 texture that's both an RTV (for D2D to render into)
    // and a shader resource (for the renderer's compositor pass).
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = width;
    td.Height           = height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_offscreenTex);
    if (FAILED(hr)) return false;

    // SRV for the compositor.
    hr = m_device->CreateShaderResourceView(m_offscreenTex.Get(), nullptr, &m_offscreenSRV);
    if (FAILED(hr)) return false;

    // Wrap with D2D so Render() can paint into it via the existing D2D calls.
    ComPtr<IDXGISurface> dxgiSurface;
    hr = m_offscreenTex.As(&dxgiSurface);
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96
    );
    hr = m_d2dContext->CreateBitmapFromDxgiSurface(
        dxgiSurface.Get(), &bmpProps, &m_d2dTargetBitmap);
    if (FAILED(hr)) return false;

    m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
    m_offscreenW = width;
    m_offscreenH = height;
    return true;
}

void Overlay::ReleaseD2DResources()
{
    if (m_d2dContext) m_d2dContext->SetTarget(nullptr);
    m_d2dTargetBitmap.Reset();
    // Offscreen texture + SRV go too: they're sized to the backbuffer
    // and a resize/HDR-toggle invalidates the old dimensions.
    m_offscreenTex.Reset();
    m_offscreenSRV.Reset();
    m_offscreenInUse = false;
    m_brushText.Reset();
    m_brushAccent.Reset();
    m_brushBg.Reset();
    m_brushGood.Reset();
    m_brushWarn.Reset();
    m_brushCrit.Reset();
    m_brushDim.Reset();
}

void Overlay::OnResizeBegin()
{
    // D2D bitmap holds a reference to the swap chain's backbuffer. It MUST
    // be released before the renderer calls ResizeBuffers, otherwise that
    // call hangs (DXGI waits for all references to clear).
    OvLog(L"OnResizeBegin: releasing D2D resources");
    ReleaseD2DResources();
}

void Overlay::OnResizeEnd()
{
    OvLog(L"OnResizeEnd: recreating D2D resources");
    if (!CreateD2DResources()) {
        OvLog(L"OnResizeEnd: CreateD2DResources failed");
    }
}

void Overlay::Shutdown()
{
    ReleaseD2DResources();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    m_textFormat.Reset();
    m_smallTextFormat.Reset();
    m_dwriteFactory.Reset();
    m_initialized = false;
}

bool Overlay::DeviceIsLost()
{
    if (m_deviceLost) return true;
    if (m_device && FAILED(m_device->GetDeviceRemovedReason())) {
        m_deviceLost = true;
        ReleaseD2DResources();
        OvLog(L"D3D device removed; overlay parked until the application rebuilds it");
    }
    return m_deviceLost;
}

void Overlay::Render(const Stats& stats)
{
    if (DeviceIsLost()) return;
    if (!m_initialized || !m_d2dContext || !m_d2dTargetBitmap ||
        !m_brushText || !m_brushAccent || !m_brushBg) {
        return;
    }

    // Push 0 when there's no signal so the sparkline drops to the floor.
    m_fpsHistory.push_back(stats.signalActive ? (float)stats.fps : 0.0f);
    if (m_fpsHistory.size() > kHistorySize) m_fpsHistory.pop_front();

    float totalLatency = (float)(stats.captureLatencyMs + stats.renderLatencyMs);
    m_latencyHistory.push_back(stats.signalActive ? totalLatency : 0.0f);
    if (m_latencyHistory.size() > kHistorySize) m_latencyHistory.pop_front();

    // HUD layout, matched to the F1 panel: a 280 x 160 card with square
    // corners, a title band on top, two metric columns with sparklines,
    // and a footer with the GPU time and the pipeline badges. Sizes are
    // in DIPs at the 96 DPI the Direct2D target was created with.
    const float panelW = 280.0f;
    const float panelH = 160.0f;
    const float margin = 16.0f;
    const float pad    = 14.0f;
    const float bandH  = 30.0f;

    const D2D1_RECT_F panel = D2D1::RectF(margin, margin, margin + panelW, margin + panelH);

    m_d2dContext->BeginDraw();

    // In HDR offscreen mode, clear the target to fully transparent first
    // so the previous frame's text doesn't accumulate. (In SDR the draw
    // goes directly to the backbuffer, which is already cleared by the
    // renderer's BeginFrame; the overlay just paints on top.)
    if (m_offscreenInUse) {
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));
    }

    const bool sig = stats.signalActive;

    auto drawText = [&](const std::wstring& text, IDWriteTextFormat* fmt, D2D1_RECT_F r,
                        ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT ha, DWRITE_PARAGRAPH_ALIGNMENT va) {
        fmt->SetTextAlignment(ha);
        fmt->SetParagraphAlignment(va);
        m_d2dContext->DrawText(text.c_str(), (UINT32)text.size(), fmt, r, brush);
    };
    auto textWidth = [&](const std::wstring& text, IDWriteTextFormat* fmt) -> float {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(m_dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt,
                                                     1000.0f, 100.0f, &layout))) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    };

    // ---- 1. Surfaces -------------------------------------------------------
    // Body, band, and rules mirror the drawer tokens (--bg, --band, --hair,
    // --hair2) so the HUD reads as the same material as the panel.
    ComPtr<ID2D1SolidColorBrush> bandBrush, borderBrush, hairBrush;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.173f, 0.173f, 0.173f, 0.88f), &bandBrush);
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f),       &borderBrush);
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.075f),      &hairBrush);

    m_d2dContext->FillRectangle(panel, m_brushBg.Get());
    m_d2dContext->FillRectangle(
        D2D1::RectF(panel.left, panel.top, panel.right, panel.top + bandH), bandBrush.Get());
    m_d2dContext->DrawRectangle(
        D2D1::RectF(panel.left + 0.5f, panel.top + 0.5f, panel.right - 0.5f, panel.bottom - 0.5f),
        borderBrush.Get(), 1.0f);

    // ---- 2. Title band -----------------------------------------------------
    // Brand mark (amber while a signal is live, red without one), the name,
    // and the source resolution on the right.
    {
        const float markY = panel.top + bandH * 0.5f;
        m_d2dContext->FillRectangle(
            D2D1::RectF(panel.left + pad, markY - 4.0f, panel.left + pad + 8.0f, markY + 4.0f),
            sig ? m_brushAccent.Get() : m_brushCrit.Get());

        drawText(L"NITLINK", m_textFormatLabel.Get(),
                 D2D1::RectF(panel.left + pad + 15.0f, panel.top, panel.left + 150.0f, panel.top + bandH),
                 m_brushInk2.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        std::wstringstream ss;
        if (sig) ss << stats.captureWidth << L"\u00d7" << stats.captureHeight;
        else     ss << Tr(L"overlay.noSignal");
        drawText(ss.str(), m_textFormatUnit.Get(),
                 D2D1::RectF(panel.left + 150.0f, panel.top, panel.right - pad, panel.top + bandH),
                 sig ? m_brushDim.Get() : m_brushCrit.Get(),
                 DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // ---- 3. Primary metrics: two columns ----------------------------------
    const float colMid  = panel.left + panelW * 0.5f;
    const float labelY  = panel.top + 40.0f;
    const float valueY  = panel.top + 54.0f;
    const float valueH  = 32.0f;

    // Healthy values stay in plain ink like the panel; only degraded states
    // take color. The sparkline stroke carries the accent when healthy.
    auto fpsBrush = [&]() -> ID2D1SolidColorBrush* {
        if (!sig)               return m_brushDim.Get();
        if (stats.fps >= 58)    return m_brushText.Get();
        if (stats.fps >= 31)    return m_brushWarn.Get();
        return m_brushCrit.Get();
    };
    auto fpsStroke = [&]() -> ID2D1SolidColorBrush* {
        return (sig && stats.fps >= 58) ? m_brushAccent.Get() : fpsBrush();
    };

    // App ingest: real, live, per-frame card-driver-to-app delivery time
    // measured via MFSampleExtension_DeviceTimestamp (QPC 100ns) deltaed
    // against arrivalWallNs (steady_clock ns). Both share the QPC epoch on
    // Windows. This is what NitLink can measure directly, not the full
    // photon-to-photon figure (the source device and display panel are
    // invisible from inside the app). 0 means the driver doesn't populate
    // the attribute; negative values would only appear under clock skew
    // between QPC and steady_clock, so they clamp to 0.
    const float appIngest = sig ? std::max(0.0f, (float)stats.appIngestMs) : 0.0f;

    auto appIngestBrush = [&]() -> ID2D1SolidColorBrush* {
        if (!sig)               return m_brushDim.Get();
        if (appIngest <  10.0f) return m_brushText.Get();   // PCIe / fast paths
        if (appIngest <= 25.0f) return m_brushWarn.Get();   // USB / busy systems
        return m_brushCrit.Get();                            // something's wrong
    };
    auto appIngestStroke = [&]() -> ID2D1SolidColorBrush* {
        return (sig && appIngest < 10.0f) ? m_brushAccent.Get() : appIngestBrush();
    };

    // One metric column: label, big value, unit hung off the value's baseline.
    auto drawMetric = [&](float left, float right, const std::wstring& label,
                          const std::wstring& value, const std::wstring& unit,
                          ID2D1SolidColorBrush* valueBrush) {
        drawText(label, m_textFormatUnit.Get(),
                 D2D1::RectF(left, labelY, right, labelY + 14.0f),
                 m_brushDim.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        drawText(value, m_textFormatBig.Get(),
                 D2D1::RectF(left, valueY, right, valueY + valueH),
                 valueBrush, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        const float unitX = left + textWidth(value, m_textFormatBig.Get()) + 5.0f;
        drawText(unit, m_textFormatUnit.Get(),
                 D2D1::RectF(unitX, valueY, right, valueY + 31.0f),
                 m_brushDim.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    };

    drawMetric(panel.left + pad, colMid - 4.0f, Tr(L"overlay.frameRate"),
               sig ? std::to_wstring(stats.fps) : L"--", Tr(L"overlay.fps"), fpsBrush());
    {
        std::wstring v = L"--";
        if (sig) {
            std::wstringstream ss;
            ss << std::fixed << std::setprecision(1) << appIngest;
            v = ss.str();
        }
        drawMetric(colMid + 4.0f, panel.right - pad, Tr(L"overlay.appIngest"), v,
                   Tr(L"overlay.ms"), appIngestBrush());
    }

    // ---- 4. Sparklines (frame rate left, ingest right) ---------------------
    {
        const float sparkTop    = panel.top + 92.0f;
        const float sparkBottom = panel.top + 120.0f;
        const float sparkLeftL  = panel.left + pad;
        const float sparkRightL = colMid - 4.0f;
        const float sparkLeftR  = colMid + 4.0f;
        const float sparkRightR = panel.right - pad;

        auto drawSparkline = [&](float left, float right,
                                  const std::deque<float>& hist,
                                  float maxExpected,
                                  ID2D1SolidColorBrush* stroke) {
            if (hist.size() < 2) return;
            const float w = right - left;
            const float h = sparkBottom - sparkTop;

            // Closed polygon under the data for the gradient fill, then an
            // open path on top for the stroke.
            ComPtr<ID2D1PathGeometry> geom;
            m_d2dFactory->CreatePathGeometry(&geom);
            ComPtr<ID2D1GeometrySink> sink;
            geom->Open(&sink);

            auto sampleY = [&](float v) {
                float y = sparkBottom - (v / maxExpected) * h;
                return std::clamp(y, sparkTop, sparkBottom);
            };

            sink->BeginFigure(D2D1::Point2F(left, sparkBottom), D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 0; i < hist.size(); ++i) {
                float x = left + (i / (float)(kHistorySize - 1)) * w;
                sink->AddLine(D2D1::Point2F(x, sampleY(hist[i])));
            }
            sink->AddLine(D2D1::Point2F(right, sparkBottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();

            D2D1_COLOR_F strokeCol = stroke->GetColor();
            D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, D2D1::ColorF(strokeCol.r, strokeCol.g, strokeCol.b, 0.30f) },
                { 1.0f, D2D1::ColorF(strokeCol.r, strokeCol.g, strokeCol.b, 0.00f) },
            };
            ComPtr<ID2D1GradientStopCollection> stopColl;
            m_d2dContext->CreateGradientStopCollection(stops, 2, &stopColl);
            ComPtr<ID2D1LinearGradientBrush> gradBrush;
            m_d2dContext->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(left, sparkTop),
                    D2D1::Point2F(left, sparkBottom)),
                stopColl.Get(), &gradBrush);
            m_d2dContext->FillGeometry(geom.Get(), gradBrush.Get());

            ComPtr<ID2D1PathGeometry> strokeGeom;
            m_d2dFactory->CreatePathGeometry(&strokeGeom);
            ComPtr<ID2D1GeometrySink> strokeSink;
            strokeGeom->Open(&strokeSink);
            strokeSink->BeginFigure(D2D1::Point2F(left, sampleY(hist[0])), D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < hist.size(); ++i) {
                float x = left + (i / (float)(kHistorySize - 1)) * w;
                strokeSink->AddLine(D2D1::Point2F(x, sampleY(hist[i])));
            }
            strokeSink->EndFigure(D2D1_FIGURE_END_OPEN);
            strokeSink->Close();
            m_d2dContext->DrawGeometry(strokeGeom.Get(), stroke, 1.25f);
        };

        drawSparkline(sparkLeftL, sparkRightL, m_fpsHistory,     70.0f,  fpsStroke());
        drawSparkline(sparkLeftR, sparkRightR, m_latencyHistory, 100.0f, appIngestStroke());
    }

    // ---- 5. Footer: GPU time and pipeline badges ---------------------------
    {
        const float ruleY = panel.top + 128.0f + 0.5f;
        m_d2dContext->DrawLine(D2D1::Point2F(panel.left + pad,  ruleY),
                               D2D1::Point2F(panel.right - pad, ruleY),
                               hairBrush.Get(), 1.0f);

        const D2D1_RECT_F footer = D2D1::RectF(panel.left + pad, panel.top + 130.0f,
                                               panel.right - pad, panel.top + 152.0f);

        // Real per-frame GPU work from the renderer's timestamp queries.
        // Shows "GPU --" until the query ring has filled, or permanently if
        // the driver refused to create timestamp queries.
        const float gpu     = sig ? (float)stats.gpuMs : 0.0f;
        const bool  haveGpu = sig && gpu > 0.0f;
        std::wstringstream ss;
        if (haveGpu) ss << Tr(L"overlay.gpu") << L" " << std::fixed
                        << std::setprecision(1) << gpu << L" " << Tr(L"overlay.ms");
        else         ss << Tr(L"overlay.gpu") << L" --";
        ID2D1SolidColorBrush* gpuBrush = m_brushInk2.Get();
        if (haveGpu) {
            if      (gpu >  12.0f) gpuBrush = m_brushCrit.Get();
            else if (gpu >=  5.0f) gpuBrush = m_brushWarn.Get();
        }
        drawText(ss.str(), m_smallTextFormat.Get(), footer, gpuBrush,
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Badges hang off the right edge, active in accent, inactive in the
        // muted ink. Order matches pipeline execution: scale, HDR, NIS, color.
        const std::wstring labels[4] = {
            L"CR", L"HDR", L"NIS", Tr(L"overlay.badgeColor")
        };
        const bool     active[4] = { true, stats.hdrActive, stats.nisActive, stats.colorExpansion };
        float x = footer.right;
        for (int i = 3; i >= 0; --i) {
            const std::wstring& badge = labels[i];
            const float w = textWidth(badge, m_textFormatLabel.Get());
            drawText(badge, m_textFormatLabel.Get(),
                     D2D1::RectF(x - w, footer.top, x, footer.bottom),
                     active[i] ? m_brushAccent.Get() : m_brushDim.Get(),
                     DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            x -= w + 12.0f;
        }
    }

    HRESULT hr = m_d2dContext->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        // A DXGI-surface target is only lost together with its device.
        // Recreating it here would run Direct2D against a removed device;
        // park instead and let the device-loss rebuild create a fresh overlay.
        m_deviceLost = true;
        ReleaseD2DResources();
        OvLog(L"EndDraw reported a lost target; overlay parked until the application rebuilds it");
    }
}

void Overlay::DrawNoSignal(uint32_t windowW, uint32_t windowH)
{
    if (DeviceIsLost()) return;
    if (!m_initialized || !m_d2dContext || !m_d2dTargetBitmap) {
        return;
    }

    const float w = (float)windowW;
    const float h = (float)windowH;

    // Reference design canvas is 1920x1080. Scale by min so the centered
    // card fits cleanly on any window size without stretching weird.
    const float scale = std::min(w / 1920.0f, h / 1080.0f);
    auto S = [&](float v) { return v * scale; };

    // Palette mirrors nitlink-menu.html (--bg / --bg-soft / --rule /
    // --accent / --fg* tokens). Kept local so this screen is self-contained
    // and can be tuned without touching the rest of the overlay.
    const D2D1_COLOR_F COL_BG       = D2D1::ColorF(0.047f, 0.051f, 0.059f, 1.0f); // #0C0D0F  (--bg)
    const D2D1_COLOR_F COL_CARD_BG  = D2D1::ColorF(0.075f, 0.078f, 0.094f, 1.0f); // #131418  (--bg-soft)
    const D2D1_COLOR_F COL_RULE     = D2D1::ColorF(1.0f,  1.0f,  1.0f,  0.06f);    // (--rule)
    const D2D1_COLOR_F COL_FG       = D2D1::ColorF(0.910f, 0.918f, 0.929f, 1.0f); // #E8EAED (--fg)
    const D2D1_COLOR_F COL_FG_DIM   = D2D1::ColorF(0.604f, 0.627f, 0.659f, 1.0f); // #9AA0A8 (--fg-mid)
    const D2D1_COLOR_F COL_FG_MUTED = D2D1::ColorF(0.290f, 0.310f, 0.341f, 1.0f); // #4A4F57 (--fg-muted)
    const D2D1_COLOR_F COL_ACCENT   = D2D1::ColorF(0.890f, 0.604f, 0.231f, 1.0f); // #E39A3B (--accent)

    m_d2dContext->BeginDraw();

    // Flat near-black background: the F1 menu uses a single tone rather
    // than a gradient, so the prior radial fill is dropped here.
    {
        ComPtr<ID2D1SolidColorBrush> bBg;
        m_d2dContext->CreateSolidColorBrush(COL_BG, &bBg);
        m_d2dContext->FillRectangle(D2D1::RectF(0, 0, w, h), bBg.Get());
    }

    // Brushes used throughout.
    ComPtr<ID2D1SolidColorBrush> bCard, bRule, bFg, bFgDim, bFgMuted, bAccent, bAccentSoft;
    m_d2dContext->CreateSolidColorBrush(COL_CARD_BG, &bCard);
    m_d2dContext->CreateSolidColorBrush(COL_RULE,    &bRule);
    m_d2dContext->CreateSolidColorBrush(COL_FG,      &bFg);
    m_d2dContext->CreateSolidColorBrush(COL_FG_DIM,  &bFgDim);
    m_d2dContext->CreateSolidColorBrush(COL_FG_MUTED,&bFgMuted);
    m_d2dContext->CreateSolidColorBrush(COL_ACCENT,  &bAccent);
    // --accent-soft equivalent for the status dot halo (rgba 227,154,59 / 0.16).
    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(0.890f, 0.604f, 0.231f, 0.16f), &bAccentSoft);

    // Type. Segoe UI is the closest stock-Windows analogue to Inter; paired
    // with Cascadia Mono for the version tag / brand sub-label so it matches
    // the F1 menu's Inter + JetBrains Mono pairing.
    auto MakeFormat = [&](float pt, DWRITE_FONT_WEIGHT weight, const wchar_t* family) {
        ComPtr<IDWriteTextFormat> f;
        m_dwriteFactory->CreateTextFormat(
            Localization::Instance().UiFontFamily(family), nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            S(pt), Localization::Instance().LocaleName().c_str(), &f);
        return f;
    };

    auto fBrandLogo = MakeFormat(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI");
    auto fBrandTag  = MakeFormat(11.0f, DWRITE_FONT_WEIGHT_NORMAL,    L"Cascadia Mono");
    auto fSectLabel = MakeFormat(10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI");
    auto fHeadline  = MakeFormat(24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI");
    auto fSubMsg    = MakeFormat(13.0f, DWRITE_FONT_WEIGHT_NORMAL,    L"Segoe UI");
    auto fHint      = MakeFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL,    L"Segoe UI");

    // ─── Centered card ───────────────────────────────────────────────────
    // Width caps at 520px so it stays readable on ultrawides; height is
    // intrinsic-ish (layout is top-down and trusts the design fits inside
    // the drawn rect).
    const float cardW = std::min(S(520.0f), w - S(48.0f));
    const float cardH = S(280.0f);
    const float cardX = (w - cardW) * 0.5f;
    const float cardY = (h - cardH) * 0.5f;
    D2D1_RECT_F card = D2D1::RectF(cardX, cardY, cardX + cardW, cardY + cardH);
    D2D1_ROUNDED_RECT cardRound = { card, S(12.0f), S(12.0f) };

    m_d2dContext->FillRoundedRectangle(cardRound, bCard.Get());
    m_d2dContext->DrawRoundedRectangle(cardRound, bRule.Get(), 1.0f);

    // Inner padding around all card content.
    const float ipx = S(32.0f);
    const float ipy = S(28.0f);

    // ─── Card row 1: brand block ────────────────────────────────────────
    // "NitLink" set in the foreground colour with a quiet monospaced
    // version tag to its right: mirrors the F1 menu's .brand-block layout.
    const float brandY = cardY + ipy;
    {
        D2D1_RECT_F rLogo = D2D1::RectF(
            cardX + ipx, brandY,
            cardX + ipx + S(120.0f), brandY + S(22.0f));
        fBrandLogo->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const wchar_t* logo = L"NitLink";
        m_d2dContext->DrawText(logo, (UINT32)wcslen(logo),
            fBrandLogo.Get(), rLogo, bFg.Get());

        D2D1_RECT_F rTag = D2D1::RectF(
            cardX + ipx + S(64.0f), brandY + S(5.0f),
            cardX + ipx + S(180.0f), brandY + S(22.0f));
        fBrandTag->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const wchar_t* tag = L"v1.2.0";
        m_d2dContext->DrawText(tag, (UINT32)wcslen(tag),
            fBrandTag.Get(), rTag, bFgMuted.Get());
    }

    // Status row, right-aligned in the same band: a small accent dot with
    // the section-label-style "WAITING FOR SOURCE" caption.
    {
        // Soft halo + core dot. No pulse animation: the F1 menu's
        // language is quiet, not attention-grabbing.
        const float dotX = cardX + cardW - ipx - S(150.0f);
        const float dotY = brandY + S(11.0f);
        D2D1_ELLIPSE halo = D2D1::Ellipse(D2D1::Point2F(dotX, dotY), S(7.0f), S(7.0f));
        m_d2dContext->FillEllipse(halo, bAccentSoft.Get());
        D2D1_ELLIPSE core = D2D1::Ellipse(D2D1::Point2F(dotX, dotY), S(3.5f), S(3.5f));
        m_d2dContext->FillEllipse(core, bAccent.Get());

        D2D1_RECT_F rLab = D2D1::RectF(
            dotX + S(12.0f), brandY + S(4.0f),
            cardX + cardW - ipx, brandY + S(20.0f));
        fSectLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const std::wstring lab = Tr(L"overlay.waitingForSource");
        m_d2dContext->DrawText(lab.c_str(), (UINT32)lab.size(),
            fSectLabel.Get(), rLab, bFgDim.Get());
    }

    // Thin rule under the brand row.
    m_d2dContext->DrawLine(
        D2D1::Point2F(cardX + ipx,           cardY + S(64.0f)),
        D2D1::Point2F(cardX + cardW - ipx,   cardY + S(64.0f)),
        bRule.Get(), 1.0f);

    // ─── Card row 2: headline + sub message ─────────────────────────────
    {
        D2D1_RECT_F rH = D2D1::RectF(
            cardX + ipx, cardY + S(96.0f),
            cardX + cardW - ipx, cardY + S(128.0f));
        fHeadline->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const std::wstring hd = Tr(L"overlay.inputSignalLost");
        m_d2dContext->DrawText(hd.c_str(), (UINT32)hd.size(),
            fHeadline.Get(), rH, bFg.Get());
    }
    {
        D2D1_RECT_F rS = D2D1::RectF(
            cardX + ipx, cardY + S(134.0f),
            cardX + cardW - ipx, cardY + S(176.0f));
        fSubMsg->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fSubMsg->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        const std::wstring sub = Tr(L"overlay.waitingForHdmi");
        m_d2dContext->DrawText(sub.c_str(), (UINT32)sub.size(),
            fSubMsg.Get(), rS, bFgDim.Get());
    }

    // ─── Card row 3: quiet hint ─────────────────────────────────────────
    // One soft line of recovery guidance, matching the F1 menu's
    // dynamic-desc-box tone. No bulleted list, no scary error wording.
    m_d2dContext->DrawLine(
        D2D1::Point2F(cardX + ipx,           cardY + S(196.0f)),
        D2D1::Point2F(cardX + cardW - ipx,   cardY + S(196.0f)),
        bRule.Get(), 1.0f);
    {
        D2D1_RECT_F rHint = D2D1::RectF(
            cardX + ipx, cardY + S(214.0f),
            cardX + cardW - ipx, cardY + cardH - ipy);
        fHint->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fHint->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        const std::wstring hint = Tr(L"overlay.checkHdmi");
        m_d2dContext->DrawText(hint.c_str(), (UINT32)hint.size(),
            fHint.Get(), rHint, bFgMuted.Get());
    }

    HRESULT hr = m_d2dContext->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        // A DXGI-surface target is only lost together with its device.
        // Recreating it here would run Direct2D against a removed device;
        // park instead and let the device-loss rebuild create a fresh overlay.
        m_deviceLost = true;
        ReleaseD2DResources();
        OvLog(L"EndDraw reported a lost target; overlay parked until the application rebuilds it");
    }
}

void Overlay::DrawToast(uint32_t windowW, uint32_t windowH,
                         const wchar_t* text, float alpha)
{
    if (DeviceIsLost()) return;
    if (!m_initialized || !m_d2dContext || !m_d2dTargetBitmap) return;
    if (!text || alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    const float w = (float)windowW;
    const float h = (float)windowH;
    const float scale = std::min(w / 1920.0f, h / 1080.0f);
    auto S = [&](float v) { return v * scale; };

    m_d2dContext->BeginDraw();
    if (m_offscreenInUse) {
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));
    }

    // Palette mirrors the demo card / no-signal / F1 menu, but scaled by
    // the caller-supplied alpha so the whole card fades together.
    const D2D1_COLOR_F COL_CARD_BG = D2D1::ColorF(0.075f, 0.078f, 0.094f, 0.92f * alpha);
    const D2D1_COLOR_F COL_RULE    = D2D1::ColorF(1.0f,   1.0f,   1.0f,   0.06f * alpha);
    const D2D1_COLOR_F COL_FG      = D2D1::ColorF(0.910f, 0.918f, 0.929f, alpha);
    const D2D1_COLOR_F COL_ACCENT  = D2D1::ColorF(0.890f, 0.604f, 0.231f, alpha);

    const size_t textLen  = wcslen(text);
    const float usableW   = std::max(S(80.0f), w - S(48.0f));
    const float minCardW  = std::min(S(280.0f), usableW);
    const float maxCardW  = usableW;

    ComPtr<IDWriteTextFormat> fText;
    m_dwriteFactory->CreateTextFormat(
        Localization::Instance().UiFontFamily(L"Segoe UI"), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        S(13.0f), Localization::Instance().LocaleName().c_str(), &fText);
    fText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fText->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    // Measure with a wide layout first, then wrap only when the window cannot
    // fit the complete message. This avoids clipping wider CJK glyphs and
    // makes the native toast safe for localized diagnostic messages.
    ComPtr<IDWriteTextLayout> measured;
    DWRITE_TEXT_METRICS metrics{};
    m_dwriteFactory->CreateTextLayout(text, static_cast<UINT32>(textLen), fText.Get(),
                                      S(4096.0f), S(200.0f), &measured);
    if (measured) measured->GetMetrics(&metrics);
    float cardW = std::clamp(metrics.widthIncludingTrailingWhitespace + S(52.0f),
                             minCardW, maxCardW);
    const float textW = std::max(S(1.0f), cardW - S(52.0f));
    ComPtr<IDWriteTextLayout> wrapped;
    DWRITE_TEXT_METRICS wrappedMetrics{};
    m_dwriteFactory->CreateTextLayout(text, static_cast<UINT32>(textLen), fText.Get(),
                                      textW, S(180.0f), &wrapped);
    if (wrapped) wrapped->GetMetrics(&wrappedMetrics);
    const float cardH = std::max(S(48.0f), wrappedMetrics.height + S(16.0f));
    const float cardX = (w - cardW) * 0.5f;
    const float cardY = S(40.0f);

    D2D1_RECT_F card = D2D1::RectF(cardX, cardY, cardX + cardW, cardY + cardH);
    D2D1_ROUNDED_RECT cardRound = { card, S(10.0f), S(10.0f) };

    ComPtr<ID2D1SolidColorBrush> bCard, bRule, bFg, bAccent;
    m_d2dContext->CreateSolidColorBrush(COL_CARD_BG, &bCard);
    m_d2dContext->CreateSolidColorBrush(COL_RULE,    &bRule);
    m_d2dContext->CreateSolidColorBrush(COL_FG,      &bFg);
    m_d2dContext->CreateSolidColorBrush(COL_ACCENT,  &bAccent);

    m_d2dContext->FillRoundedRectangle(cardRound, bCard.Get());
    m_d2dContext->DrawRoundedRectangle(cardRound, bRule.Get(), 1.0f);

    // Accent dot at the leading edge, vertically centered.
    D2D1_ELLIPSE dot = D2D1::Ellipse(
        D2D1::Point2F(cardX + S(20.0f), cardY + cardH * 0.5f),
        S(3.5f), S(3.5f));
    m_d2dContext->FillEllipse(dot, bAccent.Get());

    // Text wraps inside the measured card when a localized message is wider
    // than the available window.
    D2D1_RECT_F rText = D2D1::RectF(
        cardX + S(36.0f), cardY + S(8.0f),
        cardX + cardW - S(16.0f), cardY + cardH - S(8.0f));
    m_d2dContext->DrawText(text, static_cast<UINT32>(textLen),
        fText.Get(), rText, bFg.Get());

    HRESULT hr = m_d2dContext->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        // A DXGI-surface target is only lost together with its device.
        // Recreating it here would run Direct2D against a removed device;
        // park instead and let the device-loss rebuild create a fresh overlay.
        m_deviceLost = true;
        ReleaseD2DResources();
        OvLog(L"EndDraw reported a lost target; overlay parked until the application rebuilds it");
    }
}

} // namespace NitLink
