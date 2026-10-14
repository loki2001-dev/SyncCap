#include "component/PreviewComponent.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/LoopCall.h"
#include "util/log/Logger.h"

namespace {

constexpr int HTTP_OK = 200;
constexpr int HTTP_BAD_REQUEST = 400;
constexpr int HTTP_NOT_FOUND = 404;
constexpr int HTTP_CONFLICT = 409;
constexpr int HTTP_UNAVAILABLE = 503;
constexpr auto LOOP_DEADLINE = std::chrono::milliseconds(1000);
constexpr int64_t NANOS_PER_MICRO = 1000;
constexpr const char* JSON_TYPE = "application/json";

void replyError(httplib::Response& response, int status, const std::string& error) {
    response.status = status;
    response.set_content(nlohmann::json{{"error", error}}.dump(), JSON_TYPE);
}

}  // namespace

PreviewComponent::PreviewComponent(UvLoop& loop, const IPreviewProvider& provider, preview_config config)
    : _loop(loop)
    , _provider(provider)
    , _config(std::move(config))
    , _boundPort(0) {
}

PreviewComponent::~PreviewComponent() {
    stop();
}

const char* PreviewComponent::get_name() const {
    return "preview";
}

bool PreviewComponent::initialize() {
    _host = std::make_unique<HttpServerHost>();
    httplib::Server& server = _host->get_server();
    server.Get("/preview", [this](const httplib::Request& request, httplib::Response& response) {
        handleMeta(request, response);
    });
    server.Get(R"(/preview/(\d{1,2})\.jpg)", [this](const httplib::Request& request, httplib::Response& response) {
        handleImage(request, response);
    });
    return true;
}

bool PreviewComponent::start() {
    // bind now so a port conflict is reported at startup
    _boundPort = _host->bind(_config.bind, _config.port);
    if (_boundPort == 0) {
        LOG_ERROR(video, net, "preview bind failed bind={} port={}", _config.bind, _config.port);
        return false;
    }
    _host->listen();
    LOG_INFO(video, life, "preview serving bind={} port={} fps={} size={}x{}", _config.bind, _boundPort, _config.fps,
             _config.width, _config.height);
    return true;
}

void PreviewComponent::stop() {
    if (_host) {
        _host->stop();
    }
}

int PreviewComponent::get_bound_port() const {
    return _boundPort;
}

std::shared_ptr<const preview_set> PreviewComponent::readSet(const httplib::Request& request, bool& gone, bool& busy) {
    gone = false;
    busy = false;
    const IPreviewProvider* provider = &_provider;
    std::optional<std::shared_ptr<const preview_set>> set;
    if (request.has_param("tick")) {
        const std::string text = request.get_param_value("tick");
        uint64_t tick = 0;
        try {
            size_t used = 0;
            tick = std::stoull(text, &used);
            if (used != text.size()) {
                return nullptr;
            }
        } catch (const std::exception&) {
            return nullptr;
        }
        set = callOnLoop<std::shared_ptr<const preview_set>>(
            _loop,
            [provider, tick]() {
                return provider->find_preview(tick);
            },
            LOOP_DEADLINE);
        gone = set && !*set;
    } else {
        set = callOnLoop<std::shared_ptr<const preview_set>>(
            _loop,
            [provider]() {
                return provider->get_latest_preview();
            },
            LOOP_DEADLINE);
    }
    if (!set) {
        busy = true;
        return nullptr;
    }
    return *set;
}

void PreviewComponent::handleMeta(const httplib::Request& request, httplib::Response& response) {
    bool gone = false;
    bool busy = false;
    const auto set = readSet(request, gone, busy);
    if (busy) {
        replyError(response, HTTP_UNAVAILABLE, "service_busy");
        return;
    }
    if (!set) {
        if (request.has_param("tick") && !gone) {
            replyError(response, HTTP_BAD_REQUEST, "tick must be an unsigned integer");
            return;
        }
        replyError(response, gone ? HTTP_CONFLICT : HTTP_NOT_FOUND, gone ? "tick replaced" : "no preview yet");
        return;
    }
    int64_t earliest = std::numeric_limits<int64_t>::max();
    for (const preview_image& image : set->images) {
        earliest = std::min(earliest, image.timestampNs);
    }
    nlohmann::json channels = nlohmann::json::array();
    for (const preview_image& image : set->images) {
        channels.push_back({{"index", image.index},
                            {"id", image.channelId},
                            {"source", image.sourceType},
                            {"latch_us", (image.timestampNs - earliest) / NANOS_PER_MICRO},
                            {"width", image.width},
                            {"height", image.height},
                            {"bytes", image.jpeg ? image.jpeg->size() : 0}});
    }
    const nlohmann::json body{{"tick", set->tick},
                              {"offset_us", set->offsetNs / NANOS_PER_MICRO},
                              {"verdict", toString(set->verdict)},
                              {"channels", channels}};
    response.status = HTTP_OK;
    response.set_header("Cache-Control", "no-store");
    response.set_content(body.dump(), JSON_TYPE);
}

void PreviewComponent::handleImage(const httplib::Request& request, httplib::Response& response) {
    const std::string indexText = request.matches[1];
    const int index = std::stoi(indexText);
    bool gone = false;
    bool busy = false;
    const auto set = readSet(request, gone, busy);
    if (busy) {
        replyError(response, HTTP_UNAVAILABLE, "service_busy");
        return;
    }
    if (!set) {
        if (request.has_param("tick") && !gone) {
            replyError(response, HTTP_BAD_REQUEST, "tick must be an unsigned integer");
            return;
        }
        replyError(response, gone ? HTTP_CONFLICT : HTTP_NOT_FOUND, gone ? "tick replaced" : "no preview yet");
        return;
    }
    // P3: the index comes from the URL; look it up instead of indexing with it
    const auto image = std::find_if(set->images.begin(), set->images.end(), [index](const preview_image& candidate) {
        return candidate.index == index;
    });
    if (image == set->images.end() || !image->jpeg) {
        replyError(response, HTTP_NOT_FOUND, "no such channel index");
        return;
    }
    response.status = HTTP_OK;
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Preview-Tick", std::to_string(set->tick));
    response.set_content(reinterpret_cast<const char*>(image->jpeg->data()), image->jpeg->size(), "image/jpeg");
}
