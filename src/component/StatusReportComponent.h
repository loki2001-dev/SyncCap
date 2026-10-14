#pragma once

#include <cstdint>
#include <memory>

#include "component/StatusSnapshot.h"
#include "config/AppConfig.h"
#include "core/BlockingWorker.h"
#include "core/IComponent.h"
#include "core/IStateWriter.h"
#include "core/UvLoop.h"
#include "core/UvTimer.h"
#include "network/IHttpClient.h"

/**
 * @brief posts the status document to the OpenAPI server every 20 to 30 s
 *
 * provides: report_state through IReportStatusWriter
 * requires: IStateReader; IHttpClient (blocking, run on a BlockingWorker, P2)
 *
 * a failed post is not retried early: the next period is the retry, which keeps the 20 s minimum spacing
 */
class StatusReportComponent : public IComponent {
public:
    StatusReportComponent(UvLoop& loop, const IStateReader& reader, IReportStatusWriter& statusWriter,
                          IHttpClient& client, report_config config, status_identity identity);
    ~StatusReportComponent() override;

    StatusReportComponent(const StatusReportComponent&) = delete;
    StatusReportComponent& operator=(const StatusReportComponent&) = delete;

    const char* get_name() const override;
    bool initialize() override;
    bool start() override;
    void stop() override;

private:
    // timer
    void report();
    // worker result
    void handleResult(const http_result& result);

    UvLoop& _loop;
    const IStateReader& _reader;
    IReportStatusWriter& _statusWriter;
    IHttpClient& _client;
    report_config _config;
    status_identity _identity;
    std::unique_ptr<UvTimer> _timer;
    uint64_t _consecutiveFailures;
    // last member: joined first on destruction
    std::unique_ptr<BlockingWorker> _worker;
};
