#include "StatusRepository.h"

#include <utility>

#include "util/TimeUtil.h"

namespace {

constexpr double NANOS_PER_SECOND = 1e9;

double ageSeconds(std::chrono::steady_clock::time_point since) {
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count()) /
           NANOS_PER_SECOND;
}

const char* toString(status_origin origin) {
    return origin == status_origin::http ? "http" : "mqtt";
}

}  // namespace

StatusRepository::StatusRepository(size_t historyCapacity, std::chrono::seconds staleAfter)
    : _historyCapacity(historyCapacity == 0 ? 1 : historyCapacity)
    , _staleAfter(staleAfter)
    , _receivedHttp(0)
    , _receivedMqtt(0) {
}

void StatusRepository::store(nlohmann::json document, const system_status& status, status_origin origin) {
    stored_status entry;
    entry.document = std::move(document);
    entry.status = status;
    entry.origin = origin;
    entry.receivedAt = std::chrono::steady_clock::now();
    entry.receivedAtText = TimeUtil::formatIso8601Utc(std::chrono::system_clock::now());
    _history.push_back(std::move(entry));
    while (_history.size() > _historyCapacity) {
        _history.pop_front();
    }
    if (origin == status_origin::http) {
        _receivedHttp += 1;
    } else {
        _receivedMqtt += 1;
    }
}

void StatusRepository::set_collector_online(bool online) {
    _collectorOnline = online;
}

std::optional<nlohmann::json> StatusRepository::buildLatest() const {
    if (_history.empty()) {
        return std::nullopt;
    }
    const stored_status& latest = _history.back();
    nlohmann::json body = latest.document;
    const double age = ageSeconds(latest.receivedAt);
    nlohmann::json meta{{"received_at", latest.receivedAtText},
                        {"received_via", toString(latest.origin)},
                        {"age_s", age},
                        {"stale", age > static_cast<double>(_staleAfter.count())},
                        {"received_http", _receivedHttp},
                        {"received_mqtt", _receivedMqtt}};
    meta["collector_online"] = _collectorOnline ? nlohmann::json(*_collectorOnline) : nlohmann::json(nullptr);
    body["server_meta"] = meta;
    return body;
}

nlohmann::json StatusRepository::buildHistory(size_t limit) const {
    nlohmann::json items = nlohmann::json::array();
    const size_t count = std::min(limit, _history.size());
    for (size_t index = _history.size() - count; index < _history.size(); ++index) {
        const stored_status& entry = _history[index];
        items.push_back({{"received_at", entry.receivedAtText},
                         {"received_via", toString(entry.origin)},
                         {"status", entry.document}});
    }
    return nlohmann::json{{"count", items.size()}, {"items", items}};
}

std::optional<double> StatusRepository::get_latest_age_s() const {
    if (_history.empty()) {
        return std::nullopt;
    }
    return ageSeconds(_history.back().receivedAt);
}

uint64_t StatusRepository::get_received_total() const {
    return _receivedHttp + _receivedMqtt;
}
