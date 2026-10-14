#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "model/FrameBuffer.h"

/**
 * @brief fixed set of I420 buffers for one channel. the pool size is the frame memory ceiling (P5)
 *
 * acquire() hands out a buffer whose deleter returns it to the pool. a buffer released after the
 * pool is gone is simply freed
 */
class FramePool : public std::enable_shared_from_this<FramePool> {
private:
    struct construct_token {};

public:
    static std::shared_ptr<FramePool> create(int width, int height, size_t capacity);

    // use create(): the pool must be owned by a shared_ptr for its buffer deleters
    FramePool(construct_token token, int width, int height, size_t capacity);

    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;

    // waits up to timeout for a free buffer. nullptr: exhausted or closed
    std::shared_ptr<FrameBuffer> acquire(std::chrono::microseconds timeout);

    // wakes every waiter; acquire() returns nullptr afterwards
    void close();

    size_t get_capacity() const;
    size_t get_available() const;
    uint64_t get_exhausted_total() const;

private:
    void release(std::unique_ptr<FrameBuffer> buffer);

    int _width;
    int _height;
    size_t _capacity;
    mutable std::mutex _mutex;
    std::condition_variable _available;
    std::vector<std::unique_ptr<FrameBuffer>> _free;
    size_t _created;
    uint64_t _exhausted;
    bool _closed;
};
