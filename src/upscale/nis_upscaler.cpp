#include "nis_upscaler.h"

#include <d3dcompiler.h>
#include <debugapi.h>
#include <sstream>
#include <string>
#include <fstream>
#include <vector>
#include <chrono>

// Include NVIDIA's host-side config helpers (NISConfig struct, coefficient
// arrays, NVScalerUpdateConfig). This is plain C++ -- not the shader code.
// Note: the coefficient arrays live inside an anonymous namespace in
// NIS_Config.h, so they're only visible within THIS translation unit.
#include "../../third_party/nis/NIS_Config.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace NitLink {

static void NisLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/NIS] " + msg + L"\n").c_str());
}

static void NisLogHr(const std::wstring& what, HRESULT hr) {
    std::wstringstream ss;
    ss << L"[NitLink/NIS] " << what << L" hr=0x" << std::hex << hr;
    OutputDebugStringW(ss.str().c_str());
    OutputDebugStringW(L"\n");
}

NisUpscaler::NisUpscaler() = default;
NisUpscaler::~NisUpscaler() { Shutdown(); }

// ---------------------------------------------------------------------------
//  Shader compilation
// ---------------------------------------------------------------------------
// NIS_Main.hlsl is embedded as a string literal (kNisMainHlsl below).
// NIS_Scaler.h is loaded from disk via NisIncludeHandler: the handler
// tries a few plausible relative paths next to the running exe. Both
// sources come from third_party/nis/ in the repo.

// NIS_Main.hlsl (entry point + bindings) -- see third_party/nis/NIS_Main.hlsl
static const char* kNisMainHlsl = R"NISHLSL(
#define NIS_HLSL 1
#ifndef NIS_SCALER
#define NIS_SCALER 1
#endif

cbuffer cb : register(b0)
{
    float kDetectRatio;
    float kDetectThres;
    float kMinContrastRatio;
    float kRatioNorm;

    float kContrastBoost;
    float kEps;
    float kSharpStartY;
    float kSharpScaleY;

    float kSharpStrengthMin;
    float kSharpStrengthScale;
    float kSharpLimitMin;
    float kSharpLimitScale;

    float kScaleX;
    float kScaleY;

    float kDstNormX;
    float kDstNormY;
    float kSrcNormX;
    float kSrcNormY;

    uint kInputViewportOriginX;
    uint kInputViewportOriginY;
    uint kInputViewportWidth;
    uint kInputViewportHeight;

    uint kOutputViewportOriginX;
    uint kOutputViewportOriginY;
    uint kOutputViewportWidth;
    uint kOutputViewportHeight;

    float reserved0;
    float reserved1;
};

SamplerState samplerLinearClamp : register(s0);
Texture2D in_texture            : register(t0);
RWTexture2D<float4> out_texture : register(u0);
Texture2D coef_scaler           : register(t1);
Texture2D coef_usm              : register(t2);

#include "NIS_Scaler.h"

[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)
{
    NVScaler(blockIdx.xy, threadIdx.x);
}
)NISHLSL";

// Custom include-handler so D3DCompile can resolve `#include "NIS_Scaler.h"`.
// The handler loads it from disk at the well-known relative path next to
// the exe; the file is part of the repo at third_party/nis/.
class NisIncludeHandler : public ID3DInclude {
public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE /*incType*/, LPCSTR pFileName,
                            LPCVOID /*pParentData*/, LPCVOID* ppData, UINT* pBytes) override
    {
        // Resolve NIS_Scaler.h next to the executable first. CMake copies it to
        // <exeDir>/third_party/nis/ at build time, but std::ifstream resolves
        // relative paths against the current working directory, not the exe
        // directory. An installed or shortcut launch, or "Run as administrator"
        // (working directory becomes System32), has a working directory
        // unrelated to the exe, so a CWD-relative lookup misses and silently
        // disables upscaling. Build the exe-directory prefix and try it first;
        // keep the CWD-relative candidates as a development fallback.
        std::string exePrefix;
        {
            char exePath[MAX_PATH] = {};
            DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                std::string dir(exePath, exePath + n);
                size_t slash = dir.find_last_of("\\/");
                if (slash != std::string::npos) {
                    exePrefix = dir.substr(0, slash + 1) + "third_party/nis/";
                }
            }
        }

        // Try several plausible paths, in priority order.
        std::string candidates[] = {
            exePrefix,                    // next to the exe (installed launches)
            "third_party/nis/",          // running from repo root
            "../../third_party/nis/",     // running from build subdir
            "../../../third_party/nis/",  // VS multi-config layouts
        };
        std::string body;
        for (const std::string& prefix : candidates) {
            if (prefix.empty()) continue;
            std::string path = prefix + pFileName;
            std::ifstream f(path, std::ios::binary);
            if (!f.is_open()) continue;
            std::stringstream ss;
            ss << f.rdbuf();
            body = ss.str();
            break;
        }
        if (body.empty()) {
            std::wstring wpath(pFileName, pFileName + strlen(pFileName));
            NisLog(L"include not found: " + wpath);
            return E_FAIL;
        }

        char* buf = new char[body.size()];
        memcpy(buf, body.data(), body.size());
        *ppData = buf;
        *pBytes = (UINT)body.size();
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID pData) override
    {
        delete[] (char*)pData;
        return S_OK;
    }
};

