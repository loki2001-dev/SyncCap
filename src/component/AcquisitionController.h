#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/IComponent.h"
#include "core/IStateWriter.h"
#include "core/MessageBus.h"
#include "core/UvLoop.h"
#include "core/UvTimer.h"
#include "model/Messages.h"
#include "storage/IRecordSink.h"

struct acquisition_config {
    std::string deviceId;
    std::string storageRoot;
    int segmentSeconds = 180;
    bool autostart = true;
    int stopTimeoutMs = 10000;
};

/**
 * @brief start/stop state machine and session owner
 *
 * provides: acquisition_started, acquisition_stopping, acquisition_stopped, control_result on the bus;
 *           acquisition_state through IAcquisitionStatusWriter, storage_state through IStorageStatusWriter
 * requires: control_command, video_pipeline_started, video_pipeline_stopped, video_pipeline_failed from the bus;
 *           IRecordSink for the session directory
 *
 * idle --start--> running --stop/failure--> stopping --video stopped or timeout--> idle | error
 */
class AcquisitionController : public IComponent {
public:
    AcquisitionController(UvLoop& loop, MessageBus& bus, IAcquisitionStatusWriter& acquisitionWriter,
                          IStorageStatusWriter& storageWriter, IRecordSink& recorder, acquisition_config config);
    ~AcquisitionController() override;

    AcquisitionController(const AcquisitionController&) = delete;
    AcquisitionController& operator=(const AcquisitionController&) = delete;

    const char* get_name() const override;
    bool initialize() override;
    bool start() override;
    void stop() override;

    acquisition_phase get_phase() const;

private:
    // bus
    void handleCommand(const control_command& command);
    void handlePipelineStarted(const video_pipeline_started& message);
    void handlePipelineStopped(const video_pipeline_stopped& message);
    void handlePipelineFailed(const video_pipeline_failed& message);

    // state machine
    void beginSession(const control_command& command);
    void beginStop(const std::string& reason, bool failure);
    void finishStop(const std::string& note);
    void set_phase(acquisition_phase phase, const std::string& reason);
    void reply(const control_command& command, bool accepted, const std::string& reason);

    // timer
    void publishStorageStats();

    UvLoop& _loop;
    MessageBus& _bus;
    IAcquisitionStatusWriter& _acquisitionWriter;
    IStorageStatusWriter& _storageWriter;
    IRecordSink& _recorder;
    acquisition_config _config;
    std::unique_ptr<UvTimer> _statsTimer;
    std::unique_ptr<UvTimer> _stopTimer;
    std::vector<subscription_id_t> _subscriptions;

    acquisition_phase _phase;
    std::string _sessionId;
    std::string _stopReason;
    bool _stopIsFailure;
    bool _videoActive;
};
