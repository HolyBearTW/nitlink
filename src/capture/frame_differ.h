#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace NitLink {

// FrameDiffer: GPU-side new-frame detection for capture sources.
//
// Why this is needed:
//   The Elgato delivers 60 frames/sec over HDMI regardless of game framerate.
//   A 30fps game sends each rendered frame TWICE. To get the real game FPS
//   a reliable signal is needed: "is this frame actually new content?"
//
// Architecture:
//   FrameDiffer is self-contained: it owns two small internal textures (the
//   "current downsampled luma" and "previous downsampled luma"), a pixel
//   shader that converts the caller's capture SRV to those textures, and a
//   compute shader that sums absolute differences between them.
//
//   Each Process() call:
//     1. Renders the caller's capture (any format: RGBA, BGRA, NV12 Y plane)
//        through the luma shader into the 640x360 R8 working texture.
//     2. Runs the compute diff against the previously-stored 640x360 R8.
//     3. Copies current to previous for next frame.
//     4. Reads back the diff result from the PREVIOUS dispatch (no stall).
//
// One-frame latency on the classification: frame N's verdict is known AT
// frame N+1. Irrelevant for FPS counting.
//
// Cost: ~0.1ms on a 5080 at any capture resolution. Downsampling to 640x360
// first is the heavy lifting; the diff itself is cheap.
class FrameDiffer {
public:
    FrameDiffer();
    ~FrameDiffer();

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Clear the differ's per-stream state without destroying GPU resources.
    // Called from the application's ReconcileCaptureFormat after the
    // FrameBuffer is rebuilt at a new capture format (e.g. SDR<->HDR10
    // teardown). Without this, m_prevTex still holds luma from the previous
    // format's frames and the ring-buffer smoother carries pre-reconcile
    // votes: the first post-reconcile diff is against stale data and a
    // sticky "duplicate" classification can persist across the format swap,
    // leaving contentFps wedged at 0 even when fresh content is arriving.
    //
    // Safe to call from the main thread at any point when no Process() call
    // is in flight (the renderer thread also calls Process from the main
    // thread, so there is no concurrent caller in practice).
    void Reset();

    // Called once per captured frame. inputSRV must be a 2D texture SRV with
    // luma sampleable from .r (works for BGRA, RGBA, R8 Y plane of NV12).
    void Process(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* inputSRV);

    // True if the PREVIOUS Process() call's frame was classified as new
    // content. Available after >=2 calls; returns true conservatively until
    // then to avoid undercounting during startup.
    bool WasPreviousFrameNew() const { return m_wasNew; }
    float GetLastDiffValue()  const { return m_lastDiff; }

    // Max-tile SAD from the previous Process() (see the tiled compute shader).
    // Exposed alongside GetLastDiffValue (the frame-global mean) so the VRR
    // pacing log can surface both while m_tileThreshold is being tuned.
    float GetLastMaxTileValue() const { return m_lastMaxTile; }

    void  SetThreshold(float t) { m_threshold = t; }
    float GetThreshold() const  { return m_threshold; }

    void  SetTileThreshold(float t) { m_tileThreshold = t; }
    float GetTileThreshold() const  { return m_tileThreshold; }

private:
    bool CompileShaders(ID3D11Device* device);
    bool CreateWorkingTextures(ID3D11Device* device);
    bool CreateResultBuffers(ID3D11Device* device);
    bool CreateLumaQuad(ID3D11Device* device);

    // Working resolution: much smaller than full capture, so per-pixel noise
    // gets averaged out by the bilinear downsample. 640x360 = 230400 pixels,
    // enough samples for robust SAD without being expensive.
    //
    // History: started at 320x180 (57600 samples). Doubled to 640x360 because
    // slow motion like character idle animations in PS5 pause menus produces
    // sub-pixel changes that averaged out at the lower res, leading the
    // differ to misclassify those frames as duplicates. At 640x360 the same
    // motion produces whole-pixel changes the differ catches reliably.
    static constexpr uint32_t kWorkW = 640;
    static constexpr uint32_t kWorkH = 360;

    // Tile grid for the max-tile SAD rescue term. The diff compute shader
    // splits the kWorkW x kWorkH luma into kTilesX * kTilesY tiles, computes
    // each tile's MEAN SAD, and the CPU reads back the grid to take both the
    // max tile (localized-motion signal) and the average of tiles (which is
    // exactly the old frame-global mean, since the tiles are equal-size).
    //
    // 8x8 must divide kWorkW/kWorkH evenly: 640/8 = 80, 360/8 = 45, so each
    // tile is 80x45 = 3600 samples. If these change, the matching #defines in
    // g_diffCS (frame_differ.cpp) MUST be updated to agree, because the shader can't
    // see these constants (it's a separate HLSL string).
    static constexpr uint32_t kTilesX = 8;
    static constexpr uint32_t kTilesY = 8;