bool NisUpscaler::CompileShader(ID3D11Device* device)
{
    // Optimal dispatch sizes for typical NVIDIA hardware via NIS_Config.h's
    // NISOptimizer with NVIDIA_Generic. The same values get baked into the
    // shader as macros below AND used at Dispatch() time, so the shader's
    // [numthreads(...)] and the groupsX/Y math stay in sync by construction.
    NISOptimizer opt(true, NISGPUArchitecture::NVIDIA_Generic_fp16);
    m_blockWidth      = opt.GetOptimalBlockWidth();
    m_blockHeight     = opt.GetOptimalBlockHeight();
    m_threadGroupSize = opt.GetOptimalThreadGroupSize();

    std::string bw = std::to_string(m_blockWidth);
    std::string bh = std::to_string(m_blockHeight);
    std::string tg = std::to_string(m_threadGroupSize);

    // D3D_SHADER_MACRO array is null-terminated.
    D3D_SHADER_MACRO defines[] = {
        { "NIS_HLSL",               "1" },
        { "NIS_SCALER",             "1" },
        { "NIS_HDR_MODE",           "0" },
        { "NIS_USE_HALF_PRECISION", "1" },
        { "NIS_BLOCK_WIDTH",      bw.c_str() },
        { "NIS_BLOCK_HEIGHT",     bh.c_str() },
        { "NIS_THREAD_GROUP_SIZE", tg.c_str() },
        { nullptr, nullptr }
    };

    NisIncludeHandler includer;
    ComPtr<ID3DBlob> shaderBlob, errorBlob;
    HRESULT hr = D3DCompile(
        kNisMainHlsl, strlen(kNisMainHlsl),
        "NIS_Main.hlsl",
        defines,
        &includer,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob, &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob && errorBlob->GetBufferSize() > 0) {
            std::string err((const char*)errorBlob->GetBufferPointer(),
                             errorBlob->GetBufferSize());
            std::wstring werr(err.begin(), err.end());
            NisLog(L"shader compile error: " + werr);
        }
        NisLogHr(L"D3DCompile failed", hr);
        return false;
    }

    hr = device->CreateComputeShader(shaderBlob->GetBufferPointer(),
                                       shaderBlob->GetBufferSize(),
                                       nullptr, &m_shader);
    if (FAILED(hr)) {
        NisLogHr(L"CreateComputeShader failed", hr);
        return false;
    }

    NisLog(L"shader compiled OK");
    return true;
}

// ---------------------------------------------------------------------------
//  Coefficient textures
// ---------------------------------------------------------------------------
// NIS_Config.h declares (in an anonymous namespace, accessible here):
//   constexpr float coef_scale[kPhaseCount][kFilterSize]   // 64 x 8
//   constexpr float coef_usm  [kPhaseCount][kFilterSize]   // 64 x 8
//
// kFilterSize = 8 (already includes 2 padding lanes the shader ignores:
// NIS_Scaler.h actually only uses the first 6 per row). Uploaded as a
// 2-texels-wide x 64-rows-tall R32G32B32A32_FLOAT texture so each row is
// exactly 2 RGBA texels = 8 floats. NIS_Scaler.h's LoadFilterBanksSh loads
// these with .Load(int2(vIdx, phase)) for vIdx in {0, 1}.
bool NisUpscaler::CreateCoefficientTextures(ID3D11Device* device)
{
    constexpr int kPhases = (int)kPhaseCount;   // 64
    constexpr int kFilter = (int)kFilterSize;   // 8
    constexpr int kTexW   = kFilter / 4;        // 2 RGBA texels per row

    // Flatten the row-major arrays into linear float vectors that DX11 can
    // accept as initial subresource data.
    std::vector<uint16_t> scaleData(kPhases * kFilter);
    std::vector<uint16_t> usmData  (kPhases * kFilter);
    for (int p = 0; p < kPhases; p++) {
        for (int f = 0; f < kFilter; f++) {
            scaleData[p * kFilter + f] = coef_scale_fp16[p][f];
            usmData  [p * kFilter + f] = coef_usm_fp16  [p][f];
        }
    }

    auto makeTex = [device](const std::vector<uint16_t>& data,
                             ComPtr<ID3D11ShaderResourceView>& srv,
                             const wchar_t* label) -> bool
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = kTexW;
        td.Height           = kPhases;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_IMMUTABLE;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem     = data.data();
        init.SysMemPitch = kTexW * 4 * sizeof(uint16_t); // 2 texels * 4 halfs * 2 bytes

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&td, &init, &tex);
        if (FAILED(hr)) {
            NisLogHr(std::wstring(label) + L" CreateTexture2D failed", hr);
            return false;
        }
        hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        if (FAILED(hr)) {
            NisLogHr(std::wstring(label) + L" CreateSRV failed", hr);
            return false;
        }
        return true;
    };

    if (!makeTex(scaleData, m_coefScaleSRV, L"coef_scale")) return false;
    if (!makeTex(usmData,   m_coefUsmSRV,   L"coef_usm"))   return false;

    NisLog(L"coefficient textures uploaded (64x2 RGBA32F each)");
    return true;
}

