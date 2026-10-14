#include "net/ServerClient.h"

#include <algorithm>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "util/log/Logger.h"

namespace {

constexpr size_t REQUEST_QUEUE = 2;
constexpr int HTTP_OK = 200;
constexpr int HTTP_ACCEPTED = 202;
constexpr uint64_t DROP_LOG_EVERY = 50;
constexpr const char* JSON_TYPE = "application/json";

}  // namespace

ServerClient::ServerClient(std::string baseUrl, IHttpClient& http, std::chrono::milliseconds pollPeriod,
                           std::chrono::milliseconds timeout, BlockingQueue<model_update_t>& inbox)
    : _baseUrl(std::move(baseUrl))
    , _http(http)
    , _pollPeriod(pollPeriod)
    , _timeout(timeout)
    , _inbox(inbox)
    , _requests(REQUEST_QUEUE, overflow_policy::reject)
    , _running(false) {
    while (!_baseUrl.empty() && _baseUrl.back() == '/') {
        _baseUrl.pop_back();
    }
}

ServerClient::~ServerClient() {
    stop();
}

void ServerClient::start() {
    _running = true;
    _thread = std::thread([this]() {
        // an exception must not leave the thread: it would std::terminate the process
        try {
            run();
        } catch (const std::exception& e) {
            LOG_ERROR(http, sys, "dashboard client thread threw message={}", e.what());
        }
    });
}

void ServerClient::stop() {
    _running = false;
    _requests.close();
    if (_thread.joinable()) {
        _thread.join();
    }
}

bool ServerClient::requestControl(control_action action) {
    return _requests.push(action) == push_result::accepted;
}

void ServerClient::run() {
    auto nextPoll = std::chrono::steady_clock::now();
    while (_running) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPoll) {
            poll();
            nextPoll = std::max(nextPoll + _pollPeriod, std::chrono::steady_clock::now());
            continue;
        }
        control_action action = control_action::start;
        const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(nextPoll - now);
        if (_requests.popFor(action, wait)) {
            submit(action);
        }
    }
}

void ServerClient::poll() {
    http_result status = _http.get(_baseUrl + "/status", _timeout);
    publish([status](DashboardModel& model) {
        model.applyStatus(status);
    });
    if (status.status == 0) {
        // unreachable: skip the other calls, each would wait for the same timeout
        return;
    }
    http_result health = _http.get(_baseUrl + "/health", _timeout);
    publish([health](DashboardModel& model) {
        model.applyHealth(health);
    });

    const auto now = std::chrono::steady_clock::now();
    std::vector<pending_request> still;
    for (const pending_request& request : _pending) {
        http_result record = _http.get(_baseUrl + "/control/" + request.requestId, _timeout);
        const nlohmann::json body = nlohmann::json::parse(record.body, nullptr, false);
        const bool answered = record.status == HTTP_OK && body.is_object() && body.value("result", "") != "pending";
        publish([record](DashboardModel& model) {
            model.applyControlRecord(record);
        });
        if (!answered && now - request.since < PENDING_GIVE_UP) {
            still.push_back(request);
        } else if (!answered) {
            LOG_WARN(http, cmd, "control answer never arrived request_id={} waited_s={}", request.requestId,
                     PENDING_GIVE_UP.count());
        }
    }
    _pending.swap(still);
}

void ServerClient::submit(control_action action) {
    const std::string body = nlohmann::json{{"command", toString(action)}}.dump();
    http_result result = _http.post(_baseUrl + "/control", body, JSON_TYPE, _timeout);
    LOG_INFO(http, cmd, "control sent command={} status={} error={}", toString(action), result.status, result.error);
    if (result.status == HTTP_ACCEPTED) {
        const nlohmann::json record = nlohmann::json::parse(result.body, nullptr, false);
        if (record.is_object() && record.contains("request_id") && record["request_id"].is_string() &&
            _pending.size() < MAX_PENDING_REQUESTS) {
            _pending.push_back(
                pending_request{record["request_id"].get<std::string>(), std::chrono::steady_clock::now()});
        }
    }
    publish([action, result](DashboardModel& model) {
        model.applyControlSubmitted(action, result);
    });
}

void ServerClient::publish(model_update_t update) {
    if (_inbox.push(std::move(update)) != push_result::accepted) {
        const uint64_t dropped = _inbox.get_dropped_total();
        if (dropped == 1 || dropped % DROP_LOG_EVERY == 0) {
            LOG_WARN(http, sys, "dashboard update dropped reason=inbox_full dropped_total={}", dropped);
        }
    }
}
