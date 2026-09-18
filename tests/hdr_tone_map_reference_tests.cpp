#include "renderer/hdr_tone_map.h"
#include "renderer/hdr_tone_map_hlsl.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

struct ReferenceVector {
    double inputNits;
    double expectedYPrimeSdr;
    double expectedLinearSdr;
    double expectedSrgb;
};

// Independently precomputed from ITU-R BT.2446-1 Method A Tables 2 and 3.
// The test never calls the production implementation to create expected data.
// For achromatic R=G=B, Cb'=Cr'=0, so Table 3 does not alter this curve.
constexpr std::array<ReferenceVector, 21> kReference = {{
    {0.0,      0.000000000, 0.000000000, 0.000000000},
    {0.1,      0.039433344, 0.000426651, 0.005512330},
    {0.5,      0.075027090, 0.001997669, 0.025809879},
    {1.0,      0.098476136, 0.003837023, 0.048892323},
    {2.0,      0.128774282, 0.007304491, 0.080856868},
    {5.0,      0.182415932, 0.016848089, 0.137448808},
    {10.0,     0.236163229, 0.031311903, 0.194152207},
    {25.0,     0.329892154, 0.069838156, 0.293036222},
    {50.0,     0.422521156, 0.126484207, 0.390759819},
    {75.0,     0.487318572, 0.178135760, 0.459121094},
    {100.0,    0.538747872, 0.226634155, 0.513379004},
    {150.0,    0.619817485, 0.317273045, 0.598907447},
    {203.0,    0.686855044, 0.405953435, 0.669632071},
    {300.0,    0.772031274, 0.537432275, 0.759492994},
    {400.0,    0.832773618, 0.644562002, 0.823576167},
    {600.0,    0.912909237, 0.803574679, 0.908119245},
    {1000.0,   1.000000000, 1.000000000, 1.000000000},
    {1000.001, 1.000000000, 1.000000000, 1.000000000},
    {2000.0,   1.000000000, 1.000000000, 1.000000000},
    {4000.0,   1.000000000, 1.000000000, 1.000000000},
    {10000.0,  1.000000000, 1.000000000, 1.000000000},
}};

bool CheckHr(HRESULT hr, const char* operation)
{
    if (SUCCEEDED(hr)) return true;
    std::cerr << operation << " failed: 0x" << std::hex
              << static_cast<unsigned long>(hr) << std::dec << '\n';
    return false;
}

bool RunCpuTest()
{
    constexpr double tolerance = 1.0e-9;
    double maxError = 0.0;
    double maxErrorInput = 0.0;
    double previous = -1.0;
    bool pass = true;

    for (const auto& vector : kReference) {
        const double yPrime = NitLink::HdrToneMap::Bt2446ADerivedYPrimeSdr(
            vector.inputNits);
        const double linear = NitLink::HdrToneMap::Bt2446ADerivedLuminance(
            vector.inputNits);
        const double srgb = NitLink::HdrToneMap::LinearToSrgb(linear);
        const double errors[] = {
            std::abs(yPrime - vector.expectedYPrimeSdr),
            std::abs(linear - vector.expectedLinearSdr),
            std::abs(srgb - vector.expectedSrgb),
        };
        for (double error : errors) {
            if (error > maxError) {
                maxError = error;
                maxErrorInput = vector.inputNits;
            }
            pass = pass && error <= tolerance;
        }
        pass = pass && std::isfinite(yPrime) && std::isfinite(linear) &&
               std::isfinite(srgb) && linear >= 0.0 && linear <= 1.0 &&
               linear + tolerance >= previous;
        previous = linear;
    }

    pass = pass &&
        NitLink::HdrToneMap::Bt2446ADerivedLuminance(0.0) == 0.0 &&
        std::abs(NitLink::HdrToneMap::Bt2446ADerivedLuminance(1000.0) - 1.0)
            <= tolerance;
    std::cout << "CPU " << (pass ? "PASS" : "FAIL")
              << " maxAbsError=" << std::setprecision(12) << maxError
              << " inputNits=" << maxErrorInput << '\n';
    return pass;
}

