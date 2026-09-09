#include "frame_differ.h"

#include <d3dcompiler.h>
#include <debugapi.h>
#include <sstream>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace NitLink {

static void FDLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/FrameDiff] " + msg + L"\n").c_str());
}
static void FDLogHr(const std::wstring& what, HRESULT hr) {
    std::wstringstream ss;
    ss << L"[NitLink/FrameDiff] " << what << L" hr=0x" << std::hex << hr << L"\n";
    OutputDebugStringW(ss.str().c_str());
}

// ---------------------------------------------------------------------------
//  Shaders
// ---------------------------------------------------------------------------

// Fullscreen-triangle VS: takes a 3-vertex strip covering the screen and
// emits clip-space pos + UV. No transform, no flip: rendering targets the
// private working texture so the orientation is controlled end-to-end.
static const char* g_diffQuadVS = R"(
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VS_OUT main(float2 pos : POSITION, float2 uv : TEXCOORD0) {
    VS_OUT o; o.pos = float4(pos, 0, 1); o.uv = uv; return o;
}
)";

// Luma extraction PS: sample input, output to single-channel R8 RT.
//
// Input format varies:
//   - SDR BGRA path: src is full RGB. Apply BT.709 luma weighting.
//   - HDR P010 path: src is R16_UNORM (Y-plane). G and B sample as 0.
//     Applying BT.709 weights to (R, 0, 0) yields 0.2126*R, which
//     crushes the luma signal to 21% of its real value. This breaks the
//     differ in HDR mode: real frame-to-frame motion produces diffs ~5x
//     smaller than expected, so the threshold misses most real changes
//     and classifies live content as "duplicates".
//   - NV12 Y-plane (untested): R8_UNORM, same single-channel situation.
//
// Solution: detect "R-only" inputs by checking G == B == 0, and use R
// directly. For genuine RGB inputs, apply BT.709 weighting as before.
static const char* g_diffLumaPS = R"(
Texture2D    src : register(t0);
SamplerState s   : register(s0);
float main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float3 rgb = src.Sample(s, uv).rgb;
    // Single-channel format detection: if G and B are exactly zero, the
    // input is R-only (P010 Y-plane as R16_UNORM, NV12 Y as R8) and .r
    // already IS the luma. Use it directly.
    if (rgb.g == 0.0 && rgb.b == 0.0) {
        return rgb.r;
    }
    // True RGB input: BT.709 luma.
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}
)";

// Compute shader: per-TILE mean SAD between two R8 textures.
//
// The 640x360 luma is split into an 8x8 grid of 80x45 tiles. One thread group
// per tile (Dispatch(8,8,1)); each group's 64 threads stride over the tile's
// 3600 pixels, groupshared-reduce the sum, and write the tile's MEAN SAD to
// g_result[tileX, tileY]. The CPU reads the 8x8 grid back one frame later and
// takes both the MAX tile (localized-motion rescue signal) and the AVERAGE of
// tiles. Because the tiles are equal-size, that average is exactly the old
// frame-global mean SAD, so the established mean-based classifier is
// unchanged and the max tile is a pure additive rescue.
//
// The TILES_*/TILE_* defines below MUST match kTilesX/kTilesY (frame_differ.h)
// and kWorkW/kWorkH: 640/8 = 80, 360/8 = 45.
static const char* g_diffCS = R"HLSL(
Texture2D<float>  g_current  : register(t0);
Texture2D<float>  g_previous : register(t1);
RWTexture2D<float> g_result  : register(u0);

#define TILES_X     8
#define TILES_Y     8
#define TILE_W      80               // kWorkW / TILES_X
#define TILE_H      45               // kWorkH / TILES_Y
#define TILE_PIXELS (TILE_W * TILE_H) // 3600
#define THREADS     64

groupshared float gs_partial[THREADS];