// ---------------------------------------------------------------------------
//  Constant buffer (NISConfig)
// ---------------------------------------------------------------------------
bool NisUpscaler::CreateConfigBuffer(ID3D11Device* device)
{
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(NISConfig);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device->CreateBuffer(&bd, nullptr, &m_configCB);
    if (FAILED(hr)) { NisLogHr(L"CB CreateBuffer failed", hr); return false; }
    return true;
}

// ---------------------------------------------------------------------------
//  Linear-clamp sampler
// ---------------------------------------------------------------------------
bool NisUpscaler::CreateSampler(ID3D11Device* device)
{
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;

    HRESULT hr = device->CreateSamplerState(&sd, &m_samplerLinearClamp);
    if (FAILED(hr)) { NisLogHr(L"CreateSamplerState failed", hr); return false; }
    return true;
}

// ---------------------------------------------------------------------------
//  Output texture (recreated on size change)
// ---------------------------------------------------------------------------
bool NisUpscaler::CreateOutputTexture(ID3D11Device* device, uint32_t w, uint32_t h)
{
    m_outputSRV.Reset();
    m_outputUAV.Reset();
    m_outputTex.Reset();

    if (w == 0 || h == 0) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_outputTex);
    if (FAILED(hr)) { NisLogHr(L"output CreateTexture2D failed", hr); return false; }

    hr = device->CreateUnorderedAccessView(m_outputTex.Get(), nullptr, &m_outputUAV);
    if (FAILED(hr)) { NisLogHr(L"output CreateUAV failed", hr); return false; }

    hr = device->CreateShaderResourceView(m_outputTex.Get(), nullptr, &m_outputSRV);
    if (FAILED(hr)) { NisLogHr(L"output CreateSRV failed", hr); return false; }

    std::wstringstream ss;
    ss << L"output texture (re)created " << w << L"x" << h;
    NisLog(ss.str());
    return true;
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
bool NisUpscaler::Initialize(ID3D11Device* device)
{
    if (!device) return false;
    NisLog(L"Initialize");

    if (!CompileShader(device))             return false;
    if (!CreateCoefficientTextures(device)) return false;
    if (!CreateConfigBuffer(device))        return false;
    if (!CreateSampler(device))             return false;

    NisLog(L"Initialize OK");
    return true;
}

void NisUpscaler::Shutdown()
{
    m_outputSRV.Reset();
    m_outputUAV.Reset();
    m_outputTex.Reset();
    m_coefUsmSRV.Reset();
    m_coefScaleSRV.Reset();
    m_samplerLinearClamp.Reset();
    m_configCB.Reset();
    m_shader.Reset();
}

