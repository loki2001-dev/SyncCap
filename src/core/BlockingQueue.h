#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "core/BoundedQueue.h"

/**
 * @brief thread-safe bounded producer-consumer queue (EX-P1-001 data plane)
 *
 * push never blocks: a full queue drops the oldest item or rejects, as the policy says.
 * pop blocks until an item arrives or close() is called. one mutex, predicate waits only,
 * evicted items are destroyed after the lock is released (their deleters may take other locks)
 */
template <typename T>
class BlockingQueue {
public:
    BlockingQueue(size_t capacity, overflow_policy policy)
        : _queue(capacity, policy) {
    }

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    push_result push(T item) {
        std::optional<T> evicted;
        push_result result = push_result::rejected;
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            if (_closed) {
                _rejectedClosed += 1;
                return push_result::rejected;
            }
            result = _queue.push(std::move(item), evicted);
            if (_queue.get_size() > _highWater) {
                _highWater = _queue.get_size();
            }
        }
        if (result != push_result::rejected) {
            _notEmpty.notify_one();
        }
        return result;
    }

    // false: closed and drained
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(_mutex);
        _notEmpty.wait(lock, [this]() {
            return _closed || _queue.get_size() > 0;
        });
        return takeLocked(out);
    }

    // false: timed out, or closed and drained
    bool popFor(T& out, std::chrono::microseconds timeout) {
        std::unique_lock<std::mutex> lock(_mutex);
        _notEmpty.wait_for(lock, timeout, [this]() {
            return _closed || _queue.get_size() > 0;
        });
        return takeLocked(out);
    }

    bool tryPop(T& out) {
        const std::lock_guard<std::mutex> lock(_mutex);
        return takeLocked(out);
    }

    // wakes every consumer. items already queued can still be popped
    void close() {
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            _closed = true;
        }
        _notEmpty.notify_all();
    }

    // drops everything still queued. returns how many were dropped. items die outside the lock
    size_t clear() {
        std::deque<T> drained;
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            drained = _queue.takeAll();
        }
        return drained.size();
    }

    bool isClosed() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _closed;
    }

    size_t get_size() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _queue.get_size();
    }

    size_t get_capacity() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _queue.get_capacity();
    }

    size_t get_free() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _queue.get_capacity() - _queue.get_size();
    }

    size_t get_high_water() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _highWater;
    }

    uint64_t get_dropped_total() const {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _queue.get_dropped_total() + _rejectedClosed;
    }

private:
    bool takeLocked(T& out) {
        auto item = _queue.pop();
        if (!item) {
            return false;
        }
        out = std::move(*item);
        return true;
    }

    mutable std::mutex _mutex;
    std::condition_variable _notEmpty;
    BoundedQueue<T> _queue;
    bool _closed = false;
    size_t _highWater = 0;
    uint64_t _rejectedClosed = 0;
};
