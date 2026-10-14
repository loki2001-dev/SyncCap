#pragma once

#include <string>
#include <vector>

#include "model/States.h"

constexpr int STATUS_SCHEMA_VERSION = 1;

// the status document sent by HTTP POST /status and MQTT video/status
struct system_status {
    int schemaVersion = STATUS_SCHEMA_VERSION;
    std::string deviceId;
    std::string timestamp;
    double uptimeS = 0.0;
    acquisition_state acquisition;
    std::vector<channel_stats> channels;
    sync_report sync;
    sensor_state sensor;
    storage_state storage;
};
