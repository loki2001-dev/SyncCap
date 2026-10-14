#include "network/PahoMqttClient.h"

#include <cstring>

#include <utility>

#include "util/log/Logger.h"

namespace {

constexpr int DISCONNECT_TIMEOUT_MS = 1000;
constexpr int MAX_BUFFERED_MESSAGES = 100;

}  // namespace

PahoMqttClient::PahoMqttClient(mqtt_options options)
    : _options(std::move(options))
    , _client(nullptr)
    , _connecting(false) {
    MQTTAsync_setTraceCallback(on_trace);
    MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
    const std::string uri = "tcp://" + _options.host + ":" + std::to_string(_options.port);
    MQTTAsync_createOptions createOptions = MQTTAsync_createOptions_initializer;
    createOptions.sendWhileDisconnected = 0;
    createOptions.maxBufferedMessages = MAX_BUFFERED_MESSAGES;
    createOptions.MQTTVersion = MQTTVERSION_3_1_1;
    const int result = MQTTAsync_createWithOptions(&_client, uri.c_str(), _options.clientId.c_str(),
                                                   MQTTCLIENT_PERSISTENCE_NONE, nullptr, &createOptions);
    if (result != MQTTASYNC_SUCCESS) {
        LOG_ERROR(mqtt, net, "client create failed uri={} code={}", uri, result);
        _client = nullptr;
        return;
    }
    MQTTAsync_setCallbacks(_client, this, on_connection_lost, on_message, nullptr);
    MQTTAsync_setConnected(_client, this, on_connected);
}

PahoMqttClient::~PahoMqttClient() {
    {
        const std::lock_guard<std::mutex> lock(_callbackMutex);
        _callbacks = mqtt_callbacks{};
    }
    disconnect();
    if (_client != nullptr) {
        MQTTAsync_destroy(&_client);
    }
}

void PahoMqttClient::set_callbacks(mqtt_callbacks callbacks) {
    const std::lock_guard<std::mutex> lock(_callbackMutex);
    _callbacks = std::move(callbacks);
}

bool PahoMqttClient::connect() {
    if (_client == nullptr) {
        return false;
    }
    if (_connecting.exchange(true)) {
        return true;
    }
    MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer;
    options.keepAliveInterval = _options.keepAliveS;
    options.cleansession = 1;
    options.automaticReconnect = 1;
    options.minRetryInterval = _options.minRetryS;
    options.maxRetryInterval = _options.maxRetryS;
    options.connectTimeout = _options.keepAliveS;
    options.onFailure = on_connect_failure;
    options.context = this;
    MQTTAsync_willOptions will = MQTTAsync_willOptions_initializer;
    if (!_options.willTopic.empty()) {
        will.topicName = _options.willTopic.c_str();
        will.message = _options.willPayload.c_str();
        will.retained = _options.willRetain ? 1 : 0;
        will.qos = 1;
        options.will = &will;
    }
    if (!_options.username.empty()) {
        options.username = _options.username.c_str();
        options.password = _options.password.c_str();
    }
    const int result = MQTTAsync_connect(_client, &options);
    if (result != MQTTASYNC_SUCCESS) {
        _connecting = false;
        LOG_WARN(mqtt, net, "connect request failed host={} port={} code={}", _options.host, _options.port, result);
        return false;
    }
    LOG_INFO(mqtt, net, "connecting host={} port={} client_id={} username={} password={}", _options.host, _options.port,
             _options.clientId, _options.username, _options.username.empty() ? "" : "***");
    return true;
}

void PahoMqttClient::disconnect() {
    if (_client == nullptr || MQTTAsync_isConnected(_client) == 0) {
        return;
    }
    MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer;
    options.timeout = DISCONNECT_TIMEOUT_MS;
    MQTTAsync_disconnect(_client, &options);
}

