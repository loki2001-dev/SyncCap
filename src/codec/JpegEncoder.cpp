#include "codec/JpegEncoder.h"

#include <array>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "util/log/Logger.h"

namespace {

constexpr int FRAME_ALIGN = 32;
constexpr int PREVIEW_TIME_BASE = 30;

std::string describeError(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    return text.data();
}

}  // namespace

JpegEncoder::JpegEncoder()
    : _width(0)
    , _height(0)
    , _quality(0)
    , _sourceWidth(0)
    , _sourceHeight(0)
    , _context(nullptr)
    , _frame(nullptr)
    , _packet(nullptr)
    , _scaler(nullptr) {
}

JpegEncoder::~JpegEncoder() {
    close();
}

bool JpegEncoder::open(int outputWidth, int outputHeight, int quality) {
    close();
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (codec == nullptr) {
        LOG_ERROR(enc, sys, "mjpeg encoder not available");
        return false;
    }
    _context = avcodec_alloc_context3(codec);
    if (_context == nullptr) {
        return false;
    }
    _width = outputWidth;
    _height = outputHeight;
    _quality = quality;
    _context->width = outputWidth;
    _context->height = outputHeight;
    _context->pix_fmt = AV_PIX_FMT_YUVJ420P;
    _context->color_range = AVCOL_RANGE_JPEG;
    _context->time_base = AVRational{1, PREVIEW_TIME_BASE};
    // fixed quantizer: qscale times lambda
    _context->flags |= AV_CODEC_FLAG_QSCALE;
    _context->global_quality = quality * FF_QP2LAMBDA;
    const int result = avcodec_open2(_context, codec, nullptr);
    if (result < 0) {
        LOG_ERROR(enc, sys, "mjpeg open failed message={}", describeError(result));
        close();
        return false;
    }
    _frame = av_frame_alloc();
    _packet = av_packet_alloc();
    if (_frame == nullptr || _packet == nullptr) {
        close();
        return false;
    }
    _frame->format = AV_PIX_FMT_YUVJ420P;
    _frame->width = outputWidth;
    _frame->height = outputHeight;
    _frame->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(_frame, FRAME_ALIGN) < 0) {
        close();
        return false;
    }
    return true;
}

bool JpegEncoder::encode(const FrameBuffer& picture, std::vector<uint8_t>& jpeg) {
    if (_context == nullptr) {
        return false;
    }
    if (_scaler == nullptr || picture.get_width() != _sourceWidth || picture.get_height() != _sourceHeight) {
        _sourceWidth = picture.get_width();
        _sourceHeight = picture.get_height();
        // YUV420P (TV range) to YUVJ420P (full range): swscale expands the range on the way
        _scaler = sws_getCachedContext(_scaler, _sourceWidth, _sourceHeight, AV_PIX_FMT_YUV420P, _width, _height,
                                       AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (_scaler == nullptr) {
            LOG_ERROR(enc, sys, "preview scaler setup failed from={}x{} to={}x{}", _sourceWidth, _sourceHeight, _width,
                      _height);
            return false;
        }
    }
    if (av_frame_make_writable(_frame) < 0) {
        return false;
    }
    const std::array<const uint8_t*, FrameBuffer::PLANE_COUNT> source{picture.get_plane(0), picture.get_plane(1),
                                                                      picture.get_plane(2)};
    const std::array<int, FrameBuffer::PLANE_COUNT> stride{picture.get_stride(0), picture.get_stride(1),
                                                           picture.get_stride(2)};
    sws_scale(_scaler, source.data(), stride.data(), 0, _sourceHeight, _frame->data, _frame->linesize);
    _frame->quality = _context->global_quality;
    _frame->pts += 1;

    int result = avcodec_send_frame(_context, _frame);
    if (result < 0) {
        LOG_WARN(enc, sys, "jpeg send failed message={}", describeError(result));
        return false;
    }
    result = avcodec_receive_packet(_context, _packet);
    if (result < 0) {
        LOG_WARN(enc, sys, "jpeg receive failed message={}", describeError(result));
        return false;
    }
    jpeg.assign(_packet->data, _packet->data + _packet->size);
    av_packet_unref(_packet);
    return true;
}

void JpegEncoder::close() {
    if (_scaler != nullptr) {
        sws_freeContext(_scaler);
        _scaler = nullptr;
    }
    if (_packet != nullptr) {
        av_packet_free(&_packet);
    }
    if (_frame != nullptr) {
        av_frame_free(&_frame);
    }
    if (_context != nullptr) {
        avcodec_free_context(&_context);
    }
    _sourceWidth = 0;
    _sourceHeight = 0;
}
