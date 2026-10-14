#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "source/FfmpegVideoSource.h"

/**
 * @brief RTSP (over TCP) or HLS stream. live: paced by the sender
 *
 * a read error or 5 s without data closes the session; the reader reopens it after a delay
 */
class NetworkStreamVideoSource : public FfmpegVideoSource {
public:
    NetworkStreamVideoSource(std::string url, int width, int height);

protected:
    std::map<std::string, std::string> get_open_options() const override;
    read_status on_end_of_stream() override;
    read_status on_read_error(int code) override;
    int64_t get_io_timeout_us() const override;

private:
    std::string _url;
};
