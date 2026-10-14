#include "catch_amalgamated.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ApiServer.h"
#include "ControlRelay.h"
#include "FakeMqttClient.h"
#include "StatusRepository.h"
#include "codec/ControlCommandCodec.h"
#include "codec/StatusJsonCodec.h"
#include "core/UvLoop.h"

namespace {

std::string sampleStatus() {
    system_status status;
    status.deviceId = "synccap-test";
    for (int id = 0; id < 3; ++id) {
        channel_stats channel;
        channel.id = id;
        channel.fps = 30.0;
        status.channels.push_back(channel);
    }
    status.sync.locked = true;
    status.sync.windowMaxOffsetMs = 0.4;
    status.sensor.temperatureC = 24.5;
    status.storage.recentFiles = {"session/seg_0000/ch0.h264"};
    return StatusJsonCodec::encode(status);
}

// the real ApiServer on an ephemeral port, its service loop on a thread, and a fake broker
class ServerHarness {
public:
    ServerHarness()
        : repository(100, std::chrono::seconds(60))
        , control(mqtt, "video/control", 64)
        , api(loop, repository, control, mqtt, makeConfig(), "openapi: 3.0.3\n") {
        port = api.TDD_bindToAnyPort();
        loopThread = std::thread([this]() {
            loop.run();
        });
        httpThread = std::thread([this]() {
            api.TDD_listenAfterBind();
        });
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
        client->set_connection_timeout(std::chrono::seconds(2));
        client->set_read_timeout(std::chrono::seconds(5));
    }

    ~ServerHarness() {
        api.stop();
        httpThread.join();
        loop.requestStop();
        loopThread.join();
    }

    static server_config makeConfig() {
        server_config config;
        config.bind = "127.0.0.1";
        config.requestTimeoutMs = 2000;
        return config;
    }

    // lets the service loop handle an ack as the broker callback would
    void deliverAck(const control_ack& ack) {
        std::promise<void> done;
        loop.post([this, ack, &done]() {
            control.handleAck(ack);
            done.set_value();
        });
        done.get_future().wait();
    }

    UvLoop loop;
    FakeMqttClient mqtt;
    StatusRepository repository;
    ControlRelay control;
    ApiServer api;
    int port = 0;
    std::thread loopThread;
    std::thread httpThread;
    std::unique_ptr<httplib::Client> client;
};

}  // namespace

TEST_CASE("ApiServer answers 404 before the first status and the stored status after it") {
    ServerHarness server;
    REQUIRE(server.port > 0);
    auto response = server.client->Get("/status");
    REQUIRE(response);
    REQUIRE(response->status == 404);

    response = server.client->Post("/status", sampleStatus(), "application/json");
    REQUIRE(response);
    REQUIRE(response->status == 204);

    response = server.client->Get("/status");
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body["device_id"] == "synccap-test");
    REQUIRE(body["channel_status"].size() == 3);
    REQUIRE(body["sync_status"]["locked"] == true);
    REQUIRE(body["storage"]["files"][0] == "session/seg_0000/ch0.h264");
    REQUIRE(body["server_meta"]["received_via"] == "http");
    REQUIRE(body["server_meta"]["stale"] == false);
}

TEST_CASE("ApiServer rejects an invalid status document with the reason") {
    ServerHarness server;
    auto response = server.client->Post("/status", R"({"schema_version":1})", "application/json");
    REQUIRE(response->status == 400);
    REQUIRE(nlohmann::json::parse(response->body)["error"].get<std::string>().find("channel_status") !=
            std::string::npos);
    response = server.client->Post("/status", "not json", "application/json");
    REQUIRE(response->status == 400);
}

TEST_CASE("ApiServer forwards POST /control to MQTT and reports the collector answer") {
    ServerHarness server;
    auto response = server.client->Post("/control", R"({"command":"stop"})", "application/json");
    REQUIRE(response->status == 202);
    const auto record = nlohmann::json::parse(response->body);
    const std::string requestId = record["request_id"];
    REQUIRE(record["result"] == "pending");

    // judged by what the broker actually received
    const auto sent = server.mqtt.get_published("video/control");
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].qos == 1);
    control_request request;
    std::string reason;
    REQUIRE(ControlCommandCodec::decode(sent[0].payload, request, reason));
    REQUIRE(request.action == control_action::stop);
    REQUIRE(request.requestId == requestId);

    server.deliverAck(control_ack{requestId, "stop", false, "rejected_not_running"});
    response = server.client->Get(("/control/" + requestId).c_str());
    REQUIRE(response->status == 200);
    const auto answered = nlohmann::json::parse(response->body);
    REQUIRE(answered["result"] == "rejected");
    REQUIRE(answered["reason"] == "rejected_not_running");
}

TEST_CASE("ApiServer refuses bad control requests") {
    ServerHarness server;
    REQUIRE(server.client->Post("/control", R"({"command":"reboot"})", "application/json")->status == 400);
    REQUIRE(server.client->Post("/control", "{", "application/json")->status == 400);
    REQUIRE(server.client->Get("/control/unknown-id")->status == 404);
    server.mqtt.connected = false;
    const auto response = server.client->Post("/control", R"({"command":"start"})", "application/json");
    REQUIRE(response->status == 503);
    REQUIRE(nlohmann::json::parse(response->body)["error"] == "mqtt_disconnected");
    REQUIRE(server.mqtt.get_published("video/control").empty());
}

TEST_CASE("ApiServer serves history, health and the OpenAPI document") {
    ServerHarness server;
    server.client->Post("/status", sampleStatus(), "application/json");
    server.client->Post("/status", sampleStatus(), "application/json");
    auto response = server.client->Get("/status/history?limit=1");
    REQUIRE(response->status == 200);
    REQUIRE(nlohmann::json::parse(response->body)["count"] == 1);
    REQUIRE(server.client->Get("/status/history?limit=0")->status == 400);
    REQUIRE(server.client->Get("/status/history?limit=abc")->status == 400);
    response = server.client->Get("/health");
    REQUIRE(response->status == 200);
    REQUIRE(nlohmann::json::parse(response->body)["mqtt_connected"] == true);
    response = server.client->Get("/openapi.yaml");
    REQUIRE(response->status == 200);
    REQUIRE(response->body.rfind("openapi: 3.0.3", 0) == 0);
    REQUIRE(server.client->Get("/docs")->status == 200);
}
