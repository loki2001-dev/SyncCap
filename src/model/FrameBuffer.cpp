#include "model/FrameBuffer.h"

namespace {

constexpr int CHROMA_DIVISOR = 2;

}  // namespace

FrameBuffer::FrameBuffer(int width, int height)
    : _width(width)
    , _height(height)
    , _data(computeSize(width, height)) {
}

int FrameBuffer::get_width() const {
    return _width;
}

int FrameBuffer::get_height() const {
    return _height;
}

size_t FrameBuffer::get_size() const {
    return _data.size();
}

uint8_t* FrameBuffer::get_plane(int index) {
    return const_cast<uint8_t*>(static_cast<const FrameBuffer*>(this)->get_plane(index));
}

const uint8_t* FrameBuffer::get_plane(int index) const {
    const size_t lumaSize = static_cast<size_t>(_width) * static_cast<size_t>(_height);
    const size_t chromaSize = lumaSize / (CHROMA_DIVISOR * CHROMA_DIVISOR);
    switch (index) {
        case 0:
            return _data.data();
        case 1:
            return _data.data() + lumaSize;
        case 2:
            return _data.data() + lumaSize + chromaSize;
        default:
            return nullptr;
    }
}

int FrameBuffer::get_stride(int index) const {
    return index == 0 ? _width : _width / CHROMA_DIVISOR;
}

size_t FrameBuffer::computeSize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const size_t lumaSize = static_cast<size_t>(width) * static_cast<size_t>(height);
    return lumaSize + lumaSize / CHROMA_DIVISOR;
}
