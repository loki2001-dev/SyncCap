#pragma once

#include <memory>
#include <string>

#include "config/AppConfig.h"
#include "source/ISensorSource.h"

class SensorSourceFactory {
public:
    static std::unique_ptr<ISensorSource> create(const sensor_config& config, std::string& error);
};
