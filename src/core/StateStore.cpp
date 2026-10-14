#include "core/StateStore.h"

void StateStore::updateAcquisitionState(const std::function<void(acquisition_state&)>& edit) {
    _acquisition.update(edit);
}

void StateStore::updateStorageState(const std::function<void(storage_state&)>& edit) {
    _storage.update(edit);
}

void StateStore::updateVideoState(const std::function<void(video_state&)>& edit) {
    _video.update(edit);
}

void StateStore::updateSensorState(const std::function<void(sensor_state&)>& edit) {
    _sensor.update(edit);
}

void StateStore::updateReportState(const std::function<void(report_state&)>& edit) {
    _report.update(edit);
}

void StateStore::updateMqttState(const std::function<void(mqtt_state&)>& edit) {
    _mqtt.update(edit);
}

std::shared_ptr<const acquisition_state> StateStore::get_acquisition_state() const {
    return _acquisition.get_snapshot();
}

std::shared_ptr<const storage_state> StateStore::get_storage_state() const {
    return _storage.get_snapshot();
}

std::shared_ptr<const video_state> StateStore::get_video_state() const {
    return _video.get_snapshot();
}

std::shared_ptr<const sensor_state> StateStore::get_sensor_state() const {
    return _sensor.get_snapshot();
}

std::shared_ptr<const report_state> StateStore::get_report_state() const {
    return _report.get_snapshot();
}

std::shared_ptr<const mqtt_state> StateStore::get_mqtt_state() const {
    return _mqtt.get_snapshot();
}
