#include "component/AcquisitionController.h"

#include <chrono>
#include <filesystem>
#include <utility>

#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr uint64_t STATS_PERIOD_MS = 1000;
constexpr int64_t NANOS_PER_SECOND = 1000000000LL;

}  // namespace

AcquisitionController::AcquisitionController(UvLoop& loop, MessageBus& bus, IAcquisitionStatusWriter& acquisitionWriter,
                                             IStorageStatusWriter& storageWriter, IRecordSink& recorder,
                                             acquisition_config config)
    : _loop(loop)
    , _bus(bus)
    , _acquisitionWriter(acquisitionWriter)
    , _storageWriter(storageWriter)
    , _recorder(recorder)
    , _config(std::move(config))
    , _phase(acquisition_phase::idle)
    , _stopIsFailure(false)
    , _videoActive(false) {
}

AcquisitionController::~AcquisitionController() {
    stop();
}

const char* AcquisitionController::get_name() const {
    return "acquisition";
}

bool AcquisitionController::initialize() {
    _subscriptions.push_back(_bus.subscribe<control_command>([this](const control_command& command) {
        handleCommand(command);
    }));
    _subscriptions.push_back(_bus.subscribe<video_pipeline_started>([this](const video_pipeline_started& message) {
        handlePipelineStarted(message);
    }));
    _subscriptions.push_back(_bus.subscribe<video_pipeline_stopped>([this](const video_pipeline_stopped& message) {
        handlePipelineStopped(message);
    }));
    _subscriptions.push_back(_bus.subscribe<video_pipeline_failed>([this](const video_pipeline_failed& message) {
        handlePipelineFailed(message);
    }));
    _statsTimer = std::make_unique<UvTimer>(_loop, "acquisition_stats");
    _stopTimer = std::make_unique<UvTimer>(_loop, "acquisition_stop_timeout");
    set_phase(acquisition_phase::idle, "");
    LOG_INFO(ctrl, life, "initialized storage_root={} segment_s={} autostart={}", _config.storageRoot,
             _config.segmentSeconds, _config.autostart);
    return true;
}

bool AcquisitionController::start() {
    _statsTimer->start(STATS_PERIOD_MS, STATS_PERIOD_MS, [this]() {
        publishStorageStats();
    });
    if (_config.autostart) {
        _bus.publish(control_command{control_action::start, control_origin::config, "autostart"});
    }
    return true;
}

void AcquisitionController::stop() {
    if (_statsTimer) {
        _statsTimer->stop();
    }
    if (_stopTimer) {
        _stopTimer->stop();
    }
    for (const subscription_id_t id : _subscriptions) {
        _bus.unsubscribe(id);
    }
    _subscriptions.clear();
    if (_phase == acquisition_phase::running || _phase == acquisition_phase::stopping) {
        // the loop is gone: close the session so the files are complete
        _recorder.endSession();
        _phase = acquisition_phase::idle;
    }
}

acquisition_phase AcquisitionController::get_phase() const {
    return _phase;
}

void AcquisitionController::handleCommand(const control_command& command) {
    LOG_INFO(ctrl, cmd, "command received action={} origin={} request_id={} phase={}", toString(command.action),
             toString(command.origin), command.requestId, toString(_phase));
    if (command.action == control_action::start) {
        if (_phase == acquisition_phase::running) {
            reply(command, false, "rejected_already_running");
            return;
        }
        if (_phase == acquisition_phase::stopping) {
            reply(command, false, "rejected_busy_stopping");
            return;
        }
        beginSession(command);
        return;
    }
    if (_phase != acquisition_phase::running) {
        reply(command, false, "rejected_not_running");
        return;
    }
    reply(command, true, "");
    beginStop(std::string("stopped_command origin=") + toString(command.origin), false);
}

