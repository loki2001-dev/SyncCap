#pragma once

#include <vector>

#include "config/AppConfig.h"
#include "model/VideoFrame.h"

/**
 * @brief H.264 encoder for one channel. one encoder thread owns one instance
 *
 * encode() appends zero or more Annex-B access units to packets. forceKey makes the
 * picture an IDR with SPS/PPS in front so a new segment file starts decodable
 */
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual bool open(int channel, int width, int height, int fps, const encoder_config& config) = 0;
    virtual bool encode(const video_frame& frame, bool forceKey, std::vector<encoded_packet>& packets) = 0;
    virtual void flush(std::vector<encoded_packet>& packets) = 0;
    virtual void close() = 0;
};
