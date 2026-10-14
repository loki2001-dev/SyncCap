#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "config/AppConfig.h"
#include "core/BlockingWorker.h"
#include "core/IComponent.h"
#include "core/IStateWriter.h"
#include "core/MessageBus.h"
#include "core/UvLoop.h"
#include "core/UvTimer.h"
#include "model/Messages.h"
#include "model/SensorSample.h"
#include "source/ISensorSource.h"
#include "storage/IRecordSink.h"
#include "storage/SegmentClock.h"

/**
 * @brief reads the sensors every period, publishes the latest values and records them during a session
 *
 * provides: sensor_state through ISensorStatusWriter; sensor records to IRecordSink
 * requires: acquisition_started, acquisition_stopping from the bus; ISensorSource (read on a BlockingWorker, P2)
 */
class SensorComponent : public IComponent {
public:
    SensorComponent(UvLoop& loop, MessageBus& bus, ISensorStatusWriter& statusWriter, IRecordSink& recorder,
                    std::unique_ptr<ISensorSource> source, sensor_config config);
    ~SensorComponent() override;

    SensorComponent(const SensorComponent&) = delete;
    SensorComponent& operator=(const SensorComponent&) = delete;

    const char* get_name() const override;
    bool initialize() override;
    bool start() override;
    void stop() override;

    static double dynamicAcceleration(const sensor_sample& sample);

private:
    // timer
    void poll();
    // worker result
    void handleSample(std::optional<sensor_sample> sample);

    UvLoop& _loop;
    MessageBus& _bus;
    ISensorStatusWriter& _statusWriter;
    IRecordSink& _recorder;
    std::unique_ptr<ISensorSource> _source;
    sensor_config _config;
    std::unique_ptr<UvTimer> _timer;
    std::vector<subscription_id_t> _subscriptions;
    bool _recording;
    SegmentClock _segments;
    uint64_t _failures;
    uint64_t _skipped;
    bool _opened;
    // last member: joined first on destruction, before the source it reads
    std::unique_ptr<BlockingWorker> _worker;
};
