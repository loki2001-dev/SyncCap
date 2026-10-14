#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "codec/IVideoEncoder.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

/**
 * @brief libx264 through libavcodec. refuses any other H.264 encoder
 *
 * no global header, so libx264 repeats SPS/PPS before every IDR; no B-frames, so packets come out
 * in input order and every packet maps back to the tick of its picture
 */
class X264Encoder : public IVideoEncoder {
public:
    X264Encoder();
    ~X264Encoder() override;

    X264Encoder(const X264Encoder&) = delete;
    X264Encoder& operator=(const X264Encoder&) = delete;

    bool open(int channel, int width, int height, int fps, const encoder_config& config) override;
    bool encode(const video_frame& frame, bool forceKey, std::vector<encoded_packet>& packets) override;
    void flush(std::vector<encoded_packet>& packets) override;
    void close() override;

private:
    struct frame_tag {
        uint64_t tick;
        int64_t segment;
    };

    bool receivePackets(std::vector<encoded_packet>& packets);

    int _channel;
    int _width;
    int _height;
    AVCodecContext* _context;
    AVFrame* _frame;
    AVPacket* _packet;
    int64_t _nextPts;
    std::map<int64_t, frame_tag> _tags;
};
