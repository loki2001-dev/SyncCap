#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "source/IVideoSource.h"

/**
 * @brief generated test pattern: color bars tinted per channel, a moving bar and the frame number in bits
 *
 * needs no file, camera or network; used for tests and as the default simulation input
 */
class SyntheticVideoSource : public IVideoSource {
public:
    SyntheticVideoSource(int channel, int width, int height, double fps);

    bool open() override;
    read_status read(FrameBuffer& out) override;
    void close() override;
    void interrupt() override;
    video_source_info get_info() const override;

private:
    void buildBase();

    int _channel;
    int _width;
    int _height;
    double _fps;
    uint64_t _sequence;
    std::vector<uint8_t> _base;
    std::atomic<bool> _interrupted;
};
