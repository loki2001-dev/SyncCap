#include "catch_amalgamated.hpp"

#include <string>

#include "codec/StatusJsonCodec.h"

namespace {

system_status makeStatus() {
    system_status status;
    status.deviceId = "synccap-test";
    status.timestamp = "2026-10-14T13:00:00.000Z";
    status.uptimeS = 12.5;
    status.acquisition.phase = acquisition_phase::running;
    status.acquisition.sessionId = "session";
    for (int id = 0; id < 4; ++id) {
        channel_stats channel;
        channel.id = id;
        channel.fps = 29.97;
        channel.queueLength = static_cast<size_t>(id);
        channel.sourceType = "synthetic";
        status.channels.push_back(channel);
    }
    status.sync.windowMaxOffsetMs = 1.8;
    status.sync.locked = true;
    status.sensor.temperatureC = 24.5;
    status.sensor.humidityPct = 60.2;
    status.sensor.accelerationG = 0.03;
    status.storage.recentFiles = {"a/ch0.h264", "a/ch1.h264"};
    return status;
}

}  // namespace

TEST_CASE("StatusJsonCodec encodes the three required status fields") {
    const nlohmann::json json = StatusJsonCodec::toJson(makeStatus());
    REQUIRE(json["channel_status"].size() == 4);
    REQUIRE(json["channel_status"][0]["id"] == 0);
    REQUIRE(json["channel_status"][0]["fps"] == Catch::Approx(29.97));
    REQUIRE(json["channel_status"][2]["queue_length"] == 2);
    REQUIRE(json["sync_status"]["max_offset_ms"] == Catch::Approx(1.8));
    REQUIRE(json["sync_status"]["locked"] == true);
    REQUIRE(json["sensor_status"]["temperature"] == Catch::Approx(24.5));
    REQUIRE(json["sensor_status"]["humidity"] == Catch::Approx(60.2));
    REQUIRE(json["sensor_status"]["acceleration"] == Catch::Approx(0.03));
    REQUIRE(json["acquisition"]["state"] == "running");
}

TEST_CASE("StatusJsonCodec round-trips a status document") {
    system_status decoded;
    nlohmann::json document;
    std::string error;
    REQUIRE(StatusJsonCodec::decode(StatusJsonCodec::encode(makeStatus()), decoded, document, error));
    REQUIRE(decoded.deviceId == "synccap-test");
    REQUIRE(decoded.channels.size() == 4);
    REQUIRE(decoded.channels[3].queueLength == 3);
    REQUIRE(decoded.sync.locked);
    REQUIRE(decoded.sensor.humidityPct == Catch::Approx(60.2));
    REQUIRE(decoded.acquisition.phase == acquisition_phase::running);
    REQUIRE(decoded.storage.recentFiles.size() == 2);
}

TEST_CASE("StatusJsonCodec accepts a minimal document and ignores unknown fields") {
    const std::string text = R"({"schema_version":1,
        "channel_status":[{"id":0,"fps":28,"queue_length":5}],
        "sync_status":{"max_offset_ms":1.8,"locked":true},
        "sensor_status":{"temperature":24.5,"humidity":60.2,"acceleration":0.03},
        "future_field":{"x":1}})";
    system_status status;
    nlohmann::json document;
    std::string error;
    REQUIRE(StatusJsonCodec::decode(text, status, document, error));
    REQUIRE(status.channels[0].queueLength == 5);
}

TEST_CASE("StatusJsonCodec rejects malformed documents with a reason") {
    system_status status;
    nlohmann::json document;
    std::string error;

    SECTION("not JSON") {
        REQUIRE_FALSE(StatusJsonCodec::decode("{oops", status, document, error));
        REQUIRE(error == "body is not a JSON object");
    }
    SECTION("unsupported schema version") {
        REQUIRE_FALSE(StatusJsonCodec::decode(R"({"schema_version":2})", status, document, error));
        REQUIRE(error.find("schema_version") != std::string::npos);
    }
    SECTION("missing channel_status") {
        REQUIRE_FALSE(StatusJsonCodec::decode(
            R"({"schema_version":1,"sync_status":{"max_offset_ms":1,"locked":true},"sensor_status":{"temperature":1,"humidity":1,"acceleration":0}})",
            status, document, error));
        REQUIRE(error.find("channel_status") != std::string::npos);
    }
    SECTION("locked is not a boolean") {
        REQUIRE_FALSE(StatusJsonCodec::decode(
            R"({"schema_version":1,"channel_status":[],"sync_status":{"max_offset_ms":1,"locked":"yes"},"sensor_status":{"temperature":1,"humidity":1,"acceleration":0}})",
            status, document, error));
        REQUIRE(error.find("locked") != std::string::npos);
    }
    SECTION("negative queue length") {
        REQUIRE_FALSE(StatusJsonCodec::decode(
            R"({"schema_version":1,"channel_status":[{"id":0,"fps":1,"queue_length":-1}],"sync_status":{"max_offset_ms":1,"locked":true},"sensor_status":{"temperature":1,"humidity":1,"acceleration":0}})",
            status, document, error));
        REQUIRE(error.find("queue_length") != std::string::npos);
    }
    SECTION("document over the size ceiling") {
        const std::string huge(StatusJsonCodec::MAX_DOCUMENT_BYTES + 1, ' ');
        REQUIRE_FALSE(StatusJsonCodec::decode(huge, status, document, error));
        REQUIRE(error.find("too large") != std::string::npos);
    }
}
