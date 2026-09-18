#pragma once

#include "hdr_tone_map.h"

#include <d3dcompiler.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <string>

namespace NitLink::HdrToneMap {

inline std::string HlslFloat(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    std::string text = stream.str();
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

struct HlslMacroSet {
    std::array<std::string, 12> values;
    std::array<D3D_SHADER_MACRO, 13> macros{};

    HlslMacroSet()
    {
        values = {
            HlslFloat(kHdrPeakNits),
            HlslFloat(kSdrPeakNits),
            HlslFloat(kTransferExponent),
            HlslFloat(kLogScale),
            HlslFloat(kKneeBoundaryLow),
            HlslFloat(kKneeBoundaryHigh),
            HlslFloat(kKneeLowSlope),
            HlslFloat(kKneeQuadratic),
            HlslFloat(kKneeLinear),
            HlslFloat(kKneeOffset),
            HlslFloat(kKneeHighSlope),
            HlslFloat(kKneeHighOffset),
        };
        static constexpr const char* names[] = {
            "NITLINK_TM_HDR_PEAK_NITS",
            "NITLINK_TM_SDR_PEAK_NITS",
            "NITLINK_TM_TRANSFER_EXPONENT",
            "NITLINK_TM_LOG_SCALE",
            "NITLINK_TM_KNEE_BOUNDARY_LOW",
            "NITLINK_TM_KNEE_BOUNDARY_HIGH",
            "NITLINK_TM_KNEE_LOW_SLOPE",
            "NITLINK_TM_KNEE_QUADRATIC",
            "NITLINK_TM_KNEE_LINEAR",
            "NITLINK_TM_KNEE_OFFSET",
            "NITLINK_TM_KNEE_HIGH_SLOPE",
            "NITLINK_TM_KNEE_HIGH_OFFSET",
        };
        for (size_t i = 0; i < values.size(); ++i) {
            macros[i] = {names[i], values[i].c_str()};
        }
        macros.back() = {nullptr, nullptr};
    }
};

// This exact function is injected into the production P010 pixel shader and
// compiled by the GPU reference test. Constants have no HLSL-side defaults;
// omission is a compile error so the C++ table cannot silently drift.
inline constexpr const char* kDerivedLuminanceHlsl = R"(
#ifndef NITLINK_TM_HDR_PEAK_NITS
#error NITLINK_TM_HDR_PEAK_NITS must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_SDR_PEAK_NITS
#error NITLINK_TM_SDR_PEAK_NITS must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_TRANSFER_EXPONENT
#error NITLINK_TM_TRANSFER_EXPONENT must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_LOG_SCALE
#error NITLINK_TM_LOG_SCALE must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_KNEE_BOUNDARY_LOW
#error NITLINK_TM_KNEE_BOUNDARY_LOW must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_KNEE_BOUNDARY_HIGH
#error NITLINK_TM_KNEE_BOUNDARY_HIGH must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_KNEE_LOW_SLOPE
#error NITLINK_TM_KNEE_LOW_SLOPE must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_KNEE_QUADRATIC
#error NITLINK_TM_KNEE_QUADRATIC must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_KNEE_LINEAR
#error NITLINK_TM_KNEE_LINEAR must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_KNEE_OFFSET
#error NITLINK_TM_KNEE_OFFSET must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_KNEE_HIGH_SLOPE
#error NITLINK_TM_KNEE_HIGH_SLOPE must be injected from the C++ constant table
#endif
#ifndef NITLINK_TM_KNEE_HIGH_OFFSET
#error NITLINK_TM_KNEE_HIGH_OFFSET must be injected from the C++ constant table
#endif

float Bt2446ADerivedYPrimeSdr(float nits) {
    float hdrLinear = saturate(nits / NITLINK_TM_HDR_PEAK_NITS);
    float hdrPrime = pow(hdrLinear, 1.0 / NITLINK_TM_TRANSFER_EXPONENT);
    float rhoHdr = 1.0 + NITLINK_TM_LOG_SCALE *
        pow(NITLINK_TM_HDR_PEAK_NITS / 10000.0,
            1.0 / NITLINK_TM_TRANSFER_EXPONENT);
    float perceptual = log(1.0 + (rhoHdr - 1.0) * hdrPrime) / log(rhoHdr);

    float compressed;
    if (perceptual <= NITLINK_TM_KNEE_BOUNDARY_LOW) {
        compressed = NITLINK_TM_KNEE_LOW_SLOPE * perceptual;
    } else if (perceptual < NITLINK_TM_KNEE_BOUNDARY_HIGH) {
        compressed = NITLINK_TM_KNEE_QUADRATIC * perceptual * perceptual +
                     NITLINK_TM_KNEE_LINEAR * perceptual +
                     NITLINK_TM_KNEE_OFFSET;
    } else {
        compressed = NITLINK_TM_KNEE_HIGH_SLOPE * perceptual +
                     NITLINK_TM_KNEE_HIGH_OFFSET;
    }

    float rhoSdr = 1.0 + NITLINK_TM_LOG_SCALE *
        pow(NITLINK_TM_SDR_PEAK_NITS / 10000.0,
            1.0 / NITLINK_TM_TRANSFER_EXPONENT);
    return saturate((pow(rhoSdr, compressed) - 1.0) / (rhoSdr - 1.0));
}

float ToneMapLuminanceNits(float nits) {
    float sdrPrime = Bt2446ADerivedYPrimeSdr(max(nits, 0.0));
    return pow(sdrPrime, NITLINK_TM_TRANSFER_EXPONENT);
}
)";

inline bool InjectDerivedLuminanceFunction(std::string& shaderSource,
                                           const char* marker)
{
    const size_t position = shaderSource.find(marker);
    if (position == std::string::npos) return false;
    shaderSource.replace(position, std::char_traits<char>::length(marker),
                         kDerivedLuminanceHlsl);
    return true;
}

} // namespace NitLink::HdrToneMap
