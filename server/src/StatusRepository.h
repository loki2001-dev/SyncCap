#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "model/SystemStatus.h"

enum class status_origin {
    http,
    mqtt
};

struct stored_status {
    nlohmann::json document;
    system_status status;
    status_origin origin = status_origin::http;
    std::chrono::steady_clock::time_point receivedAt;
    std::string receivedAtText;
};

/**
 * @brief latest status and a bounded history. service loop thread only, so no lock
 *
 * the latest status comes from whichever channel delivered last (HTTP every 20-30 s, MQTT every few seconds)
 */
class StatusRepository {
public:
    StatusRepository(size_t historyCapacity, std::chrono::seconds staleAfter);

    void store(nlohmann::json document, const system_status& status, status_origin origin);
    void set_collector_online(bool online);

    // GET /status body; nullopt when nothing was received yet
    std::optional<nlohmann::json> buildLatest() const;
    nlohmann::json buildHistory(size_t limit) const;
    std::optional<double> get_latest_age_s() const;
    uint64_t get_received_total() const;

private:
    size_t _historyCapacity;
    std::chrono::seconds _staleAfter;
    std::deque<stored_status> _history;
    std::optional<bool> _collectorOnline;
    uint64_t _receivedHttp;
    uint64_t _receivedMqtt;
};
