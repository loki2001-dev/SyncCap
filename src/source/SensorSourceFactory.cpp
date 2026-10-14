#include "source/SensorSourceFactory.h"

#include "source/CsvSensorSource.h"
#include "source/SimulatedSensorSource.h"

std::unique_ptr<ISensorSource> SensorSourceFactory::create(const sensor_config& config, std::string& error) {
    if (config.type == "simulated") {
        return std::make_unique<SimulatedSensorSource>(config.seed);
    }
    if (config.type == "csv") {
        return std::make_unique<CsvSensorSource>(config.csvPath, config.loop);
    }
    error = "unknown sensor type=" + config.type;
    return nullptr;
}
