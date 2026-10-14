#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "component/StatusSnapshot.h"
#include "config/AppConfig.h"
#include "core/IComponent.h"
#include "core/IStateWriter.h"
#include "core/MessageBus.h"
#include "core/UvLoop.h"
#include "core/UvTimer.h"
#include "model/Messages.h"
#include "network/IMqttClient.h"

/**
 * @brief MQTT status publisher and control receiver
 *
 * provides: control_command on the bus (validated video/control); video/status, video/control/ack,
 *           video/online on the broker; mqtt_state through IMqttStatusWriter
 * requires: control_result from the bus; IStateReader; IMqttClient (non-blocking, callbacks posted to the loop)
 */
class MqttComponent : public IComponent {
public:
    MqttComponent(UvLoop& loop, MessageBus& bus, const IStateReader& reader, IMqttStatusWriter& statusWriter,
                  IMqttClient& client, mqtt_config config, status_identity identity);
    ~MqttComponent() override;

    MqttComponent(const MqttComponent&) = delete;
    MqttComponent& operator=(const MqttComponent&) = delete;

    const char* get_name() const override;
    bool initialize() override;
    bool start() override;
    void stop() override;

    // test helper: the same path a broker message takes after it reached the loop thread
    void TDD_handleMessage(const mqtt_message& message);

private:
    // client callback, on the loop thread
    void handleConnected();
    void handleConnectionLost(const std::string& cause);
    void handleConnectFailed(const std::string& reason);
    void handleMessage(const mqtt_message& message);

    // bus
    void handleResult(const control_result& result);

    // timer
    void publishStatus();
    void scheduleReconnect();

    UvLoop& _loop;
    MessageBus& _bus;
    const IStateReader& _reader;
    IMqttStatusWriter& _statusWriter;
    IMqttClient& _client;
    mqtt_config _config;
    status_identity _identity;
    std::unique_ptr<UvTimer> _statusTimer;
    std::unique_ptr<UvTimer> _reconnectTimer;
    std::vector<subscription_id_t> _subscriptions;
    std::shared_ptr<int> _lifetime;
    int _reconnectDelayS;
    bool _connected;
};
