#include "storage/SegmentClock.h"

namespace {

constexpr int64_t DEFAULT_SEGMENT_NS = 180LL * 1000 * 1000 * 1000;

}  // namespace

SegmentClock::SegmentClock()
    : _epochNs(0)
    , _segmentNs(DEFAULT_SEGMENT_NS) {
}

SegmentClock::SegmentClock(int64_t epochNs, int64_t segmentNs)
    : _epochNs(epochNs)
    , _segmentNs(segmentNs > 0 ? segmentNs : DEFAULT_SEGMENT_NS) {
}

int64_t SegmentClock::indexOf(int64_t timeNs) const {
    if (timeNs <= _epochNs) {
        return 0;
    }
    return (timeNs - _epochNs) / _segmentNs;
}

int64_t SegmentClock::startOf(int64_t index) const {
    return _epochNs + index * _segmentNs;
}

int64_t SegmentClock::get_epoch_ns() const {
    return _epochNs;
}

int64_t SegmentClock::get_segment_ns() const {
    return _segmentNs;
}
