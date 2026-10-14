#include "core/UvLoop.h"

#include <exception>
#include <utility>

#include "util/log/Logger.h"

namespace {

constexpr double NANOS_PER_MILLI = 1e6;

}  // namespace

UvLoop::UvLoop(size_t postCapacity, std::chrono::milliseconds handlerBudget)
    : _loop{}
    , _async{}
    , _postCapacity(postCapacity)
    , _postRejected(0)
    , _postHighWater(0)
    , _stopRequested(false)
    , _handlerBudget(handlerBudget) {
    uv_loop_init(&_loop);
    uv_async_init(&_loop, &_async, on_async);
    _async.data = this;
}

UvLoop::~UvLoop() {
    uv_close(reinterpret_cast<uv_handle_t*>(&_async), nullptr);
    // let pending close callbacks (timers, signals) run before the loop memory goes away
    uv_run(&_loop, UV_RUN_DEFAULT);
    const int result = uv_loop_close(&_loop);
    if (result != 0) {
        LOG_ERROR(loop, life, "loop close failed code={} message={}", result, uv_strerror(result));
    }
}

uv_loop_t* UvLoop::get_handle() {
    return &_loop;
}

void UvLoop::run() {
    uv_run(&_loop, UV_RUN_DEFAULT);
}

bool UvLoop::post(std::function<void()> task) {
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (_pending.size() >= _postCapacity) {
            _postRejected += 1;
            return false;
        }
        _pending.push_back(std::move(task));
        if (_pending.size() > _postHighWater) {
            _postHighWater = _pending.size();
        }
    }
    uv_async_send(&_async);
    return true;
}

void UvLoop::requestStop() {
    _stopRequested = true;
    uv_async_send(&_async);
}

void UvLoop::recordHandler(const char* name, std::chrono::steady_clock::duration elapsed) {
    const double elapsedMs =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / NANOS_PER_MILLI;
    _stats.handlersRun += 1;
    if (elapsedMs > _stats.worstHandlerMs) {
        _stats.worstHandlerMs = elapsedMs;
        _stats.worstHandler = name;
    }
    if (elapsed > _handlerBudget) {
        _stats.budgetExceeded += 1;
        LOG_WARN(loop, sys, "budget exceeded handler={} elapsed_ms={:.3f} budget_ms={}", name, elapsedMs,
                 _handlerBudget.count());
    }
}

loop_stats UvLoop::takeStats() {
    loop_stats result = _stats;
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        result.postRejected = _postRejected;
        result.postHighWater = _postHighWater;
        _postHighWater = _pending.size();
    }
    _stats = loop_stats{};
    return result;
}

void UvLoop::on_async(uv_async_t* handle) {
    auto* uvLoop = static_cast<UvLoop*>(handle->data);
    std::vector<std::function<void()>> tasks;
    {
        const std::lock_guard<std::mutex> lock(uvLoop->_mutex);
        tasks.swap(uvLoop->_pending);
    }
    for (auto& task : tasks) {
        const auto begin = std::chrono::steady_clock::now();
        // an exception must not cross the libuv C callback
        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR(loop, sys, "posted task threw message={}", e.what());
        } catch (...) {
            LOG_ERROR(loop, sys, "posted task threw unknown exception");
        }
        uvLoop->recordHandler("post", std::chrono::steady_clock::now() - begin);
    }
    if (uvLoop->_stopRequested) {
        uv_stop(&uvLoop->_loop);
    }
}
