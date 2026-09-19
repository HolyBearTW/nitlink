#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace NitLink {

// Retains the measured source cadence while captured pictures repeat.
// Cadence holds keep still menus feeding external frame generation and
// let isolated picture changes reach a present before the 250 ms keepalive.
//
// Cadence is measured in captured frames between new-content verdicts.
// Extended duplicate runs repeat the latest picture at the retained cadence
// without changing the frame differ's verdicts.
class SourceCadence {
public:
    // Call once per fresh captured frame the differ classified.
    void OnClassifiedFrame(bool isNew)
    {
        if (m_freshSincePresent < kCountCap) ++m_freshSincePresent;
        if (isNew) {
            const uint32_t spacing = m_framesSinceNew + 1;
            if (spacing <= kMaxCadenceFrames) RecordSpacing(spacing);
            m_framesSinceNew = 0;
            m_duplicateRun   = 0;
        } else {
            if (m_framesSinceNew < kCountCap) ++m_framesSinceNew;
            if (m_duplicateRun < kCountCap)   ++m_duplicateRun;
        }
    }

    // Call once per fresh captured frame that received no differ verdict.
    void OnUnclassifiedFrame()
    {
        if (m_freshSincePresent < kCountCap) ++m_freshSincePresent;
    }

    // True when a still picture has outlasted the next expected new frame
    // and a present is due at the source cadence.
    bool ShouldHold() const
    {
        return m_duplicateRun >= std::max<uint32_t>(2, m_cadenceFrames) &&
               m_freshSincePresent >= m_cadenceFrames;
    }

    // Call on every present, whatever triggered it.
    void OnPresent() { m_freshSincePresent = 0; }

    // Call when capture restarts, since the new stream may run at another rate.
    void Reset() { *this = SourceCadence{}; }

    uint32_t CadenceFrames() const { return m_cadenceFrames; }
    uint32_t DuplicateRun()  const { return m_duplicateRun; }

private:
    // Exclude long gaps so a pause does not lower the retained cadence.
    static constexpr uint32_t kMaxCadenceFrames = 4;
    static constexpr size_t   kHistory          = 8;
    static constexpr uint32_t kCountCap         = 1u << 30;

    void RecordSpacing(uint32_t spacing)
    {
        m_history[m_historyNext] = static_cast<uint8_t>(spacing);
        m_historyNext = (m_historyNext + 1) % kHistory;
        if (m_historyCount < kHistory) ++m_historyCount;

        // Lower median: an even split between two spacings resolves to the
        // shorter one, so a still picture presents at the faster rate.
        std::array<uint8_t, kHistory> sorted = m_history;
        std::sort(sorted.begin(), sorted.begin() + m_historyCount);
        m_cadenceFrames = sorted[(m_historyCount - 1) / 2];
    }

    std::array<uint8_t, kHistory> m_history{};
    size_t   m_historyNext       = 0;
    size_t   m_historyCount      = 0;
    uint32_t m_cadenceFrames     = 1;
    uint32_t m_framesSinceNew    = 0;
    uint32_t m_duplicateRun      = 0;
    uint32_t m_freshSincePresent = 0;
};

} // namespace NitLink
