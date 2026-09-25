#include "capture/frame_buffer.h"

#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    NitLink::FrameBuffer buffer(2, 2, 8);
    std::thread droppedPlaceholder([&buffer] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        buffer.NotifyFrameActivity();
    });

    const bool activitySignaled = buffer.WaitForFrame(1000);
    droppedPlaceholder.join();

    NitLink::FrameBuffer::FrameData frame{};
    if (!activitySignaled) {
        std::cerr << "filtered capture activity did not wake the waiter\n";
        return 1;
    }
    if (buffer.Read(frame)) {
        std::cerr << "filtered capture activity exposed a readable frame\n";
        return 2;
    }
    if (frame.data != nullptr || frame.size != 0) {
        std::cerr << "Read did not clear frame data after an activity-only wake\n";
        return 3;
    }

    std::cout << "frame buffer activity tests passed\n";
    return 0;
}
