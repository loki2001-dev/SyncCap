#pragma once

#include <memory>

#include <httplib.h>

#include "config/AppConfig.h"
#include "core/IComponent.h"
#include "core/UvLoop.h"
#include "network/HttpServerHost.h"
#include "pipeline/IPreviewProvider.h"

/**
 * @brief serves the live preview to the dashboard over HTTP
 *
 * provides: GET /preview (metadata of the latest set), GET /preview/{index}.jpg?tick=T (one channel of that set)
 * requires: IPreviewProvider (the video pipeline); the loop, for every read (P1)
 *
 * the HTTP server runs on HttpServerHost threads (transport, P2); handlers only parse, then read through the loop
 */
class PreviewComponent : public IComponent {
public:
    PreviewComponent(UvLoop& loop, const IPreviewProvider& provider, preview_config config);
    ~PreviewComponent() override;

    PreviewComponent(const PreviewComponent&) = delete;
    PreviewComponent& operator=(const PreviewComponent&) = delete;

    const char* get_name() const override;
    bool initialize() override;
    bool start() override;
    void stop() override;

    // 0 before start. with port 0 in the config, the port the system picked
    int get_bound_port() const;

private:
    void handleMeta(const httplib::Request& request, httplib::Response& response);
    void handleImage(const httplib::Request& request, httplib::Response& response);
    std::shared_ptr<const preview_set> readSet(const httplib::Request& request, bool& gone, bool& busy);

    UvLoop& _loop;
    const IPreviewProvider& _provider;
    preview_config _config;
    std::unique_ptr<HttpServerHost> _host;
    int _boundPort;
};
