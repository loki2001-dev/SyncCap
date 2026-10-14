#include "source/CsvSensorSource.h"

#include <cmath>

#include <array>
#include <sstream>
#include <utility>

#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr size_t CSV_FIELD_COUNT = 6;
constexpr size_t CSV_MAX_LINE = 512;
constexpr int MAX_SKIPPED_PER_READ = 1000;

bool parseDouble(const std::string& text, double& value) {
    if (text.empty()) {
        return false;
    }
    size_t consumed = 0;
    try {
        value = std::stod(text, &consumed);
    } catch (const std::exception&) {
        return false;
    }
    return consumed == text.size() && std::isfinite(value);
}

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

CsvSensorSource::CsvSensorSource(std::string path, bool loop)
    : _path(std::move(path))
    , _loop(loop)
    , _lineNumber(0)
    , _rejectedRows(0) {
}

bool CsvSensorSource::open() {
    _file.close();
    _file.clear();
    _file.open(_path);
    if (!_file) {
        LOG_ERROR(sensor, hw, "csv open failed path={}", _path);
        return false;
    }
    _lineNumber = 0;
    LOG_INFO(sensor, hw, "csv opened path={} loop={}", _path, _loop);
    return true;
}

bool CsvSensorSource::rewind() {
    _file.clear();
    _file.seekg(0);
    _lineNumber = 0;
    return static_cast<bool>(_file);
}

std::optional<sensor_sample> CsvSensorSource::read() {
    if (!_file.is_open()) {
        return std::nullopt;
    }
    std::string line;
    for (int attempt = 0; attempt < MAX_SKIPPED_PER_READ; ++attempt) {
        if (!std::getline(_file, line)) {
            if (!_loop || !rewind()) {
                return std::nullopt;
            }
            continue;
        }
        _lineNumber += 1;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line.rfind("timestamp", 0) == 0) {
            continue;
        }
        sensor_sample sample;
        std::string reason;
        if (!parseRow(line, sample, reason)) {
            _rejectedRows += 1;
            LOG_WARN(sensor, prot, "csv row rejected path={} line={} reason={} rejected_total={}", _path, _lineNumber,
                     reason, _rejectedRows);
            continue;
        }
        // replayed rows are stamped with the time they are read, the file time is informational only
        sample.timestampNs = TimeUtil::steadyNowNs();
        return sample;
    }
    return std::nullopt;
}

bool CsvSensorSource::parseRow(const std::string& line, sensor_sample& sample, std::string& reason) {
    if (line.size() > CSV_MAX_LINE) {
        reason = "line_too_long";
        return false;
    }
    std::array<double, CSV_FIELD_COUNT> values{};
    std::stringstream stream(line);
    std::string field;
    size_t count = 0;
    while (std::getline(stream, field, ',')) {
        if (count >= CSV_FIELD_COUNT) {
            reason = "too_many_fields";
            return false;
        }
        if (!parseDouble(trim(field), values[count])) {
            reason = "not_a_number field=" + std::to_string(count);
            return false;
        }
        count += 1;
    }
    if (count != CSV_FIELD_COUNT) {
        reason = "field_count=" + std::to_string(count);
        return false;
    }
    sample.temperatureC = values[1];
    sample.humidityPct = values[2];
    sample.accelX = values[3];
    sample.accelY = values[4];
    sample.accelZ = values[5];
    if (sample.temperatureC < SENSOR_TEMPERATURE_MIN_C || sample.temperatureC > SENSOR_TEMPERATURE_MAX_C) {
        reason = "temperature_out_of_range";
        return false;
    }
    if (sample.humidityPct < SENSOR_HUMIDITY_MIN_PCT || sample.humidityPct > SENSOR_HUMIDITY_MAX_PCT) {
        reason = "humidity_out_of_range";
        return false;
    }
    if (std::fabs(sample.accelX) > SENSOR_ACCEL_LIMIT_G || std::fabs(sample.accelY) > SENSOR_ACCEL_LIMIT_G ||
        std::fabs(sample.accelZ) > SENSOR_ACCEL_LIMIT_G) {
        reason = "acceleration_out_of_range";
        return false;
    }
    return true;
}

void CsvSensorSource::close() {
    _file.close();
}

std::string CsvSensorSource::get_name() const {
    return "csv";
}

uint64_t CsvSensorSource::get_rejected_rows() const {
    return _rejectedRows;
}
