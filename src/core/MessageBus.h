#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/BoundedQueue.h"

class UvLoop;

using subscription_id_t = uint64_t;

/**
 * @brief typed publish and subscribe. loop thread only. producers never know their consumers
 *
 * publish() queues the message; delivery happens on a later loop turn, so a handler that
 * publishes never re-enters another handler
 */
class MessageBus {
public:
    MessageBus(UvLoop& loop, size_t capacity);

    MessageBus(const MessageBus&) = delete;
    MessageBus& operator=(const MessageBus&) = delete;

    template <typename T>
    subscription_id_t subscribe(std::function<void(const T&)> handler) {
        const subscription_id_t id = ++_lastId;
        _handlers[std::type_index(typeid(T))].push_back({id, [handler](const void* message) {
                                                             handler(*static_cast<const T*>(message));
                                                         }});
        return id;
    }

    void unsubscribe(subscription_id_t id);

    template <typename T>
    void publish(T message) {
        auto shared = std::make_shared<const T>(std::move(message));
        enqueue([this, shared]() {
            dispatch(std::type_index(typeid(T)), shared.get());
        });
    }

    // delivers everything queued so far. tests call it directly
    void drain();

    uint64_t get_dropped_total() const;

private:
    struct handler_entry {
        subscription_id_t id;
        std::function<void(const void*)> invoke;
    };

    void enqueue(std::function<void()> task);
    void dispatch(std::type_index type, const void* message);

    UvLoop& _loop;
    BoundedQueue<std::function<void()>> _queue;
    std::unordered_map<std::type_index, std::vector<handler_entry>> _handlers;
    subscription_id_t _lastId;
    bool _drainScheduled;
};
