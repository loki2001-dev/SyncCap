#include "ui/DashboardView.h"

#include <cfloat>
#include <cstdio>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

namespace {

const ImVec4 COLOR_GOOD{0.30F, 0.82F, 0.42F, 1.0F};
const ImVec4 COLOR_WARN{0.96F, 0.74F, 0.22F, 1.0F};
const ImVec4 COLOR_BAD{0.95F, 0.36F, 0.32F, 1.0F};
const ImVec4 COLOR_MUTED{0.58F, 0.60F, 0.64F, 1.0F};
const ImVec4 COLOR_ACCENT{0.40F, 0.66F, 0.98F, 1.0F};

constexpr float TITLE_SCALE = 1.35F;
constexpr float PHASE_SCALE = 1.6F;
constexpr float LOCK_SCALE = 1.8F;
constexpr float DOT_RADIUS_EM = 0.32F;
constexpr float SPARK_HEIGHT_EM = 1.6F;
constexpr float PLOT_HEIGHT_EM = 4.0F;
constexpr float SENSOR_PLOT_HEIGHT_EM = 2.6F;
constexpr float FPS_MIN_REQUIRED = 20.0F;
constexpr float FPS_PLOT_MAX = 35.0F;
constexpr float OFFSET_PLOT_HEADROOM = 1.5F;
constexpr double BYTES_PER_MB = 1024.0 * 1024.0;
constexpr int LABEL_SIZE = 96;
constexpr float LEFT_COLUMN_SHARE = 0.62F;
constexpr float VIDEO_PANEL_MIN_EM = 12.0F;
constexpr float CHANNEL_PANEL_MIN_EM = 9.0F;
constexpr size_t RECENT_FILES_SHOWN = 3;
constexpr double MICROS_PER_MILLI = 1000.0;
constexpr double SYNC_LIMIT_MS = 2.0;
constexpr float DEFAULT_ASPECT = 16.0F / 9.0F;
constexpr float CAPTION_MARGIN = 6.0F;
constexpr float CAPTION_PAD = 2.0F;
constexpr float HOVER_BORDER = 2.0F;
const ImVec4 COLOR_TILE{0.0F, 0.0F, 0.0F, 1.0F};
const ImVec4 COLOR_CAPTION{0.0F, 0.0F, 0.0F, 0.65F};
const ImVec4 COLOR_TEXT{1.0F, 1.0F, 1.0F, 1.0F};

void statusDot(const ImVec4& color) {
    const float radius = ImGui::GetFontSize() * DOT_RADIUS_EM;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cursor.x + radius, cursor.y + lineHeight * 0.5F), radius,
                                                ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(radius * 2.0F, lineHeight));
    ImGui::SameLine();
}

void labeled(const char* label, const ImVec4& color, const std::string& value) {
    ImGui::TextColored(COLOR_MUTED, "%s", label);
    ImGui::SameLine();
    ImGui::TextColored(color, "%s", value.c_str());
}

ImVec4 phaseColor(acquisition_phase phase) {
    switch (phase) {
        case acquisition_phase::running:
            return COLOR_GOOD;
        case acquisition_phase::stopping:
            return COLOR_WARN;
        case acquisition_phase::error:
            return COLOR_BAD;
        case acquisition_phase::idle:
        default:
            return COLOR_MUTED;
    }
}

std::string format(const char* pattern, double value) {
    char text[LABEL_SIZE];
    std::snprintf(text, sizeof(text), pattern, value);
    return text;
}

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char character) {
        return static_cast<char>(character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character);
    });
    return text;
}

void plotSeries(const char* id, const MetricSeries& series, float scaleMin, float scaleMax, float heightEm,
                const char* overlay) {
    const std::vector<float> values = series.get_ordered();
    const float height = ImGui::GetFontSize() * heightEm;
    if (values.empty()) {
        ImGui::Dummy(ImVec2(-1.0F, height));
        return;
    }
    ImGui::PlotLines(id, values.data(), static_cast<int>(values.size()), 0, overlay, scaleMin, scaleMax,
                     ImVec2(-1.0F, height));
}

