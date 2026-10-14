#pragma once

#include <cstdint>
#include <deque>
#include <mutex>

#include "model/RecordItem.h"
#include "model/States.h"

/**
 * @brief keeps the verdict of every checked frame set and answers "are the channels synchronized?"
 *
 * locked means: inside the recent window there was at least one set, no violation and no incomplete set.
 * thread-safe: the synchronizer thread records, the loop thread reads
 */
class SyncMonitor {
public:
    SyncMonitor(double toleranceMs, int64_t windowNs);

    void record(const sync_record& record);
    sync_report get_report() const;
    bool isSynchronized() const;
    void reset();

private:
    struct window_entry {
        int64_t targetNs;
        int64_t offsetNs;
        sync_verdict verdict;
    };

    void trimLocked(int64_t nowNs);
    sync_report buildLocked() const;

    mutable std::mutex _mutex;
    double _toleranceMs;
    int64_t _windowNs;
    std::deque<window_entry> _window;
    uint64_t _windowTrimmed = 0;
    sync_report _totals;
};