bool PahoMqttClient::publish(const std::string& topic, const std::string& payload, int qos, bool retain) {
    if (_client == nullptr || MQTTAsync_isConnected(_client) == 0) {
        return false;
    }
    MQTTAsync_message message = MQTTAsync_message_initializer;
    message.payload = const_cast<char*>(payload.data());
    message.payloadlen = static_cast<int>(payload.size());
    message.qos = qos;
    message.retained = retain ? 1 : 0;
    MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
    const int result = MQTTAsync_sendMessage(_client, topic.c_str(), &message, &response);
    if (result != MQTTASYNC_SUCCESS) {
        LOG_WARN(mqtt, tx, "publish failed topic={} bytes={} code={}", topic, payload.size(), result);
        return false;
    }
    return true;
}

bool PahoMqttClient::subscribe(const std::string& topic, int qos) {
    if (_client == nullptr) {
        return false;
    }
    MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
    const int result = MQTTAsync_subscribe(_client, topic.c_str(), qos, &response);
    if (result != MQTTASYNC_SUCCESS) {
        LOG_WARN(mqtt, net, "subscribe failed topic={} code={}", topic, result);
        return false;
    }
    LOG_INFO(mqtt, net, "subscribed topic={} qos={}", topic, qos);
    return true;
}

bool PahoMqttClient::isConnected() const {
    return _client != nullptr && MQTTAsync_isConnected(_client) != 0;
}

mqtt_callbacks PahoMqttClient::copyCallbacks() const {
    const std::lock_guard<std::mutex> lock(_callbackMutex);
    return _callbacks;
}

void PahoMqttClient::on_connected(void* context, char* /*cause*/) {
    auto* client = static_cast<PahoMqttClient*>(context);
    client->_connecting = false;
    const mqtt_callbacks callbacks = client->copyCallbacks();
    if (callbacks.onConnected) {
        callbacks.onConnected();
    }
}

void PahoMqttClient::on_connection_lost(void* context, char* cause) {
    auto* client = static_cast<PahoMqttClient*>(context);
    const mqtt_callbacks callbacks = client->copyCallbacks();
    if (callbacks.onConnectionLost) {
        callbacks.onConnectionLost(cause != nullptr ? cause : "unknown");
    }
}

int PahoMqttClient::on_message(void* context, char* topicName, int topicLength, MQTTAsync_message* message) {
    auto* client = static_cast<PahoMqttClient*>(context);
    mqtt_message received;
    received.topic =
        topicLength > 0 ? std::string(topicName, static_cast<size_t>(topicLength)) : std::string(topicName);
    // P3: the broker's length is checked before copying
    if (message->payloadlen >= 0 && static_cast<size_t>(message->payloadlen) <= MAX_PAYLOAD_BYTES) {
        received.payload.assign(static_cast<const char*>(message->payload), static_cast<size_t>(message->payloadlen));
    } else {
        LOG_WARN(mqtt, rx, "message dropped reason=payload_too_large topic={} bytes={}", received.topic,
                 message->payloadlen);
        received.topic.clear();
    }
    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topicName);
    if (received.topic.empty()) {
        return 1;
    }
    const mqtt_callbacks callbacks = client->copyCallbacks();
    if (callbacks.onMessage) {
        callbacks.onMessage(received);
    }
    return 1;
}

void PahoMqttClient::on_connect_failure(void* context, MQTTAsync_failureData* response) {
    auto* client = static_cast<PahoMqttClient*>(context);
    client->_connecting = false;
    std::string reason = "code=" + std::to_string(response != nullptr ? response->code : 0);
    if (response != nullptr && response->message != nullptr) {
        reason += " message=" + std::string(response->message);
    }
    const mqtt_callbacks callbacks = client->copyCallbacks();
    if (callbacks.onConnectFailed) {
        callbacks.onConnectFailed(reason);
    }
}

void PahoMqttClient::on_trace(enum MQTTASYNC_TRACE_LEVELS /*level*/, char* message) {
    // connection failures are reported through our callbacks; paho's own trace is detail
    LOG_DEBUG(mqtt, sys, "paho trace={}", message != nullptr ? message : "");
}