void sectionTitle(const char* text) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.1F);
    ImGui::TextColored(COLOR_ACCENT, "%s", text);
    ImGui::PopFont();
    ImGui::Separator();
}

}  // namespace

DashboardView::DashboardView(std::string serverUrl)
    : _serverUrl(std::move(serverUrl)) {
}

view_actions DashboardView::render(const DashboardModel& model, const std::vector<preview_texture>& textures) {
    view_actions actions;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("SyncCap dashboard", nullptr, flags);

    drawHeader(model, actions);
    ImGui::Spacing();
    drawBody(model, textures);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
        actions.quit = true;
    }
    ImGui::End();
    return actions;
}

void DashboardView::drawBody(const DashboardModel& model, const std::vector<preview_texture>& textures) {
    // one screen: video and channels on the left, sync, sensors, storage and events on the right
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gap = style.ItemSpacing.x;
    const float left = (available.x - gap) * LEFT_COLUMN_SHARE;
    const float right = available.x - gap - left;

    // the video area is as tall as two 16:9 rows of tiles need, up to a share of the screen
    const float tileWidth = (left - style.WindowPadding.x * 2.0F - gap) * 0.5F;
    const float wanted =
        tileWidth / DEFAULT_ASPECT * 2.0F + gap + ImGui::GetFrameHeightWithSpacing() + style.WindowPadding.y * 2.0F;
    const float minimumBelow = ImGui::GetFontSize() * CHANNEL_PANEL_MIN_EM;
    const float videoHeight = std::max(ImGui::GetFontSize() * VIDEO_PANEL_MIN_EM,
                                       std::min(wanted, available.y - minimumBelow - style.ItemSpacing.y));

    ImGui::BeginGroup();
    ImGui::BeginChild("video", ImVec2(left, videoHeight), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawVideo(model, textures);
    ImGui::EndChild();
    ImGui::BeginChild("channels", ImVec2(left, 0.0F), ImGuiChildFlags_Borders);
    drawChannels(model);
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginChild("side", ImVec2(right, 0.0F), ImGuiChildFlags_Borders);
    drawSync(model);
    ImGui::Spacing();
    drawSensors(model);
    ImGui::Spacing();
    drawStorage(model);
    ImGui::Spacing();
    drawEvents(model);
    ImGui::EndChild();
}

void DashboardView::drawVideo(const DashboardModel& model, const std::vector<preview_texture>& textures) {
    const preview_state& preview = model.get_preview();
    // title line: link, tick, the offset of this very set, rate
    ImGui::TextColored(COLOR_ACCENT, "Live video");
    ImGui::SameLine(0.0F, ImGui::GetFontSize());
    const ImVec4 linkColor = preview.link == link_state::connected     ? COLOR_GOOD
                             : preview.link == link_state::unreachable ? COLOR_BAD
                                                                       : COLOR_WARN;
    statusDot(linkColor);
    if (!preview.error.empty() || preview.frames.empty()) {
        ImGui::TextColored(preview.link == link_state::unreachable ? COLOR_BAD : COLOR_MUTED, "%s",
                           preview.error.empty() ? toString(preview.link) : preview.error.c_str());
    } else {
        labeled("tick", COLOR_MUTED, std::to_string(preview.tick));
        ImGui::SameLine(0.0F, ImGui::GetFontSize());
        const double offsetMs = static_cast<double>(preview.offsetUs) / MICROS_PER_MILLI;
        labeled("set offset", offsetMs <= SYNC_LIMIT_MS ? COLOR_GOOD : COLOR_BAD, format("%.3f ms", offsetMs));
        ImGui::SameLine(0.0F, ImGui::GetFontSize());
        labeled("rate", COLOR_MUTED, format("%.1f sets/s", preview.setsPerSecond));
        ImGui::SameLine(0.0F, ImGui::GetFontSize());
        ImGui::TextColored(COLOR_MUTED, _focused >= 0 ? "click to return to 4 channels"
                                                      : "same tick on every tile, click to enlarge");
    }

    const ImVec2 area = ImGui::GetContentRegionAvail();
    if (preview.frames.empty()) {
        const char* text = "no video: start recording; the collector serves it on port 8090 by default";
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (area.x - textSize.x) * 0.5F,
                                   ImGui::GetCursorPosY() + (area.y - textSize.y) * 0.5F));
        ImGui::TextColored(COLOR_MUTED, "%s", text);
        return;
    }
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    auto textureOf = [&textures](int index) -> const preview_texture* {
        const auto slot = static_cast<size_t>(index);
        return index >= 0 && slot < textures.size() && textures[slot].id != 0 ? &textures[slot] : nullptr;
    };

    const preview_frame* focused = nullptr;
    for (const preview_frame& frame : preview.frames) {
        if (frame.index == _focused) {
            focused = &frame;
        }
    }
    if (focused != nullptr) {
        if (drawTile(*focused, textureOf(focused->index), start, area)) {
            _focused = -1;
        }
        return;
    }
    _focused = -1;
    const int count = static_cast<int>(preview.frames.size());
    const int columns = count <= 1 ? 1 : 2;
    const int rows = (count + columns - 1) / columns;
    const ImVec2 cell((area.x - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns),
                      (area.y - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows));
    for (int position = 0; position < count; ++position) {
        const preview_frame& frame = preview.frames[static_cast<size_t>(position)];
        const ImVec2 origin(start.x + static_cast<float>(position % columns) * (cell.x + gap),
                            start.y + static_cast<float>(position / columns) * (cell.y + gap));
        if (drawTile(frame, textureOf(frame.index), origin, cell)) {
            _focused = frame.index;
        }
    }
}

