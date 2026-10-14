#include "codec/X264Encoder.h"

#include <array>
#include <iterator>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "util/log/Logger.h"

namespace {

constexpr const char* X264_ENCODER_NAME = "libx264";
constexpr int64_t BITS_PER_KILOBIT = 1000;

std::string describeError(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    return text.data();
}

// AVBufferRef free callback: drops our reference to the pooled picture
void releasePooledBuffer(void* opaque, uint8_t*) {
    std::unique_ptr<std::shared_ptr<const FrameBuffer>> holder(
        static_cast<std::shared_ptr<const FrameBuffer>*>(opaque));
}

}  // namespace

X264Encoder::X264Encoder()
    : _channel(0)
    , _width(0)
    , _height(0)
    , _context(nullptr)
    , _frame(nullptr)
    , _packet(nullptr)
    , _nextPts(0) {
}

X264Encoder::~X264Encoder() {
    close();
}

bool X264Encoder::open(int channel, int width, int height, int fps, const encoder_config& config) {
    close();
    _channel = channel;
    _width = width;
    _height = height;
    const AVCodec* codec = avcodec_find_encoder_by_name(X264_ENCODER_NAME);
    if (codec == nullptr) {
        LOG_ERROR(enc, sys, "libx264 encoder not available in this FFmpeg build channel={}", channel);
        return false;
    }
    _context = avcodec_alloc_context3(codec);
    if (_context == nullptr) {
        LOG_ERROR(enc, sys, "encoder context alloc failed channel={}", channel);
        return false;
    }
    _context->width = width;
    _context->height = height;
    _context->time_base = AVRational{1, fps};
    _context->framerate = AVRational{fps, 1};
    _context->pix_fmt = AV_PIX_FMT_YUV420P;
    _context->gop_size = config.gop > 0 ? config.gop : fps;
    _context->max_b_frames = 0;
    _context->bit_rate = static_cast<int64_t>(config.bitrateKbps) * BITS_PER_KILOBIT;
    _context->rc_max_rate = _context->bit_rate;
    _context->rc_buffer_size = static_cast<int>(_context->bit_rate);
    _context->thread_count = config.threads;
    av_opt_set(_context->priv_data, "preset", config.preset.c_str(), 0);
    av_opt_set(_context->priv_data, "tune", config.tune.c_str(), 0);
    // a forced key picture must be an IDR, otherwise a new segment file would not start decodable
    av_opt_set(_context->priv_data, "forced-idr", "1", 0);

    const int result = avcodec_open2(_context, codec, nullptr);
    if (result < 0) {
        LOG_ERROR(enc, sys, "encoder open failed channel={} preset={} message={}", channel, config.preset,
                  describeError(result));
        close();
        return false;
    }
    _frame = av_frame_alloc();
    _packet = av_packet_alloc();
    if (_frame == nullptr || _packet == nullptr) {
        LOG_ERROR(enc, sys, "encoder frame alloc failed channel={}", channel);
        close();
        return false;
    }
    _nextPts = 0;
    LOG_INFO(enc, life,
             "opened channel={} codec={} size={}x{} fps={} preset={} tune={} bitrate_kbps={} gop={} threads={}",
             channel, codec->name, width, height, fps, config.preset, config.tune, config.bitrateKbps,
             _context->gop_size, config.threads);
    return true;
}

bool X264Encoder::encode(const video_frame& frame, bool forceKey, std::vector<encoded_packet>& packets) {
    if (_context == nullptr || !frame.buffer) {
        return false;
    }
    const FrameBuffer& buffer = *frame.buffer;
    if (buffer.get_width() != _width || buffer.get_height() != _height) {
        LOG_ERROR(enc, prot, "frame size mismatch channel={} expected={}x{} actual={}x{}", _channel, _width, _height,
                  buffer.get_width(), buffer.get_height());
        return false;
    }
    // zero copy: the AVFrame borrows the pooled picture and holds a reference until libavcodec lets go
    auto holder = std::make_unique<std::shared_ptr<const FrameBuffer>>(frame.buffer);
    AVBufferRef* reference = av_buffer_create(const_cast<uint8_t*>(buffer.get_plane(0)), buffer.get_size(),
                                              releasePooledBuffer, holder.get(), AV_BUFFER_FLAG_READONLY);
    if (reference == nullptr) {
        LOG_ERROR(enc, sys, "buffer wrap failed channel={}", _channel);
        return false;
    }
    holder.release();

    av_frame_unref(_frame);
    _frame->format = AV_PIX_FMT_YUV420P;
    _frame->width = _width;
    _frame->height = _height;
    _frame->buf[0] = reference;
    for (int plane = 0; plane < FrameBuffer::PLANE_COUNT; ++plane) {
        _frame->data[plane] = const_cast<uint8_t*>(buffer.get_plane(plane));
        _frame->linesize[plane] = buffer.get_stride(plane);
    }
    _frame->pts = _nextPts;
    _frame->pict_type = forceKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    _tags[_nextPts] = frame_tag{frame.tick, frame.segment};
    _nextPts += 1;

    const int result = avcodec_send_frame(_context, _frame);
    av_frame_unref(_frame);
    if (result < 0) {
        LOG_ERROR(enc, sys, "send frame failed channel={} tick={} message={}", _channel, frame.tick,
                  describeError(result));
        _tags.erase(_nextPts - 1);
        return false;
    }
    return receivePackets(packets);
}

void X264Encoder::flush(std::vector<encoded_packet>& packets) {
    if (_context == nullptr) {
        return;
    }
    avcodec_send_frame(_context, nullptr);
    receivePackets(packets);
}

bool X264Encoder::receivePackets(std::vector<encoded_packet>& packets) {
    while (true) {
        const int result = avcodec_receive_packet(_context, _packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return true;
        }
        if (result < 0) {
            LOG_ERROR(enc, sys, "receive packet failed channel={} message={}", _channel, describeError(result));
            return false;
        }
        encoded_packet packet;
        packet.channel = _channel;
        packet.pts = _packet->pts;
        packet.keyFrame = (_packet->flags & AV_PKT_FLAG_KEY) != 0;
        packet.data.assign(_packet->data, _packet->data + _packet->size);
        const auto tag = _tags.find(_packet->pts);
        if (tag != _tags.end()) {
            packet.tick = tag->second.tick;
            packet.segment = tag->second.segment;
            _tags.erase(_tags.begin(), std::next(tag));
        } else {
            LOG_WARN(enc, prot, "packet without tag channel={} pts={}", _channel, _packet->pts);
        }
        av_packet_unref(_packet);
        packets.push_back(std::move(packet));
    }
}

void X264Encoder::close() {
    if (_packet != nullptr) {
        av_packet_free(&_packet);
    }
    if (_frame != nullptr) {
        av_frame_free(&_frame);
    }
    if (_context != nullptr) {
        avcodec_free_context(&_context);
    }
    _tags.clear();
}