    // Pixel shader that samples any-format input -> luma value in R8.
    ComPtr<ID3D11VertexShader>       m_quadVS;
    ComPtr<ID3D11PixelShader>        m_lumaPS;
    ComPtr<ID3D11InputLayout>        m_quadLayout;
    ComPtr<ID3D11Buffer>             m_quadVB;
    ComPtr<ID3D11SamplerState>       m_sampler;

    // Compute shader for the diff.
    ComPtr<ID3D11ComputeShader>      m_diffCS;

    // Working textures: current and previous 640x360 R8 luma.
    ComPtr<ID3D11Texture2D>          m_curTex;
    ComPtr<ID3D11RenderTargetView>   m_curRTV;
    ComPtr<ID3D11ShaderResourceView> m_curSRV;
    ComPtr<ID3D11Texture2D>          m_prevTex;
    ComPtr<ID3D11ShaderResourceView> m_prevSRV;

    // Result of the diff: kTilesX x kTilesY R32_FLOAT (one mean SAD per tile)
    // + a staging copy read back on the CPU one frame later.
    ComPtr<ID3D11Texture2D>            m_resultTex;
    ComPtr<ID3D11UnorderedAccessView>  m_resultUAV;
    ComPtr<ID3D11Texture2D>            m_resultStaging;

    bool   m_wasNew    = true;
    float  m_lastDiff  = 0.0f;     // frame-global mean SAD (= average of tiles)
    float  m_lastMaxTile = 0.0f;   // max per-tile mean SAD (localized motion)
    // Threshold tuned empirically with VRR present pacing on real HDR content
    // at 640x360 working resolution:
    //   - Active gameplay (60fps PS5): lastDiff = 0.010 to 0.025
    //   - 30fps content (quality mode): lastDiff = 0.010 to 0.025
    //   - Character idle animations / slow motion: lastDiff varies widely,
    //     typically 0.0005 to 0.003 depending on how much of the scene is moving
    //   - Truly static screens (dashboard, paused with no motion): lastDiff < 0.0003
    // 0.0005 catches subtle idle motion while still rejecting the static-noise
    // floor.
    float  m_threshold = 0.0005f;

    // Rescue-OR threshold for the max-tile metric. A frame whose global mean
    // (m_lastDiff) falls below m_threshold but whose strongest tile exceeds
    // this value is promoted to "new content": that signature is localized
    // motion (e.g. a character idle animation in one corner) whose energy is
    // diluted to near-zero when averaged across the static rest of the frame.
    // The rescue only ever flips a duplicate verdict to new, never the
    // reverse, so the mean-based behavior validated below is preserved exactly.
    //
    // STARTING VALUE, must be tuned on real hardware. The shipped value is a
    // first guess; the max-of-64-tiles static-noise floor depends on the
    // specific capture card's sensor noise, which can't be predicted offline.
    // To tune: watch maxTile in the [NitLink/App] "VRR pacing" log and set
    // this ABOVE the value seen on a truly static dashboard / paused screen,
    // but BELOW the value seen during a subtle idle animation. If the static
    // floor and the idle signal overlap at 8x8, drop kTilesX/kTilesY to 4x4
    // (coarser tiles -> lower noise floor).
    float  m_tileThreshold = 0.006f;

    bool   m_firstFrame = true;
    bool   m_havePrev   = false;

    // Temporal smoothing: kills single-frame and double-frame
    // misclassifications, while preserving legitimate alternating patterns
    // from sub-native-rate sources.
    //
    // Two-stage classifier:
    //   Stage 1 (hysteresis, see .cpp): handles boundary noise on the
    //   per-frame diff value. Two thresholds, sticky deadband.
    //   Stage 2 (this): looks at the last kSmoothWindow stage-1 votes and
    //   decides what to commit to m_wasNew.
    //
    // Stage 2 is pattern-aware. A naive "all votes agree" rule breaks
    // legitimate 30fps content (where the differ correctly votes new/dup/
    // new/dup at the HDMI level), because the ring buffer never sees
    // unanimous agreement and m_wasNew gets stuck. Instead, alternations
    // in the recent votes are counted:
    //
    //   - High alternations (>= kSmoothWindow - 1): pure alternating
    //     pattern, characteristic of a sub-native-rate source. Trust the
    //     raw vote, no smoothing. The differ is correctly seeing real
    //     duplicates.
    //
    //   - Low alternations: sustained-state content (60fps motion or
    //     static screen) with possible noise. Apply majority vote across
    //     the ring buffer. Filters isolated misclassifications.
    //
    // Window of 3 = ~50ms response latency on real state transitions at
    // 60Hz HDMI, imperceptible. The threshold "high alternations" with
    // window 3 is 2 (the max possible). Larger windows would trade real-
    // transition latency for more aggressive smoothing of midcontent noise.
    static constexpr int kSmoothWindow = 3; // assumes odd; ties on even windows resolve to false
    bool   m_recentNew[kSmoothWindow] = { true, true, true };
    int    m_recentIdx = 0;
    int    m_recentCount = 0; // how many slots have been filled so far

