#pragma once

#include <cstdint>
#include <string>
#include <vector>

// shared states. one writer per kind, readers see immutable snapshots

enum class acquisition_phase {
    idle,
    running,
    stopping,
    error
};

struct acquisition_state {
    acquisition_phase phase = acquisition_phase::idle;
    std::string sessionId;
    std::string reason;
};

struct channel_stats {
    int id = 0;
    std::string sourceType;
    double fps = 0.0;  // frames delivered to the encoder per second
    size_t queueLength = 0;
    uint64_t framesEncoded = 0;
    uint64_t dropped = 0;
    uint64_t duplicated = 0;
    uint64_t missedTicks = 0;
    uint64_t sourceErrors = 0;
    double encodeMsAvg = 0.0;
};

struct sync_report {
    double toleranceMs = 0.0;
    double lastOffsetMs = 0.0;
    double windowMaxOffsetMs = 0.0;
    double totalMaxOffsetMs = 0.0;
    uint64_t setsChecked = 0;
    uint64_t setsLocked = 0;
    uint64_t violations = 0;
    uint64_t incomplete = 0;
    uint64_t backpressure = 0;
    uint64_t windowViolations = 0;
    uint64_t windowIncomplete = 0;
    bool locked = false;
};

struct video_state {
    bool running = false;
    std::vector<channel_stats> channels;
    sync_report sync;
};

struct sensor_state {
    bool valid = false;
    double temperatureC = 0.0;
    double humidityPct = 0.0;
    double accelerationG = 0.0;  // magnitude of the dynamic part (|a| - 1 g)
    uint64_t samples = 0;
    uint64_t failures = 0;
    uint64_t skippedPeriods = 0;
};

struct storage_state {
    int64_t segment = -1;
    std::vector<std::string> recentFiles;
    uint64_t bytesWritten = 0;
    uint64_t dropped = 0;
    uint64_t writeErrors = 0;
    size_t queueLength = 0;
};

struct report_state {
    uint64_t sent = 0;
    uint64_t failures = 0;
    uint64_t consecutiveFailures = 0;
    uint64_t skipped = 0;
    std::string lastError;
    std::string lastSuccessAt;
};

struct mqtt_state {
    bool connected = false;
    uint64_t commandsReceived = 0;
    uint64_t commandsRejected = 0;
    uint64_t statusPublished = 0;
};

inline const char* toString(acquisition_phase phase) {
    switch (phase) {
        case acquisition_phase::idle:
            return "idle";
        case acquisition_phase::running:
            return "running";
        case acquisition_phase::stopping:
            return "stopping";
        case acquisition_phase::error:
            return "error";
        default:
            return "unknown";
    }
}
