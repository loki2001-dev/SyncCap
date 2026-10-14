#include "net/PreviewClient.h"

#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "stb_image.h"
#include "util/log/Logger.h"

namespace {

constexpr int HTTP_OK = 200;
constexpr int HTTP_NOT_FOUND = 404;
constexpr int RGBA_CHANNELS = 4;
constexpr int MAX_DIMENSION = 4096;

}  // namespace

PreviewClient::PreviewClient(std::string baseUrl, IHttpClient& http, std::chrono::milliseconds period,
                             std::chrono::milliseconds timeout, BlockingQueue<model_update_t>& inbox)
    : _baseUrl(std::move(baseUrl))
    , _http(http)
    , _period(period)
    , _timeout(timeout)
    , _inbox(inbox)
    , _running(false)
    , _lastTick(0)
    , _hasTick(false) {
    while (!_baseUrl.empty() && _baseUrl.back() == '/') {
        _baseUrl.pop_back();
    }
}

PreviewClient::~PreviewClient() {
    stop();
}

void PreviewClient::start() {
    {
        const std::lock_guard<std::mutex> lock(_stopMutex);
        _running = true;
    }
    _thread = std::thread([this]() {
        // an exception must not leave the thread: it would std::terminate the process
        try {
            run();
        } catch (const std::exception& e) {
            LOG_ERROR(http, sys, "preview client thread threw message={}", e.what());
        }
    });
}

void PreviewClient::stop() {
    {
        const std::lock_guard<std::mutex> lock(_stopMutex);
        _running = false;
    }
    _stopSignal.notify_all();
    if (_thread.joinable()) {
        _thread.join();
    }
}

bool PreviewClient::decodeJpeg(const std::string& bytes, int& width, int& height, std::vector<uint8_t>& rgba) {
    // P3: size ceiling before handing external bytes to the decoder
    if (bytes.empty() || bytes.size() > MAX_JPEG_BYTES) {
        return false;
    }
    int components = 0;
    stbi_uc* pixels =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &width,
                              &height, &components, RGBA_CHANNELS);
    if (pixels == nullptr) {
        return false;
    }
    const bool sane = width > 0 && height > 0 && width <= MAX_DIMENSION && height <= MAX_DIMENSION;
    if (sane) {
        const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * RGBA_CHANNELS;
        rgba.assign(pixels, pixels + size);
    }
    stbi_image_free(pixels);
    return sane;
}

void PreviewClient::run() {
    while (true) {
        const auto next = std::chrono::steady_clock::now() + _period;
        fetchRound();
        std::unique_lock<std::mutex> lock(_stopMutex);
        // wakes at once on stop()
        if (_stopSignal.wait_until(lock, next, [this]() {
                return !_running;
            })) {
            return;
        }
    }
}

bool PreviewClient::isRunning() {
    const std::lock_guard<std::mutex> lock(_stopMutex);
    return _running;
}

void PreviewClient::fetchRound() {
    const http_result meta = _http.get(_baseUrl + "/preview", _timeout);
    if (meta.status != HTTP_OK) {
        const bool reachable = meta.status != 0;
        const std::string reason = meta.status == HTTP_NOT_FOUND ? "no preview yet (not recording?)"
                                   : reachable                   ? "http status " + std::to_string(meta.status)
                                                                 : meta.error;
        publish([reachable, reason](DashboardModel& model) {
            model.applyPreviewUnavailable(reachable, reason);
        });
        return;
    }
    const nlohmann::json body = nlohmann::json::parse(meta.body, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("tick") || !body["tick"].is_number_unsigned() ||
        !body.contains("channels") || !body["channels"].is_array()) {
        LOG_WARN(http, prot, "preview metadata rejected bytes={}", meta.body.size());
        return;
    }
    const uint64_t tick = body["tick"].get<uint64_t>();
    if (_hasTick && tick == _lastTick) {
        return;
    }
    const nlohmann::json& channels = body["channels"];
    if (channels.size() > static_cast<size_t>(MAX_CHANNELS_IN_PREVIEW)) {
        LOG_WARN(http, prot, "preview metadata rejected reason=too_many_channels count={}", channels.size());
        return;
    }
    preview_update update;
    update.tick = tick;
    update.offsetUs = body.value("offset_us", static_cast<int64_t>(0));
    update.verdict = body.value("verdict", std::string());
    for (const nlohmann::json& channel : channels) {
        if (!isRunning()) {
            return;
        }
        if (!channel.is_object() || !channel.contains("index") || !channel["index"].is_number_integer()) {
            continue;
        }
        const int index = channel["index"].get<int>();
        const http_result image =
            _http.get(_baseUrl + "/preview/" + std::to_string(index) + ".jpg?tick=" + std::to_string(tick), _timeout);
        if (image.status != HTTP_OK) {
            // replaced while we were fetching (409) or gone: skip the whole set, never mix two ticks
            return;
        }
        preview_frame frame;
        auto rgba = std::make_shared<std::vector<uint8_t>>();
        if (!decodeJpeg(image.body, frame.width, frame.height, *rgba)) {
            LOG_WARN(http, prot, "preview image rejected index={} bytes={}", index, image.body.size());
            return;
        }
        frame.index = index;
        frame.channelId = channel.value("id", index);
        frame.sourceType = channel.value("source", std::string());
        frame.latchUs = channel.value("latch_us", static_cast<int64_t>(0));
        frame.rgba = std::move(rgba);
        update.frames.push_back(std::move(frame));
    }
    _lastTick = tick;
    _hasTick = true;
    publish([update = std::move(update)](DashboardModel& model) mutable {
        model.applyPreview(std::move(update));
    });
}

void PreviewClient::publish(model_update_t update) {
    if (_inbox.push(std::move(update)) != push_result::accepted) {
        LOG_WARN(http, sys, "preview update dropped reason=inbox_full dropped_total={}", _inbox.get_dropped_total());
    }
}
