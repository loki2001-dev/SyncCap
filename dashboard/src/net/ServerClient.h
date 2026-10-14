#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "model/DashboardModel.h"
#include "core/BlockingQueue.h"
#include "model/Messages.h"
#include "network/IHttpClient.h"

using model_update_t = std::function<void(DashboardModel&)>;

/**
 * @brief the dashboard's only network thread (P2): polls the OpenAPI server and sends control requests
 *
 * every period: GET /status, GET /health, and GET /control/{id} for each request still pending.
 * results go to the UI thread as updates in a bounded inbox; the UI thread never waits on the network
 */
class ServerClient {
public:
    ServerClient(std::string baseUrl, IHttpClient& http, std::chrono::milliseconds pollPeriod,
                 std::chrono::milliseconds timeout, BlockingQueue<model_update_t>& inbox);
    ~ServerClient();

    ServerClient(const ServerClient&) = delete;
    ServerClient& operator=(const ServerClient&) = delete;

    void start();
    // joins the thread; a request in flight finishes first (bounded by the timeout)
    void stop();

    // UI thread. false: a previous control request is still queued
    bool requestControl(control_action action);

    static constexpr size_t MAX_PENDING_REQUESTS = 16;
    static constexpr std::chrono::seconds PENDING_GIVE_UP{15};

private:
    struct pending_request {
        std::string requestId;
        std::chrono::steady_clock::time_point since;
    };

    void run();
    void poll();
    void submit(control_action action);
    void publish(model_update_t update);

    std::string _baseUrl;
    IHttpClient& _http;
    std::chrono::milliseconds _pollPeriod;
    std::chrono::milliseconds _timeout;
    BlockingQueue<model_update_t>& _inbox;
    BlockingQueue<control_action> _requests;
    std::atomic<bool> _running;
    std::thread _thread;

    // worker thread only
    std::vector<pending_request> _pending;
};
