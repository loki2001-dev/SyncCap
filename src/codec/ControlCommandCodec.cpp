#include "codec/ControlCommandCodec.h"

#include <cctype>

#include <algorithm>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool isValidRequestId(const std::string& id) {
    if (id.size() > ControlCommandCodec::MAX_REQUEST_ID) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '-' || character == '_' || character == '.';
    });
}

}  // namespace

bool ControlCommandCodec::parseAction(const std::string& text, control_action& action) {
    if (text == "start") {
        action = control_action::start;
        return true;
    }
    if (text == "stop") {
        action = control_action::stop;
        return true;
    }
    return false;
}

bool ControlCommandCodec::decode(const std::string& payload, control_request& request, std::string& reason) {
    if (payload.size() > MAX_PAYLOAD_BYTES) {
        reason = "rejected_payload_too_large";
        return false;
    }
    const std::string text = trim(payload);
    if (text.empty() || text.front() != '{') {
        // plain text form: start / stop
        if (!parseAction(text, request.action)) {
            reason = "rejected_unknown_command";
            return false;
        }
        request.requestId.clear();
        return true;
    }
    const json document = json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        reason = "rejected_invalid_json";
        return false;
    }
    const auto command = document.find("command");
    if (command == document.end() || !command->is_string() ||
        !parseAction(command->get<std::string>(), request.action)) {
        reason = "rejected_unknown_command";
        return false;
    }
    request.requestId.clear();
    const auto id = document.find("request_id");
    if (id != document.end()) {
        if (!id->is_string() || !isValidRequestId(id->get<std::string>())) {
            reason = "rejected_invalid_request_id";
            return false;
        }
        request.requestId = id->get<std::string>();
    }
    return true;
}

std::string ControlCommandCodec::encode(const control_request& request) {
    json document{{"command", toString(request.action)}};
    if (!request.requestId.empty()) {
        document["request_id"] = request.requestId;
    }
    return document.dump();
}

std::string ControlCommandCodec::encodeAck(const control_ack& ack) {
    return json{{"request_id", ack.requestId},
                {"command", ack.command},
                {"result", ack.accepted ? "accepted" : "rejected"},
                {"reason", ack.reason}}
        .dump();
}

bool ControlCommandCodec::decodeAck(const std::string& payload, control_ack& ack, std::string& reason) {
    if (payload.size() > MAX_PAYLOAD_BYTES) {
        reason = "payload_too_large";
        return false;
    }
    const json document = json::parse(payload, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        reason = "invalid_json";
        return false;
    }
    const auto id = document.find("request_id");
    const auto result = document.find("result");
    if (id == document.end() || !id->is_string() || result == document.end() || !result->is_string()) {
        reason = "missing_fields";
        return false;
    }
    const std::string resultText = result->get<std::string>();
    if (resultText != "accepted" && resultText != "rejected") {
        reason = "unknown_result";
        return false;
    }
    ack.requestId = id->get<std::string>();
    ack.accepted = resultText == "accepted";
    const auto command = document.find("command");
    ack.command = command != document.end() && command->is_string() ? command->get<std::string>() : std::string();
    const auto why = document.find("reason");
    ack.reason = why != document.end() && why->is_string() ? why->get<std::string>() : std::string();
    return true;
}
