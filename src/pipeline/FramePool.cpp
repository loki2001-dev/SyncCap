#include "pipeline/FramePool.h"

#include <utility>

std::shared_ptr<FramePool> FramePool::create(int width, int height, size_t capacity) {
    return std::make_shared<FramePool>(construct_token{}, width, height, capacity);
}

FramePool::FramePool(construct_token /*token*/, int width, int height, size_t capacity)
    : _width(width)
    , _height(height)
    , _capacity(capacity == 0 ? 1 : capacity)
    , _created(0)
    , _exhausted(0)
    , _closed(false) {
    _free.reserve(_capacity);
}

std::shared_ptr<FrameBuffer> FramePool::acquire(std::chrono::microseconds timeout) {
    std::unique_ptr<FrameBuffer> buffer;
    {
        std::unique_lock<std::mutex> lock(_mutex);
        if (_free.empty() && _created < _capacity && !_closed) {
            // grow lazily up to the ceiling; allocation happens outside the lock below
            _created += 1;
        } else {
            const bool ready = _available.wait_for(lock, timeout, [this]() {
                return _closed || !_free.empty();
            });
            if (!ready || _closed || _free.empty()) {
                _exhausted += 1;
                return nullptr;
            }
            buffer = std::move(_free.back());
            _free.pop_back();
        }
    }
    if (!buffer) {
        buffer = std::make_unique<FrameBuffer>(_width, _height);
    }
    std::weak_ptr<FramePool> weakPool = weak_from_this();
    FrameBuffer* raw = buffer.release();
    return std::shared_ptr<FrameBuffer>(raw, [weakPool](FrameBuffer* released) {
        std::unique_ptr<FrameBuffer> owned(released);
        if (auto pool = weakPool.lock()) {
            pool->release(std::move(owned));
        }
    });
}

void FramePool::release(std::unique_ptr<FrameBuffer> buffer) {
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _free.push_back(std::move(buffer));
    }
    _available.notify_one();
}

void FramePool::close() {
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _closed = true;
    }
    _available.notify_all();
}

size_t FramePool::get_capacity() const {
    return _capacity;
}

size_t FramePool::get_available() const {
    const std::lock_guard<std::mutex> lock(_mutex);
    return _free.size() + (_capacity - _created);
}

uint64_t FramePool::get_exhausted_total() const {
    const std::lock_guard<std::mutex> lock(_mutex);
    return _exhausted;
}
