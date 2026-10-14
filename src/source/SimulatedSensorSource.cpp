#include "source/SimulatedSensorSource.h"

#include <algorithm>

#include "util/TimeUtil.h"

namespace {

constexpr double START_TEMPERATURE_C = 24.5;
constexpr double START_HUMIDITY_PCT = 60.0;
constexpr double TEMPERATURE_STEP_C = 0.02;
constexpr double HUMIDITY_STEP_PCT = 0.05;
constexpr double TEMPERATURE_LOW_C = 15.0;
constexpr double TEMPERATURE_HIGH_C = 35.0;
constexpr double HUMIDITY_LOW_PCT = 30.0;
constexpr double HUMIDITY_HIGH_PCT = 90.0;
constexpr double GRAVITY_G = 1.0;
constexpr double ACCEL_NOISE_G = 0.01;
constexpr double BUMP_PROBABILITY = 0.01;
constexpr double BUMP_G = 0.3;

}  // namespace

SimulatedSensorSource::SimulatedSensorSource(uint32_t seed)
    : _seed(seed)
    , _engine(seed)
    , _temperatureC(START_TEMPERATURE_C)
    , _humidityPct(START_HUMIDITY_PCT) {
}

bool SimulatedSensorSource::open() {
    _engine.seed(_seed);
    _temperatureC = START_TEMPERATURE_C;
    _humidityPct = START_HUMIDITY_PCT;
    return true;
}

std::optional<sensor_sample> SimulatedSensorSource::read() {
    std::normal_distribution<double> temperatureStep(0.0, TEMPERATURE_STEP_C);
    std::normal_distribution<double> humidityStep(0.0, HUMIDITY_STEP_PCT);
    std::normal_distribution<double> noise(0.0, ACCEL_NOISE_G);
    std::bernoulli_distribution bump(BUMP_PROBABILITY);
    std::uniform_real_distribution<double> bumpSize(-BUMP_G, BUMP_G);

    _temperatureC = std::clamp(_temperatureC + temperatureStep(_engine), TEMPERATURE_LOW_C, TEMPERATURE_HIGH_C);
    _humidityPct = std::clamp(_humidityPct + humidityStep(_engine), HUMIDITY_LOW_PCT, HUMIDITY_HIGH_PCT);

    sensor_sample sample;
    sample.timestampNs = TimeUtil::steadyNowNs();
    sample.temperatureC = _temperatureC;
    sample.humidityPct = _humidityPct;
    sample.accelX = noise(_engine);
    sample.accelY = noise(_engine);
    sample.accelZ = GRAVITY_G + noise(_engine);
    if (bump(_engine)) {
        sample.accelX += bumpSize(_engine);
    }
    return sample;
}

void SimulatedSensorSource::close() {
}

std::string SimulatedSensorSource::get_name() const {
    return "simulated";
}
