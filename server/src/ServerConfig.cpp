#include "ServerConfig.h"

#include <cstdlib>

#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

constexpr int PORT_MAX = 65535;
constexpr int STALE_MIN_S = 30;
constexpr int STALE_MAX_S = 3600;
constexpr int REQUEST_TIMEOUT_MIN_MS = 100;
constexpr int REQUEST_TIMEOUT_MAX_MS = 30000;

std::string readEnvironment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);  // NOLINT(cppcoreguidelines-no-malloc) _dupenv_s allocates with malloc
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

bool parsePort(const std::string& text, int& port) {
    try {
        size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size() || value < 1 || value > PORT_MAX) {
            return false;
        }
        port = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void readString(const json& object, const char* key, std::string& value, std::vector<std::string>& errors) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return;
    }
    if (!it->is_string()) {
        errors.push_back(std::string("wrong type key=") + key + " expected=string");
        return;
    }
    value = it->get<std::string>();
}

void readInt(const json& object, const char* key, int& value, int minValue, int maxValue,
             std::vector<std::string>& errors) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return;
    }
    if (!it->is_number_integer() || it->get<long long>() < minValue || it->get<long long>() > maxValue) {
        errors.push_back(std::string("out of range key=") + key + " range=[" + std::to_string(minValue) + "," +
                         std::to_string(maxValue) + "]");
        return;
    }
    value = it->get<int>();
}

}  // namespace

bool ServerConfig::load(const std::string& path, server_config& config, std::vector<std::string>& errors,
                        std::vector<std::string>& warnings) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        warnings.push_back("config not found path=" + path + " using defaults");
        return true;
    }
    std::ostringstream text;
    text << file.rdbuf();
    const json root = json::parse(text.str(), nullptr, false, true);
    if (root.is_discarded() || !root.is_object()) {
        errors.push_back("config is not a JSON object path=" + path);
        return false;
    }
    const std::set<std::string> known{"bind",         "port", "mqtt", "stale_after_s", "request_timeout_ms",
                                      "openapi_path", "log"};
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (known.count(it.key()) == 0) {
            warnings.push_back("unknown key=" + it.key() + " (typo?) ignored");
        }
    }
    readString(root, "bind", config.bind, errors);
    readInt(root, "port", config.port, 1, PORT_MAX, errors);
    readInt(root, "stale_after_s", config.staleAfterS, STALE_MIN_S, STALE_MAX_S, errors);
    readInt(root, "request_timeout_ms", config.requestTimeoutMs, REQUEST_TIMEOUT_MIN_MS, REQUEST_TIMEOUT_MAX_MS,
            errors);
    readString(root, "openapi_path", config.openapiPath, errors);
    const auto mqtt = root.find("mqtt");
    if (mqtt != root.end() && mqtt->is_object()) {
        readString(*mqtt, "host", config.mqttHost, errors);
        readInt(*mqtt, "port", config.mqttPort, 1, PORT_MAX, errors);
        readString(*mqtt, "client_id", config.mqttClientId, errors);
        readString(*mqtt, "status_topic", config.statusTopic, errors);
        readString(*mqtt, "control_topic", config.controlTopic, errors);
        readString(*mqtt, "ack_topic", config.ackTopic, errors);
        readString(*mqtt, "online_topic", config.onlineTopic, errors);
    }
    const auto log = root.find("log");
    if (log != root.end() && log->is_object()) {
        readString(*log, "dir", config.logDirectory, errors);
        readString(*log, "level", config.logLevel, errors);
    }
    return errors.empty();
}

void ServerConfig::applyEnvironment(server_config& config, std::vector<std::string>& warnings) {
    const std::string bind = readEnvironment("SYNCCAP_SERVER_BIND");
    if (!bind.empty()) {
        config.bind = bind;
    }
    const std::string port = readEnvironment("SYNCCAP_SERVER_PORT");
    if (!port.empty() && !parsePort(port, config.port)) {
        warnings.push_back("invalid SYNCCAP_SERVER_PORT=" + port + " using=" + std::to_string(config.port));
    }
    const std::string mqttHost = readEnvironment("SYNCCAP_MQTT_HOST");
    if (!mqttHost.empty()) {
        config.mqttHost = mqttHost;
    }
    const std::string mqttPort = readEnvironment("SYNCCAP_MQTT_PORT");
    if (!mqttPort.empty() && !parsePort(mqttPort, config.mqttPort)) {
        warnings.push_back("invalid SYNCCAP_MQTT_PORT=" + mqttPort + " using=" + std::to_string(config.mqttPort));
    }
    const std::string openapi = readEnvironment("SYNCCAP_OPENAPI_PATH");
    if (!openapi.empty()) {
        config.openapiPath = openapi;
    }
    const std::string logDirectory = readEnvironment("SYNCCAP_LOG_DIR");
    if (!logDirectory.empty()) {
        config.logDirectory = logDirectory;
    }
}

std::string ServerConfig::summarize(const server_config& config) {
    return "bind=" + config.bind + " port=" + std::to_string(config.port) + " mqtt=" + config.mqttHost + ":" +
           std::to_string(config.mqttPort) + " client_id=" + config.mqttClientId +
           " stale_after_s=" + std::to_string(config.staleAfterS) +
           " request_timeout_ms=" + std::to_string(config.requestTimeoutMs) + " openapi=" + config.openapiPath;
}