bool DashboardView::drawTile(const preview_frame& frame, const preview_texture* texture, const ImVec2& origin,
                             const ImVec2& size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(COLOR_TILE));
    // keep the picture aspect ratio inside the cell
    const float aspect =
        frame.height > 0 ? static_cast<float>(frame.width) / static_cast<float>(frame.height) : DEFAULT_ASPECT;
    ImVec2 imageSize(size.x, size.x / aspect);
    if (imageSize.y > size.y) {
        imageSize = ImVec2(size.y * aspect, size.y);
    }
    const ImVec2 imageMin(origin.x + (size.x - imageSize.x) * 0.5F, origin.y + (size.y - imageSize.y) * 0.5F);
    const ImVec2 imageMax(imageMin.x + imageSize.x, imageMin.y + imageSize.y);
    if (texture != nullptr) {
        draw->AddImage(static_cast<ImTextureID>(texture->id), imageMin, imageMax);
    }
    // caption: channel, source, latch time relative to the earliest channel of the set
    char caption[LABEL_SIZE * 2];
    std::snprintf(caption, sizeof(caption), " CH%d  %s  latch +%.3f ms ", frame.channelId, frame.sourceType.c_str(),
                  static_cast<double>(frame.latchUs) / MICROS_PER_MILLI);
    const ImVec2 textSize = ImGui::CalcTextSize(caption);
    const ImVec2 captionMin(imageMin.x + CAPTION_MARGIN, imageMin.y + CAPTION_MARGIN);
    draw->AddRectFilled(captionMin, ImVec2(captionMin.x + textSize.x, captionMin.y + textSize.y + CAPTION_PAD * 2.0F),
                        ImGui::GetColorU32(COLOR_CAPTION), CAPTION_PAD);
    draw->AddText(ImVec2(captionMin.x, captionMin.y + CAPTION_PAD), ImGui::GetColorU32(COLOR_TEXT), caption);

    ImGui::SetCursorScreenPos(origin);
    char id[LABEL_SIZE];
    std::snprintf(id, sizeof(id), "##tile%d", frame.index);
    const bool clicked = ImGui::InvisibleButton(id, size);
    if (ImGui::IsItemHovered()) {
        draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(COLOR_ACCENT), 0.0F, 0,
                      HOVER_BORDER);
    }
    return clicked;
}

