#include "catch_amalgamated.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "FakeMqttClient.h"
#include "component/MqttComponent.h"
#include "core/StateStore.h"

namespace {

class Harness {
public:
    Harness()
        : bus(loop, 64)
        , component(loop, bus, state, state, client, mqtt_config{}, status_identity{"test", {}}) {
        bus.subscribe<control_command>([this](const control_command& command) {
            commands.push_back(command);
        });
        component.initialize();
        component.start();
        // the client reports a connection, as Paho would after connect()
        client.get_callbacks().onConnected();
        pump();
    }

    ~Harness() {
        component.stop();
    }

    void pump() {
        for (int turn = 0; turn < 8; ++turn) {
            uv_run(loop.get_handle(), UV_RUN_NOWAIT);
        }
    }

    UvLoop loop;
    MessageBus bus;
    StateStore state;
    FakeMqttClient client;
    MqttComponent component;
    std::vector<control_command> commands;
};

}  // namespace

TEST_CASE("MqttComponent announces itself and publishes status on connect") {
    Harness harness;
    const auto online = harness.client.get_published("video/online");
    REQUIRE(online.size() == 1);
    REQUIRE(online[0].payload == "online");
    REQUIRE(online[0].retain);
    const auto status = harness.client.get_published("video/status");
    REQUIRE(status.size() == 1);
    REQUIRE(nlohmann::json::parse(status[0].payload)["schema_version"] == 1);
    REQUIRE(harness.state.get_mqtt_state()->connected);
}

TEST_CASE("MqttComponent turns a valid video/control message into a bus command") {
    Harness harness;
    harness.component.TDD_handleMessage({"video/control", R"({"command":"stop","request_id":"r-1"})"});
    harness.pump();
    REQUIRE(harness.commands.size() == 1);
    REQUIRE(harness.commands[0].action == control_action::stop);
    REQUIRE(harness.commands[0].origin == control_origin::mqtt);
    REQUIRE(harness.commands[0].requestId == "r-1");
}

TEST_CASE("MqttComponent rejects a bad control message with an ack and no command") {
    Harness harness;
    harness.component.TDD_handleMessage({"video/control", "reboot"});
    harness.pump();
    REQUIRE(harness.commands.empty());
    const auto acks = harness.client.get_published("video/control/ack");
    REQUIRE(acks.size() == 1);
    const auto ack = nlohmann::json::parse(acks[0].payload);
    REQUIRE(ack["result"] == "rejected");
    REQUIRE(ack["reason"] == "rejected_unknown_command");
    REQUIRE(harness.state.get_mqtt_state()->commandsRejected == 1);
}

TEST_CASE("MqttComponent acknowledges results of MQTT commands only") {
    Harness harness;
    harness.bus.publish(control_result{control_action::start, control_origin::mqtt, "r-2", true, ""});
    harness.bus.publish(control_result{control_action::start, control_origin::config, "autostart", true, ""});
    harness.pump();
    const auto acks = harness.client.get_published("video/control/ack");
    REQUIRE(acks.size() == 1);
    const auto ack = nlohmann::json::parse(acks[0].payload);
    REQUIRE(ack["request_id"] == "r-2");
    REQUIRE(ack["result"] == "accepted");
}

TEST_CASE("MqttComponent ignores messages on other topics") {
    Harness harness;
    harness.component.TDD_handleMessage({"video/other", "start"});
    harness.pump();
    REQUIRE(harness.commands.empty());
}
