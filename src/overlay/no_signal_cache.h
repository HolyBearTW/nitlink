#pragma once

namespace NitLink {

// An explicit user selection reloads even when the path is unchanged because
// the file may have been replaced externally. Resource recreation is separate:
// it rebuilds only the device-dependent bitmap from the retained CPU pixels.
inline bool ShouldReloadNoSignalImage(bool forceReload,
                                      bool pathChanged,
                                      bool modeChanged,
                                      bool hasCpuPixels)
{
    return forceReload || pathChanged || modeChanged || !hasCpuPixels;
}

} // namespace NitLink
