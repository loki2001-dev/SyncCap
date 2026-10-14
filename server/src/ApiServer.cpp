#include "ApiServer.h"

#include <utility>

#include "codec/ControlCommandCodec.h"
#include "codec/StatusJsonCodec.h"
#include "util/log/Logger.h"

namespace {

constexpr int HTTP_OK = 200;
constexpr int HTTP_ACCEPTED = 202;
constexpr int HTTP_NO_CONTENT = 204;
constexpr int HTTP_FOUND = 302;
constexpr int HTTP_BAD_REQUEST = 400;
constexpr int HTTP_NOT_FOUND = 404;
constexpr int HTTP_INTERNAL_ERROR = 500;
constexpr int HTTP_UNAVAILABLE = 503;
constexpr const char* JSON_TYPE = "application/json";

// Swagger UI from a pinned CDN version; the spec itself is served by us
constexpr const char* SWAGGER_PAGE = R"(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SyncCap API</title>
<link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui.css">
</head>
<body>
<div id="swagger"></div>
<script src="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui-bundle.js"></script>
<script>window.ui = SwaggerUIBundle({url: "/openapi.yaml", dom_id: "#swagger"});</script>
</body>
</html>
)";

}  // namespace

ApiServer::ApiServer(UvLoop& loop, StatusRepository& repository, ControlRelay& control, const IMqttClient& mqtt,
                     server_config config, std::string openapiDocument)
    : _loop(loop)
    , _repository(repository)
    , _control(control)
    , _mqtt(mqtt)
    , _config(std::move(config))
    , _openapiDocument(std::move(openapiDocument))
    , _requestTimeout(_config.requestTimeoutMs) {
    setupRoutes();
}

bool ApiServer::listen() {
    LOG_INFO(server, life, "listening bind={} port={}", _config.bind, _config.port);
    if (!_server.listen(_config.bind, _config.port)) {
        LOG_ERROR(server, net, "listen failed bind={} port={}", _config.bind, _config.port);
        return false;
    }
    return true;
}

int ApiServer::TDD_bindToAnyPort() {
    return _server.bind_to_any_port(_config.bind);
}

bool ApiServer::TDD_listenAfterBind() {
    return _server.listen_after_bind();
}

void ApiServer::stop() {
    _server.stop();
}

void ApiServer::reply(httplib::Response& response, int status, const nlohmann::json& body) {
    response.status = status;
    response.set_content(body.dump(), JSON_TYPE);
}

void ApiServer::replyError(httplib::Response& response, int status, const std::string& error) {
    reply(response, status, nlohmann::json{{"error", error}});
}

void ApiServer::setupRoutes() {
    _server.set_payload_max_length(MAX_BODY_BYTES);
    _server.set_exception_handler(
        [](const httplib::Request& request, httplib::Response& response, std::exception_ptr exception) {
            std::string message = "unknown";
            try {
                if (exception) {
                    std::rethrow_exception(exception);
                }
            } catch (const std::exception& e) {
                message = e.what();
            } catch (...) {
                message = "unknown exception";
            }
            LOG_ERROR(server, sys, "handler threw method={} path={} message={}", request.method, request.path, message);
            replyError(response, HTTP_INTERNAL_ERROR, "internal_error");
        });
    _server.set_logger([](const httplib::Request& request, const httplib::Response& response) {
        LOG_DEBUG(server, rx, "request method={} path={} status={} remote={}", request.method, request.path,
                  response.status, request.remote_addr);
    });

    _server.Post("/status", [this](const httplib::Request& request, httplib::Response& response) {
        handlePostStatus(request, response);
    });
    _server.Get("/status", [this](const httplib::Request& request, httplib::Response& response) {
        handleGetStatus(request, response);
    });
    _server.Get("/status/history", [this](const httplib::Request& request, httplib::Response& response) {
        handleGetHistory(request, response);
    });
    _server.Post("/control", [this](const httplib::Request& request, httplib::Response& response) {
        handlePostControl(request, response);
    });
    _server.Get(R"(/control/([A-Za-z0-9._-]{1,64}))",
                [this](const httplib::Request& request, httplib::Response& response) {
                    handleGetControl(request, response);
                });
    _server.Get("/health", [this](const httplib::Request& request, httplib::Response& response) {
        handleHealth(request, response);
    });
    _server.Get("/openapi.yaml", [this](const httplib::Request&, httplib::Response& response) {
        if (_openapiDocument.empty()) {
            replyError(response, HTTP_NOT_FOUND, "openapi document not loaded");
            return;
        }
        response.set_content(_openapiDocument, "application/yaml");
    });
    _server.Get("/docs", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(SWAGGER_PAGE, "text/html; charset=utf-8");
    });
    _server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        response.status = HTTP_FOUND;
        response.set_header("Location", "/docs");
    });
}