void DashboardView::drawHeader(const DashboardModel& model, view_actions& actions) {
    const status_snapshot& snapshot = model.get_status();
    const system_status& status = snapshot.status;

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * TITLE_SCALE);
    ImGui::TextUnformatted("SyncCap acquisition dashboard");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(COLOR_MUTED, "  %s", _serverUrl.c_str());

    // links: server, broker, collector, freshness
    const link_state link = model.get_server_link();
    statusDot(link == link_state::connected ? COLOR_GOOD : link == link_state::unreachable ? COLOR_BAD : COLOR_WARN);
    labeled("server", COLOR_MUTED, toString(link));
    if (link == link_state::unreachable && !model.get_server_error().empty()) {
        ImGui::SameLine();
        ImGui::TextColored(COLOR_BAD, "(%s)", model.get_server_error().c_str());
    }
    ImGui::SameLine(0.0F, ImGui::GetFontSize() * 1.5F);
    statusDot(model.isMqttConnected() ? COLOR_GOOD : COLOR_BAD);
    labeled("mqtt", COLOR_MUTED, model.isMqttConnected() ? "connected" : "disconnected");
    ImGui::SameLine(0.0F, ImGui::GetFontSize() * 1.5F);
    const std::optional<bool> online = snapshot.meta.collectorOnline;
    statusDot(!online.has_value() ? COLOR_MUTED : (*online ? COLOR_GOOD : COLOR_BAD));
    labeled("collector", COLOR_MUTED, !online.has_value() ? "unknown" : (*online ? "online" : "offline"));
    if (snapshot.available) {
        ImGui::SameLine(0.0F, ImGui::GetFontSize() * 1.5F);
        const ImVec4 ageColor = snapshot.meta.stale ? COLOR_BAD : COLOR_MUTED;
        labeled("status age", ageColor,
                format("%.1f s", snapshot.meta.ageS) + " via " + snapshot.meta.receivedVia +
                    (snapshot.meta.stale ? "  STALE" : ""));
        ImGui::SameLine(0.0F, ImGui::GetFontSize() * 1.5F);
        labeled("device", COLOR_MUTED, status.deviceId + format("  up %.0f s", status.uptimeS));
    }
    ImGui::Separator();

    // acquisition state and commands
    const acquisition_phase phase = snapshot.available ? status.acquisition.phase : acquisition_phase::idle;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * PHASE_SCALE);
    ImGui::TextColored(snapshot.available ? phaseColor(phase) : COLOR_MUTED, "%s",
                       snapshot.available ? upper(toString(phase)).c_str() : "NO STATUS YET");
    ImGui::PopFont();
    ImGui::SameLine(0.0F, ImGui::GetFontSize() * 1.5F);
    ImGui::BeginGroup();
    if (snapshot.available) {
        labeled("session", COLOR_MUTED, status.acquisition.sessionId.empty() ? "-" : status.acquisition.sessionId);
        labeled("reason", phase == acquisition_phase::error ? COLOR_BAD : COLOR_MUTED,
                status.acquisition.reason.empty() ? "-" : status.acquisition.reason);
    } else {
        ImGui::TextColored(COLOR_MUTED, "waiting for the collector to report to the server");
    }
    ImGui::EndGroup();
    // the last command and its answer from the collector
    if (!model.get_controls().empty()) {
        const control_entry& last = model.get_controls().front();
        ImGui::SameLine(0.0F, ImGui::GetFontSize() * 2.0F);
        ImGui::BeginGroup();
        ImGui::TextColored(COLOR_MUTED, "last command  %s  %s", last.time.c_str(), last.command.c_str());
        const ImVec4 color = last.result == "accepted"                                ? COLOR_GOOD
                             : (last.result == "rejected" || last.result == "failed") ? COLOR_BAD
                                                                                      : COLOR_WARN;
        ImGui::TextColored(color, "%s%s%s", last.result.c_str(), last.reason.empty() ? "" : "  ", last.reason.c_str());
        ImGui::EndGroup();
    }

    const float buttonWidth = ImGui::GetFontSize() * 7.0F;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - buttonWidth * 2.0F - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(!model.canStart());
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18F, 0.45F, 0.26F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22F, 0.56F, 0.32F, 1.0F));
    if (ImGui::Button("Start", ImVec2(buttonWidth, ImGui::GetFrameHeight() * 1.4F))) {
        actions.start = true;
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!model.canStop());
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55F, 0.20F, 0.18F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68F, 0.25F, 0.22F, 1.0F));
    if (ImGui::Button("Stop", ImVec2(buttonWidth, ImGui::GetFrameHeight() * 1.4F))) {
        actions.stop = true;
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
}

