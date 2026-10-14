#include "model/DashboardModel.h"

#include <chrono>
#include <utility>

#include <nlohmann/json.hpp>

#include "codec/StatusJsonCodec.h"
#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr int HTTP_OK = 200;
constexpr int HTTP_ACCEPTED = 202;
constexpr int HTTP_NOT_FOUND = 404;
constexpr double PREVIEW_RATE_SMOOTHING = 0.8;

std::string now() {
    return TimeUtil::formatClockLocal(std::chrono::system_clock::now());
}

server_meta parseMeta(const nlohmann::json& document) {
    server_meta meta;
    const auto found = document.find("server_meta");
    if (found == document.end() || !found->is_object()) {
        return meta;
    }
    const nlohmann::json& object = *found;
    meta.present = true;
    meta.receivedAt = object.value("received_at", std::string());
    meta.receivedVia = object.value("received_via", std::string());
    meta.ageS = object.value("age_s", 0.0);
    meta.stale = object.value("stale", false);
    meta.receivedHttp = object.value("received_http", static_cast<uint64_t>(0));
    meta.receivedMqtt = object.value("received_mqtt", static_cast<uint64_t>(0));
    const auto online = object.find("collector_online");
    if (online != object.end() && online->is_boolean()) {
        meta.collectorOnline = online->get<bool>();
    }
    return meta;
}

std::string errorText(const http_result& result) {
    if (!result.error.empty()) {
        return result.error;
    }
    return "http status " + std::to_string(result.status);
}

}  // namespace

const char* toString(link_state link) {
    switch (link) {
        case link_state::connected:
            return "connected";
        case link_state::unreachable:
            return "unreachable";
        case link_state::unknown:
        default:
            return "connecting";
    }
}

DashboardModel::DashboardModel(size_t historyLength)
    : _historyLength(historyLength)
    , _link(link_state::unknown)
    , _mqttConnected(false)
    , _offsetHistory(historyLength)
    , _temperatureHistory(historyLength)
    , _humidityHistory(historyLength)
    , _accelerationHistory(historyLength)
    , _polls(0)
    , _pollFailures(0) {
}

void DashboardModel::applyStatus(const http_result& result) {
    _polls += 1;
    if (result.status == HTTP_NOT_FOUND) {
        // the server is up but no collector has reported yet
        set_link(link_state::connected, "");
        _status.available = false;
        return;
    }
    if (result.status != HTTP_OK) {
        _pollFailures += 1;
        set_link(link_state::unreachable, errorText(result));
        return;
    }
    status_snapshot next;
    nlohmann::json document;
    std::string error;
    if (!StatusJsonCodec::decode(result.body, next.status, document, error)) {
        _pollFailures += 1;
        noteEvent(event_level::error, "status rejected: " + error);
        return;
    }
    set_link(link_state::connected, "");
    next.available = true;
    next.meta = parseMeta(document);
    // polling is faster than the collector publishes: only a new document extends the plots
    const bool fresh = !_status.available || next.status.timestamp != _status.status.timestamp;
    track(next);
    _status = std::move(next);
    if (!fresh) {
        return;
    }

    const system_status& status = _status.status;
    _offsetHistory.push(static_cast<float>(status.sync.windowMaxOffsetMs));
    for (const channel_stats& channel : status.channels) {
        auto found = _fpsHistory.find(channel.id);
        if (found == _fpsHistory.end()) {
            found = _fpsHistory.emplace(channel.id, MetricSeries(_historyLength)).first;
        }
        found->second.push(static_cast<float>(channel.fps));
    }
    if (status.sensor.valid) {
        _temperatureHistory.push(static_cast<float>(status.sensor.temperatureC));
        _humidityHistory.push(static_cast<float>(status.sensor.humidityPct));
        _accelerationHistory.push(static_cast<float>(status.sensor.accelerationG));
    }
}

void DashboardModel::applyHealth(const http_result& result) {
    if (result.status != HTTP_OK) {
        return;
    }
    const nlohmann::json body = nlohmann::json::parse(result.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return;
    }
    const bool connected = body.value("mqtt_connected", false);
    if (connected != _mqttConnected) {
        noteEvent(connected ? event_level::info : event_level::warn,
                  connected ? "server connected to the MQTT broker" : "server lost the MQTT broker");
    }
    _mqttConnected = connected;
}