[numthreads(THREADS, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    // This group owns tile (gid.x, gid.y); its top-left pixel:
    uint baseX = gid.x * TILE_W;
    uint baseY = gid.y * TILE_H;

    float sum = 0.0;
    for (uint i = gtid.x; i < TILE_PIXELS; i += THREADS) {
        uint lx = i % TILE_W;
        uint ly = i / TILE_W;
        float c = g_current.Load(int3(baseX + lx, baseY + ly, 0));
        float p = g_previous.Load(int3(baseX + lx, baseY + ly, 0));
        sum += abs(c - p);
    }

    gs_partial[gtid.x] = sum;
    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction within the tile's thread group.
    [unroll] for (uint stride = THREADS / 2; stride > 0; stride >>= 1) {
        if (gtid.x < stride) {
            gs_partial[gtid.x] += gs_partial[gtid.x + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (gtid.x == 0) {
        // Tile MEAN SAD: 0 = identical, ~1.0 = every pixel maximally different.
        g_result[uint2(gid.x, gid.y)] = gs_partial[0] / TILE_PIXELS;
    }
}
)HLSL";

// ---------------------------------------------------------------------------

FrameDiffer::FrameDiffer()  = default;
FrameDiffer::~FrameDiffer() { Shutdown(); }

bool FrameDiffer::CompileShaders(ID3D11Device* device)
{
    auto compile = [&](const char* src, const char* profile,
                        ComPtr<ID3DBlob>& outBlob) -> bool
    {
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                                  "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                  0, &outBlob, &err);
        if (FAILED(hr)) {
            if (err && err->GetBufferSize() > 0) {
                std::string e((char*)err->GetBufferPointer(), err->GetBufferSize());
                std::wstring we(e.begin(), e.end());
                FDLog(L"compile error: " + we);
            }
            FDLogHr(L"D3DCompile failed", hr);
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vsBlob, psBlob, csBlob;
    if (!compile(g_diffQuadVS, "vs_5_0", vsBlob))   return false;
    if (!compile(g_diffLumaPS, "ps_5_0", psBlob))   return false;
    if (!compile(g_diffCS,     "cs_5_0", csBlob))   return false;

    HRESULT hr;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      nullptr, &m_quadVS);
    if (FAILED(hr)) { FDLogHr(L"CreateVertexShader failed", hr); return false; }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                     nullptr, &m_lumaPS);
    if (FAILED(hr)) { FDLogHr(L"CreatePixelShader failed", hr); return false; }
    hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(),
                                       nullptr, &m_diffCS);
    if (FAILED(hr)) { FDLogHr(L"CreateComputeShader failed", hr); return false; }

    D3D11_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(elems, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                     &m_quadLayout);
    if (FAILED(hr)) { FDLogHr(L"CreateInputLayout failed", hr); return false; }

    return true;
}

bool FrameDiffer::CreateLumaQuad(ID3D11Device* device)
{
    // Fullscreen tri-strip in clip space. No V flip: the caller's SRV is
    // expected to be sampled with whatever native orientation it has, and
    // since the result is used purely for comparison (not display), the
    // orientation doesn't matter as long as it's CONSISTENT across frames.
    struct V { float x, y, u, v; };
    V quad[] = {
        { -1, -1, 0, 1 },
        { -1,  1, 0, 0 },
        {  1, -1, 1, 1 },
        {  1,  1, 1, 0 },
    };
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(quad);
    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{ quad, 0, 0 };
    HRESULT hr = device->CreateBuffer(&bd, &init, &m_quadVB);
    if (FAILED(hr)) { FDLogHr(L"vb CreateBuffer failed", hr); return false; }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sd, &m_sampler);
    if (FAILED(hr)) { FDLogHr(L"CreateSamplerState failed", hr); return false; }

    return true;
}

bool FrameDiffer::CreateWorkingTextures(ID3D11Device* device)
{
    auto makeR8 = [&](bool needRTV, ComPtr<ID3D11Texture2D>& tex,
                       ComPtr<ID3D11RenderTargetView>* outRTV,
                       ComPtr<ID3D11ShaderResourceView>* outSRV) -> bool
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = kWorkW;
        td.Height           = kWorkH;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        if (needRTV) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &tex);
        if (FAILED(hr)) { FDLogHr(L"working CreateTexture2D failed", hr); return false; }
        if (needRTV) {
            hr = device->CreateRenderTargetView(tex.Get(), nullptr, outRTV->GetAddressOf());
            if (FAILED(hr)) { FDLogHr(L"working CreateRTV failed", hr); return false; }
        }
        hr = device->CreateShaderResourceView(tex.Get(), nullptr, outSRV->GetAddressOf());
        if (FAILED(hr)) { FDLogHr(L"working CreateSRV failed", hr); return false; }
        return true;
    };

    if (!makeR8(true,  m_curTex,  &m_curRTV,  &m_curSRV))         return false;
    if (!makeR8(false, m_prevTex, nullptr,    &m_prevSRV))        return false;
    return true;
}

