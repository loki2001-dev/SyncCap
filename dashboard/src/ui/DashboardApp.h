#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "model/DashboardModel.h"
#include "ui/DashboardView.h"
#include "ui/PreviewTextures.h"
#include "net/PreviewClient.h"
#include "net/ServerClient.h"
#include "core/BlockingQueue.h"
#include "network/HttpLibClient.h"

struct GLFWwindow;

struct dashboard_options {
    std::string serverUrl = "http://127.0.0.1:8080";
    std::chrono::milliseconds pollPeriod{1000};
    std::chrono::milliseconds timeout{1500};
    bool previewEnabled = true;
    std::string previewUrl = "http://127.0.0.1:8090";
    std::chrono::milliseconds previewPeriod{33};  // 30 fps; a tick already shown is never fetched twice
    int width = 1440;
    int height = 900;
};

/**
 * @brief window, ImGui context and the UI loop. the UI thread is the dashboard's single loop thread (P1)
 *
 * each frame: drain at most a bounded number of server updates from the inbox, draw, hand clicks to the
 * server client. the loop waits for input with a short timeout instead of spinning
 */
class DashboardApp {
public:
    explicit DashboardApp(dashboard_options options);
    ~DashboardApp();

    DashboardApp(const DashboardApp&) = delete;
    DashboardApp& operator=(const DashboardApp&) = delete;

    // 0: window closed normally
    int run();

    static constexpr size_t INBOX_CAPACITY = 256;
    static constexpr int UPDATES_PER_FRAME = 64;

private:
    bool createWindow();
    void setupImGui();
    void destroyWindow();
    void drainInbox();

    dashboard_options _options;
    GLFWwindow* _window;
    bool _imguiReady;
    DashboardModel _model;
    DashboardView _view;
    HttpLibClient _http;
    HttpLibClient _previewHttp{true};  // five requests per frame at 30 fps: keep the connection
    BlockingQueue<model_update_t> _inbox;
    PreviewTextures _textures;
    // last members: joined first on destruction
    std::unique_ptr<ServerClient> _client;
    std::unique_ptr<PreviewClient> _previewClient;
};
