#pragma once

#include <algorithm>
#include <cmath>

namespace NitLink::HdrToneMap {

// Single source of truth for the production CPU implementation and the HLSL
// macro table. The curve is the achromatic luminance mapping derived from
// ITU-R BT.2446-1 Method A; it is not the complete Method A color pipeline.
constexpr double kHdrPeakNits = 1000.0;
constexpr double kSdrPeakNits = 100.0;
constexpr double kTransferExponent = 2.4;
constexpr double kLogScale = 32.0;
constexpr double kKneeBoundaryLow = 0.7399;
constexpr double kKneeBoundaryHigh = 0.9909;
constexpr double kKneeLowSlope = 1.0770;
constexpr double kKneeQuadratic = -1.1510;
constexpr double kKneeLinear = 2.7811;
constexpr double kKneeOffset = -0.6302;
constexpr double kKneeHighSlope = 0.5000;
constexpr double kKneeHighOffset = 0.5000;
constexpr const char* kReferenceVectorVersion =
    "bt2446a-derived-achromatic-v1-20260919";

inline double ClampUnit(double value)
{
    return std::max(0.0, std::min(1.0, value));
}

// For achromatic R=G=B input, Cb'=Cr'=0, so the later Method A chroma/luma
// correction leaves this reference luminance curve unchanged.
inline double Bt2446ADerivedYPrimeSdr(double nits)
{
    if (!(nits > 0.0) || !std::isfinite(nits)) return 0.0;

    const double hdrLinear = ClampUnit(nits / kHdrPeakNits);
    const double hdrPrime = std::pow(hdrLinear, 1.0 / kTransferExponent);
    const double rhoHdr = 1.0 + kLogScale *
        std::pow(kHdrPeakNits / 10000.0, 1.0 / kTransferExponent);
    const double perceptual = std::log(1.0 + (rhoHdr - 1.0) * hdrPrime) /
                              std::log(rhoHdr);

    double compressed = 0.0;
    if (perceptual <= kKneeBoundaryLow) {
        compressed = kKneeLowSlope * perceptual;
    } else if (perceptual < kKneeBoundaryHigh) {
        compressed = kKneeQuadratic * perceptual * perceptual +
                     kKneeLinear * perceptual + kKneeOffset;
    } else {
        compressed = kKneeHighSlope * perceptual + kKneeHighOffset;
    }

    const double rhoSdr = 1.0 + kLogScale *
        std::pow(kSdrPeakNits / 10000.0, 1.0 / kTransferExponent);
    const double sdrPrime = (std::pow(rhoSdr, compressed) - 1.0) /
                            (rhoSdr - 1.0);
    return std::isfinite(sdrPrime) ? ClampUnit(sdrPrime) : 0.0;
}

inline double Bt2446ADerivedLuminance(double nits)
{
    const double sdrPrime = Bt2446ADerivedYPrimeSdr(nits);
    const double mapped = std::pow(sdrPrime, kTransferExponent);
    return std::isfinite(mapped) ? ClampUnit(mapped) : 0.0;
}

inline double LinearToSrgb(double value)
{
    value = ClampUnit(value);
    return value <= 0.0031308
        ? value * 12.92
        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

} // namespace NitLink::HdrToneMap
