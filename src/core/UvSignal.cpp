#include "core/UvSignal.h"

#include <memory>
#include <utility>

UvSignal::UvSignal(UvLoop& loop, int signalNumber)
    : _signalNumber(signalNumber)
    , _signal(nullptr) {
    auto signal = std::make_unique<signal_handle>();
    signal->owner = this;
    uv_signal_init(loop.get_handle(), &signal->handle);
    signal->handle.data = signal.get();
    _signal = signal.release();
}

UvSignal::~UvSignal() {
    if (_signal == nullptr) {
        return;
    }
    uv_signal_stop(&_signal->handle);
    _signal->owner = nullptr;
    uv_close(reinterpret_cast<uv_handle_t*>(&_signal->handle), on_close);
    _signal = nullptr;
}

void UvSignal::start(std::function<void(int)> callback) {
    _callback = std::move(callback);
    uv_signal_start(&_signal->handle, on_signal, _signalNumber);
    // a pending signal handle must not keep the loop alive on its own
    uv_unref(reinterpret_cast<uv_handle_t*>(&_signal->handle));
}

void UvSignal::on_signal(uv_signal_t* handle, int signalNumber) {
    auto* signal = static_cast<signal_handle*>(handle->data);
    if (signal->owner != nullptr && signal->owner->_callback) {
        signal->owner->_callback(signalNumber);
    }
}

void UvSignal::on_close(uv_handle_t* handle) {
    std::unique_ptr<signal_handle> signal(static_cast<signal_handle*>(handle->data));
}
