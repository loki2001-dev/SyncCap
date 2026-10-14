#include "app/Application.h"

#include <csignal>
#include <cstdlib>

#include <utility>

#include "component/AcquisitionController.h"
#include "component/MqttComponent.h"
#include "component/PreviewComponent.h"
#include "component/SensorComponent.h"
#include "component/StatusReportComponent.h"
#include "component/VideoCaptureComponent.h"
#include "codec/X264Encoder.h"
#include "pipeline/VideoPipeline.h"
#include "source/SensorSourceFactory.h"
#include "source/VideoSourceFactory.h"
#include "util/log/Logger.h"

namespace {

constexpr size_t BUS_CAPACITY = 1024;
constexpr uint64_t LOOP_STATS_PERIOD_MS = 10000;
constexpr uint64_t SHUTDOWN_TIMEOUT_MS = 15000;
constexpr uint64_t MILLIS_PER_SECOND = 1000;
constexpr int EXIT_ASSEMBLY_FAILED = 3;
constexpr int MQTT_KEEPALIVE_S = 10;

// secrets come from the environment, never from the committed config
std::string readEnvironment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);  // NOLINT(cppcoreguidelines-no-malloc) _dupenv_s allocates with malloc
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

}  // namespace

Application::Application(app_config config, std::chrono::seconds runFor)
    : _config(std::move(config))
    , _runFor(runFor)
    , _startedAt(std::chrono::steady_clock::now())
    , _bus(_loop, BUS_CAPACITY)
    , _stoppedSubscription(0)
    , _shuttingDown(false) {
}

Application::~Application() {
    stopComponents();
    if (_recorder) {
        _recorder->shutdown();
    }
}

bool Application::assemble() {
    // checks that only make sense once the build options are known
    for (const channel_config& channel : _config.video.channels) {
        if (!VideoSourceFactory::isSupported(channel.type)) {
            LOG_ERROR(app, cfg, "refusing to run reason=unsupported_source channel={} type={}", channel.id,
                      channel.type);
            return false;
        }
    }

    _recorder = std::make_unique<SegmentedRecorder>(_config.storage.recordQueue);
    _http = std::make_unique<HttpLibClient>();
    mqtt_options mqttOptions;
    mqttOptions.host = _config.mqtt.host;
    mqttOptions.port = _config.mqtt.port;
    mqttOptions.clientId = _config.mqtt.clientId;
    mqttOptions.username = _config.mqtt.username;
    mqttOptions.password = readEnvironment("SYNCCAP_MQTT_PASSWORD");
    mqttOptions.willTopic = _config.mqtt.onlineTopic;
    mqttOptions.willPayload = "offline";
    mqttOptions.keepAliveS = MQTT_KEEPALIVE_S;
    _mqtt = std::make_unique<PahoMqttClient>(mqttOptions);

    const status_identity identity{_config.deviceId, _startedAt};

    acquisition_config acquisition;
    acquisition.deviceId = _config.deviceId;
    acquisition.storageRoot = _config.storage.root;
    acquisition.segmentSeconds = _config.storage.segmentSeconds;
    acquisition.autostart = _config.autostart;

    const video_config video = _config.video;
    _pipeline = std::make_unique<VideoPipeline>(
        _config.video, _config.encoder, _config.preview, *_recorder,
        [video](const channel_config& channel, std::string& error) {
            return VideoSourceFactory::create(channel, video, error);
        },
        []() {
            return std::make_unique<X264Encoder>();
        });

    std::string sensorError;
    std::unique_ptr<ISensorSource> sensorSource = SensorSourceFactory::create(_config.sensor, sensorError);
    if (!sensorSource) {
        LOG_ERROR(app, cfg, "sensor source create failed reason={}", sensorError);
    }

    // registration order = dependency order: the controller first, the reporters last
    registerComponent(std::make_unique<AcquisitionController>(_loop, _bus, _state, _state, *_recorder, acquisition));
    registerComponent(std::make_unique<VideoCaptureComponent>(_loop, _bus, _state, *_pipeline));
    registerComponent(
        std::make_unique<SensorComponent>(_loop, _bus, _state, *_recorder, std::move(sensorSource), _config.sensor));
    registerComponent(std::make_unique<MqttComponent>(_loop, _bus, _state, _state, *_mqtt, _config.mqtt, identity));
    registerComponent(std::make_unique<StatusReportComponent>(_loop, _state, _state, *_http, _config.report, identity));
    if (_config.preview.enabled) {
        registerComponent(std::make_unique<PreviewComponent>(_loop, *_pipeline, _config.preview));
    } else {
        LOG_INFO(app, cfg, "preview disabled by configuration");
    }
    return true;
}

