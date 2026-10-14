#include "source/NetworkStreamVideoSource.h"

#include <utility>

#include "util/log/Logger.h"

namespace {

constexpr int64_t NETWORK_IO_TIMEOUT_US = 5000000;

bool isRtsp(const std::string& url) {
    return url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0;
}

}  // namespace

NetworkStreamVideoSource::NetworkStreamVideoSource(std::string url, int width, int height)
    : FfmpegVideoSource("network", url, width, height, true)
    , _url(std::move(url)) {
}

std::map<std::string, std::string> NetworkStreamVideoSource::get_open_options() const {
    std::map<std::string, std::string> options;
    if (isRtsp(_url)) {
        // TCP interleaving: no lost UDP packets on a busy laptop network
        options["rtsp_transport"] = "tcp";
        options["timeout"] = std::to_string(NETWORK_IO_TIMEOUT_US);
    } else {
        options["rw_timeout"] = std::to_string(NETWORK_IO_TIMEOUT_US);
        options["reconnect"] = "1";
    }
    // low latency: do not buffer frames inside the demuxer
    options["fflags"] = "nobuffer";
    return options;
}

read_status NetworkStreamVideoSource::on_end_of_stream() {
    LOG_WARN(cap, net, "stream ended url={} reconnecting", _url);
    return read_status::error;
}

read_status NetworkStreamVideoSource::on_read_error(int code) {
    LOG_WARN(cap, net, "stream read failed url={} message={} reconnecting", _url, describeError(code));
    return read_status::error;
}

int64_t NetworkStreamVideoSource::get_io_timeout_us() const {
    return NETWORK_IO_TIMEOUT_US;
}
