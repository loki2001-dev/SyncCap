#pragma once

#include <cstddef>
#include <string>

#include "model/Messages.h"

struct control_request {
    control_action action = control_action::start;
    std::string requestId;
};

struct control_ack {
    std::string requestId;
    std::string command;
    bool accepted = false;
    std::string reason;
};

/**
 * @brief video/control and video/control/ack payloads
 *
 * accepts {"command":"start"|"stop","request_id":"..."} or the plain words start / stop.
 * rejection reasons are the strings sent back in the ack
 */
class ControlCommandCodec {
public:
    static bool decode(const std::string& payload, control_request& request, std::string& reason);
    static std::string encode(const control_request& request);
    static std::string encodeAck(const control_ack& ack);
    static bool decodeAck(const std::string& payload, control_ack& ack, std::string& reason);
    static bool parseAction(const std::string& text, control_action& action);

    static constexpr size_t MAX_PAYLOAD_BYTES = 1024;
    static constexpr size_t MAX_REQUEST_ID = 64;
};
