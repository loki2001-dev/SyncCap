#include "catch_amalgamated.hpp"

#include <chrono>

#include "storage/SegmentClock.h"
#include "storage/SegmentedRecorder.h"

TEST_CASE("SegmentClock maps times to segment indexes at the boundaries") {
    constexpr int64_t SECOND = 1000000000LL;
    const SegmentClock clock(10 * SECOND, 180 * SECOND);
    REQUIRE(clock.indexOf(0) == 0);
    REQUIRE(clock.indexOf(10 * SECOND) == 0);
    REQUIRE(clock.indexOf(190 * SECOND - 1) == 0);
    REQUIRE(clock.indexOf(190 * SECOND) == 1);
    REQUIRE(clock.indexOf(370 * SECOND) == 2);
    REQUIRE(clock.startOf(2) == 370 * SECOND);
}

TEST_CASE("SegmentClock falls back to the default length for a non-positive length") {
    const SegmentClock clock(0, 0);
    REQUIRE(clock.get_segment_ns() == 180LL * 1000000000LL);
}

TEST_CASE("SegmentedRecorder names a segment directory by index and wall time") {
    const std::string name = SegmentedRecorder::segmentDirectoryName(3, std::chrono::system_clock::now(), 1000000000LL);
    REQUIRE(name.rfind("seg_0003_", 0) == 0);
    REQUIRE(name.size() == std::string("seg_0003_20261014_130000").size());
}