bool FrameDiffer::CreateResultBuffers(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = kTilesX;
    td.Height           = kTilesY;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_resultTex);
    if (FAILED(hr)) { FDLogHr(L"result CreateTexture2D failed", hr); return false; }
    hr = device->CreateUnorderedAccessView(m_resultTex.Get(), nullptr, &m_resultUAV);
    if (FAILED(hr)) { FDLogHr(L"result CreateUAV failed", hr); return false; }

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width            = kTilesX;
    sd.Height           = kTilesY;
    sd.MipLevels        = 1;
    sd.ArraySize        = 1;
    sd.Format           = DXGI_FORMAT_R32_FLOAT;
    sd.SampleDesc.Count = 1;
    sd.Usage            = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    for (uint32_t i = 0; i < kStagingRing; ++i) {
        hr = device->CreateTexture2D(&sd, nullptr, &m_resultStaging[i]);
        if (FAILED(hr)) { FDLogHr(L"staging CreateTexture2D failed", hr); return false; }
    }
    m_stagingWrite   = 0;
    m_stagingWritten = 0;
    return true;
}

bool FrameDiffer::Initialize(ID3D11Device* device)
{
    if (!device) return false;
    FDLog(L"Initialize");
    if (!CompileShaders(device))        return false;
    if (!CreateLumaQuad(device))        return false;
    if (!CreateWorkingTextures(device)) return false;
    if (!CreateResultBuffers(device))   return false;
    FDLog(L"Initialize OK");
    return true;
}

void FrameDiffer::Reset()
{
    // Ignore all readbacks from the previous stream. ReconcileCaptureFormat
    // calls Reset when capture restarts, including device switches and
    // HDR/SDR changes, but a staging slot can still hold an unread duplicate
    // vote from the preceding stream. Reading that vote in the next Process
    // would classify the first frame of the new stream as a duplicate and
    // overwrite the initial m_wasNew=true state below. Application::Run's
    // unique pacing would then withhold the new image, potentially until its
    // roughly 250 ms fallback present despite capture already delivering it.
    //
    // Invalidate the readback ring before accepting any new-stream result.
    // D3D commands on this context stay ordered; only copies submitted after
    // Reset are eligible, so keeping the GPU allocations does not make old
    // readbacks eligible for the new stream's classification.
    m_stagingWrite = 0;
    m_stagingWritten = 0;
    // Per-stream state only; GPU resources stay alive. The next Process()
    // call will repopulate m_prevTex via the copy at the end of the
    // dispatch, and rebuild the ring buffer from the first kSmoothWindow
    // raw votes against the new content.
    m_havePrev    = false;
    m_firstFrame  = true;
    m_wasNew      = true;
    m_lastDiff    = 0.0f;
    m_recentIdx   = 0;
    m_recentCount = 0;
    for (int i = 0; i < kSmoothWindow; ++i) m_recentNew[i] = true;
    FDLog(L"Reset: cleared per-stream state");
}

void FrameDiffer::Shutdown()
{
    for (auto& slot : m_resultStaging) slot.Reset();
    m_stagingWrite   = 0;
    m_stagingWritten = 0;
    m_resultUAV.Reset();
    m_resultTex.Reset();
    m_prevSRV.Reset();
    m_prevTex.Reset();
    m_curSRV.Reset();
    m_curRTV.Reset();
    m_curTex.Reset();
    m_diffCS.Reset();
    m_sampler.Reset();
    m_quadVB.Reset();
    m_quadLayout.Reset();
    m_lumaPS.Reset();
    m_quadVS.Reset();
}

