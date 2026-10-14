#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <string>

#include "source/ISensorSource.h"

/**
 * @brief random sensor readings from std::mt19937. the same seed gives the same sequence
 *
 * temperature and humidity drift as bounded random walks, acceleration is 1 g on z plus noise
 * and an occasional bump
 */
class SimulatedSensorSource : public ISensorSource {
public:
    explicit SimulatedSensorSource(uint32_t seed);

    bool open() override;
    std::optional<sensor_sample> read() override;
    void close() override;
    std::string get_name() const override;

private:
    uint32_t _seed;
    std::mt19937 _engine;
    double _temperatureC;
    double _humidityPct;
};
