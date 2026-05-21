#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

namespace NitLink {

// NisUpscaler -- wraps NVIDIA's NIS compute-shader spatial upscaler.
//
// NIS is a single compute-shader pass that takes a source texture (SRV) and
// writes to a UAV at a different (typically larger) resolution. It does
// directional edge-aware upscaling plus adaptive sharpening, with quality
// noticeably better than bilinear or Catmull-Rom.
//
// Integration architecture:
//   1. Renderer's existing Catmull-Rom + range-expansion pass writes to a
//      post-input intermediate texture (the m_postInputRTV in DX11Renderer).
//   2. NisUpscaler samples that texture as its input SRV.
//   3. NisUpscaler compute-shades onto its own UAV at a larger resolution.
//   4. The renderer then composites the output SRV onto the backbuffer with
//      a simple passthrough draw.
//
// Lifecycle:
//   Initialize(device)                  - compile shader, upload coefficients
//   Configure(inputW, inputH, outputW, outputH, sharpness) - sets dispatch
//                                                            params for the
//                                                            current source/
//                                                            target size pair
//   Dispatch(ctx, inputSRV)             - actually run NIS into m_outputUAV
//   GetOutputSRV()                      - the result, ready to composite
//
// All inputs and outputs are DXGI_FORMAT_R8G8B8A8_UNORM. NIS does not modify
// alpha; content is treated as opaque.
class NisUpscaler {
public:
    NisUpscaler();
    ~NisUpscaler();

    // One-time setup: compile shader, upload NVIDIA's coefficient arrays
    // into two SRV textures. Safe to call once at app start.
    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Configure dispatch for the next sequence of frames. Recreates the
    // output UAV/SRV if the output dimensions change. Returns false on a
    // genuine error; the caller should fall back to passthrough.
    //
    // sharpness in [0..1]: NIS recommends 0.5 as a neutral default.
    bool Configure(ID3D11DeviceContext* ctx,
                    uint32_t inputW, uint32_t inputH,
                    uint32_t outputW, uint32_t outputH,
                    float sharpness);

    // Run the compute shader. Caller provides the input SRV. Dispatch
    // makes the result available via GetOutputSRV().
    //
    // Caller must ensure: input SRV is a R8G8B8A8_UNORM (or compatible)
    // 2D texture whose dimensions match the inputW/inputH passed to
    // Configure. No runtime validation.
    void Dispatch(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* inputSRV);

    // The output texture as an SRV: feed this into the final composite
    // pass. nullptr if Initialize/Configure haven't succeeded yet.
    ID3D11ShaderResourceView* GetOutputSRV() const { return m_outputSRV.Get(); }
    uint32_t GetOutputWidth()  const { return m_outputW; }
    uint32_t GetOutputHeight() const { return m_outputH; }

    bool IsReady() const { return m_outputSRV != nullptr; }

private:
    bool CompileShader(ID3D11Device* device);
    bool CreateCoefficientTextures(ID3D11Device* device);
    bool CreateConfigBuffer(ID3D11Device* device);
    bool CreateSampler(ID3D11Device* device);
    bool CreateOutputTexture(ID3D11Device* device, uint32_t w, uint32_t h);

    ComPtr<ID3D11ComputeShader>      m_shader;
    ComPtr<ID3D11Buffer>             m_configCB;
    ComPtr<ID3D11SamplerState>       m_samplerLinearClamp;

    // NIS coefficient textures (uploaded once from NIS_Config.h coef_scale
    // and coef_USM arrays).
    ComPtr<ID3D11ShaderResourceView> m_coefScaleSRV;
    ComPtr<ID3D11ShaderResourceView> m_coefUsmSRV;

    // Output texture (recreated when output dimensions change).
    ComPtr<ID3D11Texture2D>           m_outputTex;
    ComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    ComPtr<ID3D11ShaderResourceView>  m_outputSRV;

    // Optimal dispatch dimensions, populated at Initialize() time. NIS
    // documents [32, 24] @ 256 threads for NVIDIA generic.
    uint32_t m_blockWidth      = 32;
    uint32_t m_blockHeight     = 24;
    uint32_t m_threadGroupSize = 256;

    // Last-configured dimensions and sharpness, so the config CB is not
    // re-uploaded if nothing changed.
    uint32_t m_inputW  = 0;
    uint32_t m_inputH  = 0;
    uint32_t m_outputW = 0;
    uint32_t m_outputH = 0;
    float    m_sharpness = -1.0f;
};

} // namespace NitLink
