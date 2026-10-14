#include "catch_amalgamated.hpp"

#include "pipeline/SyncMonitor.h"

namespace {

constexpr int64_t MS = 1000000LL;
constexpr int64_t WINDOW = 10000 * MS;

sync_record makeRecord(int64_t targetNs, int64_t offsetNs, sync_verdict verdict) {
    sync_record record;
    record.targetNs = targetNs;
    record.offsetNs = offsetNs;
    record.verdict = verdict;
    record.channelCount = 4;
    return record;
}

}  // namespace

TEST_CASE("SyncMonitor is not locked before any set") {
    SyncMonitor monitor(2.0, WINDOW);
    REQUIRE_FALSE(monitor.isSynchronized());
}

TEST_CASE("SyncMonitor is locked while every recent set is within tolerance") {
    SyncMonitor monitor(2.0, WINDOW);
    monitor.record(makeRecord(0, MS / 2, sync_verdict::locked));
    monitor.record(makeRecord(33 * MS, 18 * MS / 10, sync_verdict::locked));
    const sync_report report = monitor.get_report();
    REQUIRE(report.locked);
    REQUIRE(report.windowMaxOffsetMs == Catch::Approx(1.8));
    REQUIRE(report.lastOffsetMs == Catch::Approx(1.8));
    REQUIRE(report.setsChecked == 2);
    REQUIRE(report.setsLocked == 2);
}

TEST_CASE("SyncMonitor loses lock on a violation and regains it once the window passes") {
    SyncMonitor monitor(2.0, WINDOW);
    monitor.record(makeRecord(0, 3 * MS, sync_verdict::violation));
    REQUIRE_FALSE(monitor.isSynchronized());
    REQUIRE(monitor.get_report().violations == 1);
    // 11 s later the violation has left the 10 s window
    monitor.record(makeRecord(11000 * MS, MS / 10, sync_verdict::locked));
    const sync_report report = monitor.get_report();
    REQUIRE(report.locked);
    REQUIRE(report.violations == 1);
    REQUIRE(report.totalMaxOffsetMs == Catch::Approx(3.0));
    REQUIRE(report.windowMaxOffsetMs == Catch::Approx(0.1));
}

TEST_CASE("SyncMonitor counts an incomplete set against the lock but not the offset") {
    SyncMonitor monitor(2.0, WINDOW);
    monitor.record(makeRecord(0, 0, sync_verdict::locked));
    monitor.record(makeRecord(33 * MS, 25 * MS, sync_verdict::incomplete));
    const sync_report report = monitor.get_report();
    REQUIRE_FALSE(report.locked);
    REQUIRE(report.incomplete == 1);
    REQUIRE(report.windowMaxOffsetMs == Catch::Approx(0.0));
}

TEST_CASE("SyncMonitor reset clears everything") {
    SyncMonitor monitor(2.0, WINDOW);
    monitor.record(makeRecord(0, 3 * MS, sync_verdict::violation));
    monitor.reset();
    const sync_report report = monitor.get_report();
    REQUIRE(report.setsChecked == 0);
    REQUIRE(report.toleranceMs == Catch::Approx(2.0));
}
