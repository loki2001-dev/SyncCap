#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include "config/AppConfig.h"
#include "core/IComponent.h"
#include "core/MessageBus.h"
#include "core/StateStore.h"
#include "core/UvLoop.h"
#include "core/UvSignal.h"
#include "core/UvTimer.h"
#include "network/HttpLibClient.h"
#include "network/PahoMqttClient.h"
#include "pipeline/VideoPipeline.h"
#include "storage/SegmentedRecorder.h"

/**
 * @brief composition root. the only place that knows the concrete component types
 *
 * start: components in registration (dependency) order. shutdown: stop acquisition through the normal
 * stop handshake so the files are complete, leave the loop, stop components in reverse, join the storage thread
 */
class Application {
public:
    Application(app_config config, std::chrono::seconds runFor);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 0: clean exit. returns after a signal, the run duration or a fatal assembly error
    int run();

private:
    bool assemble();
    void registerComponent(std::unique_ptr<IComponent> component);
    void startComponents();
    void stopComponents();
    void requestShutdown(const char* reason);
    void logLoopStats();

    app_config _config;
    std::chrono::seconds _runFor;
    std::chrono::steady_clock::time_point _startedAt;

    UvLoop _loop;
    MessageBus _bus;
    StateStore _state;
    std::unique_ptr<SegmentedRecorder> _recorder;
    std::unique_ptr<HttpLibClient> _http;
    std::unique_ptr<PahoMqttClient> _mqtt;
    // shared by the video and preview components; outlives both
    std::unique_ptr<VideoPipeline> _pipeline;
    std::vector<std::unique_ptr<IComponent>> _components;
    std::vector<IComponent*> _started;

    std::unique_ptr<UvSignal> _interruptSignal;
    std::unique_ptr<UvSignal> _terminateSignal;
    std::unique_ptr<UvTimer> _loopStatsTimer;
    std::unique_ptr<UvTimer> _shutdownTimer;
    std::unique_ptr<UvTimer> _durationTimer;
    subscription_id_t _stoppedSubscription;
    bool _shuttingDown;
};
