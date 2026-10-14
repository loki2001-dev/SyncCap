#include "component/MqttComponent.h"

#include <algorithm>
#include <string>
#include <utility>

#include "codec/ControlCommandCodec.h"
#include "codec/StatusJsonCodec.h"
#include "util/log/Logger.h"

namespace {

constexpr int QOS_STATUS = 0;
constexpr int QOS_CONTROL = 1;
constexpr int RECONNECT_MIN_S = 1;
constexpr int RECONNECT_MAX_S = 30;
constexpr uint64_t MILLIS_PER_SECOND = 1000;
constexpr const char* ONLINE = "online";

}  // namespace

MqttComponent::MqttComponent(UvLoop& loop, MessageBus& bus, const IStateReader& reader, IMqttStatusWriter& statusWriter,
                             IMqttClient& client, mqtt_config config, status_identity identity)
    : _loop(loop)
    , _bus(bus)
    , _reader(reader)
    , _statusWriter(statusWriter)
    , _client(client)
    , _config(std::move(config))
    , _identity(std::move(identity))
    , _lifetime(std::make_shared<int>(0))
    , _reconnectDelayS(RECONNECT_MIN_S)
    , _connected(false) {
}

MqttComponent::~MqttComponent() {
    stop();
}

const char* MqttComponent::get_name() const {
    return "mqtt";
}

bool MqttComponent::initialize() {
    _statusTimer = std::make_unique<UvTimer>(_loop, "mqtt_status");
    _reconnectTimer = std::make_unique<UvTimer>(_loop, "mqtt_reconnect");
    _subscriptions.push_back(_bus.subscribe<control_result>([this](const control_result& result) {
        handleResult(result);
    }));

    // the client calls back on its own thread: hop to the loop, and drop the hop if we are gone
    std::weak_ptr<int> lifetime = _lifetime;
    UvLoop* loop = &_loop;
    mqtt_callbacks callbacks;
    callbacks.onConnected = [this, loop, lifetime]() {
        loop->post([this, lifetime]() {
            if (!lifetime.expired()) {
                handleConnected();
            }
        });
    };
    callbacks.onConnectionLost = [this, loop, lifetime](const std::string& cause) {
        loop->post([this, lifetime, cause]() {
            if (!lifetime.expired()) {
                handleConnectionLost(cause);
            }
        });
    };
    callbacks.onConnectFailed = [this, loop, lifetime](const std::string& reason) {
        loop->post([this, lifetime, reason]() {
            if (!lifetime.expired()) {
                handleConnectFailed(reason);
            }
        });
    };
    callbacks.onMessage = [this, loop, lifetime](const mqtt_message& message) {
        if (!loop->post([this, lifetime, message]() {
                if (!lifetime.expired()) {
                    handleMessage(message);
                }
            })) {
            LOG_WARN(mqtt, rx, "message dropped reason=loop_queue_full topic={}", message.topic);
        }
    };
    _client.set_callbacks(std::move(callbacks));
    return true;
}

bool MqttComponent::start() {
    _statusTimer->start(static_cast<uint64_t>(_config.statusIntervalMs),
                        static_cast<uint64_t>(_config.statusIntervalMs), [this]() {
                            publishStatus();
                        });
    if (!_client.connect()) {
        scheduleReconnect();
    }
    return true;
}

void MqttComponent::stop() {
    if (_statusTimer) {
        _statusTimer->stop();
    }
    if (_reconnectTimer) {
        _reconnectTimer->stop();
    }
    for (const subscription_id_t id : _subscriptions) {
        _bus.unsubscribe(id);
    }
    _subscriptions.clear();
    _lifetime.reset();
    _client.set_callbacks(mqtt_callbacks{});
    if (_connected) {
        // a clean disconnect does not fire the will, so say offline ourselves
        _client.publish(_config.onlineTopic, "offline", QOS_CONTROL, true);
        _client.disconnect();
        _connected = false;
    }
}

void MqttComponent::handleConnected() {
    _connected = true;
    _reconnectDelayS = RECONNECT_MIN_S;
    _reconnectTimer->stop();
    LOG_INFO(mqtt, net, "connected host={} port={}", _config.host, _config.port);
    // clean session: subscriptions are gone after every reconnect, so subscribe again each time
    _client.subscribe(_config.controlTopic, QOS_CONTROL);
    _client.publish(_config.onlineTopic, ONLINE, QOS_CONTROL, true);
    _statusWriter.updateMqttState([](mqtt_state& state) {
        state.connected = true;
    });
    publishStatus();
}

void MqttComponent::handleConnectionLost(const std::string& cause) {
    _connected = false;
    LOG_WARN(mqtt, net, "connection lost cause={} auto_reconnect=on", cause);
    _statusWriter.updateMqttState([](mqtt_state& state) {
        state.connected = false;
    });
}

void MqttComponent::handleConnectFailed(const std::string& reason) {
    LOG_WARN(mqtt, net, "connect failed host={} port={} reason={} retry_in_s={}", _config.host, _config.port, reason,
             _reconnectDelayS);
    scheduleReconnect();
}

void MqttComponent::scheduleReconnect() {
    _reconnectTimer->start(static_cast<uint64_t>(_reconnectDelayS) * MILLIS_PER_SECOND, 0, [this]() {
        if (!_client.connect()) {
            scheduleReconnect();
        }
    });
    _reconnectDelayS = std::min(_reconnectDelayS * 2, RECONNECT_MAX_S);
}

void MqttComponent::TDD_handleMessage(const mqtt_message& message) {
    handleMessage(message);
}

void MqttComponent::handleMessage(const mqtt_message& message) {
    if (message.topic != _config.controlTopic) {
        LOG_WARN(mqtt, rx, "message ignored reason=unexpected_topic topic={}", message.topic);
        return;
    }
    control_request request;
    std::string reason;
    if (!ControlCommandCodec::decode(message.payload, request, reason)) {
        LOG_WARN(mqtt, cmd, "rejected reason={} topic={} bytes={}", reason, message.topic, message.payload.size());
        _statusWriter.updateMqttState([](mqtt_state& state) {
            state.commandsReceived += 1;
            state.commandsRejected += 1;
        });
        control_ack ack;
        ack.accepted = false;
        ack.reason = reason;
        _client.publish(_config.ackTopic, ControlCommandCodec::encodeAck(ack), QOS_CONTROL, false);
        return;
    }
    _statusWriter.updateMqttState([](mqtt_state& state) {
        state.commandsReceived += 1;
    });
    LOG_INFO(mqtt, cmd, "control received action={} request_id={}", toString(request.action), request.requestId);
    _bus.publish(control_command{request.action, control_origin::mqtt, request.requestId});
}

void MqttComponent::handleResult(const control_result& result) {
    if (result.origin != control_origin::mqtt) {
        return;
    }
    control_ack ack;
    ack.requestId = result.requestId;
    ack.command = toString(result.action);
    ack.accepted = result.accepted;
    ack.reason = result.reason;
    if (!_client.publish(_config.ackTopic, ControlCommandCodec::encodeAck(ack), QOS_CONTROL, false)) {
        LOG_WARN(mqtt, tx, "ack not sent reason=disconnected request_id={}", result.requestId);
    }
}

void MqttComponent::publishStatus() {
    if (!_connected) {
        return;
    }
    const std::string payload = StatusJsonCodec::encode(StatusSnapshot::build(_reader, _identity));
    if (_client.publish(_config.statusTopic, payload, QOS_STATUS, true)) {
        _statusWriter.updateMqttState([](mqtt_state& state) {
            state.statusPublished += 1;
        });
    }
}