void AcquisitionController::beginSession(const control_command& command) {
    const auto wallNow = std::chrono::system_clock::now();
    const std::string sessionId = TimeUtil::formatCompactLocal(wallNow) + "_" + _config.deviceId;
    const std::string directory = (std::filesystem::path(_config.storageRoot) / sessionId).generic_string();
    session_info session;
    session.sessionId = sessionId;
    session.directory = directory;
    session.epochNs = TimeUtil::steadyNowNs();
    session.segmentNs = static_cast<int64_t>(_config.segmentSeconds) * NANOS_PER_SECOND;
    session.wallStart = wallNow;
    if (!_recorder.beginSession(session)) {
        set_phase(acquisition_phase::error, "rejected_storage");
        reply(command, false, "rejected_storage");
        return;
    }
    _sessionId = sessionId;
    _videoActive = false;
    _stopIsFailure = false;
    set_phase(acquisition_phase::running, "");
    reply(command, true, "");
    LOG_INFO(ctrl, life, "session started session={} path={} segment_s={}", sessionId, directory,
             _config.segmentSeconds);
    _bus.publish(acquisition_started{sessionId, directory, session.epochNs, session.segmentNs});
}

void AcquisitionController::beginStop(const std::string& reason, bool failure) {
    if (_phase != acquisition_phase::running) {
        return;
    }
    _stopReason = reason;
    _stopIsFailure = failure;
    set_phase(acquisition_phase::stopping, reason);
    LOG_INFO(ctrl, life, "session stopping session={} reason={}", _sessionId, reason);
    _bus.publish(acquisition_stopping{_sessionId, reason});
    _stopTimer->start(static_cast<uint64_t>(_config.stopTimeoutMs), 0, [this]() {
        LOG_WARN(ctrl, life, "stop timed out session={} timeout_ms={} closing session anyway", _sessionId,
                 _config.stopTimeoutMs);
        finishStop("stop_timeout");
    });
}

void AcquisitionController::finishStop(const std::string& note) {
    if (_phase != acquisition_phase::stopping) {
        return;
    }
    _stopTimer->stop();
    // every record of this session is queued before the end marker, so the files close complete
    _recorder.endSession();
    const std::string sessionId = _sessionId;
    const std::string reason = note.empty() ? _stopReason : _stopReason + " " + note;
    set_phase(_stopIsFailure ? acquisition_phase::error : acquisition_phase::idle, reason);
    LOG_INFO(ctrl, life, "session stopped session={} reason={}", sessionId, reason);
    _bus.publish(acquisition_stopped{sessionId, reason});
    _videoActive = false;
}

void AcquisitionController::handlePipelineStarted(const video_pipeline_started& message) {
    if (_phase != acquisition_phase::running) {
        return;
    }
    if (message.ok) {
        _videoActive = true;
        return;
    }
    LOG_ERROR(ctrl, life, "video start failed session={} reason={}", _sessionId, message.reason);
    beginStop("video_start_failed " + message.reason, true);
}

void AcquisitionController::handlePipelineStopped(const video_pipeline_stopped& /*message*/) {
    _videoActive = false;
    finishStop("");
}

void AcquisitionController::handlePipelineFailed(const video_pipeline_failed& message) {
    LOG_ERROR(ctrl, life, "video pipeline failed session={} reason={}", _sessionId, message.reason);
    beginStop("video_failed " + message.reason, true);
}

void AcquisitionController::set_phase(acquisition_phase phase, const std::string& reason) {
    _phase = phase;
    const std::string sessionId = _sessionId;
    _acquisitionWriter.updateAcquisitionState([phase, reason, sessionId](acquisition_state& state) {
        state.phase = phase;
        state.reason = reason;
        state.sessionId = sessionId;
    });
}

void AcquisitionController::reply(const control_command& command, bool accepted, const std::string& reason) {
    if (accepted) {
        LOG_INFO(ctrl, cmd, "accepted action={} origin={} request_id={}", toString(command.action),
                 toString(command.origin), command.requestId);
    } else {
        LOG_WARN(ctrl, cmd, "rejected reason={} action={} origin={} request_id={} phase={}", reason,
                 toString(command.action), toString(command.origin), command.requestId, toString(_phase));
    }
    _bus.publish(control_result{command.action, command.origin, command.requestId, accepted, reason});
}

void AcquisitionController::publishStorageStats() {
    const recorder_stats stats = _recorder.get_stats();
    _storageWriter.updateStorageState([&stats](storage_state& state) {
        state.segment = stats.segment;
        state.recentFiles = stats.recentFiles;
        state.bytesWritten = stats.bytesWritten;
        state.dropped = stats.dropped;
        state.writeErrors = stats.writeErrors;
        state.queueLength = stats.queueLength;
    });
}
