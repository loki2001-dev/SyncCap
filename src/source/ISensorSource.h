#pragma once

#include <optional>
#include <string>

#include "model/SensorSample.h"

/**
 * @brief temperature, humidity and acceleration input. simulators and real inputs share it
 *
 * read() may block on file IO; callers run it on a blocking worker
 */
class ISensorSource {
public:
    virtual ~ISensorSource() = default;

    virtual bool open() = 0;
    virtual std::optional<sensor_sample> read() = 0;
    virtual void close() = 0;
    virtual std::string get_name() const = 0;
};
