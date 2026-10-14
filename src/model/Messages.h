#pragma once

#include <cstdint>
#include <string>

// bus messages. value types only

enum class control_action {
    start,
    stop
};

enum class control_origin {
    mqtt,
    config,
    signal
};

struct control_command {
    control_action action = control_action::start;
    control_origin origin = control_origin::mqtt;
    std::string requestId;
};

struct control_result {
    control_action action = control_action::start;
    control_origin origin = control_origin::mqtt;
    std::string requestId;
    bool accepted = false;
    std::string reason;
};

struct acquisition_started {
    std::string sessionId;
    std::string sessionDirectory;
    int64_t epochNs = 0;  // steady_clock, tick 0 and segment 0 start here
    int64_t segmentNs = 0;
};

// stop handshake: stopping → producers stop → video_pipeline_stopped → recorder session end → stopped
struct acquisition_stopping {
    std::string sessionId;
    std::string reason;
};

struct acquisition_stopped {
    std::string sessionId;
    std::string reason;
};

struct video_pipeline_started {
    bool ok = false;
    std::string reason;
};

struct video_pipeline_stopped {
    std::string reason;
};

struct video_pipeline_failed {
    std::string reason;
};

inline const char* toString(control_action action) {
    return action == control_action::start ? "start" : "stop";
}

inline const char* toString(control_origin origin) {
    switch (origin) {
        case control_origin::mqtt:
            return "mqtt";
        case control_origin::config:
            return "config";
        case control_origin::signal:
            return "signal";
        default:
            return "unknown";
    }
}
