#pragma once

#include <string>
#include <vector>

#include "model/DashboardModel.h"
#include "ui/PreviewTextures.h"
#include "imgui.h"

// what the user asked for in this frame
struct view_actions {
    bool start = false;
    bool stop = false;
    bool quit = false;
};

/**
 * @brief draws the dashboard with Dear ImGui from a model. UI thread only
 *
 * one screen: live video (2x2 tiles of one synchronized tick, click to enlarge) and channels on the left;
 * sync, sensors, storage and events on the right. the only state kept here is the enlarged tile
 */
class DashboardView {
public:
    explicit DashboardView(std::string serverUrl);

    view_actions render(const DashboardModel& model, const std::vector<preview_texture>& textures);

private:
    void drawHeader(const DashboardModel& model, view_actions& actions);
    void drawBody(const DashboardModel& model, const std::vector<preview_texture>& textures);
    void drawVideo(const DashboardModel& model, const std::vector<preview_texture>& textures);
    bool drawTile(const preview_frame& frame, const preview_texture* texture, const ImVec2& origin, const ImVec2& size);
    void drawChannels(const DashboardModel& model);
    void drawSync(const DashboardModel& model);
    void drawSensors(const DashboardModel& model);
    void drawStorage(const DashboardModel& model);
    void drawEvents(const DashboardModel& model);

    std::string _serverUrl;
    int _focused = -1;
};
