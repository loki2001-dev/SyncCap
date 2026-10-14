#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

#include <httplib.h>

#include "ControlRelay.h"
#include "ServerConfig.h"
#include "StatusRepository.h"
#include "core/LoopCall.h"
#include "core/UvLoop.h"
#include "network/IMqttClient.h"

/**
 * @brief REST endpoints of the OpenAPI server (server/openapi.yaml)
 *
 * httplib runs handlers on its own threads; they only parse and validate. everything that touches the
 * repository or the control table is posted to the service loop and awaited through a future with a
 * deadline, so the service state needs no lock (P1). a full loop or a missed deadline answers 503
 */
class ApiServer {
public:
    ApiServer(UvLoop& loop, StatusRepository& repository, ControlRelay& control, const IMqttClient& mqtt,
              server_config config, std::string openapiDocument);

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // blocking until stop(). false: bind failed
    bool listen();
    // thread-safe
    void stop();
    // test helper: binds to an ephemeral port, returns it (0 on failure)
    int TDD_bindToAnyPort();
    bool TDD_listenAfterBind();

    static constexpr size_t MAX_BODY_BYTES = 1 << 20;
    static constexpr size_t HISTORY_DEFAULT = 20;
    static constexpr size_t HISTORY_MAX = 100;

private:
    template <typename Result>
    std::optional<Result> callOnLoop(std::function<Result()> task) {
        return ::callOnLoop<Result>(_loop, std::move(task), _requestTimeout);
    }

    void setupRoutes();
    void handlePostStatus(const httplib::Request& request, httplib::Response& response);
    void handleGetStatus(const httplib::Request& request, httplib::Response& response);
    void handleGetHistory(const httplib::Request& request, httplib::Response& response);
    void handlePostControl(const httplib::Request& request, httplib::Response& response);
    void handleGetControl(const httplib::Request& request, httplib::Response& response);
    void handleHealth(const httplib::Request& request, httplib::Response& response);

    static void reply(httplib::Response& response, int status, const nlohmann::json& body);
    static void replyError(httplib::Response& response, int status, const std::string& error);

    UvLoop& _loop;
    StatusRepository& _repository;
    ControlRelay& _control;
    const IMqttClient& _mqtt;
    server_config _config;
    std::string _openapiDocument;
    std::chrono::milliseconds _requestTimeout;
    httplib::Server _server;
};
