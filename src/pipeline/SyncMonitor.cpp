#include "pipeline/SyncMonitor.h"

#include <algorithm>

namespace {

constexpr double NANOS_PER_MILLI = 1e6;
constexpr size_t MAX_WINDOW_ENTRIES = 4096;

}  // namespace

SyncMonitor::SyncMonitor(double toleranceMs, int64_t windowNs)
    : _toleranceMs(toleranceMs)
    , _windowNs(windowNs) {
    _totals.toleranceMs = toleranceMs;
}

void SyncMonitor::record(const sync_record& record) {
    const std::lock_guard<std::mutex> lock(_mutex);
    _totals.setsChecked += 1;
    const double offsetMs = static_cast<double>(record.offsetNs) / NANOS_PER_MILLI;
    switch (record.verdict) {
        case sync_verdict::locked:
            _totals.setsLocked += 1;
            break;
        case sync_verdict::violation:
            _totals.violations += 1;
            break;
        case sync_verdict::incomplete:
            _totals.incomplete += 1;
            break;
        case sync_verdict::backpressure:
            _totals.backpressure += 1;
            break;
        default:
            break;
    }
    // an incomplete set has no meaningful offset across all channels
    if (record.verdict != sync_verdict::incomplete) {
        _totals.lastOffsetMs = offsetMs;
        _totals.totalMaxOffsetMs = std::max(_totals.totalMaxOffsetMs, offsetMs);
    }
    _window.push_back(window_entry{record.targetNs, record.offsetNs, record.verdict});
    trimLocked(record.targetNs);
    // the time window bounds it at fps * window; the ceiling guards against a broken clock
    while (_window.size() > MAX_WINDOW_ENTRIES) {
        _window.pop_front();
        _windowTrimmed += 1;
    }
}

void SyncMonitor::trimLocked(int64_t nowNs) {
    while (!_window.empty() && nowNs - _window.front().targetNs > _windowNs) {
        _window.pop_front();
    }
}

sync_report SyncMonitor::buildLocked() const {
    sync_report report = _totals;
    report.windowMaxOffsetMs = 0.0;
    report.windowViolations = 0;
    report.windowIncomplete = 0;
    for (const window_entry& entry : _window) {
        if (entry.verdict == sync_verdict::violation) {
            report.windowViolations += 1;
        }
        if (entry.verdict == sync_verdict::incomplete) {
            report.windowIncomplete += 1;
            continue;
        }
        report.windowMaxOffsetMs =
            std::max(report.windowMaxOffsetMs, static_cast<double>(entry.offsetNs) / NANOS_PER_MILLI);
    }
    report.locked = !_window.empty() && report.windowViolations == 0 && report.windowIncomplete == 0 &&
                    report.windowMaxOffsetMs <= _toleranceMs;
    return report;
}

sync_report SyncMonitor::get_report() const {
    const std::lock_guard<std::mutex> lock(_mutex);
    return buildLocked();
}

bool SyncMonitor::isSynchronized() const {
    const std::lock_guard<std::mutex> lock(_mutex);
    return buildLocked().locked;
}

void SyncMonitor::reset() {
    const std::lock_guard<std::mutex> lock(_mutex);
    _window.clear();
    _totals = sync_report{};
    _totals.toleranceMs = _toleranceMs;
}
