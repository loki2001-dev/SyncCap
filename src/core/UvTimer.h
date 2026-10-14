#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <uv.h>

#include "core/UvLoop.h"

/**
 * @brief repeating or one-shot libuv timer. loop thread only
 *
 * the handle memory is released in the uv_close callback, never before
 */
class UvTimer {
public:
    UvTimer(UvLoop& loop, std::string name);
    ~UvTimer();

    UvTimer(const UvTimer&) = delete;
    UvTimer& operator=(const UvTimer&) = delete;

    void start(uint64_t timeoutMs, uint64_t repeatMs, std::function<void()> callback);
    void stop();
    bool isActive() const;

private:
    struct timer_handle {
        uv_timer_t handle;
        UvTimer* owner;
    };

    // libuv callback
    static void on_timer(uv_timer_t* handle);
    static void on_close(uv_handle_t* handle);

    UvLoop& _loop;
    std::string _name;
    timer_handle* _timer;
    std::function<void()> _callback;
};