void DashboardModel::applyPreview(preview_update update) {
    const auto now = std::chrono::steady_clock::now();
    if (_preview.version > 0) {
        const double seconds = std::chrono::duration<double>(now - _lastPreviewAt).count();
        if (seconds > 0.0) {
            // smoothed rate of new sets
            _preview.setsPerSecond =
                _preview.setsPerSecond * PREVIEW_RATE_SMOOTHING + (1.0 - PREVIEW_RATE_SMOOTHING) / seconds;
        }
    }
    _lastPreviewAt = now;
    if (_preview.link != link_state::connected) {
        noteEvent(event_level::info, "video preview connected");
    }
    _preview.link = link_state::connected;
    _preview.error.clear();
    _preview.tick = update.tick;
    _preview.offsetUs = update.offsetUs;
    _preview.verdict = std::move(update.verdict);
    _preview.frames = std::move(update.frames);
    _preview.version += 1;
}

void DashboardModel::applyPreviewUnavailable(bool reachable, const std::string& reason) {
    const link_state link = reachable ? link_state::connected : link_state::unreachable;
    if (link == link_state::unreachable && _preview.link != link_state::unreachable) {
        noteEvent(event_level::warn, "video preview unreachable: " + reason);
    }
    _preview.link = link;
    _preview.error = reason;
    _preview.setsPerSecond = 0.0;
    // never leave the last pictures on screen as if they were live
    if (!_preview.frames.empty()) {
        _preview.frames.clear();
        _preview.version += 1;
    }
}

void DashboardModel::noteControlRequested(control_action action) {
    control_entry entry;
    entry.time = now();
    entry.command = toString(action);
    entry.result = "sending";
    addControl(std::move(entry));
}

void DashboardModel::noteControlBusy(control_action action) {
    noteEvent(event_level::warn,
              std::string("control ") + toString(action) + " ignored: previous request still running");
}

void DashboardModel::applyControlSubmitted(control_action action, const http_result& result) {
    // requests go out in order: the oldest entry still "sending" for this command is the one answered
    control_entry* entry = nullptr;
    for (auto it = _controls.rbegin(); it != _controls.rend(); ++it) {
        if (it->result == "sending" && it->command == toString(action)) {
            entry = &*it;
            break;
        }
    }
    if (entry == nullptr) {
        control_entry fresh;
        fresh.time = now();
        fresh.command = toString(action);
        addControl(std::move(fresh));
        entry = &_controls.front();
    }
    const nlohmann::json body = nlohmann::json::parse(result.body, nullptr, false);
    if (result.status == HTTP_ACCEPTED && body.is_object()) {
        entry->requestId = body.value("request_id", std::string());
        entry->result = body.value("result", std::string("pending"));
        noteEvent(event_level::info, "control " + entry->command + " published request_id=" + entry->requestId);
        return;
    }
    entry->result = "failed";
    entry->reason = body.is_object() && body.contains("error") && body["error"].is_string()
                        ? body["error"].get<std::string>()
                        : errorText(result);
    noteEvent(event_level::error, "control " + entry->command + " failed: " + entry->reason);
}

void DashboardModel::applyControlRecord(const http_result& result) {
    if (result.status != HTTP_OK) {
        return;
    }
    const nlohmann::json body = nlohmann::json::parse(result.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return;
    }
    control_entry* entry = findControl(body.value("request_id", std::string()));
    if (entry == nullptr) {
        return;
    }
    const std::string answer = body.value("result", std::string("pending"));
    if (answer == entry->result) {
        return;
    }
    entry->result = answer;
    entry->reason = body.value("reason", std::string());
    if (answer == "accepted") {
        noteEvent(event_level::info, "control " + entry->command + " accepted by the collector");
    } else if (answer == "rejected") {
        noteEvent(event_level::warn, "control " + entry->command + " rejected: " + entry->reason);
    }
}

void DashboardModel::noteEvent(event_level level, const std::string& text) {
    _events.push_front(event_entry{now(), level, text});
    while (_events.size() > MAX_EVENTS) {
        _events.pop_back();
    }
    if (level == event_level::error) {
        LOG_WARN(app, stat, "dashboard event level=error text={}", text);
    } else {
        LOG_INFO(app, stat, "dashboard event text={}", text);
    }
}

