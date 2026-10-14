#include "ControlRelay.h"

#include <cstdio>

#include <array>
#include <chrono>
#include <utility>

#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr int QOS_CONTROL = 1;
constexpr size_t REQUEST_ID_SIZE = 40;

const char* toString(control_state state) {
    switch (state) {
        case control_state::pending:
            return "pending";
        case control_state::accepted:
            return "accepted";
        case control_state::rejected:
            return "rejected";
        default:
            return "unknown";
    }
}

}  // namespace

ControlRelay::ControlRelay(IMqttClient& client, std::string controlTopic, size_t capacity)
    : _client(client)
    , _controlTopic(std::move(controlTopic))
    , _capacity(capacity == 0 ? 1 : capacity)
    , _random(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()))
    , _sequence(0) {
}

std::string ControlRelay::newRequestId() {
    _sequence += 1;
    std::array<char, REQUEST_ID_SIZE> text{};
    std::snprintf(text.data(), text.size(), "req-%06llx-%08llx", static_cast<unsigned long long>(_sequence),
                  static_cast<unsigned long long>(_random() & 0xFFFFFFFFULL));
    return text.data();
}

submit_outcome ControlRelay::submit(control_action action, control_record& record) {
    if (!_client.isConnected()) {
        LOG_WARN(server, cmd, "control rejected reason=mqtt_disconnected command={}", toString(action));
        return submit_outcome::mqtt_disconnected;
    }
    record = control_record{};
    record.requestId = newRequestId();
    record.command = toString(action);
    record.createdAt = TimeUtil::formatIso8601Utc(std::chrono::system_clock::now());
    const std::string payload = ControlCommandCodec::encode(control_request{action, record.requestId});
    if (!_client.publish(_controlTopic, payload, QOS_CONTROL, false)) {
        LOG_WARN(server, cmd, "control publish failed command={} request_id={}", record.command, record.requestId);
        return submit_outcome::publish_failed;
    }
    _records.push_back(record);
    while (_records.size() > _capacity) {
        LOG_INFO(server, cmd, "control record evicted request_id={} state={}", _records.front().requestId,
                 toString(_records.front().state));
        _records.pop_front();
    }
    LOG_INFO(server, cmd, "control published topic={} command={} request_id={}", _controlTopic, record.command,
             record.requestId);
    return submit_outcome::published;
}

void ControlRelay::handleAck(const control_ack& ack) {
    for (control_record& record : _records) {
        if (record.requestId != ack.requestId) {
            continue;
        }
        record.state = ack.accepted ? control_state::accepted : control_state::rejected;
        record.reason = ack.reason;
        record.answeredAt = TimeUtil::formatIso8601Utc(std::chrono::system_clock::now());
        LOG_INFO(server, cmd, "control answered request_id={} result={} reason={}", ack.requestId,
                 toString(record.state), ack.reason);
        return;
    }
    LOG_INFO(server, cmd, "ack for unknown request request_id={} result={} reason={}", ack.requestId,
             ack.accepted ? "accepted" : "rejected", ack.reason);
}

std::optional<control_record> ControlRelay::find(const std::string& requestId) const {
    for (const control_record& record : _records) {
        if (record.requestId == requestId) {
            return record;
        }
    }
    return std::nullopt;
}

nlohmann::json ControlRelay::toJson(const control_record& record) {
    return nlohmann::json{{"request_id", record.requestId},   {"command", record.command},
                          {"result", toString(record.state)}, {"reason", record.reason},
                          {"created_at", record.createdAt},   {"answered_at", record.answeredAt}};
}
