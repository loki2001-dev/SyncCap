#pragma once

#include <chrono>
#include <string>

#include "core/IStateWriter.h"
#include "model/SystemStatus.h"

struct status_identity {
    std::string deviceId;
    std::chrono::steady_clock::time_point startedAt;
};

// assembles the status document from the state snapshots. shared by the HTTP and MQTT reporters
class StatusSnapshot {
public:
    static system_status build(const IStateReader& reader, const status_identity& identity);
};
