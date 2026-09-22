#include "overlay/no_signal_cache.h"

#include <iostream>

using NitLink::ShouldReloadNoSignalImage;

int main()
{
    // A changed path reloads.
    if (!ShouldReloadNoSignalImage(false, true, false, true)) return 1;
    // Re-selecting the same path explicitly still reloads.
    if (!ShouldReloadNoSignalImage(true, false, false, true)) return 2;
    // An unchanged path may reuse a valid CPU cache.
    if (ShouldReloadNoSignalImage(false, false, false, true)) return 3;
    // A missing/failed cache must be retried only on a settings event.
    if (!ShouldReloadNoSignalImage(false, false, false, false)) return 4;
    // Entering image mode is a reload boundary.
    if (!ShouldReloadNoSignalImage(false, false, true, true)) return 5;

    std::cout << "no signal cache tests passed\n";
    return 0;
}
