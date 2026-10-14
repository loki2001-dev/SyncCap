#include "source/FfmpegVideoSource.h"

#include <cstring>

#include <array>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr int DECODER_THREADS = 2;
constexpr int64_t NANOS_PER_MICRO = 1000;

}  // namespace

FfmpegVideoSource::FfmpegVideoSource(std::string type, std::string uri, int width, int height, bool live)
    : _type(std::move(type))
    , _uri(std::move(uri))
    , _width(width)
    , _height(height)
    , _live(live)
    , _nativeFps(0.0)
    , _format(nullptr)
    , _decoder(nullptr)
    , _frame(nullptr)
    , _packet(nullptr)
    , _scaler(nullptr)
    , _streamIndex(-1)
    , _draining(false)
    , _interrupted(false)
    , _ioDeadlineNs(0) {
}

FfmpegVideoSource::~FfmpegVideoSource() {
    close();
}

bool FfmpegVideoSource::open() {
    close();
    _interrupted = false;
    _format = avformat_alloc_context();
    if (_format == nullptr) {
        LOG_ERROR(cap, hw, "format context alloc failed uri={}", _uri);
        return false;
    }
    _format->interrupt_callback.callback = on_interrupt;
    _format->interrupt_callback.opaque = this;

    AVDictionary* options = nullptr;
    for (const auto& option : get_open_options()) {
        av_dict_set(&options, option.first.c_str(), option.second.c_str(), 0);
    }
    _ioDeadlineNs = TimeUtil::steadyNowNs() + get_io_timeout_us() * NANOS_PER_MICRO;
    int result = avformat_open_input(&_format, _uri.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0) {
        // avformat_open_input frees the context on failure
        _format = nullptr;
        LOG_ERROR(cap, hw, "open failed type={} uri={} message={}", _type, _uri, describeError(result));
        return false;
    }
    result = avformat_find_stream_info(_format, nullptr);
    if (result < 0) {
        LOG_ERROR(cap, hw, "stream info failed uri={} message={}", _uri, describeError(result));
        close();
        return false;
    }
    if (!openDecoder()) {
        close();
        return false;
    }
    _ioDeadlineNs = 0;
    const AVStream* stream = _format->streams[_streamIndex];
    LOG_INFO(cap, hw, "opened type={} uri={} codec={} size={}x{} fps={:.2f} output={}x{}", _type, _uri,
             avcodec_get_name(stream->codecpar->codec_id), stream->codecpar->width, stream->codecpar->height,
             _nativeFps, _width, _height);
    return true;
}

