#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/BlockingQueue.h"
#include "model/VideoFrame.h"
#include "pipeline/FrameClock.h"
#include "pipeline/FramePool.h"
#include "pipeline/LatestFrameSlot.h"
#include "source/IVideoSource.h"

struct capture_stats {
    uint64_t produced = 0;     // pictures read from the source
    uint64_t latched = 0;      // frames handed to the synchronizer
    uint64_t duplicated = 0;   // latched the same picture twice (source slower than the tick)
    uint64_t overwritten = 0;  // pictures never latched (source faster than the tick)
    uint64_t missedTicks = 0;  // tick skipped: woke too late or no picture yet
    uint64_t poolExhausted = 0;
    uint64_t sourceErrors = 0;
    int64_t worstLatchLateNs = 0;
};

/**
 * @brief one channel: a reader thread (source → latest slot) and a capture thread (latch at each tick)
 *
 * the capture thread sleeps until shortly before the tick target, spins on steady_clock up to it,
 * then stamps the latest picture with the measured time. a latch later than half a period skips the tick
 * rather than reporting a wrong time. part of EX-P1-001
 *
 * a non-live source at the tick rate is read half a period before each tick (like a sensor that exposes
 * on the FSYNC edge), so every tick finds exactly one new picture; other rates free-run at their own fps
 */
class CaptureWorker {
public:
    CaptureWorker(int index, int channelId, std::unique_ptr<IVideoSource> source, std::shared_ptr<FramePool> pool,
                  BlockingQueue<video_frame>& output);
    ~CaptureWorker();

    CaptureWorker(const CaptureWorker&) = delete;
    CaptureWorker& operator=(const CaptureWorker&) = delete;

    // blocking (network connect). called before start
    bool open();
    // returns one future per thread; a thread failure surfaces as its future's exception
    // every channel latches from firstTick on, so the first frame sets are complete
    void start(const FrameClock& clock, uint64_t firstTick, std::chrono::nanoseconds spinMargin,
               std::future<void>& readerDone, std::future<void>& captureDone);
    void requestStop();
    void join();

    capture_stats get_stats() const;
    video_source_info get_source_info() const;
    int get_channel_id() const;

private:
    void runReader();
    void runCapture();
    // false: stop was requested while waiting
    bool waitUntil(std::chrono::steady_clock::time_point deadline);
    void raisePriority(std::thread& thread, const char* role);

    int _index;
    int _channelId;
    std::unique_ptr<IVideoSource> _source;
    video_source_info _info;
    std::shared_ptr<FramePool> _pool;
    BlockingQueue<video_frame>& _output;
    LatestFrameSlot _slot;
    FrameClock _clock;
    uint64_t _firstTick;
    std::chrono::nanoseconds _spinMargin;

    std::mutex _stopMutex;
    std::condition_variable _stopSignal;
    std::atomic<bool> _running;
    std::thread _reader;
    std::thread _capture;

    std::atomic<uint64_t> _produced;
    std::atomic<uint64_t> _latched;
    std::atomic<uint64_t> _duplicated;
    std::atomic<uint64_t> _missedTicks;
    std::atomic<uint64_t> _poolExhausted;
    std::atomic<uint64_t> _sourceErrors;
    std::atomic<int64_t> _worstLatchLateNs;
};