void DashboardModel::set_link(link_state link, const std::string& error) {
    if (link != _link) {
        if (link == link_state::connected) {
            noteEvent(event_level::info, "server reachable");
        } else if (link == link_state::unreachable) {
            noteEvent(event_level::error, "server unreachable: " + error);
        }
    }
    _link = link;
    _serverError = error;
    if (link == link_state::unreachable) {
        _mqttConnected = false;
    }
}

void DashboardModel::track(const status_snapshot& next) {
    const system_status& after = next.status;
    if (!_status.available) {
        noteEvent(event_level::info, "first status from " + after.deviceId);
        return;
    }
    const system_status& before = _status.status;
    if (before.acquisition.phase != after.acquisition.phase) {
        const event_level level =
            after.acquisition.phase == acquisition_phase::error ? event_level::error : event_level::info;
        std::string text = std::string("acquisition ") + toString(before.acquisition.phase) + " -> " +
                           toString(after.acquisition.phase);
        if (!after.acquisition.reason.empty()) {
            text += " (" + after.acquisition.reason + ")";
        }
        noteEvent(level, text);
    }
    if (before.sync.locked != after.sync.locked && after.acquisition.phase == acquisition_phase::running) {
        noteEvent(after.sync.locked ? event_level::info : event_level::warn,
                  after.sync.locked ? "sync locked" : "sync lost: violations or incomplete sets in the last 10 s");
    }
    if (_status.meta.collectorOnline != next.meta.collectorOnline && next.meta.collectorOnline.has_value()) {
        noteEvent(*next.meta.collectorOnline ? event_level::info : event_level::warn,
                  *next.meta.collectorOnline ? "collector online" : "collector offline");
    }
    if (!_status.meta.stale && next.meta.stale) {
        noteEvent(event_level::warn, "status is stale: no report for over 60 s");
    }
}

control_entry* DashboardModel::findControl(const std::string& requestId) {
    if (requestId.empty()) {
        return nullptr;
    }
    for (control_entry& entry : _controls) {
        if (entry.requestId == requestId) {
            return &entry;
        }
    }
    return nullptr;
}

void DashboardModel::addControl(control_entry entry) {
    _controls.push_front(std::move(entry));
    while (_controls.size() > MAX_CONTROLS) {
        _controls.pop_back();
    }
}

link_state DashboardModel::get_server_link() const {
    return _link;
}

const std::string& DashboardModel::get_server_error() const {
    return _serverError;
}

bool DashboardModel::isMqttConnected() const {
    return _mqttConnected;
}

const status_snapshot& DashboardModel::get_status() const {
    return _status;
}

const preview_state& DashboardModel::get_preview() const {
    return _preview;
}

bool DashboardModel::canStart() const {
    if (_link != link_state::connected || !_mqttConnected) {
        return false;
    }
    if (!_status.available) {
        return true;
    }
    const acquisition_phase phase = _status.status.acquisition.phase;
    return phase == acquisition_phase::idle || phase == acquisition_phase::error;
}

bool DashboardModel::canStop() const {
    return _link == link_state::connected && _mqttConnected && _status.available &&
           _status.status.acquisition.phase == acquisition_phase::running;
}

const std::deque<control_entry>& DashboardModel::get_controls() const {
    return _controls;
}

const std::deque<event_entry>& DashboardModel::get_events() const {
    return _events;
}

const MetricSeries& DashboardModel::get_offset_history() const {
    return _offsetHistory;
}

const MetricSeries* DashboardModel::findFpsHistory(int channel) const {
    const auto found = _fpsHistory.find(channel);
    return found == _fpsHistory.end() ? nullptr : &found->second;
}

const MetricSeries& DashboardModel::get_temperature_history() const {
    return _temperatureHistory;
}

const MetricSeries& DashboardModel::get_humidity_history() const {
    return _humidityHistory;
}

const MetricSeries& DashboardModel::get_acceleration_history() const {
    return _accelerationHistory;
}

uint64_t DashboardModel::get_polls() const {
    return _polls;
}

uint64_t DashboardModel::get_poll_failures() const {
    return _pollFailures;
}