void ApiServer::handlePostStatus(const httplib::Request& request, httplib::Response& response) {
    // validation runs here on the HTTP thread; only the store goes to the loop
    system_status status;
    nlohmann::json document;
    std::string error;
    if (!StatusJsonCodec::decode(request.body, status, document, error)) {
        LOG_WARN(server, prot, "status rejected reason={} remote={} bytes={}", error, request.remote_addr,
                 request.body.size());
        replyError(response, HTTP_BAD_REQUEST, error);
        return;
    }
    StatusRepository* repository = &_repository;
    const auto stored = callOnLoop<bool>([repository, document = std::move(document), status]() mutable {
        repository->store(std::move(document), status, status_origin::http);
        return true;
    });
    if (!stored) {
        replyError(response, HTTP_UNAVAILABLE, "service_busy");
        return;
    }
    LOG_INFO(server, rx, "status stored via=http device={} channels={} locked={} max_offset_ms={:.3f}", status.deviceId,
             status.channels.size(), status.sync.locked, status.sync.windowMaxOffsetMs);
    response.status = HTTP_NO_CONTENT;
}

void ApiServer::handleGetStatus(const httplib::Request& /*request*/, httplib::Response& response) {
    StatusRepository* repository = &_repository;
    const auto latest = callOnLoop<std::optional<nlohmann::json>>([repository]() {
        return repository->buildLatest();
    });
    if (!latest) {
        replyError(response, HTTP_UNAVAILABLE, "service_busy");
        return;
    }
    if (!*latest) {
        replyError(response, HTTP_NOT_FOUND, "no status received yet");
        return;
    }
    reply(response, HTTP_OK, **latest);
}

void ApiServer::handleGetHistory(const httplib::Request& request, httplib::Response& response) {
    size_t limit = HISTORY_DEFAULT;
    if (request.has_param("limit")) {
        const std::string text = request.get_param_value("limit");
        try {
            size_t used = 0;
            const long long value = std::stoll(text, &used);
            if (used != text.size() || value < 1 || value > static_cast<long long>(HISTORY_MAX)) {
                throw std::out_of_range("limit");
            }
            limit = static_cast<size_t>(value);
        } catch (const std::exception&) {
            replyError(response, HTTP_BAD_REQUEST,
                       "limit must be an integer in [1," + std::to_string(HISTORY_MAX) + "]");
            return;
        }
    }
    StatusRepository* repository = &_repository;
    const auto history = callOnLoop<nlohmann::json>([repository, limit]() {
        return repository->buildHistory(limit);
    });
    if (!history) {
        replyError(response, HTTP_UNAVAILABLE, "service_busy");
        return;
    }
    reply(response, HTTP_OK, *history);
}

void ApiServer::handlePostControl(const httplib::Request& request, httplib::Response& response) {
    const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        replyError(response, HTTP_BAD_REQUEST, "body must be a JSON object like {\"command\":\"start\"}");
        return;
    }
    const auto command = body.find("command");
    control_action action = control_action::start;
    if (command == body.end() || !command->is_string() ||
        !ControlCommandCodec::parseAction(command->get<std::string>(), action)) {
        replyError(response, HTTP_BAD_REQUEST, "command must be \"start\" or \"stop\"");
        return;
    }
    struct submit_reply {
        submit_outcome outcome;
        control_record record;
    };
    ControlRelay* control = &_control;
    const auto submitted = callOnLoop<submit_reply>([control, action]() {
        submit_reply result{};
        result.outcome = control->submit(action, result.record);
        return result;
    });
    if (!submitted) {
        replyError(response, HTTP_UNAVAILABLE, "service_busy");
        return;
    }
    switch (submitted->outcome) {
        case submit_outcome::published:
            reply(response, HTTP_ACCEPTED, ControlRelay::toJson(submitted->record));
            return;
        case submit_outcome::mqtt_disconnected:
            replyError(response, HTTP_UNAVAILABLE, "mqtt_disconnected");
            return;
        case submit_outcome::publish_failed:
        default:
            replyError(response, HTTP_UNAVAILABLE, "mqtt_publish_failed");
            return;
    }
}

void ApiServer::handleGetControl(const httplib::Request& request, httplib::Response& response) {
    const std::string requestId = request.matches[1];
    ControlRelay* control = &_control;
    const auto found = callOnLoop<std::optional<control_record>>([control, requestId]() {
        return control->find(requestId);
    });
    if (!found) {
        replyError(response, HTTP_UNAVAILABLE, "service_busy");
        return;
    }
    if (!*found) {
        replyError(response, HTTP_NOT_FOUND, "unknown request_id");
        return;
    }
    reply(response, HTTP_OK, ControlRelay::toJson(**found));
}

void ApiServer::handleHealth(const httplib::Request& /*request*/, httplib::Response& response) {
    StatusRepository* repository = &_repository;
    const auto age = callOnLoop<std::optional<double>>([repository]() {
        return repository->get_latest_age_s();
    });
    nlohmann::json body{{"status", age ? "ok" : "degraded"}, {"mqtt_connected", _mqtt.isConnected()}};
    body["last_status_age_s"] = (age && *age) ? nlohmann::json(**age) : nlohmann::json(nullptr);
    reply(response, HTTP_OK, body);
}
