#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "codec/IVideoEncoder.h"
#include "config/AppConfig.h"
#include "core/BlockingQueue.h"
#include "model/States.h"
#include "model/VideoFrame.h"
#include "pipeline/CaptureWorker.h"
#include "pipeline/EncodeWorker.h"
#include "pipeline/FrameClock.h"
#include "pipeline/FrameSynchronizer.h"
#include "pipeline/IPreviewProvider.h"
#include "pipeline/PreviewWorker.h"
#include "pipeline/SyncMonitor.h"
#include "source/IVideoSource.h"
#include "storage/IRecordSink.h"
#include "storage/SegmentClock.h"

using video_source_factory_t =
    std::function<std::unique_ptr<IVideoSource>(const channel_config& channel, std::string& error)>;
using video_encoder_factory_t = std::function<std::unique_ptr<IVideoEncoder>()>;

struct pipeline_channel_stats {
    int id = 0;
    std::string sourceType;
    capture_stats capture;
    encode_stats encode;
};

struct pipeline_stats {
    bool running = false;
    std::vector<pipeline_channel_stats> channels;
    sync_report sync;
    size_t captureQueueLength = 0;
    size_t captureQueueHighWater = 0;
    uint64_t captureQueueDropped = 0;
    uint64_t lateFrames = 0;
    uint64_t previewsEncoded = 0;
    uint64_t previewsReplaced = 0;
};

/**
 * @brief the video data plane: N × (reader, capture) → synchronizer → N × encoder → record sink
 *
 * the only entry points from the control plane are start/stop (blocking, run on a worker thread),
 * get_stats/get_sync_report (copies) and poll_failure. EX-P1-001
 */
class VideoPipeline : public IPreviewProvider {
public:
    VideoPipeline(video_config video, encoder_config encoder, preview_config preview, IRecordSink& sink,
                  video_source_factory_t sourceFactory, video_encoder_factory_t encoderFactory);
    ~VideoPipeline() override;

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // blocking: opens every source and encoder, then starts the threads. false + error on any failure
    bool start(int64_t epochNs, int64_t segmentNs, std::string& error);
    // blocking: capture stops first, then the synchronizer drains, then the encoders flush
    void stop();
    bool isRunning() const;

    pipeline_stats get_stats() const;
    sync_report get_sync_report() const;
    bool isSynchronized() const;

    // the first thread that ended with an exception, or ended while it should still be running
    std::optional<std::string> poll_failure();

    // live preview (nullptr when disabled or before the first set)
    std::shared_ptr<const preview_set> get_latest_preview() const override;
    std::shared_ptr<const preview_set> find_preview(uint64_t tick) const override;

private:
    struct thread_future {
        std::string name;
        std::future<void> done;
    };

    void runSynchronizer();
    void route(frame_set& set);
    void logSyncProgress(const sync_record& record);
    void offerPreview(const frame_set& set);
    void releaseWorkers();

    video_config _video;
    encoder_config _encoder;
    preview_config _previewConfig;
    IRecordSink& _sink;
    video_source_factory_t _sourceFactory;
    video_encoder_factory_t _encoderFactory;

    std::unique_ptr<BlockingQueue<video_frame>> _captureQueue;
    std::vector<std::unique_ptr<CaptureWorker>> _captures;
    std::vector<std::unique_ptr<EncodeWorker>> _encoders;
    std::vector<std::string> _sourceTypes;
    std::vector<int> _channelIds;
    std::unique_ptr<PreviewWorker> _preview;
    uint64_t _previewStride;
    std::thread _synchronizer;
    SyncMonitor _monitor;
    FrameClock _clock;
    SegmentClock _segments;
    std::vector<thread_future> _futures;
    std::atomic<bool> _running;
    std::atomic<bool> _stopping;
    std::atomic<uint64_t> _lateFrames;
    mutable std::mutex _lifecycleMutex;

    // synchronizer thread only
    int64_t _lastStatLogNs;
    uint64_t _violationLogged;
#ifdef _WIN32
    bool _timerResolutionRaised;
#endif
};
