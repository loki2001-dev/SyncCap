#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "network/IMqttClient.h"

// test double: records what would have reached the broker
class FakeMqttClient : public IMqttClient {
public:
    struct published {
        std::string topic;
        std::string payload;
        int qos;
        bool retain;
    };

    void set_callbacks(mqtt_callbacks callbacks) override {
        const std::lock_guard<std::mutex> lock(_mutex);
        _callbacks = std::move(callbacks);
    }
    bool connect() override {
        return true;
    }
    void disconnect() override {
    }
    bool publish(const std::string& topic, const std::string& payload, int qos, bool retain) override {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!connected) {
            return false;
        }
        _published.push_back({topic, payload, qos, retain});
        return true;
    }
    bool subscribe(const std::string& topic, int /*qos*/) override {
        const std::lock_guard<std::mutex> lock(_mutex);
        _subscribed.push_back(topic);
        return true;
    }
    bool isConnected() const override {
        return connected;
    }

    std::vector<published> get_published() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _published;
    }

    std::vector<published> get_published(const std::string& topic) const {
        std::vector<published> matching;
        for (const published& entry : get_published()) {
            if (entry.topic == topic) {
                matching.push_back(entry);
            }
        }
        return matching;
    }

    mqtt_callbacks get_callbacks() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _callbacks;
    }

    std::atomic<bool> connected{true};

private:
    mutable std::mutex _mutex;
    mqtt_callbacks _callbacks;
    std::vector<published> _published;
    std::vector<std::string> _subscribed;
};
