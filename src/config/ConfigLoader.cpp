#include "config/ConfigLoader.h"

#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

constexpr int SPIN_MARGIN_MAX_US = 5000;
constexpr double SYNC_TOLERANCE_MIN_MS = 0.1;
constexpr int POOL_FRAMES_MIN = 6;
constexpr int POOL_FRAMES_MAX = 64;
constexpr int ENCODER_QUEUE_MIN = 2;
constexpr int ENCODER_QUEUE_MAX = 32;
constexpr int PENDING_TICKS_MIN = 1;
constexpr int PENDING_TICKS_MAX = 30;
constexpr int BITRATE_MIN_KBPS = 500;
constexpr int BITRATE_MAX_KBPS = 50000;
constexpr int ENCODER_THREADS_MAX = 8;
constexpr int GOP_MAX = 300;
constexpr int SEGMENT_MIN_S = 1;
constexpr int SEGMENT_MAX_S = 3600;
constexpr int SENSOR_PERIOD_MIN_MS = 10;
constexpr int SENSOR_PERIOD_MAX_MS = 10000;
constexpr int REPORT_TIMEOUT_MIN_MS = 500;
constexpr int REPORT_TIMEOUT_MAX_MS = 10000;
constexpr int PORT_MAX = 65535;
constexpr int STATUS_INTERVAL_MIN_MS = 1000;
constexpr int STATUS_INTERVAL_MAX_MS = 60000;
constexpr int CHANNEL_ID_MAX = 15;
constexpr int PREVIEW_SIZE_MIN = 64;
constexpr int PREVIEW_QUALITY_BEST = 2;
constexpr int PREVIEW_QUALITY_WORST = 31;

enum class severity {
    warning,
    error
};

// reads one JSON object. every key read is remembered so the rest can be reported as unknown
class ObjectReader {
public:
    ObjectReader(const json& object, std::string path, config_issues& issues)
        : _object(object)
        , _path(std::move(path))
        , _issues(issues) {
    }

    ~ObjectReader() {
        for (auto it = _object.begin(); it != _object.end(); ++it) {
            if (_known.count(it.key()) == 0) {
                _issues.warnings.push_back("unknown key=" + qualify(it.key()) + " (typo?) ignored");
            }
        }
    }

    ObjectReader(const ObjectReader&) = delete;
    ObjectReader& operator=(const ObjectReader&) = delete;

    void readString(const char* key, std::string& value) {
        const json* found = find(key);
        if (found == nullptr) {
            return;
        }
        if (!found->is_string()) {
            typeMismatch(key, "string", value);
            return;
        }
        value = found->get<std::string>();
    }

    void readBool(const char* key, bool& value) {
        const json* found = find(key);
        if (found == nullptr) {
            return;
        }
        if (!found->is_boolean()) {
            typeMismatch(key, "bool", value ? "true" : "false");
            return;
        }
        value = found->get<bool>();
    }

    template <typename Int>
    void readInt(const char* key, Int& value, long long minValue, long long maxValue, severity onRange) {
        const json* found = find(key);
        if (found == nullptr) {
            return;
        }
        if (!found->is_number_integer()) {
            typeMismatch(key, "integer", std::to_string(value));
            return;
        }
        const long long parsed = found->get<long long>();
        if (parsed < minValue || parsed > maxValue) {
            outOfRange(key, std::to_string(parsed), std::to_string(minValue), std::to_string(maxValue),
                       std::to_string(value), onRange);
            return;
        }
        value = static_cast<Int>(parsed);
    }

    void readDouble(const char* key, double& value, double minValue, double maxValue, severity onRange) {
        const json* found = find(key);
        if (found == nullptr) {
            return;
        }
        if (!found->is_number()) {
            typeMismatch(key, "number", std::to_string(value));
            return;
        }
        const double parsed = found->get<double>();
        if (parsed < minValue || parsed > maxValue) {
            outOfRange(key, std::to_string(parsed), std::to_string(minValue), std::to_string(maxValue),
                       std::to_string(value), onRange);
            return;
        }
        value = parsed;
    }

    // nullptr when absent or not an object (the latter is recorded)
    const json* child(const char* key, json::value_t expected) {
        const json* found = find(key);
        if (found == nullptr) {
            return nullptr;
        }
        if (found->type() != expected) {
            _issues.errors.push_back("wrong type key=" + qualify(key));
            return nullptr;
        }
        return found;
    }

    std::string qualify(const std::string& key) const {
        return _path.empty() ? key : _path + "." + key;
    }

private:
    const json* find(const char* key) {
        _known.insert(key);
        const auto it = _object.find(key);
        return it == _object.end() ? nullptr : &*it;
    }

