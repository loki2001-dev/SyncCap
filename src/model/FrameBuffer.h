#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief one I420 (YUV 4:2:0 planar) picture in a single contiguous allocation
 *
 * layout: Y (width*height), U (width/2*height/2), V (width/2*height/2). width and height are even
 */
class FrameBuffer {
public:
    FrameBuffer(int width, int height);

    int get_width() const;
    int get_height() const;
    size_t get_size() const;

    uint8_t* get_plane(int index);
    const uint8_t* get_plane(int index) const;
    int get_stride(int index) const;

    static size_t computeSize(int width, int height);

    static constexpr int PLANE_COUNT = 3;

private:
    int _width;
    int _height;
    std::vector<uint8_t> _data;
};
