#include "pipeline/LatestFrameSlot.h"

#include <utility>

void LatestFrameSlot::put(std::shared_ptr<const FrameBuffer> buffer) {
    std::shared_ptr<const FrameBuffer> previous;
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (_latest.buffer && _latest.sequence != _lastPeeked) {
            _overwritten += 1;
        }
        previous = std::move(_latest.buffer);
        _latest.buffer = std::move(buffer);
        _latest.sequence += 1;
    }
    // the previous picture returns to its pool here, outside our lock
}

bool LatestFrameSlot::peekLatest(slot_frame& out) {
    const std::lock_guard<std::mutex> lock(_mutex);
    if (!_latest.buffer) {
        return false;
    }
    out = _latest;
    _lastPeeked = _latest.sequence;
    return true;
}

void LatestFrameSlot::clear() {
    std::shared_ptr<const FrameBuffer> previous;
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        previous = std::move(_latest.buffer);
    }
}

uint64_t LatestFrameSlot::get_overwritten_total() const {
    const std::lock_guard<std::mutex> lock(_mutex);
    return _overwritten;
}
