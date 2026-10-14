#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <uv.h>

struct loop_stats {
    double worstHandlerMs = 0.0;
    std::string worstHandler;
    uint64_t handlersRun = 0;
    uint64_t budgetExceeded = 0;
    uint64_t postRejected = 0;
    size_t postHighWater = 0;
};

/**
 * @brief owns the libuv loop. post() is the only cross-thread channel into the loop thread
 *
 * every handler run through post() or UvTimer is timed; runs over the budget are logged
 */
class UvLoop {
public:
    explicit UvLoop(size_t postCapacity = DEFAULT_POST_CAPACITY,
                    std::chrono::milliseconds handlerBudget = DEFAULT_HANDLER_BUDGET);
    ~UvLoop();

    UvLoop(const UvLoop&) = delete;
    UvLoop& operator=(const UvLoop&) = delete;

    uv_loop_t* get_handle();

    // loop thread. returns after requestStop()
    void run();

    // thread-safe. false: the post queue is full and the task was dropped
    bool post(std::function<void()> task);

    // thread-safe
    void requestStop();

    // loop thread. measure one handler run against the budget
    void recordHandler(const char* name, std::chrono::steady_clock::duration elapsed);

    // loop thread. returns and resets the window counters
    loop_stats takeStats();

    static constexpr size_t DEFAULT_POST_CAPACITY = 4096;
    static constexpr std::chrono::milliseconds DEFAULT_HANDLER_BUDGET{10};

private:
    static void on_async(uv_async_t* handle);

    uv_loop_t _loop;
    uv_async_t _async;
    std::mutex _mutex;
    std::vector<std::function<void()>> _pending;
    size_t _postCapacity;
    uint64_t _postRejected;
    size_t _postHighWater;
    std::atomic<bool> _stopRequested;
    std::chrono::milliseconds _handlerBudget;
    loop_stats _stats;
};