void DashboardView::drawChannels(const DashboardModel& model) {
    sectionTitle("Channels");
    const status_snapshot& snapshot = model.get_status();
    if (!snapshot.available || snapshot.status.channels.empty()) {
        ImGui::TextColored(COLOR_MUTED, "no channel data");
        return;
    }
    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("channel_table", 10, flags)) {
        return;
    }
    ImGui::TableSetupColumn("CH");
    ImGui::TableSetupColumn("Source");
    ImGui::TableSetupColumn("FPS");
    ImGui::TableSetupColumn("Queue");
    ImGui::TableSetupColumn("Encoded");
    ImGui::TableSetupColumn("Dropped");
    ImGui::TableSetupColumn("Dup");
    ImGui::TableSetupColumn("Missed");
    ImGui::TableSetupColumn("Enc ms");
    ImGui::TableSetupColumn("FPS (last 5 min)", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const bool running = snapshot.status.acquisition.phase == acquisition_phase::running;
    for (const channel_stats& channel : snapshot.status.channels) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", channel.id);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(channel.sourceType.empty() ? "-" : channel.sourceType.c_str());
        ImGui::TableNextColumn();
        const bool fpsOk = channel.fps >= FPS_MIN_REQUIRED;
        ImGui::TextColored(!running ? COLOR_MUTED : (fpsOk ? COLOR_GOOD : COLOR_BAD), "%.2f", channel.fps);
        ImGui::TableNextColumn();
        ImGui::Text("%zu", channel.queueLength);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(channel.framesEncoded));
        ImGui::TableNextColumn();
        ImGui::TextColored(channel.dropped > 0 ? COLOR_WARN : COLOR_MUTED, "%llu",
                           static_cast<unsigned long long>(channel.dropped));
        ImGui::TableNextColumn();
        ImGui::TextColored(channel.duplicated > 0 ? COLOR_WARN : COLOR_MUTED, "%llu",
                           static_cast<unsigned long long>(channel.duplicated));
        ImGui::TableNextColumn();
        ImGui::TextColored(channel.missedTicks > 0 ? COLOR_WARN : COLOR_MUTED, "%llu",
                           static_cast<unsigned long long>(channel.missedTicks));
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", channel.encodeMsAvg);
        ImGui::TableNextColumn();
        if (const MetricSeries* history = model.findFpsHistory(channel.id)) {
            char id[LABEL_SIZE];
            std::snprintf(id, sizeof(id), "##fps%d", channel.id);
            plotSeries(id, *history, 0.0F, FPS_PLOT_MAX, SPARK_HEIGHT_EM, nullptr);
        }
    }
    ImGui::EndTable();
}