    void typeMismatch(const char* key, const char* expected, const std::string& fallback) {
        _issues.warnings.push_back("wrong type key=" + qualify(key) + " expected=" + expected +
                                   " using_default=" + fallback);
    }

    void outOfRange(const char* key, const std::string& value, const std::string& minValue, const std::string& maxValue,
                    const std::string& fallback, severity onRange) {
        const std::string text =
            "out of range key=" + qualify(key) + " value=" + value + " range=[" + minValue + "," + maxValue + "]";
        if (onRange == severity::error) {
            _issues.errors.push_back(text);
        } else {
            _issues.warnings.push_back(text + " using_default=" + fallback);
        }
    }

    const json& _object;
    std::string _path;
    config_issues& _issues;
    std::set<std::string> _known;
};

void readVideo(const json& object, video_config& video, config_issues& issues) {
    ObjectReader reader(object, "video", issues);
    reader.readInt("fps", video.fps, VIDEO_FPS_MIN, VIDEO_FPS_MAX, severity::error);
    reader.readInt("width", video.width, 2, 7680, severity::error);
    reader.readInt("height", video.height, 2, 4320, severity::error);
    reader.readDouble("sync_tolerance_ms", video.syncToleranceMs, SYNC_TOLERANCE_MIN_MS, SYNC_TOLERANCE_MAX_MS,
                      severity::error);
    reader.readInt("spin_margin_us", video.spinMarginUs, 0, SPIN_MARGIN_MAX_US, severity::warning);
    reader.readBool("drop_unsynced", video.dropUnsynced);
    reader.readInt("pool_frames", video.poolFrames, POOL_FRAMES_MIN, POOL_FRAMES_MAX, severity::warning);
    reader.readInt("encoder_queue", video.encoderQueue, ENCODER_QUEUE_MIN, ENCODER_QUEUE_MAX, severity::warning);
    reader.readInt("max_pending_ticks", video.maxPendingTicks, PENDING_TICKS_MIN, PENDING_TICKS_MAX, severity::warning);
    if (video.width % 2 != 0 || video.height % 2 != 0) {
        issues.errors.push_back("video size must be even for I420 width=" + std::to_string(video.width) +
                                " height=" + std::to_string(video.height));
    }
    if (video.width != REQUIRED_WIDTH || video.height != REQUIRED_HEIGHT) {
        issues.warnings.push_back("video size differs from the 2 MP requirement width=" + std::to_string(video.width) +
                                  " height=" + std::to_string(video.height));
    }

    const json* channels = reader.child("channels", json::value_t::array);
    if (channels == nullptr) {
        issues.errors.push_back("missing key=video.channels");
        return;
    }
    video.channels.clear();
    std::set<int> ids;
    for (size_t index = 0; index < channels->size(); ++index) {
        const json& entry = (*channels)[index];
        const std::string path = "video.channels[" + std::to_string(index) + "]";
        if (!entry.is_object()) {
            issues.errors.push_back("wrong type key=" + path);
            continue;
        }
        channel_config channel;
        channel.id = static_cast<int>(index);
        ObjectReader channelReader(entry, path, issues);
        channelReader.readInt("id", channel.id, 0, CHANNEL_ID_MAX, severity::error);
        channelReader.readString("type", channel.type);
        channelReader.readString("uri", channel.uri);
        channelReader.readBool("loop", channel.loop);
        if (channel.type != "file" && channel.type != "network" && channel.type != "webcam" &&
            channel.type != "synthetic") {
            issues.errors.push_back("unknown channel type key=" + path + ".type value=" + channel.type);
        }
        if (channel.type != "synthetic" && channel.uri.empty()) {
            issues.errors.push_back("missing key=" + path + ".uri");
        }
        if (!ids.insert(channel.id).second) {
            issues.errors.push_back("duplicate channel id=" + std::to_string(channel.id));
        }
        video.channels.push_back(channel);
    }
    const int count = static_cast<int>(video.channels.size());
    if (count < 3 || count > 4) {
        issues.errors.push_back("channel count must be 3 or 4 count=" + std::to_string(count));
    }
}

void readEncoder(const json& object, encoder_config& encoder, config_issues& issues) {
    ObjectReader reader(object, "encoder", issues);
    reader.readString("preset", encoder.preset);
    reader.readString("tune", encoder.tune);
    reader.readInt("bitrate_kbps", encoder.bitrateKbps, BITRATE_MIN_KBPS, BITRATE_MAX_KBPS, severity::warning);
    reader.readInt("threads", encoder.threads, 1, ENCODER_THREADS_MAX, severity::warning);
    reader.readInt("gop", encoder.gop, 0, GOP_MAX, severity::warning);
}

