#pragma once

#include <cstdint>
#include <vector>

#include "model/FrameBuffer.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * @brief downscales an I420 picture and encodes it as a baseline JPEG (FFmpeg mjpeg). one thread per instance
 *
 * the scaler converts limited (TV) range to the full range a JPEG viewer expects
 */
class JpegEncoder {
public:
    JpegEncoder();
    ~JpegEncoder();

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // quality: MJPEG qscale, 2 (best) .. 31
    bool open(int outputWidth, int outputHeight, int quality);
    bool encode(const FrameBuffer& picture, std::vector<uint8_t>& jpeg);
    void close();

private:
    int _width;
    int _height;
    int _quality;
    int _sourceWidth;
    int _sourceHeight;
    AVCodecContext* _context;
    AVFrame* _frame;
    AVPacket* _packet;
    SwsContext* _scaler;
};
