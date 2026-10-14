#include "catch_amalgamated.hpp"

#include <vector>

#include "pipeline/FrameSynchronizer.h"

namespace {

constexpr int64_t US = 1000LL;
constexpr int64_t TOLERANCE_NS = 2000 * US;

video_frame makeFrame(int channel, uint64_t tick, int64_t timestampNs) {
    video_frame frame;
    frame.channel = channel;
    frame.tick = tick;
    frame.targetNs = static_cast<int64_t>(tick) * 33333333LL;
    frame.timestampNs = timestampNs;
    return frame;
}

synchronizer_config makeConfig(int channels) {
    synchronizer_config config;
    config.channelCount = channels;
    config.toleranceNs = TOLERANCE_NS;
    config.maxPendingTicks = 3;
    return config;
}

// one set where channel c is late by offsets[c]
std::vector<frame_set> feedSet(FrameSynchronizer& synchronizer, uint64_t tick, const std::vector<int64_t>& offsets) {
    std::vector<frame_set> ready;
    const int64_t base = static_cast<int64_t>(tick) * 33333333LL;
    for (size_t channel = 0; channel < offsets.size(); ++channel) {
        synchronizer.add(makeFrame(static_cast<int>(channel), tick, base + offsets[channel]), ready);
    }
    return ready;
}

}  // namespace

TEST_CASE("FrameSynchronizer locks a set exactly at the 2 ms tolerance") {
    FrameSynchronizer synchronizer(makeConfig(4));
    const auto ready = feedSet(synchronizer, 1, {0, 500 * US, 2000 * US, 100 * US});
    REQUIRE(ready.size() == 1);
    REQUIRE(ready[0].record.verdict == sync_verdict::locked);
    REQUIRE(ready[0].record.offsetNs == 2000 * US);
}

TEST_CASE("FrameSynchronizer locks 1.999 ms and rejects 2.001 ms") {
    FrameSynchronizer synchronizer(makeConfig(3));
    auto ready = feedSet(synchronizer, 1, {0, 1999 * US, 0});
    REQUIRE(ready[0].record.verdict == sync_verdict::locked);
    ready = feedSet(synchronizer, 2, {0, 0, 2001 * US});
    REQUIRE(ready[0].record.verdict == sync_verdict::violation);
    REQUIRE(ready[0].record.offsetNs == 2001 * US);
}

TEST_CASE("FrameSynchronizer offset is max minus min regardless of channel order") {
    FrameSynchronizer synchronizer(makeConfig(4));
    const auto ready = feedSet(synchronizer, 5, {700 * US, -300 * US, 200 * US, 0});
    REQUIRE(ready[0].record.offsetNs == 1000 * US);
}

TEST_CASE("FrameSynchronizer keeps a set pending until every channel arrives") {
    FrameSynchronizer synchronizer(makeConfig(3));
    std::vector<frame_set> ready;
    synchronizer.add(makeFrame(0, 1, 0), ready);
    synchronizer.add(makeFrame(2, 1, 0), ready);
    REQUIRE(ready.empty());
    REQUIRE(synchronizer.get_pending_ticks() == 1);
    synchronizer.add(makeFrame(1, 1, 10 * US), ready);
    REQUIRE(ready.size() == 1);
    REQUIRE(ready[0].frames[1]->timestampNs == 10 * US);
}

TEST_CASE("FrameSynchronizer gives up on a set after maxPendingTicks newer ticks") {
    FrameSynchronizer synchronizer(makeConfig(3));
    std::vector<frame_set> ready;
    synchronizer.add(makeFrame(0, 10, 0), ready);
    synchronizer.add(makeFrame(1, 10, 0), ready);
    // channel 2 never delivers tick 10
    synchronizer.add(makeFrame(0, 12, 0), ready);
    REQUIRE(ready.empty());
    synchronizer.add(makeFrame(0, 13, 0), ready);
    REQUIRE(ready.size() == 1);
    REQUIRE(ready[0].record.tick == 10);
    REQUIRE(ready[0].record.verdict == sync_verdict::incomplete);
    REQUIRE_FALSE(ready[0].record.present[2]);
}

TEST_CASE("FrameSynchronizer emits sets in tick order") {
    FrameSynchronizer synchronizer(makeConfig(2));
    std::vector<frame_set> ready;
    synchronizer.add(makeFrame(0, 2, 0), ready);
    synchronizer.add(makeFrame(1, 2, 0), ready);
    REQUIRE(ready.size() == 1);
    ready.clear();
    synchronizer.add(makeFrame(0, 4, 0), ready);
    synchronizer.add(makeFrame(0, 3, 0), ready);
    synchronizer.add(makeFrame(1, 4, 0), ready);
    REQUIRE(ready.empty());  // tick 3 still waits for channel 1 and blocks tick 4
    synchronizer.add(makeFrame(1, 3, 0), ready);
    REQUIRE(ready.size() == 2);
    REQUIRE(ready[0].record.tick == 3);
    REQUIRE(ready[1].record.tick == 4);
}

TEST_CASE("FrameSynchronizer drops a frame for a tick already emitted") {
    FrameSynchronizer synchronizer(makeConfig(2));
    feedSet(synchronizer, 5, {0, 0});
    std::vector<frame_set> ready;
    synchronizer.add(makeFrame(0, 5, 0), ready);
    synchronizer.add(makeFrame(1, 4, 0), ready);
    REQUIRE(ready.empty());
    REQUIRE(synchronizer.get_late_frames() == 2);
}

TEST_CASE("FrameSynchronizer rejects channel indexes outside the configured range and duplicates") {
    FrameSynchronizer synchronizer(makeConfig(3));
    std::vector<frame_set> ready;
    synchronizer.add(makeFrame(-1, 1, 0), ready);
    synchronizer.add(makeFrame(3, 1, 0), ready);
    synchronizer.add(makeFrame(0, 1, 0), ready);
    synchronizer.add(makeFrame(0, 1, 0), ready);
    REQUIRE(synchronizer.get_rejected_frames() == 3);
}

TEST_CASE("FrameSynchronizer flush emits pending sets as incomplete") {
    FrameSynchronizer synchronizer(makeConfig(3));
    std::vector<frame_set> ready;
    synchronizer.add(makeFrame(0, 1, 0), ready);
    synchronizer.flush(ready);
    REQUIRE(ready.size() == 1);
    REQUIRE(ready[0].record.verdict == sync_verdict::incomplete);
    REQUIRE(synchronizer.get_pending_ticks() == 0);
}
