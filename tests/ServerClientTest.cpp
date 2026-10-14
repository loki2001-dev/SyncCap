#include "catch_amalgamated.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model/DashboardModel.h"
#include "net/ServerClient.h"

namespace {

// answers like the OpenAPI server and records every request
class FakeHttpClient : public IHttpClient {
public:
    http_result post(const std::string& url, const std::string& body, const std::string&,
                     std::chrono::milliseconds) override {
        const std::lock_guard<std::mutex> lock(_mutex);
        requests.push_back("POST " + url + " " + body);
        return answer(202, R"({"request_id":"req-7","command":"stop","result":"pending"})");
    }

    http_result get(const std::string& url, std::chrono::milliseconds) override {
        const std::lock_guard<std::mutex> lock(_mutex);
        requests.push_back("GET " + url);
        if (url.find("/control/req-7") != std::string::npos) {
            controlPolls += 1;
            return controlPolls < 2 ? answer(200, R"({"request_id":"req-7","result":"pending"})")
                                    : answer(200, R"({"request_id":"req-7","result":"accepted","reason":""})");
        }
        if (url.find("/health") != std::string::npos) {
            return answer(200, R"({"status":"ok","mqtt_connected":true})");
        }
        return answer(404, R"({"error":"no status received yet"})");
    }

    std::vector<std::string> get_requests() {
        const std::lock_guard<std::mutex> lock(_mutex);
        return requests;
    }

private:
    static http_result answer(int status, const std::string& body) {
        http_result result;
        result.status = status;
        result.ok = status >= 200 && status < 300;
        result.body = body;
        return result;
    }

    std::mutex _mutex;
    std::vector<std::string> requests;
    int controlPolls = 0;
};

void drain(BlockingQueue<model_update_t>& inbox, DashboardModel& model) {
    model_update_t update;
    while (inbox.tryPop(update)) {
        update(model);
    }
}

}  // namespace

TEST_CASE("ServerClient polls status and health and follows a control request to its answer") {
    FakeHttpClient http;
    BlockingQueue<model_update_t> inbox(256, overflow_policy::drop_oldest);
    DashboardModel model;
    ServerClient client("http://127.0.0.1:8080/", http, std::chrono::milliseconds(50), std::chrono::milliseconds(100),
                        inbox);
    client.start();
    model.noteControlRequested(control_action::stop);
    REQUIRE(client.requestControl(control_action::stop));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        drain(inbox, model);
        if (!model.get_controls().empty() && model.get_controls().front().result == "accepted") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.stop();
    drain(inbox, model);

    REQUIRE(model.get_controls().front().result == "accepted");
    REQUIRE(model.get_server_link() == link_state::connected);
    REQUIRE(model.isMqttConnected());
    const auto requests = http.get_requests();
    bool posted = false;
    for (const std::string& request : requests) {
        posted = posted || request == R"(POST http://127.0.0.1:8080/control {"command":"stop"})";
    }
    REQUIRE(posted);
    REQUIRE(requests.front() == "GET http://127.0.0.1:8080/status");
}

TEST_CASE("ServerClient refuses control requests beyond its queue") {
    FakeHttpClient http;
    BlockingQueue<model_update_t> inbox(16, overflow_policy::drop_oldest);
    ServerClient client("http://127.0.0.1:8080", http, std::chrono::seconds(10), std::chrono::milliseconds(100), inbox);
    // not started: nothing drains the request queue
    REQUIRE(client.requestControl(control_action::start));
    REQUIRE(client.requestControl(control_action::stop));
    REQUIRE_FALSE(client.requestControl(control_action::start));
}
