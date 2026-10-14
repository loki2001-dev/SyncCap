#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "codec/ControlCommandCodec.h"
#include "network/IMqttClient.h"

enum class control_state {
    pending,
    accepted,
    rejected
};

struct control_record {
    std::string requestId;
    std::string command;
    control_state state = control_state::pending;
    std::string reason;
    std::string createdAt;
    std::string answeredAt;
};

enum class submit_outcome {
    published,
    mqtt_disconnected,
    publish_failed
};

/**
 * @brief turns POST /control into a video/control message and tracks the answer from video/control/ack
 *
 * service loop thread only. the request table is bounded; the oldest entry goes first
 */
class ControlRelay {
public:
    ControlRelay(IMqttClient& client, std::string controlTopic, size_t capacity);

    submit_outcome submit(control_action action, control_record& record);
    void handleAck(const control_ack& ack);
    std::optional<control_record> find(const std::string& requestId) const;

    static nlohmann::json toJson(const control_record& record);

private:
    std::string newRequestId();

    IMqttClient& _client;
    std::string _controlTopic;
    size_t _capacity;
    std::deque<control_record> _records;
    std::mt19937_64 _random;
    uint64_t _sequence;
};
