#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/BlockingWorker.h"
#include "core/IComponent.h"
#include "core/IStateWriter.h"
#include "core/MessageBus.h"
#include "core/UvLoop.h"
#include "core/UvTimer.h"
#include "model/Messages.h"
#include "pipeline/VideoPipeline.h"

/**
 * @brief runs the video pipeline for the acquisition session and reports on it
 *
 * provides: video_pipeline_started, video_pipeline_stopped, video_pipeline_failed on the bus;
 *           video_state through IVideoStatusWriter
 * requires: acquisition_started, acquisition_stopping from the bus; the VideoPipeline owned by the assembly
 *           (blocking start/stop run on the component's BlockingWorker, P2)
 */
class VideoCaptureComponent : public IComponent {
public:
    VideoCaptureComponent(UvLoop& loop, MessageBus& bus, IVideoStatusWriter& statusWriter, VideoPipeline& pipeline);
    ~VideoCaptureComponent() override;

    VideoCaptureComponent(const VideoCaptureComponent&) = delete;
    VideoCaptureComponent& operator=(const VideoCaptureComponent&) = delete;

    const char* get_name() const override;
    bool initialize() override;
    bool start() override;
    void stop() override;

private:
    struct start_result {
        bool ok = false;
        std::string error;
    };

    // bus
    void handleStarted(const acquisition_started& message);
    void handleStopping(const acquisition_stopping& message);

    // timer
    void refreshStats();

    UvLoop& _loop;
    MessageBus& _bus;
    IVideoStatusWriter& _statusWriter;
    VideoPipeline& _pipeline;
    std::unique_ptr<UvTimer> _statsTimer;
    std::vector<subscription_id_t> _subscriptions;
    bool _failureReported;
    uint64_t _statsTicks;
    std::vector<uint64_t> _lastEncoded;
    std::chrono::steady_clock::time_point _lastStatsAt;
    // last member: joined first on destruction, before the pipeline it works on
    std::unique_ptr<BlockingWorker> _worker;
};
