#include "frame_buffer.h"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace NitLink {

FrameBuffer::FrameBuffer(uint32_t width, uint32_t height, uint32_t stride)
    : m_width(width), m_height(height), m_stride(stride)
{
    // Size buffers for the WORST CASE format that might arrive. Stride is set
    // by the capture device at Open() time based on the negotiated format:
    //   BGRA: stride = width * 4   -> frame = width*height*4 (e.g. 33 MB @4K)
    //   P010: stride = width * 2   -> Y plane only, but the actual frame is
    //                                  width*height*3 because P010 carries
    //                                  Y+UV planes (UV is half-res 16-bit).
    //   NV12: stride = width       -> Y plane, full frame is *1.5
    //
    // Sizing strictly by `stride * height` ends up too small for P010
    // (16 MB allocated vs 24.9 MB needed): the Write would truncate
    // the UV plane, which then makes UpdateCaptureTexture misidentify the
    // format and crash trying to upload a P010 frame as BGRA.
    //
    // BGRA (width*height*4) is the largest possible frame size at this
    // resolution, so use it as a safe ceiling for all formats. The small
    // extra memory cost is well worth not losing UV data.
    const uint32_t maxPossibleFrameSize = width * height * 4;
    const uint32_t strideFrameSize       = stride * height;
    const uint32_t frameSize = std::max(strideFrameSize, maxPossibleFrameSize);
    for (int i = 0; i < kNumBuffers; ++i) {
        m_buffers[i].data.resize(frameSize);
    }

    // Auto-reset, initially non-signaled. Set on every Write(); the render loop
    // blocks on it in WaitForFrame() for arrival-driven present.
    m_frameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

FrameBuffer::~FrameBuffer()
{
    // Owners join the capture worker before destroying the buffer (see the
    // teardown contract in frame_buffer.h), so no Write() can race this close.
    if (m_frameReadyEvent) {
        CloseHandle((HANDLE)m_frameReadyEvent);
        m_frameReadyEvent = nullptr;
    }
}

void FrameBuffer::WaitForFrame(unsigned long timeoutMs)
{
    if (m_frameReadyEvent) {
        WaitForSingleObject((HANDLE)m_frameReadyEvent, timeoutMs);
    }
}

void FrameBuffer::Write(const uint8_t* data, uint32_t size, int64_t timestamp,
                         int64_t arrivalWallNs, uint64_t deviceTimestamp)
{
    // Pick a free slot: anything that isn't the reader's slot or the
    // currently-fresh slot. With three buffers and at most two roles taken
    // (reader + fresh), there is always at least one free slot. The producer
    // is single-threaded (CaptureDevice::CaptureLoop), so no explicit
    // "writing" state is needed: the chosen index stays on this stack frame.
    int writeIdx;
    {
        std::lock_guard<std::mutex> lock(m_swapMutex);
        writeIdx = -1;
        for (int i = 0; i < kNumBuffers; ++i) {
            if (i != m_readIdx && i != m_freshIdx) { writeIdx = i; break; }
        }
    }
    if (writeIdx < 0) return; // unreachable with kNumBuffers == 3

    auto& buf = m_buffers[writeIdx];
    uint32_t copySize = std::min(size, (uint32_t)buf.data.size());
    std::memcpy(buf.data.data(), data, copySize);
    buf.actualSize = copySize;     // remember the real frame length so the
                                    // reader doesn't grab the buffer capacity
    buf.timestamp = timestamp;
    buf.arrivalWallNs = arrivalWallNs;
    buf.deviceTimestamp = deviceTimestamp;

    // Hash a small sample of pixels for signal-loss detection. 64 points
    // spread across the frame are enough to catch any motion (real
    // gameplay), while NO SIGNAL placeholder screens stay identical.
    // FNV-1a hash, ~256 bytes hashed total: essentially free.
    uint64_t hash = 0xcbf29ce484222325ULL;
    if (copySize >= 256) {
        const uint32_t hashStride = copySize / 64;
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t off = i * hashStride;
            // Hash 4 bytes from this offset
            for (int b = 0; b < 4 && off + b < copySize; b++) {
                hash ^= data[off + b];
                hash *= 0x100000001b3ULL;
            }
        }
    }

    if (hash == m_lastFrameHash) {
        m_identicalCount++;
        // 30 frames = ~500ms at 60fps. After that, signal is considered lost.
        if (m_identicalCount > 30) {
            m_signalActive = false;
        }
    } else {
        m_identicalCount = 0;
        m_signalActive   = true;
        m_lastFrameHash  = hash;
        // Count this as a unique-content frame. The Elgato delivers 60 frames/sec
        // over HDMI regardless of game framerate -- a 30fps game sends each
        // rendered frame twice. To measure the actual game framerate, callers
        // should sample this counter instead of GetFramesWritten().
        m_uniqueFramesWritten++;
    }

    // Commit: install the chosen slot as the fresh one. If a previous fresh
    // frame never got picked up by the reader, count it as dropped.
    {
        std::lock_guard<std::mutex> lock(m_swapMutex);
        if (m_freshIdx != -1) m_framesDropped++;
        m_freshIdx = writeIdx;
    }

    m_newFrameAvailable = true;
    m_framesWritten++;

    // Wake the render loop's arrival-driven present (low-latency mode). The
    // auto-reset event latches, so a Write that lands between the render
    // thread's Read and its next WaitForFrame is not lost -- the wait returns
    // immediately.
    if (m_frameReadyEvent) SetEvent((HANDLE)m_frameReadyEvent);
}

bool FrameBuffer::Read(FrameData& outFrame)
{
    // Explicitly null out the caller's FrameData on every failure path so a
    // caller that forgets to default-initialize never sees a stale or
    // uninitialized pointer. Cheap (a few stores) and makes the contract
    // "false return -> out is cleared" airtight rather than relying on the
    // FrameData struct's default member initializers.
    if (!m_newFrameAvailable.exchange(false)) {
        outFrame = FrameData{};
        return false; // No new frame since last read
    }

    std::lock_guard<std::mutex> lock(m_swapMutex);
    if (m_freshIdx == -1) {
        // Unreachable in practice: m_newFrameAvailable is only set after a
        // Write commits m_freshIdx, and only Read can clear m_freshIdx (and
        // there's only one reader). Defensive null-out anyway.
        outFrame = FrameData{};
        return false;
    }

    // Take ownership of the fresh slot. The previous m_readIdx (if any)
    // becomes free and is eligible to be picked by the next Write. No
    // explicit tracking is needed: the producer's free-slot search runs
    // against the new (m_readIdx, m_freshIdx) pair after this returns.
    m_readIdx  = m_freshIdx;
    m_freshIdx = -1;

    auto& buf = m_buffers[m_readIdx];
    outFrame.data      = buf.data.data();
    // Return the actual bytes written this frame, not the buffer capacity.
    // Critical for the renderer's format inference -- a 24.9 MB P010 frame
    // must report 24.9 MB, not the 33 MB allocation ceiling.
    outFrame.size      = buf.actualSize;
    outFrame.width     = m_width;
    outFrame.height    = m_height;
    outFrame.timestamp = buf.timestamp;
    outFrame.arrivalWallNs = buf.arrivalWallNs;
    outFrame.deviceTimestamp = buf.deviceTimestamp;
    return true;
}

} // namespace NitLink
