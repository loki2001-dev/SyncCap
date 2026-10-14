#include "catch_amalgamated.hpp"

#include <string>

#include "codec/ControlCommandCodec.h"

TEST_CASE("ControlCommandCodec decodes JSON commands") {
    control_request request;
    std::string reason;
    REQUIRE(ControlCommandCodec::decode(R"({"command":"stop","request_id":"req-1"})", request, reason));
    REQUIRE(request.action == control_action::stop);
    REQUIRE(request.requestId == "req-1");
    REQUIRE(ControlCommandCodec::decode(R"({"command":"start"})", request, reason));
    REQUIRE(request.action == control_action::start);
    REQUIRE(request.requestId.empty());
}

TEST_CASE("ControlCommandCodec decodes plain text commands") {
    control_request request;
    std::string reason;
    REQUIRE(ControlCommandCodec::decode("start", request, reason));
    REQUIRE(request.action == control_action::start);
    REQUIRE(ControlCommandCodec::decode("  stop\n", request, reason));
    REQUIRE(request.action == control_action::stop);
}

TEST_CASE("ControlCommandCodec rejects bad payloads with the ack reason") {
    control_request request;
    std::string reason;
    REQUIRE_FALSE(ControlCommandCodec::decode("reboot", request, reason));
    REQUIRE(reason == "rejected_unknown_command");
    REQUIRE_FALSE(ControlCommandCodec::decode(R"({"command":"START"})", request, reason));
    REQUIRE(reason == "rejected_unknown_command");
    REQUIRE_FALSE(ControlCommandCodec::decode(R"({"command":"start")", request, reason));
    REQUIRE(reason == "rejected_invalid_json");
    REQUIRE_FALSE(ControlCommandCodec::decode(R"({"command":"start","request_id":"a b"})", request, reason));
    REQUIRE(reason == "rejected_invalid_request_id");
    REQUIRE_FALSE(
        ControlCommandCodec::decode(std::string(ControlCommandCodec::MAX_PAYLOAD_BYTES + 1, 'x'), request, reason));
    REQUIRE(reason == "rejected_payload_too_large");
}

TEST_CASE("ControlCommandCodec accepts a request id at the length limit and rejects one past it") {
    control_request request;
    std::string reason;
    const std::string atLimit(ControlCommandCodec::MAX_REQUEST_ID, 'a');
    REQUIRE(ControlCommandCodec::decode(R"({"command":"start","request_id":")" + atLimit + "\"}", request, reason));
    const std::string pastLimit(ControlCommandCodec::MAX_REQUEST_ID + 1, 'a');
    REQUIRE_FALSE(
        ControlCommandCodec::decode(R"({"command":"start","request_id":")" + pastLimit + "\"}", request, reason));
}

TEST_CASE("ControlCommandCodec round-trips commands and acks") {
    control_request request{control_action::stop, "req-9"};
    control_request decoded;
    std::string reason;
    REQUIRE(ControlCommandCodec::decode(ControlCommandCodec::encode(request), decoded, reason));
    REQUIRE(decoded.action == control_action::stop);
    REQUIRE(decoded.requestId == "req-9");

    control_ack ack{"req-9", "stop", false, "rejected_not_running"};
    control_ack decodedAck;
    REQUIRE(ControlCommandCodec::decodeAck(ControlCommandCodec::encodeAck(ack), decodedAck, reason));
    REQUIRE(decodedAck.requestId == "req-9");
    REQUIRE_FALSE(decodedAck.accepted);
    REQUIRE(decodedAck.reason == "rejected_not_running");
}
