#pragma once

#include <functional>
#include <memory>

#include "core/IStateWriter.h"
#include "core/StateSlot.h"
#include "model/States.h"

// the single home of shared state. one StateSlot per state kind
class StateStore : public IStateReader,
                   public IAcquisitionStatusWriter,
                   public IStorageStatusWriter,
                   public IVideoStatusWriter,
                   public ISensorStatusWriter,
                   public IReportStatusWriter,
                   public IMqttStatusWriter {
public:
    StateStore() = default;

    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;

    // writers
    void updateAcquisitionState(const std::function<void(acquisition_state&)>& edit) override;
    void updateStorageState(const std::function<void(storage_state&)>& edit) override;
    void updateVideoState(const std::function<void(video_state&)>& edit) override;
    void updateSensorState(const std::function<void(sensor_state&)>& edit) override;
    void updateReportState(const std::function<void(report_state&)>& edit) override;
    void updateMqttState(const std::function<void(mqtt_state&)>& edit) override;

    // reader
    std::shared_ptr<const acquisition_state> get_acquisition_state() const override;
    std::shared_ptr<const storage_state> get_storage_state() const override;
    std::shared_ptr<const video_state> get_video_state() const override;
    std::shared_ptr<const sensor_state> get_sensor_state() const override;
    std::shared_ptr<const report_state> get_report_state() const override;
    std::shared_ptr<const mqtt_state> get_mqtt_state() const override;

private:
    StateSlot<acquisition_state> _acquisition;
    StateSlot<storage_state> _storage;
    StateSlot<video_state> _video;
    StateSlot<sensor_state> _sensor;
    StateSlot<report_state> _report;
    StateSlot<mqtt_state> _mqtt;
};
