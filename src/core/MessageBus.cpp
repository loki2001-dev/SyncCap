#include "core/MessageBus.h"

#include "core/UvLoop.h"
#include "util/log/Logger.h"

MessageBus::MessageBus(UvLoop& loop, size_t capacity)
    : _loop(loop)
    , _queue(capacity, overflow_policy::drop_oldest)
    , _lastId(0)
    , _drainScheduled(false) {
}

void MessageBus::unsubscribe(subscription_id_t id) {
    for (auto& entry : _handlers) {
        auto& list = entry.second;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->id == id) {
                list.erase(it);
                return;
            }
        }
    }
}

void MessageBus::drain() {
    _drainScheduled = false;
    while (auto task = _queue.pop()) {
        (*task)();
    }
}

uint64_t MessageBus::get_dropped_total() const {
    return _queue.get_dropped_total();
}

void MessageBus::enqueue(std::function<void()> task) {
    const push_result result = _queue.push(std::move(task));
    if (result != push_result::accepted) {
        LOG_WARN(bus, sys, "queue overflow dropped_total={}", _queue.get_dropped_total());
    }
    if (!_drainScheduled) {
        _drainScheduled = true;
        if (!_loop.post([this]() {
                drain();
            })) {
            _drainScheduled = false;
            LOG_ERROR(bus, sys, "drain post rejected queued={}", _queue.get_size());
        }
    }
}

void MessageBus::dispatch(std::type_index type, const void* message) {
    const auto found = _handlers.find(type);
    if (found == _handlers.end()) {
        return;
    }
    // copy: a handler may subscribe or unsubscribe while we iterate
    const std::vector<handler_entry> handlers = found->second;
    for (const auto& entry : handlers) {
        entry.invoke(message);
    }
}
