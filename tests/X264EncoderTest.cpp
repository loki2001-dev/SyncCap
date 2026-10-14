#include "catch_amalgamated.hpp"

#include <cstring>
#include <memory>
#include <vector>

#include "H264Inspect.h"
#include "codec/X264Encoder.h"

namespace {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;
constexpr int FPS = 30;

video_frame makeFrame(uint64_t tick, int64_t segment) {
    auto buffer = std::make_shared<FrameBuffer>(WIDTH, HEIGHT);
    std::memset(buffer->get_plane(0), static_cast<int>(16 + tick * 7 % 200), buffer->get_size());
    video_frame frame;
    frame.tick = tick;
    frame.segment = segment;
    frame.buffer = buffer;
    return frame;
}

encoder_config makeConfig() {
    encoder_config config;
    config.preset = "ultrafast";
    config.tune = "zerolatency";
    config.bitrateKbps = 1000;
    config.threads = 1;
    config.gop = 300;
    return config;
}

}  // namespace

TEST_CASE("X264Encoder starts the stream with SPS and an IDR picture") {
    X264Encoder encoder;
    REQUIRE(encoder.open(2, WIDTH, HEIGHT, FPS, makeConfig()));
    std::vector<encoded_packet> packets;
    REQUIRE(encoder.encode(makeFrame(100, 0), true, packets));
    REQUIRE(packets.size() == 1);
    REQUIRE(packets[0].keyFrame);
    REQUIRE(packets[0].channel == 2);
    REQUIRE(packets[0].tick == 100);
    const h264_summary summary = inspectH264(packets[0].data);
    REQUIRE(summary.sps >= 1);
    REQUIRE(summary.idrPictures == 1);
}

TEST_CASE("X264Encoder forces an IDR picture on request and keeps tick order") {
    X264Encoder encoder;
    REQUIRE(encoder.open(0, WIDTH, HEIGHT, FPS, makeConfig()));
    std::vector<encoded_packet> packets;
    for (uint64_t tick = 0; tick < 6; ++tick) {
        const bool forceKey = tick == 0 || tick == 4;
        REQUIRE(encoder.encode(makeFrame(tick, tick < 4 ? 0 : 1), forceKey, packets));
    }
    encoder.flush(packets);
    REQUIRE(packets.size() == 6);
    for (uint64_t tick = 0; tick < 6; ++tick) {
        REQUIRE(packets[tick].tick == tick);
    }
    REQUIRE(packets[0].keyFrame);
    REQUIRE_FALSE(packets[1].keyFrame);
    REQUIRE(packets[4].keyFrame);
    REQUIRE(packets[4].segment == 1);
    // a forced key picture repeats SPS/PPS so a new segment file starts decodable
    REQUIRE(inspectH264(packets[4].data).sps >= 1);
}

TEST_CASE("X264Encoder refuses a frame of the wrong size") {
    X264Encoder encoder;
    REQUIRE(encoder.open(0, WIDTH, HEIGHT, FPS, makeConfig()));
    video_frame frame;
    frame.buffer = std::make_shared<FrameBuffer>(WIDTH / 2, HEIGHT / 2);
    std::vector<encoded_packet> packets;
    REQUIRE_FALSE(encoder.encode(frame, true, packets));
}