void FrameDiffer::Process(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* inputSRV)
{
    if (!m_diffCS || !inputSRV) return;

    // ---- Read back the previous frame's diff result, if any ----
    // The value here was computed two Process() calls ago: diff(N-1, N-2).
    // It establishes whether frame N-1 was new. By picking it up here
    // (one frame later than its dispatch), the GPU has long since written
    // it and the Map call is free.
    // Prefer the slot written one frame ago; when its copy is still in
    // flight, fall back to the slot written two frames ago. DO_NOT_WAIT keeps
    // a busy GPU from stalling the present loop: if neither copy is done the
    // previous classification stands and the readback is retried next frame.
    if (m_stagingWritten >= 1) {
        uint32_t readSlot = (m_stagingWrite + kStagingRing - 1) % kStagingRing;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = ctx->Map(m_resultStaging[readSlot].Get(), 0, D3D11_MAP_READ,
                              D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && m_stagingWritten >= 2) {
            readSlot = (m_stagingWrite + kStagingRing - 2) % kStagingRing;
            hr = ctx->Map(m_resultStaging[readSlot].Get(), 0, D3D11_MAP_READ,
                          D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        }
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            m_readbackSkips++;
        } else if (SUCCEEDED(hr)) {
            // Read the kTilesX x kTilesY grid of per-tile mean SADs. Take the
            // max (localized-motion rescue signal) and the average. Because the
            // tiles are equal-size, the average IS the old frame-global mean,
            // so m_lastDiff keeps its exact prior meaning and the established
            // classifier below is unchanged. Map gives a row pitch that may be
            // padded past kTilesX floats, so index rows via RowPitch bytes.
            const uint8_t* rowBase = reinterpret_cast<const uint8_t*>(mapped.pData);
            float maxTile = 0.0f;
            float sumTiles = 0.0f;
            for (uint32_t ty = 0; ty < kTilesY; ++ty) {
                const float* row =
                    reinterpret_cast<const float*>(rowBase + ty * mapped.RowPitch);
                for (uint32_t tx = 0; tx < kTilesX; ++tx) {
                    const float v = row[tx];
                    sumTiles += v;
                    if (v > maxTile) maxTile = v;
                }
            }
            ctx->Unmap(m_resultStaging[readSlot].Get(), 0);

            m_lastMaxTile = maxTile;
            m_lastDiff    = sumTiles / static_cast<float>(kTilesX * kTilesY);

            // Classification: two-stage. Stage 1 is hysteresis on the raw
            // diff value; stage 2 is a ring-buffer smoother over the stage-1
            // vote. The header explains the rationale in detail; the short
            // version is that hysteresis handles boundary-noise oscillation
            // and the ring buffer handles bursty misclassification during
            // sustained-state content.
            //
            // Stage 1: hysteresis.
            //
            // A single threshold causes oscillation when source content sits
            // right at the boundary: small noise pushes the diff value above
            // and below the threshold randomly. Two thresholds plus a sticky
            // deadband fix that: commit only on clear above/below, hold the
            // previous classification when in between. The "previous" the
            // deadband holds is the LAST COMMITTED m_wasNew (i.e. sticky
            // from the smoother's perspective), so the two stages compose.
            //
            // Upper threshold = m_threshold (the existing tuning value).
            // Lower threshold = m_threshold * 0.4 (well below typical noise).
            const float upper = m_threshold;
            const float lower = m_threshold * 0.4f;
            bool rawNew;
            if (m_lastDiff >= upper) {
                rawNew = true;
            } else if (m_lastDiff < lower) {
                rawNew = false;
            } else {
                rawNew = m_wasNew; // sticky in deadband
            }

            // Rescue-OR (max-tile). Localized motion (a character idle
            // animation in one corner, a small HUD element) produces real
            // change concentrated in a few tiles but a tiny frame-global mean
            // once averaged across the static majority, so the mean test above
            // mislabels it a duplicate. If the strongest tile clears
            // m_tileThreshold, promote the frame to new. This only ever flips
            // false->true, so it cannot regress the mean-validated behavior:
            // real duplicates are ~0 in every tile (max stays low), and
            // full-frame motion already clears the mean test. See the header
            // for the on-hardware tuning procedure for m_tileThreshold.
            if (!rawNew && m_lastMaxTile >= m_tileThreshold) {
                rawNew = true;
            }

            // Stage 2: pattern-aware smoother.
            //
            // The naive smoother (require all kSmoothWindow votes to agree)
            // works on sustained-state content but breaks on legitimate
            // alternating patterns. A real 30fps game running over a 60Hz
            // HDMI link sends each frame TWICE: the differ correctly votes
            // [new, dup, new, dup, ...] in clean alternation, which means
            // an "all agree" rule never commits and m_wasNew gets stuck at
            // its initial value. The renderer then skips every Present and
            // contentFps reads 0 during 30fps quality-mode gameplay.
            //
            // Fix: detect the pattern of recent votes and pick the strategy.
            //
            //   - High alternation count (looks like a 30fps source on 60Hz
            //     HDMI): trust the raw vote, no smoothing. The differ is
            //     correctly seeing alternation; smoothing would suppress it.
            //
            //   - Low alternation count (looks like sustained state with
            //     possible noise): apply majority vote. This filters
            //     single-frame outliers that would otherwise flip the
            //     committed state during steady 60fps motion or static
            //     content.
            //
            // For kSmoothWindow=3, max possible alternations is 2 (e.g.
            // [T,F,T] has transitions T->F and F->T). The code uses
            // >= kSmoothWindow-1 as the threshold for "pure alternation".
            //
            // IMPORTANT: alternation count is temporally sensitive: the
            // votes must be read in insertion order (oldest first), not
            // slot order. After m_recentIdx has wrapped, slot 0 may hold a
            // newer entry than slot kSmoothWindow-1. The next slot to write
            // (m_recentIdx after the increment above) is the OLDEST slot.
            //
            // Warmup: during the first kSmoothWindow Process calls the ring
            // buffer isn't yet full, so the raw vote is committed immediately.
            m_recentNew[m_recentIdx] = rawNew;
            m_recentIdx = (m_recentIdx + 1) % kSmoothWindow;
            if (m_recentCount < kSmoothWindow) m_recentCount++;

            if (m_recentCount < kSmoothWindow) {
                m_wasNew = rawNew;
            } else {
                // Read ring in insertion order (oldest first). m_recentIdx
                // currently points at the slot to overwrite next, which
                // is the oldest slot (it was written kSmoothWindow frames
                // ago).
                bool ordered[kSmoothWindow];
                for (int i = 0; i < kSmoothWindow; i++) {
                    ordered[i] = m_recentNew[(m_recentIdx + i) % kSmoothWindow];
                }

                int alternations = 0;
                for (int i = 0; i < kSmoothWindow - 1; i++) {
                    if (ordered[i] != ordered[i + 1]) alternations++;
                }

                if (alternations >= kSmoothWindow - 1) {
                    // Pure alternating pattern: trust raw vote.
                    m_wasNew = rawNew;
                } else {
                    // Sustained state with possible noise: majority vote.
                    int trueVotes = 0;
                    for (int i = 0; i < kSmoothWindow; i++) {
                        if (ordered[i]) trueVotes++;
                    }
                    m_wasNew = (trueVotes * 2 > kSmoothWindow);
                }
            }
        }
    }

    // ---- Save D3D state about to be clobbered ----
    // The renderer might be mid-frame; this code should not permanently
    // change its bindings. The bare minimum to preserve: render targets,
    // viewport, input layout, primitive topology.
    ComPtr<ID3D11RenderTargetView> savedRTV;
    ComPtr<ID3D11DepthStencilView> savedDSV;
    ctx->OMGetRenderTargets(1, &savedRTV, &savedDSV);
    D3D11_VIEWPORT savedVP{};
    UINT vpCount = 1;
    ctx->RSGetViewports(&vpCount, &savedVP);
    ComPtr<ID3D11InputLayout> savedLayout;
    ctx->IAGetInputLayout(&savedLayout);
    D3D11_PRIMITIVE_TOPOLOGY savedTopo;
    ctx->IAGetPrimitiveTopology(&savedTopo);

    // ---- Pass 1: render input -> R8 luma working texture ----
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr); // clear binding first

    ctx->OMSetRenderTargets(1, m_curRTV.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (float)kWorkW, (float)kWorkH, 0, 1 };
    ctx->RSSetViewports(1, &vp);

    ctx->VSSetShader(m_quadVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_lumaPS.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &inputSRV);
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    UINT stride = sizeof(float) * 4, offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_quadVB.GetAddressOf(), &stride, &offset);
    ctx->IASetInputLayout(m_quadLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->Draw(4, 0);

    // ---- Pass 2: compute diff between m_curSRV and m_prevSRV ----
    if (m_havePrev) {
        // Unbind RTV first to avoid hazard if curRTV's texture is also being
        // read as curSRV. (curRTV writes to m_curTex; curSRV reads m_curTex.
        // Same resource! Need to break the binding.)
        ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

        ctx->CSSetShader(m_diffCS.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { m_curSRV.Get(), m_prevSRV.Get() };
        ctx->CSSetShaderResources(0, 2, srvs);
        UINT initCount = 0;
        ctx->CSSetUnorderedAccessViews(0, 1, m_resultUAV.GetAddressOf(), &initCount);
        ctx->Dispatch(kTilesX, kTilesY, 1); // one thread group per tile

        // Unbind compute resources
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        ctx->CSSetShaderResources(0, 2, nullSRVs);

        // Copy result to staging for CPU readback NEXT frame.
        ctx->CopyResource(m_resultStaging[m_stagingWrite].Get(), m_resultTex.Get());
        m_stagingWrite = (m_stagingWrite + 1) % kStagingRing;
        m_stagingWritten++;
    }

    // ---- Copy current to previous for next frame's diff ----
    ctx->CopyResource(m_prevTex.Get(), m_curTex.Get());
    m_havePrev = true;

    // ---- Restore the caller's D3D state ----
    ctx->OMSetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.Get());
    ctx->RSSetViewports(1, &savedVP);
    if (savedLayout) ctx->IASetInputLayout(savedLayout.Get());
    ctx->IASetPrimitiveTopology(savedTopo);

    // PS resources just bound: clear them since the caller may not re-set them.
    ID3D11ShaderResourceView* nullPSRes = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullPSRes);

    if (m_firstFrame) m_firstFrame = false;
}

} // namespace NitLink