bool NisUpscaler::Configure(ID3D11DeviceContext* ctx,
                              uint32_t inputW, uint32_t inputH,
                              uint32_t outputW, uint32_t outputH,
                              float sharpness)
{
    if (!m_shader)   { NisLog(L"Configure: m_shader is null");   return false; }
    if (!m_configCB) { NisLog(L"Configure: m_configCB is null"); return false; }
    if (inputW == 0 || inputH == 0 || outputW == 0 || outputH == 0) {
        std::wstringstream ss;
        ss << L"Configure: invalid dims " << inputW << L"x" << inputH
           << L" -> " << outputW << L"x" << outputH;
        NisLog(ss.str());
        return false;
    }

    // Recreate output texture if size changed. m_outputW/H are NOT updated
    // here: those are the "last successful config upload" markers, and the
    // cache-skip check below relies on them still showing the OLD values
    // so this call is recognized as a real change. They're updated only
    // after the config CB upload succeeds.
    if (outputW != m_outputW || outputH != m_outputH || !m_outputTex) {
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        if (!CreateOutputTexture(device.Get(), outputW, outputH)) {
            NisLog(L"Configure: CreateOutputTexture failed");
            return false;
        }
    }

    // Skip re-upload if nothing changed since last frame. m_inputW/H,
    // m_outputW/H, and m_sharpness all reflect the last SUCCESSFUL upload,
    // so any genuine change (including scale-mode changes that resize the
    // output texture) correctly invalidates the cache.
    if (inputW == m_inputW && inputH == m_inputH &&
        outputW == m_outputW && outputH == m_outputH &&
        sharpness == m_sharpness)
    {
        return true;
    }

    // Use NVIDIA's host-side helper to populate the NISConfig struct.
    NISConfig cfg{};
    bool ok = NVScalerUpdateConfig(cfg, sharpness,
        /*inputViewport origin/size*/  0, 0, inputW, inputH,
        /*inputTexture size*/          inputW, inputH,
        /*outputViewport origin/size*/ 0, 0, outputW, outputH,
        /*outputTexture size*/         outputW, outputH,
        NISHDRMode::None);
    if (!ok) {
        std::wstringstream ss;
        ss << L"Configure: NVScalerUpdateConfig returned false for "
           << inputW << L"x" << inputH << L" -> " << outputW << L"x" << outputH
           << L" (NIS may not support this scale ratio)";
        NisLog(ss.str());
        return false;
    }

    // Map the dynamic CB and copy
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(m_configCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { NisLogHr(L"Map config CB failed", hr); return false; }
    memcpy(mapped.pData, &cfg, sizeof(NISConfig));
    ctx->Unmap(m_configCB.Get(), 0);

    // Only cache after successful upload. All five values match the config
    // just sent to the GPU.
    m_inputW    = inputW;
    m_inputH    = inputH;
    m_outputW   = outputW;
    m_outputH   = outputH;
    m_sharpness = sharpness;

    std::wstringstream ss;
    ss << L"Configure OK: " << inputW << L"x" << inputH
       << L" -> " << outputW << L"x" << outputH
       << L" sharp=" << sharpness
       << L" scaleX=" << cfg.kScaleX << L" scaleY=" << cfg.kScaleY;
    NisLog(ss.str());

    return true;
}

void NisUpscaler::Dispatch(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* inputSRV)
{
    if (!m_shader)    { NisLog(L"Dispatch: m_shader is null");    return; }
    if (!m_outputUAV) { NisLog(L"Dispatch: m_outputUAV is null"); return; }
    if (!inputSRV)    { NisLog(L"Dispatch: inputSRV is null");    return; }

    // CRITICAL: Unbind all render targets before binding the input texture as
    // an SRV. The caller's previous draw (DrawCaptureFrame writing to the
    // post-input intermediate) leaves that texture set as an active RTV. D3D11
    // refuses to let the same resource be bound as both output AND input
    // simultaneously: it silently forces the SRV to NULL, producing a black
    // result. OMRenderTargets is unbound first to break the hazard.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    // ---- Bind ----
    ctx->CSSetShader(m_shader.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, m_configCB.GetAddressOf());

    // SRVs: input @ t0, coef_scaler @ t1, coef_usm @ t2 (matching NIS_Main.hlsl)
    ID3D11ShaderResourceView* srvs[3] = {
        inputSRV,
        m_coefScaleSRV.Get(),
        m_coefUsmSRV.Get()
    };
    ctx->CSSetShaderResources(0, 3, srvs);

    // UAV @ u0
    UINT initCounts = 0;
    ctx->CSSetUnorderedAccessViews(0, 1, m_outputUAV.GetAddressOf(), &initCounts);

    // Sampler @ s0
    ctx->CSSetSamplers(0, 1, m_samplerLinearClamp.GetAddressOf());

    // ---- Dispatch ----
    const UINT groupsX = (m_outputW + m_blockWidth  - 1) / m_blockWidth;
    const UINT groupsY = (m_outputH + m_blockHeight - 1) / m_blockHeight;
    ctx->Dispatch(groupsX, groupsY, 1);

    // ---- Unbind ----
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    ctx->CSSetShaderResources(0, 3, nullSRVs);

    // Log once per second to confirm dispatch happened
    static auto lastDispatchLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - lastDispatchLog).count() >= 1.0) {
        std::wstringstream ss;
        ss << L"Dispatch OK: groups " << groupsX << L"x" << groupsY
           << L" -> output " << m_outputW << L"x" << m_outputH;
        NisLog(ss.str());
        lastDispatchLog = now;
    }
}

} // namespace NitLink
