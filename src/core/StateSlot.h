#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

// readers hold an immutable snapshot. writers copy, edit, then swap the whole value
template <typename T>
class StateSlot {
public:
    std::shared_ptr<const T> get_snapshot() const {
        const std::shared_lock<std::shared_mutex> lock(_mutex);
        return _value;
    }

    template <typename Edit>
    void update(Edit&& edit) {
        std::shared_ptr<const T> previous;
        {
            const std::unique_lock<std::shared_mutex> lock(_mutex);
            auto next = std::make_shared<T>(*_value);
            std::forward<Edit>(edit)(*next);
            previous = std::move(_value);
            _value = std::move(next);
        }
    }

private:
    mutable std::shared_mutex _mutex;
    std::shared_ptr<const T> _value = std::make_shared<const T>();
};
