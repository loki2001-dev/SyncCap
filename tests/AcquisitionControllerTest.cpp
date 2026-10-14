#include "catch_amalgamated.hpp"

#include <chrono>
#include <string>
#include <vector>

#include "component/AcquisitionController.h"
#include "core/StateStore.h"

namespace {

class FakeRecordSink : public IRecordSink {
public:
    bool beginSession(const session_info& session) override {
        begins += 1;
        last = session;
        return beginResult;
    }
    void endSession() override {
        ends += 1;
    }
    bool push(record_item /*item*/) override {
        return true;
    }
    recorder_stats get_stats() const override {
        return {};
    }

    bool beginResult = true;
    int begins = 0;
    int ends = 0;
    session_info last;
};

acquisition_config makeConfig(int stopTimeoutMs) {
    acquisition_config config;
    config.deviceId = "test";
    config.storageRoot = "recordings";
    config.segmentSeconds = 180;
    config.autostart = false;
    config.stopTimeoutMs = stopTimeoutMs;
    return config;
}

// a controller on a real loop and bus, with every bus message it produces captured
class Harness {
public:
    explicit Harness(int stopTimeoutMs = 10000)
        : bus(loop, 64)
        , controller(loop, bus, state, state, sink, makeConfig(stopTimeoutMs)) {
        bus.subscribe<control_result>([this](const control_result& result) {
            results.push_back(result);
        });
        bus.subscribe<acquisition_started>([this](const acquisition_started&) {
            events.push_back("started");
        });
        bus.subscribe<acquisition_stopping>([this](const acquisition_stopping&) {
            events.push_back("stopping");
        });
        bus.subscribe<acquisition_stopped>([this](const acquisition_stopped&) {
            events.push_back("stopped");
        });
        controller.initialize();
        controller.start();
        pump();
    }

    void pump() {
        for (int turn = 0; turn < 8; ++turn) {
            uv_run(loop.get_handle(), UV_RUN_NOWAIT);
        }
    }

    void send(control_action action) {
        bus.publish(control_command{action, control_origin::mqtt, "req"});
        pump();
    }

    template <typename T>
    void publish(T message) {
        bus.publish(std::move(message));
        pump();
    }

    acquisition_phase phase() const {
        return state.get_acquisition_state()->phase;
    }

    UvLoop loop;
    MessageBus bus;
    StateStore state;
    FakeRecordSink sink;
    AcquisitionController controller;
    std::vector<control_result> results;
    std::vector<std::string> events;
};

}  // namespace

TEST_CASE("AcquisitionController starts a session on start") {
    Harness harness;
    harness.send(control_action::start);
    REQUIRE(harness.phase() == acquisition_phase::running);
    REQUIRE(harness.sink.begins == 1);
    REQUIRE(harness.sink.last.directory.find("_test") != std::string::npos);
    REQUIRE(harness.events == std::vector<std::string>{"started"});
    REQUIRE(harness.results.back().accepted);
}

TEST_CASE("AcquisitionController rejects start while running") {
    Harness harness;
    harness.send(control_action::start);
    harness.send(control_action::start);
    REQUIRE_FALSE(harness.results.back().accepted);
    REQUIRE(harness.results.back().reason == "rejected_already_running");
    REQUIRE(harness.sink.begins == 1);
}

TEST_CASE("AcquisitionController rejects stop while idle") {
    Harness harness;
    harness.send(control_action::stop);
    REQUIRE_FALSE(harness.results.back().accepted);
    REQUIRE(harness.results.back().reason == "rejected_not_running");
}

TEST_CASE("AcquisitionController stops through the video handshake") {
    Harness harness;
    harness.send(control_action::start);
    harness.publish(video_pipeline_started{true, ""});
    harness.send(control_action::stop);
    REQUIRE(harness.phase() == acquisition_phase::stopping);
    REQUIRE(harness.sink.ends == 0);

    SECTION("start is refused while stopping") {
        harness.send(control_action::start);
        REQUIRE(harness.results.back().reason == "rejected_busy_stopping");
    }

    harness.publish(video_pipeline_stopped{"done"});
    REQUIRE(harness.phase() == acquisition_phase::idle);
    REQUIRE(harness.sink.ends == 1);
    REQUIRE(harness.events == std::vector<std::string>{"started", "stopping", "stopped"});
}

TEST_CASE("AcquisitionController refuses to start when the session directory fails") {
    Harness harness;
    harness.sink.beginResult = false;
    harness.send(control_action::start);
    REQUIRE(harness.phase() == acquisition_phase::error);
    REQUIRE(harness.results.back().reason == "rejected_storage");
    REQUIRE(harness.events.empty());
}

TEST_CASE("AcquisitionController ends in error after a pipeline failure") {
    Harness harness;
    harness.send(control_action::start);
    harness.publish(video_pipeline_failed{"reader ch1: end of stream"});
    REQUIRE(harness.phase() == acquisition_phase::stopping);
    harness.publish(video_pipeline_stopped{""});
    REQUIRE(harness.phase() == acquisition_phase::error);
    REQUIRE(harness.state.get_acquisition_state()->reason.find("video_failed") != std::string::npos);
    // a new start is allowed from error
    harness.send(control_action::start);
    REQUIRE(harness.phase() == acquisition_phase::running);
}

TEST_CASE("AcquisitionController ends in error when the pipeline cannot start") {
    Harness harness;
    harness.send(control_action::start);
    harness.publish(video_pipeline_started{false, "source open failed channel=2"});
    harness.publish(video_pipeline_stopped{""});
    REQUIRE(harness.phase() == acquisition_phase::error);
    REQUIRE(harness.sink.ends == 1);
}

TEST_CASE("AcquisitionController closes the session when the video side never answers") {
    Harness harness(50);
    harness.send(control_action::start);
    harness.send(control_action::stop);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (harness.phase() == acquisition_phase::stopping && std::chrono::steady_clock::now() < deadline) {
        uv_run(harness.loop.get_handle(), UV_RUN_ONCE);
    }
    harness.pump();
    REQUIRE(harness.phase() == acquisition_phase::idle);
    REQUIRE(harness.sink.ends == 1);
    REQUIRE(harness.state.get_acquisition_state()->reason.find("stop_timeout") != std::string::npos);
}
