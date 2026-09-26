#include "overlay/no_signal_layout.h"

#include <cmath>
#include <iostream>

using NitLink::CalculateNoSignalImageLayout;
using NitLink::NoSignalImageLayout;

namespace {

bool Valid(const NoSignalImageLayout& layout)
{
    const auto finite = [](float value) {
        return std::isfinite(value) && value >= 0.0f;
    };
    const auto validRect = [&](const auto& rect) {
        return finite(rect.left) && finite(rect.top) &&
               finite(rect.right) && finite(rect.bottom) &&
               rect.right > rect.left && rect.bottom > rect.top;
    };
    return layout.valid && validRect(layout.destination) &&
           validRect(layout.source);
}

bool Approx(float a, float b)
{
    return std::abs(a - b) < 0.01f;
}

} // namespace

int main()
{
    const auto landscapeContain =
        CalculateNoSignalImageLayout(1920, 1080, 1600, 1200, "contain");
    if (!Valid(landscapeContain) ||
        !Approx(landscapeContain.destination.Width(), 1440.0f) ||
        !Approx(landscapeContain.destination.Height(), 1080.0f)) return 1;

    const auto portraitContain =
        CalculateNoSignalImageLayout(1920, 1080, 600, 1200, "contain");
    if (!Valid(portraitContain) ||
        !Approx(portraitContain.destination.Width(), 540.0f) ||
        !Approx(portraitContain.destination.Height(), 1080.0f)) return 2;

    const auto squareContain =
        CalculateNoSignalImageLayout(1920, 1080, 800, 800, "contain");
    if (!Valid(squareContain) ||
        !Approx(squareContain.destination.Width(), 1080.0f) ||
        !Approx(squareContain.destination.Height(), 1080.0f)) return 3;

    const auto landscapeCover =
        CalculateNoSignalImageLayout(1920, 1080, 1600, 1200, "cover");
    if (!Valid(landscapeCover) ||
        !Approx(landscapeCover.destination.Width(), 1920.0f) ||
        !Approx(landscapeCover.destination.Height(), 1080.0f) ||
        landscapeCover.source.Height() >= 1200.0f) return 4;

    const auto portraitCover =
        CalculateNoSignalImageLayout(1920, 1080, 600, 1200, "cover");
    if (!Valid(portraitCover) ||
        !Approx(portraitCover.destination.Width(), 1920.0f) ||
        !Approx(portraitCover.destination.Height(), 1080.0f) ||
        portraitCover.source.Height() >= 1200.0f) return 5;

    const auto stretch =
        CalculateNoSignalImageLayout(1920, 1080, 600, 1200, "stretch");
    if (!Valid(stretch) ||
        !Approx(stretch.destination.Width(), 1920.0f) ||
        !Approx(stretch.destination.Height(), 1080.0f) ||
        !Approx(stretch.source.Width(), 600.0f) ||
        !Approx(stretch.source.Height(), 1200.0f)) return 6;

    if (CalculateNoSignalImageLayout(0, 1080, 800, 600, "contain").valid) return 7;
    if (CalculateNoSignalImageLayout(1920, 0, 800, 600, "contain").valid) return 8;
    if (CalculateNoSignalImageLayout(1920, 1080, 0, 600, "cover").valid) return 9;
    if (CalculateNoSignalImageLayout(1920, 1080, 800, 0, "stretch").valid) return 10;
    if (CalculateNoSignalImageLayout(1920, 1080, 800, 600, "invalid").valid) return 11;

    std::cout << "no signal layout tests passed\n";
    return 0;
}
