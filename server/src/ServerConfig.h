#pragma once

#include <string>
#include <vector>

struct server_config {
    std::string bind = "127.0.0.1";
    int port = 8080;
    std::string mqttHost = "127.0.0.1";
    int mqttPort = 1883;
    std::string mqttClientId = "synccap-server";
    std::string statusTopic = "video/status";
    std::string controlTopic = "video/control";
    std::string ackTopic = "video/control/ack";
    std::string onlineTopic = "video/online";
    int staleAfterS = 60;  // 2 x the longest report interval
    int requestTimeoutMs = 2000;
    std::string openapiPath = "server/openapi.yaml";
    std::string logDirectory = "logs-server";
    std::string logLevel = "info";
};

/**
 * @brief config/server.json plus environment overrides for containers
 *
 * SYNCCAP_SERVER_BIND, SYNCCAP_SERVER_PORT, SYNCCAP_MQTT_HOST, SYNCCAP_MQTT_PORT, SYNCCAP_OPENAPI_PATH, SYNCCAP_LOG_DIR
 */
class ServerConfig {
public:
    static bool load(const std::string& path, server_config& config, std::vector<std::string>& errors,
                     std::vector<std::string>& warnings);
    static void applyEnvironment(server_config& config, std::vector<std::string>& warnings);
    static std::string summarize(const server_config& config);
};
