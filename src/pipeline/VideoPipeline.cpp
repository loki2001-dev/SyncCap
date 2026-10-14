#include "pipeline/VideoPipeline.h"

#include <algorithm>
#include <exception>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include "pipeline/FramePool.h"
#include "pipeline/FrameSynchronizer.h"
#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr int64_t NANOS_PER_MILLI = 1000000LL;
constexpr int64_t NANOS_PER_MICRO = 1000LL;
constexpr int64_t SYNC_WINDOW_NS = 10LL * 1000 * NANOS_PER_MILLI;
constexpr int64_t SYNC_STAT_PERIOD_NS = 10LL * 1000 * NANOS_PER_MILLI;
constexpr size_t CAPTURE_QUEUE_PER_CHANNEL = 4;
constexpr uint64_t SYNC_WARN_EVERY = 100;
constexpr int64_t START_LEAD_NS = 300LL * NANOS_PER_MILLI;
#ifdef _WIN32
constexpr UINT TIMER_RESOLUTION_MS = 1;
#endif

}  // namespace

VideoPipeline::VideoPipeline(video_config video, encoder_config encoder, preview_config preview, IRecordSink& sink,
                             video_source_factory_t sourceFactory, video_encoder_factory_t encoderFactory)
    : _video(std::move(video))
    , _encoder(std::move(encoder))
    , _previewConfig(std::move(preview))
    , _sink(sink)
    , _sourceFactory(std::move(sourceFactory))
    , _encoderFactory(std::move(encoderFactory))
    , _previewStride(1)
    , _monitor(_video.syncToleranceMs, SYNC_WINDOW_NS)
    , _running(false)
    , _stopping(false)
    , _lateFrames(0)
    , _lastStatLogNs(0)
    , _violationLogged(0)
#ifdef _WIN32
    , _timerResolutionRaised(false)
#endif
{
    if (_previewConfig.enabled) {
        _preview = std::make_unique<PreviewWorker>(_previewConfig);
        const int previewFps = std::max(1, std::min(_previewConfig.fps, _video.fps));
        _previewStride = static_cast<uint64_t>(std::max(1, _video.fps / previewFps));
    }
}

VideoPipeline::~VideoPipeline() {
    stop();
}

bool VideoPipeline::start(int64_t epochNs, int64_t segmentNs, std::string& error) {
    const std::lock_guard<std::mutex> lock(_lifecycleMutex);
    if (_running) {
        error = "already running";
        return false;
    }
    const int channelCount = static_cast<int>(_video.channels.size());
    if (channelCount < 1 || channelCount > MAX_CHANNELS) {
        error = "channel count out of range count=" + std::to_string(channelCount);
        return false;
    }
    _captureQueue = std::make_unique<BlockingQueue<video_frame>>(
        static_cast<size_t>(channelCount) * CAPTURE_QUEUE_PER_CHANNEL, overflow_policy::drop_oldest);
    _clock = FrameClock(epochNs, _video.fps);
    _segments = SegmentClock(epochNs, segmentNs);
    _monitor.reset();
    _lateFrames = 0;
    _lastStatLogNs = TimeUtil::steadyNowNs();
    _violationLogged = 0;

    for (int index = 0; index < channelCount; ++index) {
        const channel_config& channel = _video.channels[static_cast<size_t>(index)];
        std::string sourceError;
        std::unique_ptr<IVideoSource> source = _sourceFactory(channel, sourceError);
        if (!source) {
            error = "source create failed channel=" + std::to_string(channel.id) + " reason=" + sourceError;
            releaseWorkers();
            return false;
        }
        auto pool = FramePool::create(_video.width, _video.height, static_cast<size_t>(_video.poolFrames));
        auto capture = std::make_unique<CaptureWorker>(index, channel.id, std::move(source), pool, *_captureQueue);
        if (!capture->open()) {
            error = "source open failed channel=" + std::to_string(channel.id) + " uri=" + channel.uri;
            releaseWorkers();
            return false;
        }
        _sourceTypes.push_back(channel.type);
        _channelIds.push_back(channel.id);
        _captures.push_back(std::move(capture));

        std::unique_ptr<IVideoEncoder> encoder = _encoderFactory();
        if (!encoder || !encoder->open(channel.id, _video.width, _video.height, _video.fps, _encoder)) {
            error = "encoder open failed channel=" + std::to_string(channel.id);
            releaseWorkers();
            return false;
        }
        _encoders.push_back(std::make_unique<EncodeWorker>(channel.id, std::move(encoder),
                                                           static_cast<size_t>(_video.encoderQueue), _sink));
    }

#ifdef _WIN32
    // default Windows timer granularity is 15.6 ms; the latch needs ~1 ms sleeps before its spin.
    // Windows 11 ignores timeBeginPeriod for a process without a visible window unless it opts out of
    // power throttling; measured: 4 of 2031 sets late by one 15.6 ms quantum without this
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask =
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling)) == 0) {
        LOG_WARN(video, sys, "power throttling opt-out failed code={}", static_cast<unsigned long>(GetLastError()));
    }
    _timerResolutionRaised = timeBeginPeriod(TIMER_RESOLUTION_MS) == TIMERR_NOERROR;