    // -----------------------------------------------------------------------
    // Validation notes: empirical behavior on real PS5 content (4K Pro, 4K
    // HDR, full active session). The VRR pacing log surfaces three fields
    // from this class that someone debugging will look at: lastDiff (raw SAD
    // result), the smoothed isNewFrame classification, and consecutive
    // (running count of frames Present-skipped). Interpretation:
    //
    // 1. PAUSED MENU / DASHBOARD (truly static content)
    //    lastDiff: 0.00009 to 0.0002 (sub-threshold)
    //    raw vote: false (duplicate)
    //    ring pattern: [F,F,F] -> 0 alternations -> majority vote -> commit F
    //    consecutive: climbs to kMaxConsecutiveSkips (15), forces Present,
    //                 resets. Cycle repeats every ~250ms.
    //    contentFps reported: 0
    //    Verdict: differ working correctly. Monitor sees 4Hz Present rate,
    //    VRR locks low, power savings.
    //
    // 2. ACTIVE GAMEPLAY, 60fps source (sustained motion)
    //    lastDiff: 0.011 to 0.037 (well above upper threshold)
    //    raw vote: true (new content)
    //    ring pattern: [T,T,T] -> 0 alternations -> majority vote -> commit T
    //    consecutive: 0 during pure motion. Occasionally spikes to 4 to 6
    //                 during natural low-motion micro-moments (character
    //                 stops moving, camera faces a wall, brief idle).
    //                 contentFps stays at 59 to 60 because these bursts are
    //                 short and recover within one or two frames of motion
    //                 resuming.
    //    Verdict: differ working correctly. The non-zero consecutive in
    //    logs is NOT a bug: it's the differ correctly noticing that real
    //    gameplay has natural low-motion moments and skipping those
    //    Presents. Net Present rate stays at 59 to 60fps; VRR follows.
    //
    // 3. ACTIVE GAMEPLAY, 30fps source / Quality Mode (sub-native rate)
    //    Capture card delivers 60 frames/sec; game renders 30fps; each game
    //    frame goes over HDMI TWICE.
    //    lastDiff: alternates between high (~0.02) on real frames and low
    //              (~0.0001) on duplicates.
    //    raw vote: alternates true/false/true/false in clean rhythm.
    //    ring pattern: [T,F,T] / [F,T,F] -> 2 alternations ->
    //                  high-alternation branch -> trust raw vote.
    //    consecutive: oscillates 0 to 1 (every other frame is correctly
    //                 classified as duplicate and Present-skipped).
    //    contentFps reported: 30
    //    Verdict: differ working correctly. The pattern-aware smoother
    //    detects the alternation and disables majority vote so the
    //    correctly-classified duplicates actually take effect. Without
    //    the pattern detection, the smoother would suppress every Present
    //    on 30fps content and contentFps would read 0 (the bug that drove
    //    this design).
    //
    // 4. 60fps SOURCE WITH SPURIOUS MISCLASSIFICATION (boundary noise)
    //    lastDiff: mostly above upper threshold but occasionally drops
    //              briefly to ~0.0004 for one frame (signal noise).
    //    raw vote: mostly true, occasional spurious false.
    //    ring pattern: [T,T,T] most of the time; briefly [T,F,T] or
    //                  [T,T,F] during a noise frame.
    //                  [T,T,F] = 1 alternation -> majority vote -> commit T
    //                  [T,F,T] = 2 alternations -> trust raw (commit F for
    //                            that frame, but only that frame: the
    //                            window then becomes [F,T,T] = 1 alt ->
    //                            majority vote -> T again).
    //    Verdict: isolated noise frames get filtered by majority vote
    //    OR rapidly recovered after at most one frame of misclassification.
    //
    // What this means for someone debugging a future report of "stutter":
    // the question is whether contentFps tracks the source's real framerate,
    // not whether consecutive is zero. consecutive=5 during active play with
    // contentFps=59 is fine. consecutive=5 with contentFps=40 is a bug.
    // contentFps=0 during active gameplay is the failure mode this smoother
    // was specifically designed to prevent.
    // -----------------------------------------------------------------------
};

} // namespace NitLink