void Application::registerComponent(std::unique_ptr<IComponent> component) {
    _components.push_back(std::move(component));
}

void Application::startComponents() {
    for (const auto& component : _components) {
        if (!component->initialize()) {
            LOG_ERROR(app, life, "initialize failed component={} excluded", component->get_name());
            continue;
        }
        if (!component->start()) {
            LOG_ERROR(app, life, "start failed component={} excluded", component->get_name());
            component->stop();
            continue;
        }
        _started.push_back(component.get());
    }
    LOG_INFO(app, life, "assembled components={}/{}", _started.size(), _components.size());
}

void Application::stopComponents() {
    for (auto it = _started.rbegin(); it != _started.rend(); ++it) {
        (*it)->stop();
        LOG_INFO(app, life, "stopped component={}", (*it)->get_name());
    }
    _started.clear();
}

int Application::run() {
    if (!assemble()) {
        return EXIT_ASSEMBLY_FAILED;
    }
    _interruptSignal = std::make_unique<UvSignal>(_loop, SIGINT);
    _interruptSignal->start([this](int) {
        requestShutdown("sigint");
    });
#ifdef _WIN32
    // Ctrl+Break, and what a parent process sends to stop a console child gracefully
    _terminateSignal = std::make_unique<UvSignal>(_loop, SIGBREAK);
    _terminateSignal->start([this](int) {
        requestShutdown("sigbreak");
    });
#else
    _terminateSignal = std::make_unique<UvSignal>(_loop, SIGTERM);
    _terminateSignal->start([this](int) {
        requestShutdown("sigterm");
    });
#endif
    _loopStatsTimer = std::make_unique<UvTimer>(_loop, "loop_stats");
    _loopStatsTimer->start(LOOP_STATS_PERIOD_MS, LOOP_STATS_PERIOD_MS, [this]() {
        logLoopStats();
    });
    _shutdownTimer = std::make_unique<UvTimer>(_loop, "shutdown_timeout");
    _durationTimer = std::make_unique<UvTimer>(_loop, "run_duration");
    if (_runFor.count() > 0) {
        _durationTimer->start(static_cast<uint64_t>(_runFor.count()) * MILLIS_PER_SECOND, 0, [this]() {
            requestShutdown("run_duration");
        });
    }

    startComponents();
    _loop.run();

    LOG_INFO(app, life, "loop exited, stopping components");
    _loopStatsTimer->stop();
    _shutdownTimer->stop();
    _durationTimer->stop();
    if (_stoppedSubscription != 0) {
        _bus.unsubscribe(_stoppedSubscription);
        _stoppedSubscription = 0;
    }
    stopComponents();
    _recorder->shutdown();
    LOG_INFO(app, life, "shutdown complete");
    return 0;
}

void Application::requestShutdown(const char* reason) {
    if (_shuttingDown) {
        LOG_WARN(app, life, "second shutdown request reason={} leaving the loop now", reason);
        _loop.requestStop();
        return;
    }
    _shuttingDown = true;
    const acquisition_phase phase = _state.get_acquisition_state()->phase;
    LOG_INFO(app, life, "shutdown requested reason={} acquisition={}", reason, toString(phase));
    if (phase != acquisition_phase::running && phase != acquisition_phase::stopping) {
        _loop.requestStop();
        return;
    }
    _stoppedSubscription = _bus.subscribe<acquisition_stopped>([this](const acquisition_stopped&) {
        _loop.requestStop();
    });
    if (phase == acquisition_phase::running) {
        _bus.publish(control_command{control_action::stop, control_origin::signal, "shutdown"});
    }
    _shutdownTimer->start(SHUTDOWN_TIMEOUT_MS, 0, [this]() {
        LOG_WARN(app, life, "shutdown timed out timeout_ms={} leaving the loop", SHUTDOWN_TIMEOUT_MS);
        _loop.requestStop();
    });
}

void Application::logLoopStats() {
    const loop_stats stats = _loop.takeStats();
    LOG_INFO(app, stat,
             "loop handlers={} worst_ms={:.3f} worst_handler={} budget_exceeded={} post_high_water={} "
             "post_rejected={} bus_dropped={}",
             stats.handlersRun, stats.worstHandlerMs, stats.worstHandler, stats.budgetExceeded, stats.postHighWater,
             stats.postRejected, _bus.get_dropped_total());
}
