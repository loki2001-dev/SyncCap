#pragma once

#include <functional>
#include <string>

struct mqtt_options {
    std::string host = "127.0.0.1";
    int port = 1883;
    std::string clientId;
    std::string username;
    std::string password;
    std::string willTopic;
    std::string willPayload;
    bool willRetain = true;
    int keepAliveS = 10;
    int minRetryS = 1;
    int maxRetryS = 30;
};

struct mqtt_message {
    std::string topic;
    std::string payload;
};

// callbacks arrive on the client library thread. owners hand them to their own loop
struct mqtt_callbacks {
    std::function<void()> onConnected;  // first connect and every automatic reconnect
    std::function<void(const std::string& cause)> onConnectionLost;
    std::function<void(const std::string& reason)> onConnectFailed;  // the caller retries
    std::function<void(const mqtt_message& message)> onMessage;
};

/**
 * @brief non-blocking MQTT 3.1.1 client
 *
 * connect/publish/subscribe return at once; results come through the callbacks
 */
class IMqttClient {
public:
    virtual ~IMqttClient() = default;

    virtual void set_callbacks(mqtt_callbacks callbacks) = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool publish(const std::string& topic, const std::string& payload, int qos, bool retain) = 0;
    virtual bool subscribe(const std::string& topic, int qos) = 0;
    virtual bool isConnected() const = 0;
};
