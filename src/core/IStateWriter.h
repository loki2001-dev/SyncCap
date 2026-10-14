#pragma once

#include <functional>
#include <memory>

#include "model/States.h"

// one write interface per state kind. a constructor shows exactly what a component writes

class IAcquisitionStatusWriter {
public:
    virtual ~IAcquisitionStatusWriter() = default;

    virtual void updateAcquisitionState(const std::function<void(acquisition_state&)>& edit) = 0;
};

class IStorageStatusWriter {
public:
    virtual ~IStorageStatusWriter() = default;

    virtual void updateStorageState(const std::function<void(storage_state&)>& edit) = 0;
};

class IVideoStatusWriter {
public:
    virtual ~IVideoStatusWriter() = default;

    virtual void updateVideoState(const std::function<void(video_state&)>& edit) = 0;
};

class ISensorStatusWriter {
public:
    virtual ~ISensorStatusWriter() = default;

    virtual void updateSensorState(const std::function<void(sensor_state&)>& edit) = 0;
};

class IReportStatusWriter {
public:
    virtual ~IReportStatusWriter() = default;

    virtual void updateReportState(const std::function<void(report_state&)>& edit) = 0;
};

class IMqttStatusWriter {
public:
    virtual ~IMqttStatusWriter() = default;

    virtual void updateMqttState(const std::function<void(mqtt_state&)>& edit) = 0;
};

// anyone may read, only through immutable snapshots
class IStateReader {
public:
    virtual ~IStateReader() = default;

    virtual std::shared_ptr<const acquisition_state> get_acquisition_state() const = 0;
    virtual std::shared_ptr<const storage_state> get_storage_state() const = 0;
    virtual std::shared_ptr<const video_state> get_video_state() const = 0;
    virtual std::shared_ptr<const sensor_state> get_sensor_state() const = 0;
    virtual std::shared_ptr<const report_state> get_report_state() const = 0;
    virtual std::shared_ptr<const mqtt_state> get_mqtt_state() const = 0;
};
