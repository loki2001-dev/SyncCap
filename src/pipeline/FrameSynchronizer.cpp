#include "pipeline/FrameSynchronizer.h"

#include <algorithm>
#include <limits>
#include <utility>

FrameSynchronizer::FrameSynchronizer(const synchronizer_config& config)
    : _config(config)
    , _lateFrames(0)
    , _rejectedFrames(0) {
    _config.channelCount = std::clamp(_config.channelCount, 1, MAX_CHANNELS);
}

void FrameSynchronizer::add(video_frame frame, std::vector<frame_set>& ready) {
    // P3: the channel index comes from our own capture threads, still check it before indexing
    if (frame.channel < 0 || frame.channel >= _config.channelCount) {
        _rejectedFrames += 1;
        return;
    }
    if (_lastEmitted && frame.tick <= *_lastEmitted) {
        _lateFrames += 1;
        return;
    }
    const uint64_t tick = frame.tick;
    frame_set& set = _pending[tick];
    set.record.tick = tick;
    set.record.targetNs = frame.targetNs;
    set.record.channelCount = _config.channelCount;
    const auto index = static_cast<size_t>(frame.channel);
    if (set.frames[index]) {
        _rejectedFrames += 1;
        return;
    }
    set.record.present[index] = true;
    set.record.timestampNs[index] = frame.timestampNs;
    set.frames[index] = std::move(frame);

    // emit in tick order: complete groups at the front, and groups that waited too long
    while (!_pending.empty()) {
        auto front = _pending.begin();
        const bool complete =
            std::all_of(front->second.frames.begin(), front->second.frames.begin() + _config.channelCount,
                        [](const std::optional<video_frame>& entry) {
                            return entry.has_value();
                        });
        const uint64_t newest = _pending.rbegin()->first;
        const bool expired = newest - front->first >= static_cast<uint64_t>(_config.maxPendingTicks);
        if (!complete && !expired) {
            break;
        }
        emit(front, ready);
    }
}

void FrameSynchronizer::flush(std::vector<frame_set>& ready) {
    while (!_pending.empty()) {
        emit(_pending.begin(), ready);
    }
}

void FrameSynchronizer::emit(std::map<uint64_t, frame_set>::iterator it, std::vector<frame_set>& ready) {
    frame_set set = std::move(it->second);
    _lastEmitted = it->first;
    _pending.erase(it);

    int presentCount = 0;
    for (int channel = 0; channel < _config.channelCount; ++channel) {
        if (set.record.present[static_cast<size_t>(channel)]) {
            presentCount += 1;
        }
    }
    set.record.offsetNs = computeOffset(set, _config.channelCount);
    if (presentCount < _config.channelCount) {
        set.record.verdict = sync_verdict::incomplete;
    } else if (set.record.offsetNs <= _config.toleranceNs) {
        set.record.verdict = sync_verdict::locked;
    } else {
        set.record.verdict = sync_verdict::violation;
    }
    ready.push_back(std::move(set));
}

int64_t FrameSynchronizer::computeOffset(const frame_set& set, int channelCount) {
    int64_t earliest = std::numeric_limits<int64_t>::max();
    int64_t latest = std::numeric_limits<int64_t>::min();
    for (int channel = 0; channel < channelCount && channel < MAX_CHANNELS; ++channel) {
        const auto index = static_cast<size_t>(channel);
        if (!set.record.present[index]) {
            continue;
        }
        earliest = std::min(earliest, set.record.timestampNs[index]);
        latest = std::max(latest, set.record.timestampNs[index]);
    }
    return latest >= earliest ? latest - earliest : 0;
}

uint64_t FrameSynchronizer::get_late_frames() const {
    return _lateFrames;
}

uint64_t FrameSynchronizer::get_rejected_frames() const {
    return _rejectedFrames;
}

size_t FrameSynchronizer::get_pending_ticks() const {
    return _pending.size();
}
