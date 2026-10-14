#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "core/BlockingQueue.h"
#include "core/UvLoop.h"
#include "util/log/Logger.h"

/**
 * @brief runs blocking calls on its own thread and hands results back through UvLoop::post (P2)
 *
 * the request queue has a ceiling and rejects when full. results posted after the worker is
 * destroyed are discarded, so a done callback never runs against a destroyed owner
 */
class BlockingWorker {
public:
    BlockingWorker(UvLoop& loop, std::string name, size_t capacity)
        : _loop(loop)
        , _name(std::move(name))
        , _queue(capacity, overflow_policy::reject)
        , _lifetime(std::make_shared<int>(0))
        , _thread([this]() {
            run();
        }) {
    }

    ~BlockingWorker() {
        stop();
    }

    BlockingWorker(const BlockingWorker&) = delete;
    BlockingWorker& operator=(const BlockingWorker&) = delete;

    // loop thread. false: the queue is full or stopped and the request was rejected
    template <typename Result>
    bool submit(std::function<Result()> blockingCall, std::function<void(Result)> onDone) {
        std::weak_ptr<int> lifetime = _lifetime;
        UvLoop* loop = &_loop;
        auto job = [loop, lifetime, blockingCall = std::move(blockingCall), onDone = std::move(onDone)]() {
            Result result = blockingCall();
            loop->post([lifetime, onDone, result = std::move(result)]() mutable {
                if (lifetime.expired()) {
                    return;
                }
                if (onDone) {
                    onDone(std::move(result));
                }
            });
        };
        return _queue.push(std::move(job)) == push_result::accepted;
    }

    // closes the queue, lets the running call finish, joins. idempotent
    void stop() {
        _queue.close();
        if (_thread.joinable()) {
            _thread.join();
        }
        _lifetime.reset();
    }

    size_t get_pending() const {
        return _queue.get_size();
    }

private:
    void run() {
        std::function<void()> job;
        while (_queue.pop(job)) {
            // an exception must not leave the thread: it would std::terminate the process
            try {
                job();
            } catch (const std::exception& e) {
                LOG_ERROR(app, sys, "blocking call threw worker={} message={}", _name, e.what());
            } catch (...) {
                LOG_ERROR(app, sys, "blocking call threw unknown exception worker={}", _name);
            }
            job = nullptr;
        }
    }

    UvLoop& _loop;
    std::string _name;
    BlockingQueue<std::function<void()>> _queue;
    std::shared_ptr<int> _lifetime;
    std::thread _thread;
};
