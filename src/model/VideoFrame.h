#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "model/FrameBuffer.h"

constexpr int MAX_CHANNELS = 4;
constexpr int MIN_REQUIRED_CHANNELS = 3;

// one latched picture of one channel. the buffer is shared read-only and returns to its pool on release
struct video_frame {
    int channel = 0;
    uint64_t tick = 0;
    int64_t targetNs = 0;     // tick target time on steady_clock
    int64_t timestampNs = 0;  // measured latch time on steady_clock
    uint64_t sourceSequence = 0;
    bool duplicate = false;  // the source produced no new picture since the previous tick
    int64_t segment = 0;
    std::shared_ptr<const FrameBuffer> buffer;
};

// one H.264 access unit (Annex-B) for one channel
struct encoded_packet {
    int channel = 0;
    uint64_t tick = 0;
    int64_t segment = 0;
    int64_t pts = 0;
    bool keyFrame = false;
    std::vector<uint8_t> data;
};
