#pragma once

#include <cstdint>

// one reading of the temperature, humidity and 3-axis acceleration sensors
struct sensor_sample {
    int64_t timestampNs = 0;  // steady_clock
    double temperatureC = 0.0;
    double humidityPct = 0.0;
    double accelX = 0.0;  // g
    double accelY = 0.0;
    double accelZ = 0.0;
};

constexpr double SENSOR_TEMPERATURE_MIN_C = -40.0;
constexpr double SENSOR_TEMPERATURE_MAX_C = 125.0;
constexpr double SENSOR_HUMIDITY_MIN_PCT = 0.0;
constexpr double SENSOR_HUMIDITY_MAX_PCT = 100.0;
constexpr double SENSOR_ACCEL_LIMIT_G = 16.0;