void readStorage(const json& object, storage_config& storage, config_issues& issues) {
    ObjectReader reader(object, "storage", issues);
    reader.readString("root", storage.root);
    reader.readInt("segment_seconds", storage.segmentSeconds, SEGMENT_MIN_S, SEGMENT_MAX_S, severity::error);
    if (storage.root.empty()) {
        issues.errors.push_back("empty key=storage.root");
    }
}

void readSensor(const json& object, sensor_config& sensor, config_issues& issues) {
    ObjectReader reader(object, "sensor", issues);
    reader.readString("type", sensor.type);
    reader.readInt("period_ms", sensor.periodMs, SENSOR_PERIOD_MIN_MS, SENSOR_PERIOD_MAX_MS, severity::warning);
    reader.readInt("seed", sensor.seed, 0, 0xFFFFFFFFLL, severity::warning);
    reader.readString("csv_path", sensor.csvPath);
    reader.readBool("loop", sensor.loop);
    if (sensor.type != "simulated" && sensor.type != "csv") {
        issues.errors.push_back("unknown sensor type value=" + sensor.type);
    }
    if (sensor.type == "csv" && sensor.csvPath.empty()) {
        issues.errors.push_back("missing key=sensor.csv_path");
    }
}

void readReport(const json& object, report_config& report, config_issues& issues) {
    ObjectReader reader(object, "report", issues);
    reader.readString("url", report.url);
    reader.readInt("interval_s", report.intervalS, REPORT_INTERVAL_MIN_S, REPORT_INTERVAL_MAX_S, severity::error);
    reader.readInt("timeout_ms", report.timeoutMs, REPORT_TIMEOUT_MIN_MS, REPORT_TIMEOUT_MAX_MS, severity::warning);
    if (report.url.rfind("http://", 0) != 0 && report.url.rfind("https://", 0) != 0) {
        issues.errors.push_back("report.url must start with http:// or https:// value=" + report.url);
    }
}

void readMqtt(const json& object, mqtt_config& mqtt, config_issues& issues) {
    ObjectReader reader(object, "mqtt", issues);
    reader.readString("host", mqtt.host);
    reader.readInt("port", mqtt.port, 1, PORT_MAX, severity::error);
    reader.readString("client_id", mqtt.clientId);
    reader.readString("username", mqtt.username);
    reader.readString("status_topic", mqtt.statusTopic);
    reader.readString("control_topic", mqtt.controlTopic);
    reader.readString("ack_topic", mqtt.ackTopic);
    reader.readString("online_topic", mqtt.onlineTopic);
    reader.readInt("status_interval_ms", mqtt.statusIntervalMs, STATUS_INTERVAL_MIN_MS, STATUS_INTERVAL_MAX_MS,
                   severity::warning);
}

void readPreview(const json& object, preview_config& preview, int videoFps, config_issues& issues) {
    ObjectReader reader(object, "preview", issues);
    reader.readBool("enabled", preview.enabled);
    reader.readString("bind", preview.bind);
    // 0: any free port (tests, benchmarks); the bound port is logged at start
    reader.readInt("port", preview.port, 0, PORT_MAX, severity::error);
    reader.readInt("fps", preview.fps, 1, VIDEO_FPS_MAX, severity::warning);
    reader.readInt("width", preview.width, PREVIEW_SIZE_MIN, REQUIRED_WIDTH, severity::warning);
    reader.readInt("height", preview.height, PREVIEW_SIZE_MIN, REQUIRED_HEIGHT, severity::warning);
    reader.readInt("quality", preview.quality, PREVIEW_QUALITY_BEST, PREVIEW_QUALITY_WORST, severity::warning);
    if (preview.width % 2 != 0 || preview.height % 2 != 0) {
        issues.errors.push_back("preview size must be even width=" + std::to_string(preview.width) +
                                " height=" + std::to_string(preview.height));
    }
    if (preview.fps > videoFps) {
        issues.warnings.push_back("preview.fps above video.fps value=" + std::to_string(preview.fps) +
                                  " using=" + std::to_string(videoFps));
        preview.fps = videoFps;
    }
}

void readLog(const json& object, log_config& log, config_issues& issues) {
    ObjectReader reader(object, "log", issues);
    reader.readString("dir", log.directory);
    reader.readString("level", log.level);
    if (log.level != "debug" && log.level != "info" && log.level != "warn" && log.level != "error") {
        issues.warnings.push_back("unknown log level value=" + log.level + " using_default=info");
        log.level = "info";
    }
}

}  // namespace

bool ConfigLoader::loadFile(const std::string& path, app_config& config, config_issues& issues) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        issues.errors.push_back("config open failed path=" + path);
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return loadText(text.str(), config, issues);
}

