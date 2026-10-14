#include "component/VideoCaptureComponent.h"

#include <string>
#include <utility>

#include "util/log/Logger.h"

namespace {

constexpr uint64_t STATS_PERIOD_MS = 1000;
constexpr uint64_t STAT_LOG_EVERY_TICKS = 10;
constexpr size_t WORKER_QUEUE = 4;
constexpr double NANOS_PER_SECOND = 1e9;

}  // namespace

VideoCaptureComponent::VideoCaptureComponent(UvLoop& loop, MessageBus& bus, IVideoStatusWriter& statusWriter,
                                             VideoPipeline& pipeline)
    : _loop(loop)
    , _bus(bus)
    , _statusWriter(statusWriter)
    , _pipeline(pipeline)
    , _failureReported(false)
    , _statsTicks(0) {
}

VideoCaptureComponent::~VideoCaptureComponent() {
    stop();
}

const char* VideoCaptureComponent::get_name() const {
    return "video";
}

bool VideoCaptureComponent::initialize() {
    _worker = std::make_unique<BlockingWorker>(_loop, "video", WORKER_QUEUE);
    _statsTimer = std::make_unique<UvTimer>(_loop, "video_stats");
    _subscriptions.push_back(_bus.subscribe<acquisition_started>([this](const acquisition_started& message) {
        handleStarted(message);
    }));
    _subscriptions.push_back(_bus.subscribe<acquisition_stopping>([this](const acquisition_stopping& message) {
        handleStopping(message);
    }));
    return true;
}

bool VideoCaptureComponent::start() {
    _statsTimer->start(STATS_PERIOD_MS, STATS_PERIOD_MS, [this]() {
        refreshStats();
    });
    return true;
}

void VideoCaptureComponent::stop() {
    if (_statsTimer) {
        _statsTimer->stop();
    }
    for (const subscription_id_t id : _subscriptions) {
        _bus.unsubscribe(id);
    }
    _subscriptions.clear();
    // shutdown path: the loop no longer runs, so joining here blocks nobody
    if (_worker) {
        _worker->stop();
    }
    _pipeline.stop();
}

void VideoCaptureComponent::handleStarted(const acquisition_started& message) {
    _failureReported = false;
    _lastEncoded.clear();
    _lastStatsAt = std::chrono::steady_clock::now();
    VideoPipeline* pipeline = &_pipeline;
    const int64_t epochNs = message.epochNs;
    const int64_t segmentNs = message.segmentNs;
    const bool submitted = _worker->submit<start_result>(
        [pipeline, epochNs, segmentNs]() {
            start_result result;
            result.ok = pipeline->start(epochNs, segmentNs, result.error);
            return result;
        },
        [this](start_result result) {
            if (!result.ok) {
                LOG_ERROR(video, life, "pipeline start failed reason={}", result.error);
            }
            _bus.publish(video_pipeline_started{result.ok, result.error});
        });
    if (!submitted) {
        LOG_ERROR(video, sys, "pipeline start rejected reason=worker_queue_full");
        _bus.publish(video_pipeline_started{false, "worker_queue_full"});
    }
}

void VideoCaptureComponent::handleStopping(const acquisition_stopping& message) {
    VideoPipeline* pipeline = &_pipeline;
    const std::string reason = message.reason;
    const bool submitted = _worker->submit<bool>(
        [pipeline]() {
            pipeline->stop();
            return true;
        },
        [this, reason](bool) {
            _statusWriter.updateVideoState([](video_state& state) {
                state.running = false;
                for (channel_stats& channel : state.channels) {
                    channel.fps = 0.0;
                    channel.queueLength = 0;
                }
            });
            _bus.publish(video_pipeline_stopped{reason});
        });
    if (!submitted) {
        LOG_ERROR(video, sys, "pipeline stop rejected reason=worker_queue_full");
    }
}

void VideoCaptureComponent::refreshStats() {
    if (auto failure = _pipeline.poll_failure()) {
        if (!_failureReported) {
            _failureReported = true;
            LOG_ERROR(video, life, "pipeline thread failed reason={}", *failure);
            _bus.publish(video_pipeline_failed{*failure});
        }
    }
    const pipeline_stats stats = _pipeline.get_stats();
    if (!stats.running) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double elapsedS =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - _lastStatsAt).count()) /
        NANOS_PER_SECOND;
    _lastStatsAt = now;
    // the first window after start holds the source opening and the start lead: use it as the baseline only,
    // otherwise the first fps reads low (measured: 20 instead of 30 with 1 s status)
    const bool baseline = _lastEncoded.size() != stats.channels.size();
    if (baseline) {
        _lastEncoded.assign(stats.channels.size(), 0);
    }
    std::vector<channel_stats> channels;
    for (size_t index = 0; index < stats.channels.size(); ++index) {
        const pipeline_channel_stats& source = stats.channels[index];
        channel_stats channel;
        channel.id = source.id;
        channel.sourceType = source.sourceType;
        const uint64_t encoded = source.encode.framesEncoded;
        channel.fps =
            (!baseline && elapsedS > 0.0) ? static_cast<double>(encoded - _lastEncoded[index]) / elapsedS : 0.0;
        _lastEncoded[index] = encoded;
        channel.queueLength = source.encode.queueLength;
        channel.framesEncoded = encoded;
        channel.dropped = source.encode.packetsDropped + source.capture.poolExhausted;
        channel.duplicated = source.capture.duplicated;
        channel.missedTicks = source.capture.missedTicks;
        channel.sourceErrors = source.capture.sourceErrors;
        channel.encodeMsAvg = source.encode.encodeMsAvg;
        channels.push_back(channel);
    }
    _statusWriter.updateVideoState([&channels, &stats](video_state& state) {
        state.running = true;
        state.channels = channels;
        state.sync = stats.sync;
    });

    _statsTicks += 1;
    if (_statsTicks % STAT_LOG_EVERY_TICKS == 0) {
        for (size_t index = 0; index < stats.channels.size(); ++index) {
            const pipeline_channel_stats& source = stats.channels[index];
            LOG_INFO(video, stat,
                     "channel id={} fps={:.2f} produced={} latched={} encoded={} duplicated={} overwritten={} "
                     "missed_ticks={} pool_exhausted={} encode_ms_avg={:.2f} encode_ms_worst={:.2f} "
                     "queue={}/{} worst_latch_late_us={}",
                     source.id, channels[index].fps, source.capture.produced, source.capture.latched,
                     source.encode.framesEncoded, source.capture.duplicated, source.capture.overwritten,
                     source.capture.missedTicks, source.capture.poolExhausted, source.encode.encodeMsAvg,
                     source.encode.encodeMsWorst, source.encode.queueLength, source.encode.queueHighWater,
                     source.capture.worstLatchLateNs / 1000);
        }
        LOG_INFO(video, stat, "capture queue length={} high_water={} dropped={} late_frames={}",
                 stats.captureQueueLength, stats.captureQueueHighWater, stats.captureQueueDropped, stats.lateFrames);
    }
}
