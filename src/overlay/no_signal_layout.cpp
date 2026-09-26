#include "no_signal_layout.h"

#include <algorithm>
#include <cmath>

namespace NitLink {
namespace {

bool FinitePositive(float value)
{
    return std::isfinite(value) && value > 0.0f;
}

bool FiniteRect(const NoSignalRect& rect)
{
    return std::isfinite(rect.left) && std::isfinite(rect.top) &&
           std::isfinite(rect.right) && std::isfinite(rect.bottom) &&
           rect.Width() > 0.0f && rect.Height() > 0.0f;
}

} // namespace

NoSignalImageLayout CalculateNoSignalImageLayout(
    float windowWidth, float windowHeight,
    float imageWidth, float imageHeight,
    const std::string& fit)
{
    NoSignalImageLayout result;
    if (!FinitePositive(windowWidth) || !FinitePositive(windowHeight) ||
        !FinitePositive(imageWidth) || !FinitePositive(imageHeight)) {
        return result;
    }

    const NoSignalRect window = {0.0f, 0.0f, windowWidth, windowHeight};
    result.source = {0.0f, 0.0f, imageWidth, imageHeight};

    if (fit == "stretch") {
        result.destination = window;
    } else if (fit == "contain") {
        const float scale = std::min(windowWidth / imageWidth,
                                     windowHeight / imageHeight);
        if (!FinitePositive(scale)) return {};
        const float drawWidth = imageWidth * scale;
        const float drawHeight = imageHeight * scale;
        result.destination = {
            (windowWidth - drawWidth) * 0.5f,
            (windowHeight - drawHeight) * 0.5f,
            (windowWidth + drawWidth) * 0.5f,
            (windowHeight + drawHeight) * 0.5f,
        };
    } else if (fit == "cover") {
        const float scale = std::max(windowWidth / imageWidth,
                                     windowHeight / imageHeight);
        if (!FinitePositive(scale)) return {};
        const float visibleWidth = windowWidth / scale;
        const float visibleHeight = windowHeight / scale;
        const float sourceLeft = (imageWidth - visibleWidth) * 0.5f;
        const float sourceTop = (imageHeight - visibleHeight) * 0.5f;
        result.destination = window;
        result.source = {
            std::max(0.0f, sourceLeft),
            std::max(0.0f, sourceTop),
            std::min(imageWidth, sourceLeft + visibleWidth),
            std::min(imageHeight, sourceTop + visibleHeight),
        };
    } else {
        return {};
    }

    result.valid = FiniteRect(result.destination) && FiniteRect(result.source);
    return result.valid ? result : NoSignalImageLayout{};
}

} // namespace NitLink
