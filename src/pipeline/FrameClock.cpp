#include "pipeline/FrameClock.h"

namespace {

constexpr int64_t NANOS_PER_SECOND = 1000000000LL;
constexpr int DEFAULT_FPS = 30;

}  // namespace

FrameClock::FrameClock()
    : FrameClock(0, DEFAULT_FPS) {
}

FrameClock::FrameClock(int64_t epochNs, int fps)
    : _epochNs(epochNs)
    , _periodNs(NANOS_PER_SECOND / (fps > 0 ? fps : DEFAULT_FPS))
    , _fps(fps > 0 ? fps : DEFAULT_FPS) {
}

int64_t FrameClock::targetOf(uint64_t tick) const {
    // tick * 1e9 / fps keeps 30 fps exact (no 33,333,333 ns drift of 1 ns per frame)
    return _epochNs + static_cast<int64_t>(tick) * NANOS_PER_SECOND / _fps;
}

uint64_t FrameClock::nextTickAt(int64_t timeNs) const {
    if (timeNs <= _epochNs) {
        return 0;
    }
    const int64_t elapsed = timeNs - _epochNs;
    uint64_t tick = static_cast<uint64_t>(elapsed * _fps / NANOS_PER_SECOND);
    while (targetOf(tick) < timeNs) {
        tick += 1;
    }
    return tick;
}

int64_t FrameClock::get_period_ns() const {
    return _periodNs;
}

int64_t FrameClock::get_epoch_ns() const {
    return _epochNs;
}