#endif
    _stopping = false;
    _running = true;
    _futures.clear();
    for (const auto& encoder : _encoders) {
        thread_future entry{"encoder", {}};
        encoder->start(entry.done);
        _futures.push_back(std::move(entry));
    }
    if (_preview) {
        _preview->clear();
        thread_future entry{"preview", {}};
        _preview->start(entry.done);
        _futures.push_back(std::move(entry));
    }
    std::promise<void> synchronizerPromise;
    _futures.push_back(thread_future{"synchronizer", synchronizerPromise.get_future()});
    _synchronizer = std::thread([this, promise = std::move(synchronizerPromise)]() mutable {
        try {
            runSynchronizer();
            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    const auto spinMargin = std::chrono::microseconds(_video.spinMarginUs);
    // one common first tick, far enough ahead for every reader to deliver its first picture
    const uint64_t firstTick = _clock.nextTickAt(TimeUtil::steadyNowNs() + START_LEAD_NS);
    for (const auto& capture : _captures) {
        thread_future reader{"reader ch" + std::to_string(capture->get_channel_id()), {}};
        thread_future latch{"capture ch" + std::to_string(capture->get_channel_id()), {}};
        capture->start(_clock, firstTick, spinMargin, reader.done, latch.done);
        _futures.push_back(std::move(reader));
        _futures.push_back(std::move(latch));
    }
    LOG_INFO(video, life, "pipeline started channels={} fps={} size={}x{} tolerance_ms={} segment_s={} threads={}",
             channelCount, _video.fps, _video.width, _video.height, _video.syncToleranceMs,
             segmentNs / (NANOS_PER_MILLI * 1000), _futures.size());
    return true;
}

void VideoPipeline::stop() {
    const std::lock_guard<std::mutex> lock(_lifecycleMutex);
    if (!_running && _captures.empty() && _encoders.empty()) {
        return;
    }
    _stopping = true;
    // 1. producers first so nothing new enters the pipeline
    for (const auto& capture : _captures) {
        capture->requestStop();
    }
    for (const auto& capture : _captures) {
        capture->join();
    }
    // 2. the synchronizer drains the capture queue and exits on close
    if (_captureQueue) {
        _captureQueue->close();
    }
    if (_synchronizer.joinable()) {
        _synchronizer.join();
    }
    // nothing offers previews any more; a stopped pipeline must not look live to a viewer
    if (_preview) {
        _preview->stop();
        _preview->clear();
    }
    // 3. encoders drain their queues, flush libx264 and push the last packets
    for (const auto& encoder : _encoders) {
        encoder->closeInput();
    }
    for (const auto& encoder : _encoders) {
        encoder->join();
    }
    const sync_report report = _monitor.get_report();
    LOG_INFO(video, life,
             "pipeline stopped sets_checked={} locked={} violations={} incomplete={} backpressure={} "
             "max_offset_ms={:.3f}",
             report.setsChecked, report.setsLocked, report.violations, report.incomplete, report.backpressure,
             report.totalMaxOffsetMs);
    releaseWorkers();
    _futures.clear();
    _running = false;
#ifdef _WIN32
    if (_timerResolutionRaised) {
        timeEndPeriod(TIMER_RESOLUTION_MS);
        _timerResolutionRaised = false;
    }
#endif
}

void VideoPipeline::releaseWorkers() {
    _captures.clear();
    _encoders.clear();
    _sourceTypes.clear();
    _channelIds.clear();
    _captureQueue.reset();
}

bool VideoPipeline::isRunning() const {
    return _running;
}

void VideoPipeline::runSynchronizer() {
    synchronizer_config config;
    config.channelCount = static_cast<int>(_captures.size());
    config.toleranceNs = static_cast<int64_t>(_video.syncToleranceMs * static_cast<double>(NANOS_PER_MILLI));
    config.maxPendingTicks = _video.maxPendingTicks;
    FrameSynchronizer synchronizer(config);
    std::vector<frame_set> ready;
    video_frame frame;
    while (_captureQueue->pop(frame)) {
        synchronizer.add(std::move(frame), ready);
        for (frame_set& set : ready) {
            route(set);
        }
        ready.clear();
        _lateFrames = synchronizer.get_late_frames();
    }
    // sets cut short by the stop itself are not a sync failure: discard them, but say how many
    synchronizer.flush(ready);
    LOG_INFO(sync, life, "synchronizer stopped discarded_at_stop={} late_frames={}", ready.size(),
             synchronizer.get_late_frames());
}

void VideoPipeline::route(frame_set& set) {
    sync_record& record = set.record;
    record.segment = _segments.indexOf(record.targetNs);
    bool encode =
        record.verdict == sync_verdict::locked || (record.verdict == sync_verdict::violation && !_video.dropUnsynced);
    if (encode) {
        // all or nothing: dropping one channel only would break the frame alignment between files
        for (const auto& encoder : _encoders) {
            if (encoder->get_queue().get_free() == 0) {
                record.verdict = sync_verdict::backpressure;
                encode = false;
                break;
            }
        }
    }
    if (encode && record.verdict == sync_verdict::locked) {
        offerPreview(set);
    }
    if (encode) {
        for (size_t index = 0; index < _encoders.size(); ++index) {
            video_frame frame = std::move(*set.frames[index]);
            frame.segment = record.segment;
            _encoders[index]->get_queue().push(std::move(frame));
        }
    }
    _monitor.record(record);
    _sink.push(record_item(record));
    logSyncProgress(record);
}

void VideoPipeline::offerPreview(const frame_set& set) {
    if (!_preview || set.record.tick % _previewStride != 0) {
        return;
    }
    preview_job job;
    job.tick = set.record.tick;
    job.targetNs = set.record.targetNs;
    job.offsetNs = set.record.offsetNs;
    job.verdict = set.record.verdict;
    for (size_t index = 0; index < _encoders.size() && index < set.frames.size(); ++index) {
        if (!set.frames[index]) {
            continue;
        }
        preview_job::channel channel;
        channel.index = static_cast<int>(index);
        channel.channelId = index < _channelIds.size() ? _channelIds[index] : static_cast<int>(index);
        channel.sourceType = index < _sourceTypes.size() ? _sourceTypes[index] : std::string();
        channel.timestampNs = set.frames[index]->timestampNs;
        // shares the pooled picture with the encoder: no copy on the synchronizer thread
        channel.buffer = set.frames[index]->buffer;
        job.channels.push_back(std::move(channel));
    }
    _preview->offer(std::move(job));
}

std::shared_ptr<const preview_set> VideoPipeline::get_latest_preview() const {
    return _preview ? _preview->get_latest_preview() : nullptr;
}

std::shared_ptr<const preview_set> VideoPipeline::find_preview(uint64_t tick) const {
    return _preview ? _preview->find_preview(tick) : nullptr;
}

void VideoPipeline::logSyncProgress(const sync_record& record) {
    if (record.verdict != sync_verdict::locked) {
        _violationLogged += 1;
        if (_violationLogged == 1 || _violationLogged % SYNC_WARN_EVERY == 0) {
            LOG_WARN(sync, stat, "set rejected verdict={} tick={} offset_us={} tolerance_ms={} rejected_total={}",
                     toString(record.verdict), record.tick, record.offsetNs / NANOS_PER_MICRO, _video.syncToleranceMs,
                     _violationLogged);
        }
    }
    const int64_t now = TimeUtil::steadyNowNs();
    if (now - _lastStatLogNs >= SYNC_STAT_PERIOD_NS) {
        _lastStatLogNs = now;
        const sync_report report = _monitor.get_report();
        LOG_INFO(sync, stat,
                 "sync locked={} window_max_offset_ms={:.3f} last_offset_ms={:.3f} sets={} violations={} "
                 "incomplete={} backpressure={}",
                 report.locked, report.windowMaxOffsetMs, report.lastOffsetMs, report.setsChecked, report.violations,
                 report.incomplete, report.backpressure);
    }
}

pipeline_stats VideoPipeline::get_stats() const {
    pipeline_stats stats;
    // never wait on start/stop from the loop thread: they hold this lock while blocking
    std::unique_lock<std::mutex> lock(_lifecycleMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return stats;
    }
    stats.running = _running && !_stopping;
    for (size_t index = 0; index < _captures.size(); ++index) {
        pipeline_channel_stats channel;
        channel.id = _captures[index]->get_channel_id();
        channel.sourceType = index < _sourceTypes.size() ? _sourceTypes[index] : std::string();
        channel.capture = _captures[index]->get_stats();
        if (index < _encoders.size()) {
            channel.encode = _encoders[index]->get_stats();
        }
        stats.channels.push_back(channel);
    }
    stats.sync = _monitor.get_report();
    if (_captureQueue) {
        stats.captureQueueLength = _captureQueue->get_size();
        stats.captureQueueHighWater = _captureQueue->get_high_water();
        stats.captureQueueDropped = _captureQueue->get_dropped_total();
    }
    stats.lateFrames = _lateFrames;
    if (_preview) {
        stats.previewsEncoded = _preview->get_encoded_total();
        stats.previewsReplaced = _preview->get_replaced_total();
    }
    return stats;
}

sync_report VideoPipeline::get_sync_report() const {
    return _monitor.get_report();
}

bool VideoPipeline::isSynchronized() const {
    return _monitor.isSynchronized();
}

std::optional<std::string> VideoPipeline::poll_failure() {
    std::unique_lock<std::mutex> lock(_lifecycleMutex, std::try_to_lock);
    if (!lock.owns_lock() || !_running || _stopping) {
        return std::nullopt;
    }
    for (thread_future& entry : _futures) {
        if (!entry.done.valid() || entry.done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            continue;
        }
        try {
            entry.done.get();
            return entry.name + " exited while running";
        } catch (const std::exception& e) {
            return entry.name + ": " + e.what();
        } catch (...) {
            return entry.name + ": unknown exception";
        }
    }
    return std::nullopt;
}
