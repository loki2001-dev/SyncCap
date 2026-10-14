#pragma once

#include <functional>

#include <uv.h>

#include "core/UvLoop.h"

// SIGINT/SIGTERM (and Ctrl+C / console close on Windows) delivered on the loop thread
class UvSignal {
public:
    UvSignal(UvLoop& loop, int signalNumber);
    ~UvSignal();

    UvSignal(const UvSignal&) = delete;
    UvSignal& operator=(const UvSignal&) = delete;

    void start(std::function<void(int)> callback);

private:
    struct signal_handle {
        uv_signal_t handle;
        UvSignal* owner;
    };

    // libuv callback
    static void on_signal(uv_signal_t* handle, int signalNumber);
    static void on_close(uv_handle_t* handle);

    int _signalNumber;
    signal_handle* _signal;
    std::function<void(int)> _callback;
};
