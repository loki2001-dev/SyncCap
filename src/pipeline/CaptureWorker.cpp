#include "pipeline/CaptureWorker.h"

#include <cmath>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr int64_t NANOS_PER_SECOND = 1000000000LL;
constexpr auto RETRY_DELAY = std::chrono::milliseconds(1);
constexpr auto RECONNECT_DELAY = std::chrono::seconds(1);
constexpr auto PACE_RESYNC_LIMIT = std::chrono::seconds(1);
constexpr double ALIGNED_FPS_TOLERANCE = 0.5;
#ifndef _WIN32
constexpr int CAPTURE_FIFO_PRIORITY = 10;
#endif

std::chrono::steady_clock::time_point toTimePoint(int64_t ns) {
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(ns)));
}

}  // namespace

CaptureWorker::CaptureWorker(int index, int channelId, std::unique_ptr<IVideoSource> source,
                             std::shared_ptr<FramePool> pool, BlockingQueue<video_frame>& output)
    : _index(index)
    , _channelId(channelId)
    , _source(std::move(source))
    , _pool(std::move(pool))
    , _output(output)
    , _firstTick(0)
    , _spinMargin(0)
    , _running(false)
    , _produced(0)
    , _latched(0)
    , _duplicated(0)
    , _missedTicks(0)
    , _poolExhausted(0)
    , _sourceErrors(0)
    , _worstLatchLateNs(0) {
    _info = _source->get_info();
}

CaptureWorker::~CaptureWorker() {
    requestStop();
    join();
}

bool CaptureWorker::open() {
    if (!_source->open()) {
        return false;
    }
    _info = _source->get_info();
    return true;
}

