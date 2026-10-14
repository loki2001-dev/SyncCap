#pragma once

#include <memory>
#include <string>

#include "config/AppConfig.h"
#include "source/IVideoSource.h"

// the one place that maps a channel type string to a concrete source. a new input is a class and one line here
class VideoSourceFactory {
public:
    static std::unique_ptr<IVideoSource> create(const channel_config& channel, const video_config& video,
                                                std::string& error);
    static bool isSupported(const std::string& type);
};
