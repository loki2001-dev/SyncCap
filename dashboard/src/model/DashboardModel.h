#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/MetricSeries.h"
#include "model/Messages.h"
#include "model/SystemStatus.h"
#include "network/IHttpClient.h"

enum class link_state {
    unknown,
    connected,
    unreachable
};

enum class event_level {
    info,
    warn,
    error
};

struct server_meta {
    bool present = false;
    std::string receivedAt;
    std::string receivedVia;
    double ageS = 0.0;
    bool stale = false;
    std::optional<bool> collectorOnline;
    uint64_t receivedHttp = 0;
    uint64_t receivedMqtt = 0;
};

struct status_snapshot {
    bool available = false;  // the server has received at least one status
    system_status status;
    server_meta meta;
};

struct control_entry {
    std::string time;
    std::string command;
    std::string requestId;
    std::string result;  // sending, pending, accepted, rejected, failed
    std::string reason;
};

// one decoded channel picture of the live preview
struct preview_frame {
    int index = 0;
    int channelId = 0;
    std::string sourceType;
    int width = 0;
    int height = 0;
    int64_t latchUs = 0;  // latch time relative to the earliest channel of the set
    std::shared_ptr<const std::vector<uint8_t>> rgba;
};

struct preview_update {
    uint64_t tick = 0;
    int64_t offsetUs = 0;
    std::string verdict;
    std::vector<preview_frame> frames;
    // when the network thread finished the set. the UI may drain several sets in one frame, so the rate is
    // measured here, not at apply time. default: apply time
    std::chrono::steady_clock::time_point receivedAt{};
};

struct preview_state {
    link_state link = link_state::unknown;
    std::string error;
    uint64_t tick = 0;
    int64_t offsetUs = 0;
    std::string verdict;
    std::vector<preview_frame> frames;
    uint64_t version = 0;  // increases with every new set; the UI uploads textures when it changes
    double setsPerSecond = 0.0;
};

struct event_entry {
    std::string time;
    event_level level = event_level::info;
    std::string text;
};

/**
 * @brief everything the dashboard shows. UI thread only; no ImGui, no network, so it is unit-testable
 *
 * the server client hands raw HTTP results here; this class interprets them, keeps bounded histories
 * for the plots and turns changes (link lost, sync unlocked, phase changed) into log events
 */
class DashboardModel {
public:
    explicit DashboardModel(size_t historyLength = MetricSeries::DEFAULT_CAPACITY);

    // server client results
    void applyStatus(const http_result& result);
    void applyHealth(const http_result& result);
    void applyControlSubmitted(control_action action, const http_result& result);
    void applyControlRecord(const http_result& result);
    void applyPreview(preview_update update);
    void applyPreviewUnavailable(bool reachable, const std::string& reason);

    // UI side
    void noteControlRequested(control_action action);
    void noteControlBusy(control_action action);
    void noteEvent(event_level level, const std::string& text);

    link_state get_server_link() const;
    const std::string& get_server_error() const;
    bool isMqttConnected() const;
    const status_snapshot& get_status() const;
    const preview_state& get_preview() const;
    bool canStart() const;
    bool canStop() const;

    const std::deque<control_entry>& get_controls() const;
    const std::deque<event_entry>& get_events() const;
    const MetricSeries& get_offset_history() const;
    const MetricSeries* findFpsHistory(int channel) const;
    const MetricSeries& get_temperature_history() const;
    const MetricSeries& get_humidity_history() const;
    const MetricSeries& get_acceleration_history() const;
    uint64_t get_polls() const;
    uint64_t get_poll_failures() const;

    static constexpr size_t MAX_CONTROLS = 20;
    static constexpr size_t MAX_EVENTS = 200;

private:
    void set_link(link_state link, const std::string& error);
    void track(const status_snapshot& next);
    control_entry* findControl(const std::string& requestId);
    void addControl(control_entry entry);

    size_t _historyLength;
    link_state _link;
    std::string _serverError;
    bool _mqttConnected;
    status_snapshot _status;
    preview_state _preview;
    std::deque<std::chrono::steady_clock::time_point> _previewArrivals;  // last second; cleared by an outage
    std::deque<control_entry> _controls;
    std::deque<event_entry> _events;
    MetricSeries _offsetHistory;
    std::map<int, MetricSeries> _fpsHistory;
    MetricSeries _temperatureHistory;
    MetricSeries _humidityHistory;
    MetricSeries _accelerationHistory;
    uint64_t _polls;
    uint64_t _pollFailures;
};

const char* toString(link_state link);