void CaptureWorker::start(const FrameClock& clock, uint64_t firstTick, std::chrono::nanoseconds spinMargin,
                          std::future<void>& readerDone, std::future<void>& captureDone) {
    _clock = clock;
    _firstTick = firstTick;
    _spinMargin = spinMargin;
    {
        const std::lock_guard<std::mutex> lock(_stopMutex);
        _running = true;
    }
    std::promise<void> readerPromise;
    readerDone = readerPromise.get_future();
    _reader = std::thread([this, promise = std::move(readerPromise)]() mutable {
        try {
            runReader();
            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    std::promise<void> capturePromise;
    captureDone = capturePromise.get_future();
    _capture = std::thread([this, promise = std::move(capturePromise)]() mutable {
        try {
            runCapture();
            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    raisePriority(_capture, "capture");
    LOG_INFO(cap, life, "started channel={} type={} uri={} live={} native_fps={:.2f} spin_margin_us={}", _channelId,
             _info.type, _info.uri, _info.live, _info.nativeFps,
             std::chrono::duration_cast<std::chrono::microseconds>(spinMargin).count());
}

void CaptureWorker::requestStop() {
    {
        const std::lock_guard<std::mutex> lock(_stopMutex);
        _running = false;
    }
    _stopSignal.notify_all();
    _source->interrupt();
    if (_pool) {
        _pool->close();
    }
}

void CaptureWorker::join() {
    if (_reader.joinable()) {
        _reader.join();
    }
    if (_capture.joinable()) {
        _capture.join();
    }
    _source->close();
    _slot.clear();
}

bool CaptureWorker::waitUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(_stopMutex);
    return !_stopSignal.wait_until(lock, deadline, [this]() {
        return !_running;
    });
}

void CaptureWorker::runReader() {
    const int64_t periodNs = _clock.get_period_ns();
    const double clockFps = static_cast<double>(NANOS_PER_SECOND) / static_cast<double>(periodNs);
    const double nativeFps = _info.nativeFps > 0.0 ? _info.nativeFps : clockFps;
    // a non-live source at the tick rate is read in phase with the clock; anything else free-runs
    const bool aligned = !_info.live && std::fabs(nativeFps - clockFps) < ALIGNED_FPS_TOLERANCE;
    const bool freePaced = !_info.live && !aligned;
    const auto pace = std::chrono::nanoseconds(static_cast<int64_t>(static_cast<double>(NANOS_PER_SECOND) / nativeFps));
    const auto acquireTimeout =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::nanoseconds(periodNs));
    // the first picture must be in the slot before the first latch
    uint64_t readTick = _firstTick;
    auto next = std::chrono::steady_clock::now();

    while (_running) {
        std::shared_ptr<FrameBuffer> buffer = _pool->acquire(acquireTimeout);
        if (!buffer) {
            if (_running) {
                _poolExhausted += 1;
            }
            continue;
        }
        const read_status status = _source->read(*buffer);
        switch (status) {
            case read_status::ok: {
                _produced += 1;
                if (aligned) {
                    // publish half a period before tick k: tick k always finds exactly this picture
                    const int64_t publishNs = _clock.targetOf(readTick) - periodNs / 2;
                    if (!waitUntil(toTimePoint(publishNs))) {
                        return;
                    }
                    _slot.put(std::move(buffer));
                    readTick = std::max(readTick + 1, _clock.nextTickAt(TimeUtil::steadyNowNs()));
                    break;
                }
                _slot.put(std::move(buffer));
                if (freePaced) {
                    next += pace;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - next > PACE_RESYNC_LIMIT) {
                        next = now;
                    }
                    waitUntil(next);
                }
                break;
            }
            case read_status::retry:
                waitUntil(std::chrono::steady_clock::now() + RETRY_DELAY);
                break;
            case read_status::end_of_stream:
                throw std::runtime_error("end of stream channel=" + std::to_string(_channelId) + " uri=" + _info.uri);
            case read_status::error:
            default: {
                _sourceErrors += 1;
                if (!_info.live) {
                    throw std::runtime_error("source read failed channel=" + std::to_string(_channelId) +
                                             " uri=" + _info.uri);
                }
                // live source: keep the session alive, the slot keeps the last picture meanwhile
                _source->close();
                LOG_WARN(cap, net, "source lost channel={} uri={} errors_total={} reconnect_in_ms={}", _channelId,
                         _info.uri, _sourceErrors.load(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(RECONNECT_DELAY).count());
                if (!waitUntil(std::chrono::steady_clock::now() + RECONNECT_DELAY)) {
                    return;
                }
                if (_source->open()) {
                    LOG_INFO(cap, net, "source reconnected channel={} uri={}", _channelId, _info.uri);
                }
                break;
            }
        }
    }
}

void CaptureWorker::runCapture() {
    const int64_t periodNs = _clock.get_period_ns();
    uint64_t tick = std::max(_firstTick, _clock.nextTickAt(TimeUtil::steadyNowNs()));
    uint64_t lastSequence = 0;

    while (_running) {
        const int64_t targetNs = _clock.targetOf(tick);
        const auto target = toTimePoint(targetNs);
        if (!waitUntil(target - _spinMargin)) {
            break;
        }
        // the last stretch on the clock itself: sleep granularity is ~1 ms, the requirement is 2 ms
        while (std::chrono::steady_clock::now() < target) {
            std::this_thread::yield();
        }
        const int64_t latchNs = TimeUtil::steadyNowNs();
        const int64_t lateNs = latchNs - targetNs;
        if (lateNs > _worstLatchLateNs) {
            _worstLatchLateNs = lateNs;
        }
        if (lateNs > periodNs / 2) {
            // never relabel a late latch with the target time: skip the tick, the set becomes incomplete
            _missedTicks += 1;
            tick = _clock.nextTickAt(latchNs);
            continue;
        }
        slot_frame latest;
        if (!_slot.peekLatest(latest)) {
            _missedTicks += 1;
            tick += 1;
            continue;
        }
        video_frame frame;
        frame.channel = _index;
        frame.tick = tick;
        frame.targetNs = targetNs;
        frame.timestampNs = latchNs;
        frame.sourceSequence = latest.sequence;
        frame.duplicate = latest.sequence == lastSequence;
        frame.buffer = std::move(latest.buffer);
        if (frame.duplicate) {
            _duplicated += 1;
        }
        lastSequence = latest.sequence;
        _output.push(std::move(frame));
        _latched += 1;
        tick += 1;
    }
}

void CaptureWorker::raisePriority(std::thread& thread, const char* role) {
#ifdef _WIN32
    if (SetThreadPriority(static_cast<HANDLE>(thread.native_handle()), THREAD_PRIORITY_HIGHEST) == 0) {
        LOG_WARN(cap, sys, "thread priority raise failed channel={} role={} code={}", _channelId, role,
                 static_cast<unsigned long>(GetLastError()));
    }
#else
    sched_param parameter{};
    parameter.sched_priority = CAPTURE_FIFO_PRIORITY;
    const int result = pthread_setschedparam(thread.native_handle(), SCHED_FIFO, &parameter);
    if (result != 0) {
        // needs CAP_SYS_NICE; without it the latch relies on the spin margin alone
        LOG_DEBUG(cap, sys, "realtime priority unavailable channel={} role={} code={}", _channelId, role, result);
    }
#endif
}

capture_stats CaptureWorker::get_stats() const {
    capture_stats stats;
    stats.produced = _produced;
    stats.latched = _latched;
    stats.duplicated = _duplicated;
    stats.overwritten = _slot.get_overwritten_total();
    stats.missedTicks = _missedTicks;
    stats.poolExhausted = _poolExhausted;
    stats.sourceErrors = _sourceErrors;
    stats.worstLatchLateNs = _worstLatchLateNs;
    return stats;
}

video_source_info CaptureWorker::get_source_info() const {
    return _info;
}

int CaptureWorker::get_channel_id() const {
    return _channelId;
}
