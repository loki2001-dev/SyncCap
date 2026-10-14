#include "source/VideoSourceFactory.h"

#include "source/FileVideoSource.h"
#include "source/NetworkStreamVideoSource.h"
#include "source/SyntheticVideoSource.h"
#ifdef SYNCCAP_WITH_OPENCV
#include "source/WebcamVideoSource.h"
#endif

std::unique_ptr<IVideoSource> VideoSourceFactory::create(const channel_config& channel, const video_config& video,
                                                         std::string& error) {
    if (channel.type == "file") {
        return std::make_unique<FileVideoSource>(channel.uri, video.width, video.height, channel.loop);
    }
    if (channel.type == "network") {
        return std::make_unique<NetworkStreamVideoSource>(channel.uri, video.width, video.height);
    }
    if (channel.type == "synthetic") {
        return std::make_unique<SyntheticVideoSource>(channel.id, video.width, video.height, video.fps);
    }
    if (channel.type == "webcam") {
#ifdef SYNCCAP_WITH_OPENCV
        return std::make_unique<WebcamVideoSource>(channel.uri, video.width, video.height);
#else
        error = "webcam source needs OpenCV; rebuild with -DSYNCCAP_WITH_OPENCV=ON";
        return nullptr;
#endif
    }
    error = "unknown source type=" + channel.type;
    return nullptr;
}

bool VideoSourceFactory::isSupported(const std::string& type) {
#ifdef SYNCCAP_WITH_OPENCV
    const bool webcam = true;
#else
    const bool webcam = false;
#endif
    return type == "file" || type == "network" || type == "synthetic" || (type == "webcam" && webcam);
}
