#include "component/StatusReportComponent.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "codec/StatusJsonCodec.h"
#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr uint64_t MILLIS_PER_SECOND = 1000;
constexpr uint64_t FIRST_REPORT_DELAY_MS = 5000;
constexpr size_t WORKER_QUEUE = 1;
constexpr const char* JSON_CONTENT_TYPE = "application/json";

}  // namespace

StatusReportComponent::StatusReportComponent(UvLoop& loop, const IStateReader& reader,
                                             IReportStatusWriter& statusWriter, IHttpClient& client,
                                             report_config config, status_identity identity)
    : _loop(loop)
    , _reader(reader)
    , _statusWriter(statusWriter)
    , _client(client)
    , _config(std::move(config))
    , _identity(std::move(identity))
    , _consecutiveFailures(0) {
}

StatusReportComponent::~StatusReportComponent() {
    stop();
}

const char* StatusReportComponent::get_name() const {
    return "report";
}

bool StatusReportComponent::initialize() {
    _worker = std::make_unique<BlockingWorker>(_loop, "report", WORKER_QUEUE);
    _timer = std::make_unique<UvTimer>(_loop, "status_report");
    LOG_INFO(http, life, "initialized url={} interval_s={} timeout_ms={}", _config.url, _config.intervalS,
             _config.timeoutMs);
    return true;
}

bool StatusReportComponent::start() {
    const uint64_t intervalMs = static_cast<uint64_t>(_config.intervalS) * MILLIS_PER_SECOND;
    // first report early so the server sees us soon; after that the fixed 20-30 s period
    _timer->start(std::min(FIRST_REPORT_DELAY_MS, intervalMs), intervalMs, [this]() {
        report();
    });
    return true;
}

void StatusReportComponent::stop() {
    if (_timer) {
        _timer->stop();
    }
    if (_worker) {
        _worker->stop();
    }
}

void StatusReportComponent::report() {
    const std::string body = StatusJsonCodec::encode(StatusSnapshot::build(_reader, _identity));
    IHttpClient* client = &_client;
    const std::string url = _config.url;
    const auto timeout = std::chrono::milliseconds(_config.timeoutMs);
    const bool submitted = _worker->submit<http_result>(
        [client, url, body, timeout]() {
            return client->post(url, body, JSON_CONTENT_TYPE, timeout);
        },
        [this](http_result result) {
            handleResult(result);
        });
    if (!submitted) {
        LOG_WARN(http, tx, "report skipped reason=previous_post_pending url={}", _config.url);
        _statusWriter.updateReportState([](report_state& state) {
            state.skipped += 1;
        });
        return;
    }
    LOG_DEBUG(http, tx, "report queued url={} bytes={}", _config.url, body.size());
}

void StatusReportComponent::handleResult(const http_result& result) {
    if (result.ok) {
        if (_consecutiveFailures > 0) {
            LOG_INFO(http, net, "server reachable again url={} after_failures={}", _config.url, _consecutiveFailures);
        }
        _consecutiveFailures = 0;
        LOG_INFO(http, tx, "report sent url={} status={} elapsed_ms={:.1f}", _config.url, result.status,
                 result.elapsedMs);
        const std::string now = TimeUtil::formatIso8601Utc(std::chrono::system_clock::now());
        _statusWriter.updateReportState([&now](report_state& state) {
            state.sent += 1;
            state.consecutiveFailures = 0;
            state.lastSuccessAt = now;
        });
        return;
    }
    _consecutiveFailures += 1;
    LOG_WARN(http, net, "report failed url={} status={} error={} elapsed_ms={:.1f} consecutive_failures={}",
             _config.url, result.status, result.error, result.elapsedMs, _consecutiveFailures);
    const uint64_t consecutive = _consecutiveFailures;
    const std::string error = result.error;
    _statusWriter.updateReportState([consecutive, &error](report_state& state) {
        state.failures += 1;
        state.consecutiveFailures = consecutive;
        state.lastError = error;
    });
}
