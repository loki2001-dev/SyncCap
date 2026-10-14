#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

#include "source/ISensorSource.h"

/**
 * @brief replays sensor rows from a CSV file, one row per read()
 *
 * header: timestamp_ms,temperature_c,humidity_pct,accel_x_g,accel_y_g,accel_z_g
 * malformed or out-of-range rows are skipped and logged with their line number
 */
class CsvSensorSource : public ISensorSource {
public:
    CsvSensorSource(std::string path, bool loop);

    bool open() override;
    std::optional<sensor_sample> read() override;
    void close() override;
    std::string get_name() const override;

    uint64_t get_rejected_rows() const;

    static bool parseRow(const std::string& line, sensor_sample& sample, std::string& reason);

private:
    bool rewind();

    std::string _path;
    bool _loop;
    std::ifstream _file;
    uint64_t _lineNumber;
    uint64_t _rejectedRows;
};
