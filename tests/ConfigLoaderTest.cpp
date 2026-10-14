#include "catch_amalgamated.hpp"

#include <string>

#include <nlohmann/json.hpp>

#include "config/ConfigLoader.h"

namespace {

nlohmann::json validConfig() {
    return nlohmann::json::parse(R"({
        "device_id": "dev",
        "video": {"fps": 30, "channels": [
            {"id": 0, "type": "synthetic"}, {"id": 1, "type": "synthetic"}, {"id": 2, "type": "file", "uri": "a.mp4"}]},
        "report": {"url": "http://127.0.0.1:8080/status", "interval_s": 25}
    })");
}

bool load(const nlohmann::json& json, app_config& config, config_issues& issues) {
    return ConfigLoader::loadText(json.dump(), config, issues);
}

bool hasIssue(const std::vector<std::string>& list, const std::string& text) {
    for (const std::string& entry : list) {
        if (entry.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("ConfigLoader loads a valid configuration") {
    app_config config;
    config_issues issues;
    REQUIRE(load(validConfig(), config, issues));
    REQUIRE(config.deviceId == "dev");
    REQUIRE(config.video.channels.size() == 3);
    REQUIRE(config.video.channels[2].uri == "a.mp4");
    REQUIRE(config.report.intervalS == 25);
    REQUIRE(config.storage.segmentSeconds == 180);
}

TEST_CASE("ConfigLoader enforces 3 or 4 channels") {
    for (int count : {2, 3, 4, 5}) {
        nlohmann::json json = validConfig();
        json["video"]["channels"] = nlohmann::json::array();
        for (int id = 0; id < count; ++id) {
            json["video"]["channels"].push_back({{"id", id}, {"type", "synthetic"}});
        }
        app_config config;
        config_issues issues;
        const bool expected = count == 3 || count == 4;
        INFO("channels=" << count);
        REQUIRE(load(json, config, issues) == expected);
    }
}

TEST_CASE("ConfigLoader enforces the 20 to 30 fps range") {
    for (int fps : {19, 20, 30, 31}) {
        nlohmann::json json = validConfig();
        json["video"]["fps"] = fps;
        app_config config;
        config_issues issues;
        INFO("fps=" << fps);
        REQUIRE(load(json, config, issues) == (fps >= 20 && fps <= 30));
    }
}

TEST_CASE("ConfigLoader enforces the 20 to 30 s report interval") {
    for (int interval : {19, 20, 30, 31}) {
        nlohmann::json json = validConfig();
        json["report"]["interval_s"] = interval;
        app_config config;
        config_issues issues;
        INFO("interval=" << interval);
        REQUIRE(load(json, config, issues) == (interval >= 20 && interval <= 30));
    }
}

TEST_CASE("ConfigLoader refuses a sync tolerance above 2 ms") {
    nlohmann::json json = validConfig();
    json["video"]["sync_tolerance_ms"] = 2.5;
    app_config config;
    config_issues issues;
    REQUIRE_FALSE(load(json, config, issues));
    REQUIRE(hasIssue(issues.errors, "sync_tolerance_ms"));
}

TEST_CASE("ConfigLoader rejects duplicate channel ids, unknown types and missing uris") {
    nlohmann::json json = validConfig();
    json["video"]["channels"][1]["id"] = 0;
    json["video"]["channels"][2]["uri"] = "";
    json["video"]["channels"][0]["type"] = "gmsl";
    app_config config;
    config_issues issues;
    REQUIRE_FALSE(load(json, config, issues));
    REQUIRE(hasIssue(issues.errors, "duplicate channel id=0"));
    REQUIRE(hasIssue(issues.errors, "missing key=video.channels[2].uri"));
    REQUIRE(hasIssue(issues.errors, "unknown channel type"));
}

TEST_CASE("ConfigLoader warns about unknown keys and falls back on soft ranges") {
    nlohmann::json json = validConfig();
    json["vidoe"] = 1;
    json["encoder"] = {{"bitrate_kbps", 1}};
    app_config config;
    config_issues issues;
    REQUIRE(load(json, config, issues));
    REQUIRE(hasIssue(issues.warnings, "unknown key=vidoe"));
    REQUIRE(hasIssue(issues.warnings, "encoder.bitrate_kbps"));
    REQUIRE(config.encoder.bitrateKbps == 4000);
}

TEST_CASE("ConfigLoader needs a csv path for the csv sensor") {
    nlohmann::json json = validConfig();
    json["sensor"] = {{"type", "csv"}};
    app_config config;
    config_issues issues;
    REQUIRE_FALSE(load(json, config, issues));
    REQUIRE(hasIssue(issues.errors, "sensor.csv_path"));
}

TEST_CASE("ConfigLoader rejects text that is not a JSON object") {
    app_config config;
    config_issues issues;
    REQUIRE_FALSE(ConfigLoader::loadText("[1,2]", config, issues));
    REQUIRE_FALSE(ConfigLoader::loadFile("does/not/exist.json", config, issues));
}
