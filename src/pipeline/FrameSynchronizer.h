#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "model/RecordItem.h"
#include "model/VideoFrame.h"

struct synchronizer_config {
    int channelCount = MIN_REQUIRED_CHANNELS;
    int64_t toleranceNs = 2000000;
    int maxPendingTicks = 3;
};

// frames of one tick and the verdict on them
struct frame_set {
    sync_record record;
    std::array<std::optional<video_frame>, MAX_CHANNELS> frames;
};

/**
 * @brief groups latched frames by tick and judges each group. no threads, no clock: unit-testable
 *
 * complete group: offset = max(ts) - min(ts) over the channels, locked if offset <= tolerance.
 * a group still missing channels once a frame maxPendingTicks newer arrives is incomplete.
 * groups come out in tick order; a frame for a tick already emitted is late and dropped
 */
class FrameSynchronizer {
public:
    explicit FrameSynchronizer(const synchronizer_config& config);

    void add(video_frame frame, std::vector<frame_set>& ready);
    // emits every pending group (incomplete ones as incomplete). used at stop
    void flush(std::vector<frame_set>& ready);

    uint64_t get_late_frames() const;
    uint64_t get_rejected_frames() const;
    size_t get_pending_ticks() const;

    static int64_t computeOffset(const frame_set& set, int channelCount);

private:
    void emit(std::map<uint64_t, frame_set>::iterator it, std::vector<frame_set>& ready);

    synchronizer_config _config;
    std::map<uint64_t, frame_set> _pending;
    std::optional<uint64_t> _lastEmitted;
    uint64_t _lateFrames;
    uint64_t _rejectedFrames;
};
