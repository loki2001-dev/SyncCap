#include "core/UvTimer.h"

#include <chrono>
#include <exception>
#include <memory>
#include <utility>

#include "util/log/Logger.h"

UvTimer::UvTimer(UvLoop& loop, std::string name)
    : _loop(loop)
    , _name(std::move(name))
    , _timer(nullptr) {
    auto timer = std::make_unique<timer_handle>();
    timer->owner = this;
    uv_timer_init(_loop.get_handle(), &timer->handle);
    timer->handle.data = timer.get();
    _timer = timer.release();
}

UvTimer::~UvTimer() {
    if (_timer == nullptr) {
        return;
    }
    uv_timer_stop(&_timer->handle);
    _timer->owner = nullptr;
    // ownership moves to on_close
    uv_close(reinterpret_cast<uv_handle_t*>(&_timer->handle), on_close);
    _timer = nullptr;
}

void UvTimer::start(uint64_t timeoutMs, uint64_t repeatMs, std::function<void()> callback) {
    _callback = std::move(callback);
    uv_timer_start(&_timer->handle, on_timer, timeoutMs, repeatMs);
}

void UvTimer::stop() {
    if (_timer != nullptr) {
        uv_timer_stop(&_timer->handle);
    }
}

bool UvTimer::isActive() const {
    return _timer != nullptr && uv_is_active(reinterpret_cast<const uv_handle_t*>(&_timer->handle)) != 0;
}

void UvTimer::on_timer(uv_timer_t* handle) {
    auto* timer = static_cast<timer_handle*>(handle->data);
    UvTimer* owner = timer->owner;
    if (owner == nullptr || !owner->_callback) {
        return;
    }
    const auto begin = std::chrono::steady_clock::now();
    try {
        owner->_callback();
    } catch (const std::exception& e) {
        LOG_ERROR(loop, sys, "timer handler threw timer={} message={}", owner->_name, e.what());
    } catch (...) {
        LOG_ERROR(loop, sys, "timer handler threw unknown exception timer={}", owner->_name);
    }
    owner->_loop.recordHandler(owner->_name.c_str(), std::chrono::steady_clock::now() - begin);
}

void UvTimer::on_close(uv_handle_t* handle) {
    std::unique_ptr<timer_handle> timer(static_cast<timer_handle*>(handle->data));
}
