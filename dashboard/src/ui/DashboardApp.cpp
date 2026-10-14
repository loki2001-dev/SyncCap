#include "ui/DashboardApp.h"

#include <filesystem>
#include <utility>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "util/log/Logger.h"

namespace {

constexpr const char* GLSL_VERSION = "#version 130";
constexpr int GL_MAJOR = 3;
constexpr int GL_MINOR = 0;
constexpr double WAIT_EVENTS_S = 0.05;
constexpr float BASE_FONT_SIZE = 16.0F;
constexpr float CLEAR_SHADE = 0.08F;

// a proportional system font when there is one; ImGui's built-in bitmap font otherwise
const char* findSystemFont() {
    static constexpr const char* CANDIDATES[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    for (const char* candidate : CANDIDATES) {
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return nullptr;
}

void onGlfwError(int code, const char* description) {
    LOG_ERROR(app, sys, "glfw error code={} message={}", code, description != nullptr ? description : "");
}

}  // namespace

DashboardApp::DashboardApp(dashboard_options options)
    : _options(std::move(options))
    , _window(nullptr)
    , _imguiReady(false)
    , _view(_options.serverUrl)
    , _inbox(INBOX_CAPACITY, overflow_policy::drop_oldest) {
}

DashboardApp::~DashboardApp() {
    if (_previewClient) {
        _previewClient->stop();
    }
    if (_client) {
        _client->stop();
    }
    destroyWindow();
}

bool DashboardApp::createWindow() {
    glfwSetErrorCallback(onGlfwError);
    if (glfwInit() == GLFW_FALSE) {
        LOG_ERROR(app, life, "glfw init failed");
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, GL_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, GL_MINOR);
    _window = glfwCreateWindow(_options.width, _options.height, "SyncCap dashboard", nullptr, nullptr);
    if (_window == nullptr) {
        LOG_ERROR(app, life, "window create failed width={} height={}", _options.width, _options.height);
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(_window);
    glfwSwapInterval(1);
    return true;
}

void DashboardApp::setupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0F;
    style.ChildRounding = 4.0F;
    style.FrameRounding = 4.0F;

    // follow the monitor scale (125 % and 150 % laptop screens)
    float scale = 1.0F;
    float scaleY = 1.0F;
    glfwGetWindowContentScale(_window, &scale, &scaleY);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    if (const char* font = findSystemFont()) {
        io.Fonts->AddFontFromFileTTF(font, BASE_FONT_SIZE);
        LOG_INFO(app, life, "dashboard font path={} size={} scale={}", font, BASE_FONT_SIZE, scale);
    } else {
        io.Fonts->AddFontDefault();
    }
    style.FontSizeBase = BASE_FONT_SIZE;

    ImGui_ImplGlfw_InitForOpenGL(_window, true);
    ImGui_ImplOpenGL3_Init(GLSL_VERSION);
    _imguiReady = true;
}

void DashboardApp::destroyWindow() {
    // textures belong to the GL context: delete them while it still exists
    if (_window != nullptr) {
        _textures.release();
    }
    if (_imguiReady) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        _imguiReady = false;
    }
    if (_window != nullptr) {
        glfwDestroyWindow(_window);
        _window = nullptr;
        glfwTerminate();
    }
}

void DashboardApp::drainInbox() {
    model_update_t update;
    for (int count = 0; count < UPDATES_PER_FRAME && _inbox.tryPop(update); ++count) {
        update(_model);
    }
}

int DashboardApp::run() {
    if (!createWindow()) {
        return 1;
    }
    setupImGui();
    _client = std::make_unique<ServerClient>(_options.serverUrl, _http, _options.pollPeriod, _options.timeout, _inbox);
    _client->start();
    if (_options.previewEnabled) {
        _previewClient = std::make_unique<PreviewClient>(_options.previewUrl, _previewHttp, _options.previewPeriod,
                                                         _options.timeout, _inbox);
        _previewClient->start();
    }
    LOG_INFO(app, life, "dashboard started server={} poll_ms={} preview={} preview_period_ms={}", _options.serverUrl,
             _options.pollPeriod.count(), _options.previewEnabled ? _options.previewUrl : std::string("off"),
             _options.previewPeriod.count());

    while (glfwWindowShouldClose(_window) == GLFW_FALSE) {
        // sleep until input or the timeout: the window stays responsive without burning a core
        glfwWaitEventsTimeout(WAIT_EVENTS_S);
        drainInbox();
        _textures.update(_model.get_preview());

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const view_actions actions = _view.render(_model, _textures.get_textures());
        ImGui::Render();

        if (actions.start || actions.stop) {
            const control_action action = actions.start ? control_action::start : control_action::stop;
            _model.noteControlRequested(action);
            if (!_client->requestControl(action)) {
                _model.noteControlBusy(action);
            }
        }
        if (actions.quit) {
            glfwSetWindowShouldClose(_window, GLFW_TRUE);
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(_window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(CLEAR_SHADE, CLEAR_SHADE, CLEAR_SHADE, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(_window);
    }
    LOG_INFO(app, life, "dashboard window closed");
    if (_previewClient) {
        _previewClient->stop();
    }
    _client->stop();
    return 0;
}
