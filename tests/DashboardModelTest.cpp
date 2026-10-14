#include "catch_amalgamated.hpp"

#include <cstdint>

#include <algorithm>
#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "model/DashboardModel.h"
#include "codec/StatusJsonCodec.h"

namespace {

http_result response(int status, const std::string& body) {
    http_result result;
    result.status = status;
    result.ok = status >= 200 && status < 300;
    result.body = body;
    return result;
}

http_result unreachable() {
    http_result result;
    result.error = "Could not establish connection";
    return result;
}

std::string statusBody(acquisition_phase phase, bool locked, double offsetMs, bool online = true,
                       const std::string& timestamp = std::string()) {
    static int sequence = 0;
    system_status status;
    status.deviceId = "synccap-01";
    // every document is a new one unless the caller pins the timestamp
    status.timestamp = timestamp.empty() ? "t" + std::to_string(++sequence) : timestamp;
    status.acquisition.phase = phase;
    for (int id = 0; id < 4; ++id) {
        channel_stats channel;
        channel.id = id;
        channel.fps = 30.0 - id;
        status.channels.push_back(channel);
    }
    status.sync.locked = locked;
    status.sync.lastOffsetMs = offsetMs;
    status.sync.windowMaxOffsetMs = offsetMs;
    status.sync.toleranceMs = 2.0;
    status.sync.violations = locked ? 0 : 3;
    status.sensor.valid = true;
    status.sensor.temperatureC = 24.5;
    nlohmann::json document = StatusJsonCodec::toJson(status);
    document["server_meta"] = {{"received_at", "2026-10-14T13:00:00.000Z"},
                               {"received_via", "mqtt"},
                               {"age_s", 1.5},
                               {"stale", false},
                               {"collector_online", online}};
    return document.dump();
}

http_result health(bool mqtt) {
    return response(200, nlohmann::json{{"status", "ok"}, {"mqtt_connected", mqtt}}.dump());
}

preview_update previewAt(uint64_t tick, std::chrono::steady_clock::time_point receivedAt) {
    preview_update update;
    update.tick = tick;
    update.receivedAt = receivedAt;
    return update;
}

bool hasEvent(const DashboardModel& model, const std::string& text) {
    for (const event_entry& event : model.get_events()) {
        if (event.text.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("DashboardModel reads a status document with the server metadata") {
    DashboardModel model;
    model.applyStatus(response(200, statusBody(acquisition_phase::running, true, 0.4)));
    REQUIRE(model.get_server_link() == link_state::connected);
    const status_snapshot& snapshot = model.get_status();
    REQUIRE(snapshot.available);
    REQUIRE(snapshot.status.channels.size() == 4);
    REQUIRE(snapshot.status.sync.toleranceMs == Catch::Approx(2.0));
    REQUIRE(snapshot.meta.receivedVia == "mqtt");
    REQUIRE(snapshot.meta.collectorOnline == true);
    REQUIRE(model.findFpsHistory(3)->get_last() == Catch::Approx(27.0F));
    REQUIRE(model.get_offset_history().get_last() == Catch::Approx(0.4F));
}

TEST_CASE("DashboardModel treats 404 as a reachable server without status") {
    DashboardModel model;
    model.applyStatus(response(404, R"({"error":"no status received yet"})"));
    REQUIRE(model.get_server_link() == link_state::connected);
    REQUIRE_FALSE(model.get_status().available);
}

TEST_CASE("DashboardModel reports a lost server once and clears the broker flag") {
    DashboardModel model;
    model.applyHealth(health(true));
    model.applyStatus(response(200, statusBody(acquisition_phase::running, true, 0.4)));
    REQUIRE(model.isMqttConnected());
    model.applyStatus(unreachable());
    model.applyStatus(unreachable());
    REQUIRE(model.get_server_link() == link_state::unreachable);
    REQUIRE_FALSE(model.isMqttConnected());
    int lost = 0;
    for (const event_entry& event : model.get_events()) {
        lost += event.text.find("server unreachable") != std::string::npos ? 1 : 0;
    }
    REQUIRE(lost == 1);
    REQUIRE(model.get_poll_failures() == 2);
}

TEST_CASE("DashboardModel enables Start and Stop from the phase and the links") {
    DashboardModel model;
    REQUIRE_FALSE(model.canStart());
    model.applyStatus(response(200, statusBody(acquisition_phase::idle, false, 0.0)));
    REQUIRE_FALSE(model.canStart());  // broker state unknown
    model.applyHealth(health(true));
    REQUIRE(model.canStart());
    REQUIRE_FALSE(model.canStop());
    model.applyStatus(response(200, statusBody(acquisition_phase::running, true, 0.3)));
    REQUIRE_FALSE(model.canStart());
    REQUIRE(model.canStop());
    model.applyStatus(response(200, statusBody(acquisition_phase::stopping, true, 0.3)));
    REQUIRE_FALSE(model.canStart());
    REQUIRE_FALSE(model.canStop());
    model.applyStatus(response(200, statusBody(acquisition_phase::error, false, 0.0)));
    REQUIRE(model.canStart());
    model.applyHealth(health(false));
    REQUIRE_FALSE(model.canStart());
}

TEST_CASE("DashboardModel turns state changes into events") {
    DashboardModel model;
    model.applyStatus(response(200, statusBody(acquisition_phase::running, true, 0.3)));
    model.applyStatus(response(200, statusBody(acquisition_phase::running, false, 2.5)));
    REQUIRE(hasEvent(model, "sync lost"));
    model.applyStatus(response(200, statusBody(acquisition_phase::idle, false, 0.0, false)));
    REQUIRE(hasEvent(model, "acquisition running -> idle"));
    REQUIRE(hasEvent(model, "collector offline"));
}

TEST_CASE("DashboardModel follows a control request from click to the collector answer") {
    DashboardModel model;
    model.noteControlRequested(control_action::stop);
    REQUIRE(model.get_controls().front().result == "sending");
    model.applyControlSubmitted(control_action::stop,
                                response(202, R"({"request_id":"req-1","command":"stop","result":"pending"})"));
    REQUIRE(model.get_controls().front().requestId == "req-1");
    REQUIRE(model.get_controls().front().result == "pending");
    model.applyControlRecord(
        response(200, R"({"request_id":"req-1","result":"rejected","reason":"rejected_not_running"})"));
    REQUIRE(model.get_controls().front().result == "rejected");
    REQUIRE(model.get_controls().front().reason == "rejected_not_running");
    REQUIRE(hasEvent(model, "rejected: rejected_not_running"));
}

TEST_CASE("DashboardModel shows a refused control request with the server reason") {
    DashboardModel model;
    model.noteControlRequested(control_action::start);
    model.applyControlSubmitted(control_action::start, response(503, R"({"error":"mqtt_disconnected"})"));
    REQUIRE(model.get_controls().front().result == "failed");
    REQUIRE(model.get_controls().front().reason == "mqtt_disconnected");
}

TEST_CASE("DashboardModel keeps its lists and histories bounded") {
    DashboardModel model(10);
    for (int index = 0; index < 50; ++index) {
        model.applyStatus(response(200, statusBody(acquisition_phase::running, index % 2 == 0, 0.1 * index)));
        model.noteControlRequested(control_action::start);
    }
    for (size_t index = 0; index < DashboardModel::MAX_EVENTS + 10; ++index) {
        model.noteEvent(event_level::info, "event");
    }
    REQUIRE(model.get_offset_history().get_size() == 10);
    REQUIRE(model.get_offset_history().get_last() == Catch::Approx(4.9F));
    REQUIRE(model.get_controls().size() == DashboardModel::MAX_CONTROLS);
    REQUIRE(model.get_events().size() == DashboardModel::MAX_EVENTS);
}

TEST_CASE("MetricSeries returns values oldest first after wrapping") {
    MetricSeries series(3);
    for (int value = 1; value <= 5; ++value) {
        series.push(static_cast<float>(value));
    }
    const auto ordered = series.get_ordered();
    REQUIRE(ordered == std::vector<float>{3.0F, 4.0F, 5.0F});
    REQUIRE(series.get_max() == Catch::Approx(5.0F));
    REQUIRE(series.get_last() == Catch::Approx(5.0F));
}

TEST_CASE("DashboardModel extends the plots only for a new status document") {
    DashboardModel model;
    const std::string same = statusBody(acquisition_phase::running, true, 0.5, true, "2026-10-14T13:00:00.000Z");
    model.applyStatus(response(200, same));
    model.applyStatus(response(200, same));
    model.applyStatus(response(200, same));
    REQUIRE(model.get_offset_history().get_size() == 1);
    REQUIRE(model.findFpsHistory(0)->get_size() == 1);
    model.applyStatus(response(200, statusBody(acquisition_phase::running, true, 0.7)));
    REQUIRE(model.get_offset_history().get_size() == 2);
    REQUIRE(model.get_polls() == 4);
}

TEST_CASE("DashboardModel reports the preview rate of evenly spaced sets") {
    DashboardModel model(8);
    const auto start = std::chrono::steady_clock::now();
    const std::chrono::microseconds period(33333);
    for (uint64_t tick = 0; tick < 30; ++tick) {
        model.applyPreview(previewAt(tick, start + period * static_cast<int64_t>(tick)));
    }
    REQUIRE(model.get_preview().setsPerSecond == Catch::Approx(30.0).margin(0.1));
}

TEST_CASE("DashboardModel keeps the preview rate bounded when sets are applied back to back") {
    // the UI drains the inbox once per frame: two sets 66 ms apart can arrive with a few microseconds between
    DashboardModel model(8);
    const auto start = std::chrono::steady_clock::now();
    const std::chrono::microseconds pair(66667);
    const std::chrono::microseconds backToBack(5);
    double highest = 0.0;
    for (uint64_t pairIndex = 0; pairIndex < 30; ++pairIndex) {
        const auto first = start + pair * static_cast<int64_t>(pairIndex);
        model.applyPreview(previewAt(pairIndex * 2, first));
        highest = std::max(highest, model.get_preview().setsPerSecond);
        model.applyPreview(previewAt(pairIndex * 2 + 1, first + backToBack));
        highest = std::max(highest, model.get_preview().setsPerSecond);
    }
    REQUIRE(highest < 45.0);
    REQUIRE(model.get_preview().setsPerSecond == Catch::Approx(30.0).margin(8.0));
}

TEST_CASE("DashboardModel restarts the preview rate after the preview was unavailable") {
    DashboardModel model(8);
    const auto start = std::chrono::steady_clock::now();
    const std::chrono::microseconds period(33333);
    for (uint64_t tick = 0; tick < 10; ++tick) {
        model.applyPreview(previewAt(tick, start + period * static_cast<int64_t>(tick)));
    }
    model.applyPreviewUnavailable(true, "no preview yet (not recording?)");
    REQUIRE(model.get_preview().setsPerSecond == 0.0);

    // recording resumes 10 s later: the pause must not count as one long interval
    const auto resumed = start + std::chrono::seconds(10);
    model.applyPreview(previewAt(100, resumed));
    model.applyPreview(previewAt(101, resumed + period));
    REQUIRE(model.get_preview().setsPerSecond == 0.0);  // too little history yet, never a burst value
    for (uint64_t tick = 2; tick < 10; ++tick) {
        model.applyPreview(previewAt(100 + tick, resumed + period * static_cast<int64_t>(tick)));
    }
    REQUIRE(model.get_preview().setsPerSecond == Catch::Approx(30.0).margin(0.1));
}