bool RunGpuTest()
{
    constexpr double relativeTolerance = 1.0e-4;
    constexpr double absoluteFloor = 1.0e-7;
    const std::string shaderSource =
        std::string(NitLink::HdrToneMap::kDerivedLuminanceHlsl) + R"(
StructuredBuffer<float> inputNits : register(t0);
RWStructuredBuffer<float4> outputValues : register(u0);

float LinearToSrgbReference(float value) {
    value = saturate(value);
    return value <= 0.0031308
        ? value * 12.92
        : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

[numthreads(32, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 21) return;
    float nits = inputNits[id.x];
    float yPrime = Bt2446ADerivedYPrimeSdr(nits);
    float mappedLinear = ToneMapLuminanceNits(nits);
    outputValues[id.x] = float4(yPrime, mappedLinear,
        LinearToSrgbReference(mappedLinear), nits);
}
)";

    NitLink::HdrToneMap::HlslMacroSet macroSet;
    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        shaderSource.data(), shaderSource.size(), "ToneMapReferenceCS",
        macroSet.macros.data(), nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr.write(static_cast<const char*>(errorBlob->GetBufferPointer()),
                            errorBlob->GetBufferSize());
        }
        return CheckHr(hr, "D3DCompile");
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    if (!CheckHr(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, &featureLevel, &context),
            "D3D11CreateDevice(WARP)")) {
        return false;
    }

    ComPtr<ID3D11ComputeShader> shader;
    if (!CheckHr(device->CreateComputeShader(shaderBlob->GetBufferPointer(),
                                             shaderBlob->GetBufferSize(),
                                             nullptr, &shader),
                 "CreateComputeShader")) {
        return false;
    }

    std::array<float, kReference.size()> inputs{};
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i] = static_cast<float>(kReference[i].inputNits);
    }
    using GpuResult = std::array<float, 4>;
    std::array<GpuResult, kReference.size()> results{};

    D3D11_BUFFER_DESC inputDesc{};
    inputDesc.ByteWidth = static_cast<UINT>(sizeof(inputs));
    inputDesc.Usage = D3D11_USAGE_IMMUTABLE;
    inputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    inputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    inputDesc.StructureByteStride = sizeof(float);
    D3D11_SUBRESOURCE_DATA inputData{};
    inputData.pSysMem = inputs.data();
    ComPtr<ID3D11Buffer> inputBuffer;
    if (!CheckHr(device->CreateBuffer(&inputDesc, &inputData, &inputBuffer),
                 "CreateBuffer(input)")) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = static_cast<UINT>(inputs.size());
    ComPtr<ID3D11ShaderResourceView> inputSrv;
    if (!CheckHr(device->CreateShaderResourceView(inputBuffer.Get(), &srvDesc,
                                                   &inputSrv),
                 "CreateShaderResourceView")) return false;

    D3D11_BUFFER_DESC outputDesc{};
    outputDesc.ByteWidth = static_cast<UINT>(sizeof(results));
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    outputDesc.StructureByteStride = sizeof(GpuResult);
    ComPtr<ID3D11Buffer> outputBuffer;
    if (!CheckHr(device->CreateBuffer(&outputDesc, nullptr, &outputBuffer),
                 "CreateBuffer(output)")) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = static_cast<UINT>(results.size());
    ComPtr<ID3D11UnorderedAccessView> outputUav;
    if (!CheckHr(device->CreateUnorderedAccessView(outputBuffer.Get(), &uavDesc,
                                                    &outputUav),
                 "CreateUnorderedAccessView")) return false;

    D3D11_BUFFER_DESC stagingDesc = outputDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Buffer> stagingBuffer;
    if (!CheckHr(device->CreateBuffer(&stagingDesc, nullptr, &stagingBuffer),
                 "CreateBuffer(staging)")) return false;

    ID3D11ShaderResourceView* srvs[] = {inputSrv.Get()};
    ID3D11UnorderedAccessView* uavs[] = {outputUav.Get()};
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, srvs);
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context->Dispatch(1, 1, 1);
    context->CopyResource(stagingBuffer.Get(), outputBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!CheckHr(context->Map(stagingBuffer.Get(), 0, D3D11_MAP_READ, 0,
                              &mapped),
                 "Map(staging)")) return false;
    std::memcpy(results.data(), mapped.pData, sizeof(results));
    context->Unmap(stagingBuffer.Get(), 0);

    bool pass = true;
    double maxRelativeError = 0.0;
    double maxRelativeInput = 0.0;
    double maxNearZeroError = 0.0;
    double maxNearZeroInput = 0.0;
    float previous = -1.0f;
    for (size_t i = 0; i < kReference.size(); ++i) {
        const double expected[] = {
            kReference[i].expectedYPrimeSdr,
            kReference[i].expectedLinearSdr,
            kReference[i].expectedSrgb,
        };
        for (size_t channel = 0; channel < 3; ++channel) {
            const double actual = results[i][channel];
            const double absolute = std::abs(actual - expected[channel]);
            if (std::abs(expected[channel]) > 1.0e-3) {
                const double relative = absolute / std::abs(expected[channel]);
                if (relative > maxRelativeError) {
                    maxRelativeError = relative;
                    maxRelativeInput = kReference[i].inputNits;
                }
                pass = pass && relative <= relativeTolerance;
            } else {
                if (absolute > maxNearZeroError) {
                    maxNearZeroError = absolute;
                    maxNearZeroInput = kReference[i].inputNits;
                }
                pass = pass && absolute <= absoluteFloor;
            }
            pass = pass && std::isfinite(actual);
        }
        const float linear = results[i][1];
        pass = pass && linear >= 0.0f && linear <= 1.0f &&
               linear + static_cast<float>(absoluteFloor) >= previous;
        previous = linear;
    }

    std::cout << "GPU " << (pass ? "PASS" : "FAIL")
              << " maxRelativeError=" << std::setprecision(12)
              << maxRelativeError << " inputNits=" << maxRelativeInput
              << " nearZeroMaxAbsError=" << maxNearZeroError
              << " nearZeroInputNits=" << maxNearZeroInput << '\n';
    return pass;
}

} // namespace

int main()
{
    std::cout << "referenceVectorVersion="
              << NitLink::HdrToneMap::kReferenceVectorVersion << '\n';
    return RunCpuTest() && RunGpuTest() ? 0 : 1;
}
