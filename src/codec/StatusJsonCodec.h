#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "model/SystemStatus.h"

/**
 * @brief the status document (HTTP POST /status, MQTT video/status) to and from JSON
 *
 * decode() is the P3 gate on the server: size ceiling, schema version, field types. unknown
 * fields are ignored so the collector may add fields without breaking older servers
 */
class StatusJsonCodec {
public:
    static nlohmann::json toJson(const system_status& status);
    static std::string encode(const system_status& status);
    static bool decode(const std::string& text, system_status& status, nlohmann::json& document, std::string& error);

    static constexpr size_t MAX_DOCUMENT_BYTES = 1 << 20;
    static constexpr size_t MAX_FILES = 64;
};
