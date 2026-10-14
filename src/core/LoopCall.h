#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <utility>

#include "core/UvLoop.h"

/**
 * @brief runs task on the loop thread and waits for its result from another thread (an HTTP handler thread)
 *
 * nullopt when the loop queue is full or the deadline passes; the task may still run later, so it must not
 * capture anything that dies with the caller. never call it from the loop thread itself: it would wait forever
 */
template <typename Result>
std::optional<Result> callOnLoop(UvLoop& loop, std::function<Result()> task, std::chrono::milliseconds deadline) {
    auto promise = std::make_shared<std::promise<Result>>();
    std::future<Result> future = promise->get_future();
    const bool posted = loop.post([promise, task = std::move(task)]() {
        try {
            promise->set_value(task());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    if (!posted || future.wait_for(deadline) != std::future_status::ready) {
        return std::nullopt;
    }
    return future.get();
}