bool ConfigLoader::loadText(const std::string& text, app_config& config, config_issues& issues) {
    const json root = json::parse(text, nullptr, false, true);
    if (root.is_discarded() || !root.is_object()) {
        issues.errors.push_back("config is not a JSON object");
        return false;
    }
    {
        ObjectReader reader(root, "", issues);
        reader.readString("device_id", config.deviceId);
        reader.readBool("autostart", config.autostart);
        if (const json* video = reader.child("video", json::value_t::object)) {
            readVideo(*video, config.video, issues);
        } else {
            issues.errors.push_back("missing key=video");
        }
        if (const json* encoder = reader.child("encoder", json::value_t::object)) {
            readEncoder(*encoder, config.encoder, issues);
        }
        if (const json* storage = reader.child("storage", json::value_t::object)) {
            readStorage(*storage, config.storage, issues);
        }
        if (const json* sensor = reader.child("sensor", json::value_t::object)) {
            readSensor(*sensor, config.sensor, issues);
        }
        if (const json* report = reader.child("report", json::value_t::object)) {
            readReport(*report, config.report, issues);
        }
        if (const json* mqtt = reader.child("mqtt", json::value_t::object)) {
            readMqtt(*mqtt, config.mqtt, issues);
        }
        if (const json* preview = reader.child("preview", json::value_t::object)) {
            readPreview(*preview, config.preview, config.video.fps, issues);
        }
        if (const json* log = reader.child("log", json::value_t::object)) {
            readLog(*log, config.log, issues);
        }
    }
    if (config.deviceId.empty()) {
        issues.errors.push_back("empty key=device_id");
    }
    return issues.errors.empty();
}

std::vector<std::string> ConfigLoader::summarize(const app_config& config) {
    std::vector<std::string> lines;
    lines.push_back("device_id=" + config.deviceId + " autostart=" + (config.autostart ? "true" : "false"));
    const video_config& video = config.video;
    lines.push_back("video fps=" + std::to_string(video.fps) + " size=" + std::to_string(video.width) + "x" +
                    std::to_string(video.height) + " sync_tolerance_ms=" + std::to_string(video.syncToleranceMs) +
                    " spin_margin_us=" + std::to_string(video.spinMarginUs) + " drop_unsynced=" +
                    (video.dropUnsynced ? "true" : "false") + " pool_frames=" + std::to_string(video.poolFrames) +
                    " encoder_queue=" + std::to_string(video.encoderQueue) +
                    " max_pending_ticks=" + std::to_string(video.maxPendingTicks));
    for (const channel_config& channel : video.channels) {
        lines.push_back("channel id=" + std::to_string(channel.id) + " type=" + channel.type + " uri=" + channel.uri +
                        " loop=" + (channel.loop ? "true" : "false"));
    }
    lines.push_back("encoder preset=" + config.encoder.preset + " tune=" + config.encoder.tune + " bitrate_kbps=" +
                    std::to_string(config.encoder.bitrateKbps) + " threads=" + std::to_string(config.encoder.threads) +
                    " gop=" + std::to_string(config.encoder.gop));
    lines.push_back("storage root=" + config.storage.root +
                    " segment_seconds=" + std::to_string(config.storage.segmentSeconds));
    lines.push_back("sensor type=" + config.sensor.type + " period_ms=" + std::to_string(config.sensor.periodMs) +
                    " seed=" + std::to_string(config.sensor.seed) + " csv_path=" + config.sensor.csvPath);
    lines.push_back("report url=" + config.report.url + " interval_s=" + std::to_string(config.report.intervalS) +
                    " timeout_ms=" + std::to_string(config.report.timeoutMs));
    lines.push_back("mqtt host=" + config.mqtt.host + " port=" + std::to_string(config.mqtt.port) +
                    " client_id=" + config.mqtt.clientId + " username=" + config.mqtt.username +
                    " password=" + (config.mqtt.username.empty() ? "" : "***") +
                    " status_topic=" + config.mqtt.statusTopic + " control_topic=" + config.mqtt.controlTopic +
                    " status_interval_ms=" + std::to_string(config.mqtt.statusIntervalMs));
    lines.push_back("preview enabled=" + std::string(config.preview.enabled ? "true" : "false") +
                    " bind=" + config.preview.bind + " port=" + std::to_string(config.preview.port) +
                    " fps=" + std::to_string(config.preview.fps) + " size=" + std::to_string(config.preview.width) +
                    "x" + std::to_string(config.preview.height) + " quality=" + std::to_string(config.preview.quality));
    lines.push_back("log dir=" + config.log.directory + " level=" + config.log.level);
    return lines;
}
