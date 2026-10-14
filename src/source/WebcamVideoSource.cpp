#include "source/WebcamVideoSource.h"

#include <cstring>

#include <utility>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "util/log/Logger.h"

WebcamVideoSource::WebcamVideoSource(std::string device, int width, int height)
    : _device(std::move(device))
    , _width(width)
    , _height(height)
    , _nativeFps(0.0)
    , _interrupted(false) {
}

WebcamVideoSource::~WebcamVideoSource() {
    close();
}

bool WebcamVideoSource::open() {
    close();
    _interrupted = false;
    _capture = std::make_unique<cv::VideoCapture>();
    bool opened = false;
    // a plain number is a device index, anything else a device path or pipeline
    if (!_device.empty() && _device.find_first_not_of("0123456789") == std::string::npos) {
        opened = _capture->open(std::stoi(_device));
    } else {
        opened = _capture->open(_device);
    }
    if (!opened) {
        LOG_ERROR(cap, hw, "webcam open failed device={}", _device);
        _capture.reset();
        return false;
    }
    _capture->set(cv::CAP_PROP_FRAME_WIDTH, _width);
    _capture->set(cv::CAP_PROP_FRAME_HEIGHT, _height);
    _nativeFps = _capture->get(cv::CAP_PROP_FPS);
    LOG_INFO(cap, hw, "webcam opened device={} size={}x{} fps={:.2f}", _device, _capture->get(cv::CAP_PROP_FRAME_WIDTH),
             _capture->get(cv::CAP_PROP_FRAME_HEIGHT), _nativeFps);
    return true;
}

read_status WebcamVideoSource::read(FrameBuffer& out) {
    if (!_capture || _interrupted) {
        return read_status::retry;
    }
    cv::Mat bgr;
    if (!_capture->read(bgr) || bgr.empty()) {
        LOG_WARN(cap, hw, "webcam read failed device={}", _device);
        return read_status::error;
    }
    if (bgr.cols != _width || bgr.rows != _height) {
        cv::Mat resized;
        cv::resize(bgr, resized, cv::Size(_width, _height), 0, 0, cv::INTER_LINEAR);
        bgr = resized;
    }
    // write straight into the pooled buffer: I420 is a single (height*3/2) x width plane
    cv::Mat i420(_height * 3 / 2, _width, CV_8UC1, out.get_plane(0));
    cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
    return read_status::ok;
}

void WebcamVideoSource::close() {
    if (_capture) {
        _capture->release();
        _capture.reset();
    }
}

void WebcamVideoSource::interrupt() {
    _interrupted = true;
}

video_source_info WebcamVideoSource::get_info() const {
    video_source_info info;
    info.type = "webcam";
    info.uri = _device;
    info.width = _width;
    info.height = _height;
    info.nativeFps = _nativeFps;
    info.live = true;
    return info;
}
