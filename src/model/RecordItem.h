#pragma once

#include <array>
#include <cstdint>
#include <variant>

#include "model/SensorSample.h"
#include "model/VideoFrame.h"

enum class sync_verdict {
    locked,
    violation,
    incomplete,
    backpressure
};

// one checked frame set, written to sync.csv
struct sync_record {
    uint64_t tick = 0;
    int64_t segment = 0;
    int64_t targetNs = 0;
    int channelCount = 0;
    std::array<int64_t, MAX_CHANNELS> timestampNs{};  // 0 when the channel was missing
    std::array<bool, MAX_CHANNELS> present{};
    int64_t offsetNs = 0;
    sync_verdict verdict = sync_verdict::locked;
};

struct sensor_record {
    int64_t segment = 0;
    sensor_sample sample;
};

// everything the storage thread writes. std::visit dispatches each kind to its writer
using record_item = std::variant<encoded_packet, sync_record, sensor_record>;

inline const char* toString(sync_verdict verdict) {
    switch (verdict) {
        case sync_verdict::locked:
            return "locked";
        case sync_verdict::violation:
            return "violation";
        case sync_verdict::incomplete:
            return "incomplete";
        case sync_verdict::backpressure:
            return "backpressure";
        default:
            return "unknown";
    }
}
