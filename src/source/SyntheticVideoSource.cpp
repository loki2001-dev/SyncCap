#include "source/SyntheticVideoSource.h"

#include <cstring>

#include <algorithm>
#include <array>

namespace {

constexpr int BAR_COUNT = 8;
constexpr std::array<uint8_t, BAR_COUNT> BAR_LUMA{235, 210, 170, 145, 106, 81, 41, 16};
constexpr uint8_t CHROMA_NEUTRAL = 128;
constexpr int CHANNEL_TINT_STEP = 24;
constexpr uint8_t MOVER_LUMA = 235;
constexpr int MOVER_WIDTH = 48;
constexpr int MOVER_SPEED_PX = 12;
constexpr int COUNTER_BITS = 24;
constexpr int COUNTER_CELL = 32;
constexpr uint8_t BIT_ON = 235;
constexpr uint8_t BIT_OFF = 16;

}  // namespace

SyntheticVideoSource::SyntheticVideoSource(int channel, int width, int height, double fps)
    : _channel(channel)
    , _width(width)
    , _height(height)
    , _fps(fps)
    , _sequence(0)
    , _interrupted(false) {
}

bool SyntheticVideoSource::open() {
    _interrupted = false;
    _sequence = 0;
    buildBase();
    return !_base.empty();
}

void SyntheticVideoSource::buildBase() {
    FrameBuffer base(_width, _height);
    uint8_t* luma = base.get_plane(0);
    for (int y = 0; y < _height; ++y) {
        for (int x = 0; x < _width; ++x) {
            const int bar = std::min(BAR_COUNT - 1, x * BAR_COUNT / _width);
            luma[static_cast<size_t>(y) * static_cast<size_t>(_width) + static_cast<size_t>(x)] = BAR_LUMA[bar];
        }
    }
    // each channel gets its own tint so the recordings are told apart at a glance
    const auto tintU = static_cast<uint8_t>(CHROMA_NEUTRAL + (_channel % 4) * CHANNEL_TINT_STEP - CHANNEL_TINT_STEP);
    const auto tintV = static_cast<uint8_t>(CHROMA_NEUTRAL - (_channel % 4) * CHANNEL_TINT_STEP + CHANNEL_TINT_STEP);
    const size_t chromaSize = static_cast<size_t>(_width / 2) * static_cast<size_t>(_height / 2);
    std::memset(base.get_plane(1), tintU, chromaSize);
    std::memset(base.get_plane(2), tintV, chromaSize);
    _base.assign(base.get_plane(0), base.get_plane(0) + base.get_size());
}

read_status SyntheticVideoSource::read(FrameBuffer& out) {
    if (_interrupted) {
        return read_status::retry;
    }
    if (out.get_size() != _base.size()) {
        return read_status::error;
    }
    std::memcpy(out.get_plane(0), _base.data(), _base.size());
    uint8_t* luma = out.get_plane(0);
    const size_t stride = static_cast<size_t>(_width);

    const int moverX = static_cast<int>((_sequence * MOVER_SPEED_PX) % static_cast<uint64_t>(_width));
    const int moverEnd = std::min(_width, moverX + MOVER_WIDTH);
    for (int y = 0; y < _height; ++y) {
        std::memset(luma + static_cast<size_t>(y) * stride + static_cast<size_t>(moverX), MOVER_LUMA,
                    static_cast<size_t>(moverEnd - moverX));
    }

    // frame number in binary along the top edge
    const int cells = std::min(COUNTER_BITS, _width / COUNTER_CELL);
    const int cellHeight = std::min(COUNTER_CELL, _height);
    for (int bit = 0; bit < cells; ++bit) {
        const uint8_t value = ((_sequence >> bit) & 1U) != 0 ? BIT_ON : BIT_OFF;
        for (int y = 0; y < cellHeight; ++y) {
            std::memset(luma + static_cast<size_t>(y) * stride + static_cast<size_t>(bit * COUNTER_CELL), value,
                        COUNTER_CELL);
        }
    }
    _sequence += 1;
    return read_status::ok;
}

void SyntheticVideoSource::close() {
    _base.clear();
    _base.shrink_to_fit();
}

void SyntheticVideoSource::interrupt() {
    _interrupted = true;
}

video_source_info SyntheticVideoSource::get_info() const {
    video_source_info info;
    info.type = "synthetic";
    info.uri = "synthetic://" + std::to_string(_channel);
    info.width = _width;
    info.height = _height;
    info.nativeFps = _fps;
    info.live = false;
    return info;
}
