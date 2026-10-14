#include "codec/StatusJsonCodec.h"

#include <cmath>

namespace {

using json = nlohmann::json;

constexpr double ROUND_SCALE_3 = 1000.0;
constexpr double ROUND_SCALE_2 = 100.0;
constexpr size_t MAX_CHANNELS_IN_DOCUMENT = 16;

double round3(double value) {
    return std::round(value * ROUND_SCALE_3) / ROUND_SCALE_3;
}

double round2(double value) {
    return std::round(value * ROUND_SCALE_2) / ROUND_SCALE_2;
}

// field readers: absent optional fields keep their default, wrong types are errors
class FieldReader {
public:
    FieldReader(const json& object, std::string path, std::string& error)
        : _object(object)
        , _path(std::move(path))
        , _error(error) {
    }

    bool number(const char* key, double& value, bool required) {
        const auto it = _object.find(key);
        if (it == _object.end()) {
            return missing(key, required);
        }
        if (!it->is_number()) {
            return fail(key, "number");
        }
        value = it->get<double>();
        if (!std::isfinite(value)) {
            return fail(key, "finite number");
        }
        return true;
    }

    template <typename Int>
    bool integer(const char* key, Int& value, bool required) {
        const auto it = _object.find(key);
        if (it == _object.end()) {
            return missing(key, required);
        }
        if (!it->is_number_integer() || it->get<long long>() < 0) {
            return fail(key, "non-negative integer");
        }
        value = static_cast<Int>(it->get<long long>());
        return true;
    }

    bool boolean(const char* key, bool& value, bool required) {
        const auto it = _object.find(key);
        if (it == _object.end()) {
            return missing(key, required);
        }
        if (!it->is_boolean()) {
            return fail(key, "boolean");
        }
        value = it->get<bool>();
        return true;
    }

    bool text(const char* key, std::string& value, bool required) {
        const auto it = _object.find(key);
        if (it == _object.end()) {
            return missing(key, required);
        }
        if (!it->is_string()) {
            return fail(key, "string");
        }
        value = it->get<std::string>();
        return true;
    }

private:
    bool missing(const char* key, bool required) {
        if (required) {
            _error = "missing field " + _path + key;
            return false;
        }
        return true;
    }

    bool fail(const char* key, const char* expected) {
        _error = "field " + _path + key + " must be a " + expected;
        return false;
    }

    const json& _object;
    std::string _path;
    std::string& _error;
};

const json* objectField(const json& parent, const char* key, bool required, std::string& error) {
    const auto it = parent.find(key);
    if (it == parent.end()) {
        if (required) {
            error = std::string("missing field ") + key;
        }
        return nullptr;
    }
    if (!it->is_object()) {
        error = std::string("field ") + key + " must be an object";
        return nullptr;
    }
    return &*it;
}

}  // namespace

nlohmann::json StatusJsonCodec::toJson(const system_status& status) {
    json channels = json::array();
    for (const channel_stats& channel : status.channels) {
        channels.push_back({{"id", channel.id},
                            {"fps", round2(channel.fps)},
                            {"queue_length", channel.queueLength},
                            {"source", channel.sourceType},
                            {"frames_encoded", channel.framesEncoded},
                            {"dropped", channel.dropped},
                            {"duplicated", channel.duplicated},
                            {"missed_ticks", channel.missedTicks},
                            {"source_errors", channel.sourceErrors},
                            {"encode_ms_avg", round3(channel.encodeMsAvg)}});
    }
    const sync_report& sync = status.sync;
    json files = json::array();
    for (const std::string& file : status.storage.recentFiles) {
        files.push_back(file);
    }
    return json{{"schema_version", status.schemaVersion},
                {"device_id", status.deviceId},
                {"timestamp", status.timestamp},
                {"uptime_s", round2(status.uptimeS)},
                {"acquisition",
                 {{"state", toString(status.acquisition.phase)},
                  {"session_id", status.acquisition.sessionId},
                  {"reason", status.acquisition.reason}}},
                {"channel_status", channels},
                {"sync_status",
                 {{"max_offset_ms", round3(sync.windowMaxOffsetMs)},
                  {"locked", sync.locked},
                  {"last_offset_ms", round3(sync.lastOffsetMs)},
                  {"max_offset_total_ms", round3(sync.totalMaxOffsetMs)},
                  {"tolerance_ms", sync.toleranceMs},
                  {"sets_checked", sync.setsChecked},
                  {"sets_locked", sync.setsLocked},
                  {"violations", sync.violations},
                  {"incomplete", sync.incomplete},
                  {"backpressure", sync.backpressure}}},
                {"sensor_status",
                 {{"temperature", round2(status.sensor.temperatureC)},
                  {"humidity", round2(status.sensor.humidityPct)},
                  {"acceleration", round3(status.sensor.accelerationG)},
                  {"valid", status.sensor.valid},
                  {"samples", status.sensor.samples}}},
                {"storage",
                 {{"segment", status.storage.segment},
                  {"bytes_written", status.storage.bytesWritten},
                  {"files", files},
                  {"dropped", status.storage.dropped},
                  {"write_errors", status.storage.writeErrors},
                  {"queue_length", status.storage.queueLength}}}};
}

std::string StatusJsonCodec::encode(const system_status& status) {
    return toJson(status).dump();
}

