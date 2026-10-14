#include "catch_amalgamated.hpp"

#include "pipeline/FrameClock.h"

TEST_CASE("FrameClock tick targets do not drift at 30 fps") {
    const FrameClock clock(1000, 30);
    REQUIRE(clock.targetOf(0) == 1000);
    REQUIRE(clock.targetOf(1) == 1000 + 33333333);
    REQUIRE(clock.targetOf(3) == 1000 + 100000000);
    // one hour of ticks lands exactly on the hour
    REQUIRE(clock.targetOf(30ULL * 3600) == 1000 + 3600LL * 1000000000LL);
}

TEST_CASE("FrameClock nextTickAt finds the first tick at or after a time") {
    const FrameClock clock(0, 20);
    REQUIRE(clock.nextTickAt(-5) == 0);
    REQUIRE(clock.nextTickAt(0) == 0);
    REQUIRE(clock.nextTickAt(1) == 1);
    REQUIRE(clock.nextTickAt(50000000) == 1);
    REQUIRE(clock.nextTickAt(50000001) == 2);
    REQUIRE(clock.get_period_ns() == 50000000);
}
