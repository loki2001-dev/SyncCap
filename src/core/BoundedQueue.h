#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

enum class overflow_policy {
    drop_oldest,
    reject
};

enum class push_result {
    accepted,
    dropped_oldest,
    rejected
};

/**
 * @brief queue with a ceiling and an overflow policy. not thread-safe
 *
 * the caller logs get_dropped_total() whenever push is not accepted
 */
template <typename T>
class BoundedQueue {
public:
    BoundedQueue(size_t capacity, overflow_policy policy)
        : _capacity(capacity == 0 ? 1 : capacity)
        , _policy(policy) {
    }

    push_result push(T item) {
        std::optional<T> evicted;
        return push(std::move(item), evicted);
    }

    // evicted receives the dropped item so the caller can destroy it outside its lock
    push_result push(T item, std::optional<T>& evicted) {
        if (_items.size() < _capacity) {
            _items.push_back(std::move(item));
            return push_result::accepted;
        }
        _droppedTotal += 1;
        if (_policy == overflow_policy::reject) {
            return push_result::rejected;
        }
        evicted = std::move(_items.front());
        _items.pop_front();
        _items.push_back(std::move(item));
        return push_result::dropped_oldest;
    }

    std::optional<T> pop() {
        if (_items.empty()) {
            return std::nullopt;
        }
        T item = std::move(_items.front());
        _items.pop_front();
        return item;
    }

    // moves every item out so the caller can destroy them outside its lock
    std::deque<T> takeAll() {
        std::deque<T> items;
        items.swap(_items);
        return items;
    }

    size_t get_size() const {
        return _items.size();
    }

    size_t get_capacity() const {
        return _capacity;
    }

    uint64_t get_dropped_total() const {
        return _droppedTotal;
    }

private:
    std::deque<T> _items;
    size_t _capacity;
    overflow_policy _policy;
    uint64_t _droppedTotal = 0;
};
