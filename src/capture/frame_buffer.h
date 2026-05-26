#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>

namespace NitLink {

class FrameBuffer {
public:
    struct FrameData {
        const uint8_t* data = nullptr;
        uint32_t size   = 0;
        uint32_t width  = 0;
        uint32_t height = 0;
        int64_t  timestamp = 0;
        int64_t  arrivalWallNs = 0;     // steady_clock ns at MF callback
        uint64_t deviceTimestamp = 0;   // MFSampleExtension_DeviceTimestamp (QPC 100ns)
    };

    FrameBuffer(uint32_t width, uint32_t height, uint32_t stride);
    ~FrameBuffer() = default;

    // Producer (capture thread) writes frames
    void Write(const uint8_t* data, uint32_t size, int64_t timestamp,
               int64_t arrivalWallNs = 0, uint64_t deviceTimestamp = 0);

    // Consumer (render thread) reads latest frame
    // Returns true if a new frame is available since last read
    bool Read(FrameData& outFrame);

    // Stats
    uint64_t GetFramesWritten()       const { return m_framesWritten; }
    // Frames where the captured content actually changed from the previous
    // frame. This is the real game framerate -- the Elgato delivers 60 over
    // HDMI even when the game is rendering at 30 (it duplicates each frame).
    uint64_t GetUniqueFramesWritten() const { return m_uniqueFramesWritten; }
    uint64_t GetFramesDropped()       const { return m_framesDropped; }

    // Signal detection -- returns true if recent frames have been changing
    // (real gameplay), false if the same frame has repeated for ~500ms
    // (no signal, capture card showing placeholder). Vendor-agnostic, works
    // by hashing a sample of pixels from each frame.
    bool IsSignalActive() const { return m_signalActive.load(); }

private:
    // Triple buffer.
    //
    // The previous 2-buffer scheme had a torn-read race: Read() released the
    // swap mutex before the renderer finished its multi-millisecond GPU
    // upload, and Write() only locked across the index swap, not the memcpy.
    // After Read returned a pointer into m_buffers[m_readIndex] and the
    // renderer began uploading, two successive Writes could (a) commit a
    // new frame into the other buffer, then (b) start the next memcpy back
    // into the buffer the renderer was still reading from: top half from
    // frame N, bottom half from frame N+2.
    //
    // With three buffers and per-slot role tracking (reader / fresh / free),
    // the producer can always pick a slot that's neither held by the reader
    // nor the current fresh buffer, so there's never an overlap with the
    // renderer's in-flight upload. No back-pressure on the capture thread.
    struct Buffer {
        std::vector<uint8_t> data;
        uint32_t actualSize = 0;   // bytes actually written this frame
                                    // (data.size() is the capacity, which is
                                    // sized for the worst-case format:
                                    // BGRA at 4 bytes/pixel)
        int64_t  timestamp = 0;
        int64_t  arrivalWallNs = 0;
        uint64_t deviceTimestamp = 0;
    };

    static constexpr int kNumBuffers = 3;
    Buffer m_buffers[kNumBuffers];

    // Slot roles, indices into m_buffers (or -1 = no slot in that role):
    //   m_readIdx  : buffer the renderer is currently uploading from. The
    //                producer never picks this slot. Stays valid from one
    //                successful Read() until the next.
    //   m_freshIdx : most-recently-completed frame, queued for the reader.
    //                A new Write replaces it (counts as a dropped frame); a
    //                successful Read consumes it.
    // The producer's in-flight write slot lives only on the producer's stack
    // between the pick and the commit; with two slots possibly occupied
    // here (reader + fresh) there's always at least one free of three.
    int m_readIdx  = -1;
    int m_freshIdx = -1;

    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_stride;

    std::mutex        m_swapMutex;
    std::atomic<bool> m_newFrameAvailable{false};

    // Stats
    std::atomic<uint64_t> m_framesWritten{0};
    std::atomic<uint64_t> m_uniqueFramesWritten{0};
    std::atomic<uint64_t> m_framesDropped{0};

    // Signal detection -- hash a tiny pixel sample each frame, count how many
    // consecutive frames have the same hash. After ~30 (half a second at
    // 60fps), signal is declared lost.
    uint64_t              m_lastFrameHash      = 0;
    uint32_t              m_identicalCount     = 0;
    std::atomic<bool>     m_signalActive{false};
};

} // namespace NitLink
