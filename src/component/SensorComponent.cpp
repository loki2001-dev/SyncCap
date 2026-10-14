#include "component/SensorComponent.h"

#include <cmath>

#include <utility>

#include "util/log/Logger.h"

namespace {

constexpr double GRAVITY_G = 1.0;
constexpr size_t WORKER_QUEUE = 1;
constexpr uint64_t FAILURE_LOG_EVERY = 50;

}  // namespace

SensorComponent::SensorComponent(UvLoop& loop, MessageBus& bus, ISensorStatusWriter& statusWriter,
                                 IRecordSink& recorder, std::unique_ptr<ISensorSource> source, sensor_config config)
    : _loop(loop)
    , _bus(bus)
    , _statusWriter(statusWriter)
    , _recorder(recorder)
    , _source(std::move(source))
    , _config(std::move(config))
    , _recording(false)
    , _failures(0)
    , _skipped(0)
    , _opened(false) {
}

SensorComponent::~SensorComponent() {
    stop();
}

const char* SensorComponent::get_name() const {
    return "sensor";
}

bool SensorComponent::initialize() {
    if (!_source) {
        LOG_ERROR(sensor, life, "initialize failed reason=no_source");
        return false;
    }
    _worker = std::make_unique<BlockingWorker>(_loop, "sensor", WORKER_QUEUE);
    _timer = std::make_unique<UvTimer>(_loop, "sensor_poll");
    _subscriptions.push_back(_bus.subscribe<acquisition_started>([this](const acquisition_started& message) {
        _segments = SegmentClock(message.epochNs, message.segmentNs);
        _recording = true;
    }));
    _subscriptions.push_back(_bus.subscribe<acquisition_stopping>([this](const acquisition_stopping&) {
        _recording = false;
    }));
    LOG_INFO(sensor, life, "initialized source={} period_ms={}", _source->get_name(), _config.periodMs);
    return true;
}

bool SensorComponent::start() {
    _timer->start(static_cast<uint64_t>(_config.periodMs), static_cast<uint64_t>(_config.periodMs), [this]() {
        poll();
    });
    return true;
}

void SensorComponent::stop() {
    if (_timer) {
        _timer->stop();
    }
    for (const subscription_id_t id : _subscriptions) {
        _bus.unsubscribe(id);
    }
    _subscriptions.clear();
    if (_worker) {
        _worker->stop();
    }
    if (_source && _opened) {
        _source->close();
        _opened = false;
    }
}

void SensorComponent::poll() {
    // worker thread only touches _source and _opened
    const bool submitted = _worker->submit<std::optional<sensor_sample>>(
        [this]() -> std::optional<sensor_sample> {
            if (!_opened) {
                _opened = _source->open();
                if (!_opened) {
                    return std::nullopt;
                }
            }
            return _source->read();
        },
        [this](std::optional<sensor_sample> sample) {
            handleSample(std::move(sample));
        });
    if (!submitted) {
        _skipped += 1;
        const uint64_t skipped = _skipped;
        _statusWriter.updateSensorState([skipped](sensor_state& state) {
            state.skippedPeriods = skipped;
        });
        LOG_WARN(sensor, sys, "period skipped reason=previous_read_pending skipped_total={}", skipped);
    }
}

void SensorComponent::handleSample(std::optional<sensor_sample> sample) {
    if (!sample) {
        _failures += 1;
        const uint64_t failures = _failures;
        _statusWriter.updateSensorState([failures](sensor_state& state) {
            state.failures = failures;
            state.valid = false;
        });
        if (failures == 1 || failures % FAILURE_LOG_EVERY == 0) {
            LOG_WARN(sensor, hw, "read failed source={} failures_total={}", _source->get_name(), failures);
        }
        return;
    }
    const sensor_sample value = *sample;
    const double acceleration = dynamicAcceleration(value);
    _statusWriter.updateSensorState([&value, acceleration](sensor_state& state) {
        state.valid = true;
        state.temperatureC = value.temperatureC;
        state.humidityPct = value.humidityPct;
        state.accelerationG = acceleration;
        state.samples += 1;
    });
    if (_recording) {
        _recorder.push(record_item(sensor_record{_segments.indexOf(value.timestampNs), value}));
    }
}

double SensorComponent::dynamicAcceleration(const sensor_sample& sample) {
    const double magnitude =
        std::sqrt(sample.accelX * sample.accelX + sample.accelY * sample.accelY + sample.accelZ * sample.accelZ);
    return std::fabs(magnitude - GRAVITY_G);
}