bool StatusJsonCodec::decode(const std::string& text, system_status& status, nlohmann::json& document,
                             std::string& error) {
    if (text.size() > MAX_DOCUMENT_BYTES) {
        error = "document too large bytes=" + std::to_string(text.size());
        return false;
    }
    document = json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = "body is not a JSON object";
        return false;
    }
    FieldReader root(document, "", error);
    int version = 0;
    if (!root.integer("schema_version", version, true)) {
        return false;
    }
    if (version != STATUS_SCHEMA_VERSION) {
        error = "unsupported schema_version=" + std::to_string(version);
        return false;
    }
    status.schemaVersion = version;
    if (!root.text("device_id", status.deviceId, false) || !root.text("timestamp", status.timestamp, false) ||
        !root.number("uptime_s", status.uptimeS, false)) {
        return false;
    }

    const auto channels = document.find("channel_status");
    if (channels == document.end() || !channels->is_array()) {
        error = "field channel_status must be an array";
        return false;
    }
    if (channels->size() > MAX_CHANNELS_IN_DOCUMENT) {
        error = "too many channels count=" + std::to_string(channels->size());
        return false;
    }
    status.channels.clear();
    for (size_t index = 0; index < channels->size(); ++index) {
        const json& entry = (*channels)[index];
        if (!entry.is_object()) {
            error = "channel_status[" + std::to_string(index) + "] must be an object";
            return false;
        }
        channel_stats channel;
        FieldReader reader(entry, "channel_status[" + std::to_string(index) + "].", error);
        if (!reader.integer("id", channel.id, true) || !reader.number("fps", channel.fps, true) ||
            !reader.integer("queue_length", channel.queueLength, true) ||
            !reader.text("source", channel.sourceType, false) ||
            !reader.integer("frames_encoded", channel.framesEncoded, false) ||
            !reader.integer("dropped", channel.dropped, false) ||
            !reader.integer("duplicated", channel.duplicated, false) ||
            !reader.integer("missed_ticks", channel.missedTicks, false) ||
            !reader.integer("source_errors", channel.sourceErrors, false) ||
            !reader.number("encode_ms_avg", channel.encodeMsAvg, false)) {
            return false;
        }
        status.channels.push_back(channel);
    }

    const json* sync = objectField(document, "sync_status", true, error);
    if (sync == nullptr) {
        return false;
    }
    FieldReader syncReader(*sync, "sync_status.", error);
    if (!syncReader.number("max_offset_ms", status.sync.windowMaxOffsetMs, true) ||
        !syncReader.boolean("locked", status.sync.locked, true) ||
        !syncReader.number("last_offset_ms", status.sync.lastOffsetMs, false) ||
        !syncReader.number("max_offset_total_ms", status.sync.totalMaxOffsetMs, false) ||
        !syncReader.number("tolerance_ms", status.sync.toleranceMs, false) ||
        !syncReader.integer("sets_checked", status.sync.setsChecked, false) ||
        !syncReader.integer("sets_locked", status.sync.setsLocked, false) ||
        !syncReader.integer("violations", status.sync.violations, false) ||
        !syncReader.integer("incomplete", status.sync.incomplete, false) ||
        !syncReader.integer("backpressure", status.sync.backpressure, false)) {
        return false;
    }

    const json* sensor = objectField(document, "sensor_status", true, error);
    if (sensor == nullptr) {
        return false;
    }
    FieldReader sensorReader(*sensor, "sensor_status.", error);
    // a document without the field comes from a sender that only reports valid readings
    status.sensor.valid = true;
    if (!sensorReader.number("temperature", status.sensor.temperatureC, true) ||
        !sensorReader.number("humidity", status.sensor.humidityPct, true) ||
        !sensorReader.number("acceleration", status.sensor.accelerationG, true) ||
        !sensorReader.boolean("valid", status.sensor.valid, false) ||
        !sensorReader.integer("samples", status.sensor.samples, false)) {
        return false;
    }

    std::string optionalError;
    if (const json* acquisition = objectField(document, "acquisition", false, optionalError)) {
        std::string phase;
        FieldReader reader(*acquisition, "acquisition.", error);
        if (!reader.text("state", phase, false) || !reader.text("session_id", status.acquisition.sessionId, false) ||
            !reader.text("reason", status.acquisition.reason, false)) {
            return false;
        }
        status.acquisition.phase = phase == "running"    ? acquisition_phase::running
                                   : phase == "stopping" ? acquisition_phase::stopping
                                   : phase == "error"    ? acquisition_phase::error
                                                         : acquisition_phase::idle;
    } else if (!optionalError.empty()) {
        error = optionalError;
        return false;
    }
    if (const json* storage = objectField(document, "storage", false, optionalError)) {
        FieldReader reader(*storage, "storage.", error);
        const auto files = storage->find("files");
        if (files != storage->end()) {
            if (!files->is_array() || files->size() > MAX_FILES) {
                error = "field storage.files must be an array of at most " + std::to_string(MAX_FILES) + " strings";
                return false;
            }
            status.storage.recentFiles.clear();
            for (const json& file : *files) {
                if (!file.is_string()) {
                    error = "field storage.files must contain strings";
                    return false;
                }
                status.storage.recentFiles.push_back(file.get<std::string>());
            }
        }
        // segment is -1 before the first file of a session
        const auto segment = storage->find("segment");
        if (segment != storage->end()) {
            if (!segment->is_number_integer()) {
                error = "field storage.segment must be an integer";
                return false;
            }
            status.storage.segment = segment->get<int64_t>();
        }
        if (!reader.integer("bytes_written", status.storage.bytesWritten, false) ||
            !reader.integer("dropped", status.storage.dropped, false) ||
            !reader.integer("write_errors", status.storage.writeErrors, false) ||
            !reader.integer("queue_length", status.storage.queueLength, false)) {
            return false;
        }
    } else if (!optionalError.empty()) {
        error = optionalError;
        return false;
    }
    return true;
}
