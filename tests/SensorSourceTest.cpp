#include "catch_amalgamated.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "component/SensorComponent.h"
#include "source/CsvSensorSource.h"
#include "source/SimulatedSensorSource.h"

namespace {

std::filesystem::path writeCsv(const std::string& name, const std::string& content) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
    return path;
}

}  // namespace

TEST_CASE("CsvSensorSource parses a valid row") {
    sensor_sample sample;
    std::string reason;
    REQUIRE(CsvSensorSource::parseRow("100, 24.5, 60.2, 0.01, -0.02, 1.0", sample, reason));
    REQUIRE(sample.temperatureC == Catch::Approx(24.5));
    REQUIRE(sample.accelY == Catch::Approx(-0.02));
}

TEST_CASE("CsvSensorSource rejects malformed and out-of-range rows") {
    sensor_sample sample;
    std::string reason;
    REQUIRE_FALSE(CsvSensorSource::parseRow("100,24.5,60.2,0,0", sample, reason));
    REQUIRE(reason == "field_count=5");
    REQUIRE_FALSE(CsvSensorSource::parseRow("100,24.5,60.2,0,0,1,9", sample, reason));
    REQUIRE(reason == "too_many_fields");
    REQUIRE_FALSE(CsvSensorSource::parseRow("100,abc,60.2,0,0,1", sample, reason));
    REQUIRE(reason.rfind("not_a_number", 0) == 0);
    REQUIRE_FALSE(CsvSensorSource::parseRow("100,24.5,101,0,0,1", sample, reason));
    REQUIRE(reason == "humidity_out_of_range");
    REQUIRE_FALSE(CsvSensorSource::parseRow("100,126,50,0,0,1", sample, reason));
    REQUIRE(reason == "temperature_out_of_range");
    REQUIRE_FALSE(CsvSensorSource::parseRow("100,20,50,0,0,16.5", sample, reason));
    REQUIRE(reason == "acceleration_out_of_range");
    REQUIRE_FALSE(CsvSensorSource::parseRow("100,nan,50,0,0,1", sample, reason));
}

TEST_CASE("CsvSensorSource skips bad rows and loops at the end of the file") {
    const auto path =
        writeCsv("synccap_sensor_test.csv", "timestamp_ms,temperature_c,humidity_pct,accel_x_g,accel_y_g,accel_z_g\n"
                                            "0,20.0,50,0,0,1\n"
                                            "broken,row\n"
                                            "100,21.0,51,0,0,1\n");
    CsvSensorSource source(path.string(), true);
    REQUIRE(source.open());
    REQUIRE(source.read()->temperatureC == Catch::Approx(20.0));
    REQUIRE(source.read()->temperatureC == Catch::Approx(21.0));
    REQUIRE(source.get_rejected_rows() == 1);
    REQUIRE(source.read()->temperatureC == Catch::Approx(20.0));
    source.close();
    std::filesystem::remove(path);
}

TEST_CASE("CsvSensorSource without loop ends at the last row") {
    const auto path = writeCsv("synccap_sensor_once.csv", "0,20.0,50,0,0,1\n");
    CsvSensorSource source(path.string(), false);
    REQUIRE(source.open());
    REQUIRE(source.read().has_value());
    REQUIRE_FALSE(source.read().has_value());
    source.close();
    std::filesystem::remove(path);
}

TEST_CASE("SimulatedSensorSource repeats its sequence for the same seed") {
    SimulatedSensorSource first(7);
    SimulatedSensorSource second(7);
    REQUIRE(first.open());
    REQUIRE(second.open());
    for (int index = 0; index < 100; ++index) {
        const sensor_sample a = *first.read();
        const sensor_sample b = *second.read();
        REQUIRE(a.temperatureC == b.temperatureC);
        REQUIRE(a.accelX == b.accelX);
        REQUIRE(a.humidityPct >= 30.0);
        REQUIRE(a.humidityPct <= 90.0);
    }
}

TEST_CASE("SensorComponent reports the dynamic part of the acceleration") {
    sensor_sample still;
    still.accelZ = 1.0;
    REQUIRE(SensorComponent::dynamicAcceleration(still) == Catch::Approx(0.0));
    sensor_sample moving;
    moving.accelX = 0.3;
    moving.accelZ = 1.0;
    REQUIRE(SensorComponent::dynamicAcceleration(moving) == Catch::Approx(0.044030650891055).epsilon(1e-9));
}
