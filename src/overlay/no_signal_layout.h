#pragma once

#include <string>

namespace NitLink {

struct NoSignalRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
};

struct NoSignalImageLayout {
    NoSignalRect destination;
    // Pixel-space source rectangle. Cover uses a centered crop; contain and
    // stretch use the complete source image.
    NoSignalRect source;
    bool valid = false;
};

// Renderer-independent layout for a custom No Signal image. The fit string
// accepts contain, cover, or stretch. Invalid inputs return an invalid zero
// layout instead of producing NaN, infinity, or negative dimensions.
NoSignalImageLayout CalculateNoSignalImageLayout(
    float windowWidth, float windowHeight,
    float imageWidth, float imageHeight,
    const std::string& fit);

} // namespace NitLink
