#include <csignal>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ApiServer.h"
#include "ControlRelay.h"
#include "ServerConfig.h"
#include "StatusRepository.h"
#include "app/BuildInfo.h"
#include "codec/ControlCommandCodec.h"
#include "codec/StatusJsonCodec.h"
#include "core/UvLoop.h"
#include "core/UvSignal.h"
#include "core/UvTimer.h"
#include "network/PahoMqttClient.h"
#include "util/log/Logger.h"

namespace {

constexpr int EXIT_CONFIG = 2;
constexpr int EXIT_LISTEN = 3;
constexpr size_t HISTORY_CAPACITY = 100;
constexpr size_t CONTROL_CAPACITY = 64;
constexpr int QOS_STATUS = 0;
constexpr int QOS_CONTROL = 1;
constexpr int RECONNECT_MIN_S = 1;
constexpr int RECONNECT_MAX_S = 30;
constexpr uint64_t MILLIS_PER_SECOND = 1000;

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// the MQTT side of the server: everything runs on the service loop
class BrokerLink {
public:
    BrokerLink(UvLoop& loop, PahoMqttClient& client, StatusRepository& repository, ControlRelay& control,
               const server_config& config)
        : _loop(loop)
        , _client(client)
        , _repository(repository)
        , _control(control)
        , _config(config)
        , _retryTimer(loop, "mqtt_retry")
        , _retryDelayS(RECONNECT_MIN_S) {
    }

    void start() {
        mqtt_callbacks callbacks;
        callbacks.onConnected = [this]() {
            _loop.post([this]() {
                handleConnected();
            });
        };
        callbacks.onConnectionLost = [this](const std::string& cause) {
            _loop.post([cause]() {
                LOG_WARN(server, net, "mqtt connection lost cause={} auto_reconnect=on", cause);
            });
        };
        callbacks.onConnectFailed = [this](const std::string& reason) {
            _loop.post([this, reason]() {
                LOG_WARN(server, net, "mqtt connect failed reason={} retry_in_s={}", reason, _retryDelayS);
                scheduleRetry();
            });
        };
        callbacks.onMessage = [this](const mqtt_message& message) {
            if (!_loop.post([this, message]() {
                    handleMessage(message);
                })) {
                LOG_WARN(server, rx, "mqtt message dropped reason=loop_queue_full topic={}", message.topic);
            }
        };
        _client.set_callbacks(std::move(callbacks));
        if (!_client.connect()) {
            scheduleRetry();
        }
    }

    void stop() {
        _retryTimer.stop();
        _client.set_callbacks(mqtt_callbacks{});
        _client.disconnect();
    }

private:
    void scheduleRetry() {
        _retryTimer.start(static_cast<uint64_t>(_retryDelayS) * MILLIS_PER_SECOND, 0, [this]() {
            if (!_client.connect()) {
                scheduleRetry();
            }
        });
        _retryDelayS = std::min(_retryDelayS * 2, RECONNECT_MAX_S);
    }

    void handleConnected() {
        _retryDelayS = RECONNECT_MIN_S;
        LOG_INFO(server, net, "mqtt connected host={} port={}", _config.mqttHost, _config.mqttPort);
        _client.subscribe(_config.statusTopic, QOS_STATUS);
        _client.subscribe(_config.ackTopic, QOS_CONTROL);
        _client.subscribe(_config.onlineTopic, QOS_CONTROL);
    }

    void handleMessage(const mqtt_message& message) {
        if (message.topic == _config.statusTopic) {
            system_status status;
            nlohmann::json document;
            std::string error;
            if (!StatusJsonCodec::decode(message.payload, status, document, error)) {
                LOG_WARN(server, prot, "mqtt status rejected reason={} bytes={}", error, message.payload.size());
                return;
            }
            _repository.store(std::move(document), status, status_origin::mqtt);
            return;
        }
        if (message.topic == _config.ackTopic) {
            control_ack ack;
            std::string reason;
            if (!ControlCommandCodec::decodeAck(message.payload, ack, reason)) {
                LOG_WARN(server, prot, "ack rejected reason={} bytes={}", reason, message.payload.size());
                return;
            }
            _control.handleAck(ack);
            return;
        }
        if (message.topic == _config.onlineTopic) {
            const bool online = message.payload == "online";
            LOG_INFO(server, rx, "collector presence online={}", online);
            _repository.set_collector_online(online);
            return;
        }
        LOG_WARN(server, rx, "mqtt message ignored reason=unexpected_topic topic={}", message.topic);
    }

    UvLoop& _loop;
    PahoMqttClient& _client;
    StatusRepository& _repository;
    ControlRelay& _control;
    const server_config& _config;
    UvTimer _retryTimer;
    int _retryDelayS;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string configPath = argc > 1 ? argv[1] : "config/server.json";
    server_config config;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    const bool loaded = ServerConfig::load(configPath, config, errors, warnings);
    ServerConfig::applyEnvironment(config, warnings);

    log_options logOptions;
    logOptions.directory = config.logDirectory;
    logOptions.level = config.logLevel;
    if (!Logger::initialize(logOptions)) {
        return EXIT_CONFIG;
    }
    LOG_INFO(server, life, "starting version={} commit={} build={} config={}", BUILD_VERSION, BUILD_COMMIT, BUILD_TYPE,
             configPath);
    for (const std::string& warning : warnings) {
        LOG_WARN(server, cfg, "{}", warning);
    }
    if (!loaded) {
        for (const std::string& error : errors) {
            LOG_ERROR(server, cfg, "{}", error);
        }
        Logger::shutdown();
        return EXIT_CONFIG;
    }
    LOG_INFO(server, cfg, "{}", ServerConfig::summarize(config));

    const std::string openapi = readFile(config.openapiPath);
    if (openapi.empty()) {
        LOG_WARN(server, cfg, "openapi document missing path={} GET /openapi.yaml will answer 404", config.openapiPath);
    }

    int exitCode = 0;
    {
        UvLoop loop;
        StatusRepository repository(HISTORY_CAPACITY, std::chrono::seconds(config.staleAfterS));
        mqtt_options mqttOptions;
        mqttOptions.host = config.mqttHost;
        mqttOptions.port = config.mqttPort;
        mqttOptions.clientId = config.mqttClientId;
        PahoMqttClient mqtt(mqttOptions);
        ControlRelay control(mqtt, config.controlTopic, CONTROL_CAPACITY);
        BrokerLink broker(loop, mqtt, repository, control, config);
        ApiServer api(loop, repository, control, mqtt, config, openapi);

        UvSignal interrupt(loop, SIGINT);
#ifdef _WIN32
        UvSignal terminate(loop, SIGBREAK);
#else
        UvSignal terminate(loop, SIGTERM);
#endif
        auto shutdown = [&api, &loop](int signalNumber) {
            LOG_INFO(server, life, "shutdown requested signal={}", signalNumber);
            api.stop();
            loop.requestStop();
        };
        interrupt.start(shutdown);
        terminate.start(shutdown);

        broker.start();
        std::thread http([&api, &loop, &exitCode]() {
            if (!api.listen()) {
                loop.post([&exitCode, &loop]() {
                    exitCode = EXIT_LISTEN;
                    loop.requestStop();
                });
            }
        });
        loop.run();
        api.stop();
        http.join();
        broker.stop();
        LOG_INFO(server, life, "stopped received_total={}", repository.get_received_total());
    }
    Logger::shutdown();
    return exitCode;
}
