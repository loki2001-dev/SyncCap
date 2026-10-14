#pragma once

#include <cstdint>

// which segment a steady_clock time falls in. segment k covers [epoch + k*length, epoch + (k+1)*length)
class SegmentClock {
public:
    SegmentClock();
    SegmentClock(int64_t epochNs, int64_t segmentNs);

    int64_t indexOf(int64_t timeNs) const;
    int64_t startOf(int64_t index) const;
    int64_t get_epoch_ns() const;
    int64_t get_segment_ns() const;

private:
    int64_t _epochNs;
    int64_t _segmentNs;
};
