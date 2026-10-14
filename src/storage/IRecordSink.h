#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "model/RecordItem.h"

struct session_info {
    std::string sessionId;
    std::string directory;
    int64_t epochNs = 0;
    int64_t segmentNs = 0;
    std::chrono::system_clock::time_point wallStart;
};

struct recorder_stats {
    int64_t segment = -1;
    std::vector<std::string> recentFiles;
    uint64_t bytesWritten = 0;
    uint64_t dropped = 0;
    uint64_t writeErrors = 0;
    size_t queueLength = 0;
    size_t queueHighWater = 0;
    bool sessionOpen = false;
};

/**
 * @brief storage service shared by the video pipeline and the sensor component
 *
 * push() never blocks; a full queue rejects and counts. every item lands in the file of its segment
 */
class IRecordSink {
public:
    virtual ~IRecordSink() = default;

    // false: the session directory could not be created
    virtual bool beginSession(const session_info& session) = 0;
    // queued behind everything pushed before; files close when the storage thread reaches it
    virtual void endSession() = 0;
    // thread-safe. false: rejected (queue full or no session)
    virtual bool push(record_item item) = 0;
    virtual recorder_stats get_stats() const = 0;
};