void DashboardView::drawSync(const DashboardModel& model) {
    sectionTitle("Frame synchronization");
    const status_snapshot& snapshot = model.get_status();
    if (!snapshot.available) {
        ImGui::TextColored(COLOR_MUTED, "no data");
        return;
    }
    const sync_report& sync = snapshot.status.sync;
    const bool running = snapshot.status.acquisition.phase == acquisition_phase::running;
    const double tolerance = sync.toleranceMs > 0.0 ? sync.toleranceMs : 2.0;

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * LOCK_SCALE);
    if (!running) {
        ImGui::TextColored(COLOR_MUTED, "NOT RECORDING");
    } else if (sync.locked) {
        ImGui::TextColored(COLOR_GOOD, "LOCKED");
    } else {
        ImGui::TextColored(COLOR_BAD, "NOT LOCKED");
    }
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(COLOR_MUTED, "tolerance %.2f ms, window 10 s", tolerance);

    // window max against the tolerance
    const float fraction = static_cast<float>(std::min(1.0, sync.windowMaxOffsetMs / tolerance));
    char overlay[LABEL_SIZE];
    std::snprintf(overlay, sizeof(overlay), "max offset %.3f / %.2f ms", sync.windowMaxOffsetMs, tolerance);
    const ImVec4 barColor = sync.windowMaxOffsetMs <= tolerance * 0.5 ? COLOR_GOOD
                            : sync.windowMaxOffsetMs <= tolerance     ? COLOR_WARN
                                                                      : COLOR_BAD;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
    ImGui::ProgressBar(fraction, ImVec2(-1.0F, 0.0F), overlay);
    ImGui::PopStyleColor();

    // offset history with the tolerance line
    const MetricSeries& history = model.get_offset_history();
    const float scaleMax = std::max(static_cast<float>(tolerance) * OFFSET_PLOT_HEADROOM, history.get_max() * 1.1F);
    const ImVec2 plotOrigin = ImGui::GetCursorScreenPos();
    char plotOverlay[LABEL_SIZE];
    std::snprintf(plotOverlay, sizeof(plotOverlay), "max offset per 10 s window (red: %.1f ms)", tolerance);
    plotSeries("##offset", history, 0.0F, scaleMax, PLOT_HEIGHT_EM, plotOverlay);
    const ImVec2 plotEnd = ImGui::GetItemRectMax();
    const float plotHeight = plotEnd.y - plotOrigin.y;
    const float lineY = plotEnd.y - plotHeight * static_cast<float>(tolerance) / scaleMax;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(plotOrigin.x, lineY), ImVec2(plotEnd.x, lineY),
                                        ImGui::GetColorU32(COLOR_BAD), 1.5F);

    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame;
    if (ImGui::BeginTable("sync_numbers", 4, flags)) {
        auto cell = [](const char* label, const std::string& value, const ImVec4& color) {
            ImGui::TableNextColumn();
            ImGui::TextColored(COLOR_MUTED, "%s", label);
            ImGui::TextColored(color, "%s", value.c_str());
        };
        cell("sets checked", std::to_string(sync.setsChecked), COLOR_MUTED);
        cell("locked", std::to_string(sync.setsLocked), COLOR_GOOD);
        cell("violations", std::to_string(sync.violations), sync.violations > 0 ? COLOR_WARN : COLOR_MUTED);
        cell("incomplete", std::to_string(sync.incomplete), sync.incomplete > 0 ? COLOR_WARN : COLOR_MUTED);
        cell("backpressure", std::to_string(sync.backpressure), sync.backpressure > 0 ? COLOR_WARN : COLOR_MUTED);
        cell("max (session)", format("%.3f ms", sync.totalMaxOffsetMs),
             sync.totalMaxOffsetMs > tolerance ? COLOR_WARN : COLOR_MUTED);
        cell("last", format("%.3f ms", sync.lastOffsetMs), COLOR_MUTED);
        ImGui::EndTable();
    }
}

