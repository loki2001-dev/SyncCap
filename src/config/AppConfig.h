#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct channel_config {
    int id = 0;
    std::string type;  // file, network, webcam, synthetic
    std::string uri;
    bool loop = true;
};

struct video_config {
    int fps = 30;
    int width = 1920;
    int height = 1080;
    double syncToleranceMs = 2.0;
    int spinMarginUs = 1500;
    bool dropUnsynced = true;
    int poolFrames = 12;
    int encoderQueue = 4;
    int maxPendingTicks = 3;
    std::vector<channel_config> channels;
};

struct encoder_config {
    std::string preset = "ultrafast";
    std::string tune = "zerolatency";
    int bitrateKbps = 4000;
    int threads = 2;
    int gop = 0;  // 0: one second
};

struct storage_config {
    std::string root = "recordings";
    int segmentSeconds = 180;
    size_t recordQueue = 512;
};

struct sensor_config {
    std::string type = "simulated";  // simulated, csv
    int periodMs = 100;
    uint32_t seed = 42;
    std::string csvPath;
    bool loop = true;
};

struct report_config {
    std::string url = "http://127.0.0.1:8080/status";
    int intervalS = 25;
    int timeoutMs = 5000;
};

struct mqtt_config {
    std::string host = "127.0.0.1";
    int port = 1883;
    std::string clientId = "synccap-collector";
    std::string username;
    std::string statusTopic = "video/status";
    std::string controlTopic = "video/control";
    std::string ackTopic = "video/control/ack";
    std::string onlineTopic = "video/online";
    int statusIntervalMs = 5000;
};

// live preview for the dashboard: a downscaled JPEG of each channel of a locked set, served over HTTP
struct preview_config {
    bool enabled = true;
    std::string bind = "127.0.0.1";
    int port = 8090;
    int fps = 10;
    int width = 640;
    int height = 360;
    int quality = 5;  // MJPEG qscale, 2 (best) .. 31
};

struct log_config {
    std::string directory = "logs";
    std::string level = "info";
};

struct app_config {
    std::string deviceId = "synccap-01";
    bool autostart = true;
    video_config video;
    encoder_config encoder;
    storage_config storage;
    sensor_config sensor;
    report_config report;
    mqtt_config mqtt;
    preview_config preview;
    log_config log;
};

// limits shared by the loader, the components and the tests
constexpr int VIDEO_FPS_MIN = 20;
constexpr int VIDEO_FPS_MAX = 30;
constexpr int REPORT_INTERVAL_MIN_S = 20;
constexpr int REPORT_INTERVAL_MAX_S = 30;
constexpr double SYNC_TOLERANCE_MAX_MS = 2.0;
constexpr int REQUIRED_WIDTH = 1920;
constexpr int REQUIRED_HEIGHT = 1080;
