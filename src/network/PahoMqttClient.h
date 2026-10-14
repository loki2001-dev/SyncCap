#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include <MQTTAsync.h>

#include "network/IMqttClient.h"

/**
 * @brief IMqttClient on Eclipse Paho MQTT C (MQTTAsync)
 *
 * Paho runs its own threads; every callback is forwarded from there. automatic reconnect covers
 * lost connections, a failed first connect is reported through onConnectFailed for the owner to retry
 */
class PahoMqttClient : public IMqttClient {
public:
    explicit PahoMqttClient(mqtt_options options);
    ~PahoMqttClient() override;

    PahoMqttClient(const PahoMqttClient&) = delete;
    PahoMqttClient& operator=(const PahoMqttClient&) = delete;

    void set_callbacks(mqtt_callbacks callbacks) override;
    bool connect() override;
    void disconnect() override;
    bool publish(const std::string& topic, const std::string& payload, int qos, bool retain) override;
    bool subscribe(const std::string& topic, int qos) override;
    bool isConnected() const override;

    static constexpr size_t MAX_PAYLOAD_BYTES = 1 << 20;

private:
    // paho callback
    static void on_connected(void* context, char* cause);
    static void on_connection_lost(void* context, char* cause);
    static int on_message(void* context, char* topicName, int topicLength, MQTTAsync_message* message);
    static void on_connect_failure(void* context, MQTTAsync_failureData* response);
    static void on_trace(enum MQTTASYNC_TRACE_LEVELS level, char* message);

    mqtt_callbacks copyCallbacks() const;

    mqtt_options _options;
    MQTTAsync _client;
    mutable std::mutex _callbackMutex;
    mqtt_callbacks _callbacks;
    std::atomic<bool> _connecting;
};