void DashboardView::drawSensors(const DashboardModel& model) {
    sectionTitle("Sensors");
    const status_snapshot& snapshot = model.get_status();
    if (!snapshot.available) {
        ImGui::TextColored(COLOR_MUTED, "no data");
        return;
    }
    const sensor_state& sensor = snapshot.status.sensor;
    if (!ImGui::BeginTable("sensor_table", 3, ImGuiTableFlags_SizingStretchSame)) {
        return;
    }
    const ImVec4 valueColor = sensor.valid ? ImVec4(1.0F, 1.0F, 1.0F, 1.0F) : COLOR_BAD;
    ImGui::TableNextColumn();
    ImGui::TextColored(COLOR_MUTED, "temperature");
    ImGui::TextColored(valueColor, "%.2f C", sensor.temperatureC);
    plotSeries("##temperature", model.get_temperature_history(), FLT_MAX, FLT_MAX, SENSOR_PLOT_HEIGHT_EM, nullptr);
    ImGui::TableNextColumn();
    ImGui::TextColored(COLOR_MUTED, "humidity");
    ImGui::TextColored(valueColor, "%.1f %%", sensor.humidityPct);
    plotSeries("##humidity", model.get_humidity_history(), FLT_MAX, FLT_MAX, SENSOR_PLOT_HEIGHT_EM, nullptr);
    ImGui::TableNextColumn();
    ImGui::TextColored(COLOR_MUTED, "acceleration (dynamic)");
    ImGui::TextColored(valueColor, "%.3f g", sensor.accelerationG);
    plotSeries("##acceleration", model.get_acceleration_history(), 0.0F, FLT_MAX, SENSOR_PLOT_HEIGHT_EM, nullptr);
    ImGui::EndTable();
    if (!sensor.valid) {
        ImGui::TextColored(COLOR_BAD, "sensor reading invalid");
    }
}

void DashboardView::drawStorage(const DashboardModel& model) {
    sectionTitle("Storage");
    const status_snapshot& snapshot = model.get_status();
    if (!snapshot.available) {
        ImGui::TextColored(COLOR_MUTED, "no data");
        return;
    }
    const storage_state& storage = snapshot.status.storage;
    labeled("segment", COLOR_MUTED, storage.segment < 0 ? "-" : std::to_string(storage.segment));
    ImGui::SameLine(0.0F, ImGui::GetFontSize() * 1.5F);
    labeled("written", COLOR_MUTED, format("%.1f MB", static_cast<double>(storage.bytesWritten) / BYTES_PER_MB));
    ImGui::SameLine(0.0F, ImGui::GetFontSize() * 1.5F);
    labeled("queue", COLOR_MUTED, std::to_string(storage.queueLength));
    labeled("dropped", storage.dropped > 0 ? COLOR_WARN : COLOR_MUTED, std::to_string(storage.dropped));
    ImGui::SameLine(0.0F, ImGui::GetFontSize() * 1.5F);
    labeled("write errors", storage.writeErrors > 0 ? COLOR_BAD : COLOR_MUTED, std::to_string(storage.writeErrors));
    size_t shown = 0;
    for (auto it = storage.recentFiles.rbegin(); it != storage.recentFiles.rend() && shown < RECENT_FILES_SHOWN;
         ++it, ++shown) {
        ImGui::TextColored(COLOR_MUTED, "%s", it->c_str());
    }
}

void DashboardView::drawEvents(const DashboardModel& model) {
    sectionTitle("Events");
    ImGui::TextColored(COLOR_MUTED, "polls %llu, failed %llu", static_cast<unsigned long long>(model.get_polls()),
                       static_cast<unsigned long long>(model.get_poll_failures()));
    ImGui::BeginChild("event_list", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None);
    for (const event_entry& event : model.get_events()) {
        const ImVec4 color = event.level == event_level::error  ? COLOR_BAD
                             : event.level == event_level::warn ? COLOR_WARN
                                                                : ImVec4(0.85F, 0.87F, 0.90F, 1.0F);
        ImGui::TextColored(COLOR_MUTED, "%s", event.time.c_str());
        ImGui::SameLine();
        ImGui::TextColored(color, "%s", event.text.c_str());
    }
    ImGui::EndChild();
}
