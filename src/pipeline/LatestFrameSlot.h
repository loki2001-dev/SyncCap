#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "model/FrameBuffer.h"

struct slot_frame {
    std::shared_ptr<const FrameBuffer> buffer;
    uint64_t sequence = 0;  // 1 for the first picture, increases by one per put()
};

/**
 * @brief one-picture mailbox between a channel's reader and its capture thread
 *
 * the reader overwrites (a newer picture always wins); the capture thread reads the latest at each tick
 * without removing it, so a slow source repeats its last picture instead of leaving a hole
 */
class LatestFrameSlot {
public:
    void put(std::shared_ptr<const FrameBuffer> buffer);
    bool peekLatest(slot_frame& out);
    void clear();

    uint64_t get_overwritten_total() const;

private:
    mutable std::mutex _mutex;
    slot_frame _latest;
    uint64_t _lastPeeked = 0;
    uint64_t _overwritten = 0;
};