bool FfmpegVideoSource::openDecoder() {
    const AVCodec* codec = nullptr;
    const int index = av_find_best_stream(_format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (index < 0 || codec == nullptr) {
        LOG_ERROR(cap, hw, "no video stream uri={} message={}", _uri, describeError(index));
        return false;
    }
    _streamIndex = index;
    AVStream* stream = _format->streams[index];
    _decoder = avcodec_alloc_context3(codec);
    if (_decoder == nullptr || avcodec_parameters_to_context(_decoder, stream->codecpar) < 0) {
        LOG_ERROR(cap, hw, "decoder setup failed uri={}", _uri);
        return false;
    }
    _decoder->thread_count = DECODER_THREADS;
    const int result = avcodec_open2(_decoder, codec, nullptr);
    if (result < 0) {
        LOG_ERROR(cap, hw, "decoder open failed uri={} codec={} message={}", _uri, codec->name, describeError(result));
        return false;
    }
    const AVRational rate = av_guess_frame_rate(_format, stream, nullptr);
    _nativeFps = (rate.num > 0 && rate.den > 0) ? av_q2d(rate) : 0.0;
    _frame = av_frame_alloc();
    _packet = av_packet_alloc();
    _draining = false;
    return _frame != nullptr && _packet != nullptr;
}

read_status FfmpegVideoSource::read(FrameBuffer& out) {
    if (_format == nullptr || _decoder == nullptr) {
        return read_status::error;
    }
    if (out.get_width() != _width || out.get_height() != _height) {
        LOG_ERROR(cap, hw, "output buffer size mismatch expected={}x{} actual={}x{}", _width, _height, out.get_width(),
                  out.get_height());
        return read_status::error;
    }
    while (!_interrupted) {
        bool produced = false;
        const read_status received = receiveFrame(out, produced);
        if (produced || received != read_status::ok) {
            return received;
        }
        if (_draining) {
            continue;
        }
        _ioDeadlineNs = TimeUtil::steadyNowNs() + get_io_timeout_us() * NANOS_PER_MICRO;
        const int result = av_read_frame(_format, _packet);
        _ioDeadlineNs = 0;
        if (result == AVERROR_EOF) {
            // flush the decoder so the last pictures come out before we rewind
            _draining = true;
            avcodec_send_packet(_decoder, nullptr);
            continue;
        }
        if (result < 0) {
            return on_read_error(result);
        }
        if (_packet->stream_index != _streamIndex) {
            av_packet_unref(_packet);
            continue;
        }
        const int sent = avcodec_send_packet(_decoder, _packet);
        av_packet_unref(_packet);
        if (sent < 0 && sent != AVERROR(EAGAIN)) {
            LOG_WARN(cap, prot, "decode packet failed uri={} message={}", _uri, describeError(sent));
        }
    }
    return read_status::retry;
}

read_status FfmpegVideoSource::receiveFrame(FrameBuffer& out, bool& produced) {
    produced = false;
    const int result = avcodec_receive_frame(_decoder, _frame);
    if (result == AVERROR(EAGAIN)) {
        return read_status::ok;
    }
    if (result == AVERROR_EOF) {
        return on_end_of_stream();
    }
    if (result < 0) {
        LOG_WARN(cap, prot, "decode frame failed uri={} message={}", _uri, describeError(result));
        return read_status::retry;
    }
    const bool converted = convert(_frame, out);
    av_frame_unref(_frame);
    produced = converted;
    return converted ? read_status::ok : read_status::error;
}

bool FfmpegVideoSource::convert(const AVFrame* frame, FrameBuffer& out) {
    std::array<uint8_t*, FrameBuffer::PLANE_COUNT + 1> dst{out.get_plane(0), out.get_plane(1), out.get_plane(2),
                                                           nullptr};
    std::array<int, FrameBuffer::PLANE_COUNT + 1> dstStride{out.get_stride(0), out.get_stride(1), out.get_stride(2), 0};
    const auto format = static_cast<AVPixelFormat>(frame->format);
    if ((format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P) && frame->width == _width &&
        frame->height == _height) {
        // fast path: already I420 at the output size, copy planes
        av_image_copy_plane(dst[0], dstStride[0], frame->data[0], frame->linesize[0], _width, _height);
        av_image_copy_plane(dst[1], dstStride[1], frame->data[1], frame->linesize[1], _width / 2, _height / 2);
        av_image_copy_plane(dst[2], dstStride[2], frame->data[2], frame->linesize[2], _width / 2, _height / 2);
        return true;
    }
    _scaler = sws_getCachedContext(_scaler, frame->width, frame->height, format, _width, _height, AV_PIX_FMT_YUV420P,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (_scaler == nullptr) {
        LOG_ERROR(cap, hw, "scaler setup failed uri={} from={}x{} format={}", _uri, frame->width, frame->height,
                  static_cast<int>(format));
        return false;
    }
    sws_scale(_scaler, frame->data, frame->linesize, 0, frame->height, dst.data(), dstStride.data());
    return true;
}

bool FfmpegVideoSource::rewind() {
    const int result = av_seek_frame(_format, _streamIndex, 0, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        LOG_ERROR(cap, hw, "rewind failed uri={} message={}", _uri, describeError(result));
        return false;
    }
    avcodec_flush_buffers(_decoder);
    _draining = false;
    return true;
}

void FfmpegVideoSource::close() {
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
    if (_decoder != nullptr) {
        avcodec_free_context(&_decoder);
    }
    if (_format != nullptr) {
        avformat_close_input(&_format);
    }
    _streamIndex = -1;
    _draining = false;
}

void FfmpegVideoSource::interrupt() {
    _interrupted = true;
}

video_source_info FfmpegVideoSource::get_info() const {
    video_source_info info;
    info.type = _type;
    info.uri = _uri;
    info.width = _width;
    info.height = _height;
    info.nativeFps = _nativeFps;
    info.live = _live;
    return info;
}

std::string FfmpegVideoSource::describeError(int code) const {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    return text.data();
}

int FfmpegVideoSource::on_interrupt(void* opaque) {
    const auto* source = static_cast<const FfmpegVideoSource*>(opaque);
    if (source->_interrupted) {
        return 1;
    }
    const int64_t deadline = source->_ioDeadlineNs;
    return (deadline > 0 && TimeUtil::steadyNowNs() > deadline) ? 1 : 0;
}
