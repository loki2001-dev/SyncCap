#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "source/IVideoSource.h"

namespace cv {
class VideoCapture;
}

// laptop camera through OpenCV VideoCapture. live: paced by the device
class WebcamVideoSource : public IVideoSource {
public:
    WebcamVideoSource(std::string device, int width, int height);
    ~WebcamVideoSource() override;

    WebcamVideoSource(const WebcamVideoSource&) = delete;
    WebcamVideoSource& operator=(const WebcamVideoSource&) = delete;

    bool open() override;
    read_status read(FrameBuffer& out) override;
    void close() override;
    void interrupt() override;
    video_source_info get_info() const override;

private:
    std::string _device;
    int _width;
    int _height;
    double _nativeFps;
    std::unique_ptr<cv::VideoCapture> _capture;
    std::atomic<bool> _interrupted;
};
