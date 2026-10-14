#include "catch_amalgamated.hpp"

#include <cstdint>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model/DashboardModel.h"
#include "net/PreviewClient.h"

namespace {

// a 16x8 red baseline JPEG made by ffmpeg (lavfi color source, -q:v 5)
const uint8_t TINY_JPEG[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x02, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x00, 0xFF, 0xFE, 0x00, 0x10, 0x4C, 0x61, 0x76, 0x63, 0x36, 0x32, 0x2E, 0x32, 0x38, 0x2E, 0x31, 0x30, 0x31, 0x00,
    0xFF, 0xDB, 0x00, 0x43, 0x00, 0x08, 0x0A, 0x0A, 0x0B, 0x0A, 0x0B, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x10, 0x0F,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x12, 0x12, 0x12, 0x15, 0x15, 0x15, 0x12, 0x12,
    0x12, 0x10, 0x10, 0x12, 0x12, 0x14, 0x14, 0x15, 0x15, 0x17, 0x17, 0x17, 0x15, 0x15, 0x15, 0x15, 0x17, 0x17, 0x19,
    0x19, 0x19, 0x1E, 0x1E, 0x1C, 0x1C, 0x23, 0x23, 0x24, 0x2B, 0x2B, 0x33, 0xFF, 0xC4, 0x00, 0x4C, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x07, 0x10, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xC0, 0x00, 0x11, 0x08,
    0x00, 0x08, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03,
    0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0x8B, 0x00, 0x4D, 0x7F, 0x7F, 0xFF, 0xD9,
};

std::string tinyJpeg() {
    return std::string(reinterpret_cast<const char*>(TINY_JPEG), sizeof(TINY_JPEG));
}

http_result answer(int status, const std::string& body) {
    http_result result;
    result.status = status;
    result.ok = status >= 200 && status < 300;
    result.body = body;
    return result;
}

// the collector preview endpoint with four channels; can pretend a tick was replaced mid-fetch
class FakePreviewServer : public IHttpClient {
public:
    http_result post(const std::string&, const std::string&, const std::string&, std::chrono::milliseconds) override {
        return answer(405, "");
    }

    http_result get(const std::string& url, std::chrono::milliseconds) override {
        const std::lock_guard<std::mutex> lock(_mutex);
        requests.push_back(url);
        if (url.find("/preview/") != std::string::npos) {
            if (replaceTick && url.find("/preview/2.jpg") != std::string::npos) {
                return answer(409, R"({"error":"tick replaced"})");
            }
            return answer(200, tinyJpeg());
        }
        return answer(200, R"({"tick":42,"offset_us":310,"verdict":"locked","channels":[
            {"index":0,"id":0,"source":"file","latch_us":0},{"index":1,"id":1,"source":"file","latch_us":120},
            {"index":2,"id":2,"source":"file","latch_us":310},{"index":3,"id":3,"source":"synthetic","latch_us":45}]})");
    }

    size_t count(const std::string& part) {
        const std::lock_guard<std::mutex> lock(_mutex);
        size_t total = 0;
        for (const std::string& url : requests) {
            total += url.find(part) != std::string::npos ? 1 : 0;
        }
        return total;
    }

    bool replaceTick = false;

private:
    std::mutex _mutex;
    std::vector<std::string> requests;
};

void runUntil(PreviewClient& client, BlockingQueue<model_update_t>& inbox, DashboardModel& model,
              FakePreviewServer& server, size_t imageRequests) {
    client.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && server.count(".jpg") < imageRequests) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.stop();
    model_update_t update;
    while (inbox.tryPop(update)) {
        update(model);
    }
}

}  // namespace

TEST_CASE("PreviewClient decodes a baseline JPEG to RGBA") {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    REQUIRE(PreviewClient::decodeJpeg(tinyJpeg(), width, height, rgba));
    REQUIRE(width == 16);
    REQUIRE(height == 8);
    REQUIRE(rgba.size() == 16 * 8 * 4);
    REQUIRE(rgba[0] > 200);  // red
    REQUIRE(rgba[1] < 60);
    REQUIRE(rgba[3] == 255);
}

TEST_CASE("PreviewClient rejects bytes that are not a JPEG") {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    REQUIRE_FALSE(PreviewClient::decodeJpeg("", width, height, rgba));
    REQUIRE_FALSE(PreviewClient::decodeJpeg("not a jpeg at all", width, height, rgba));
    REQUIRE_FALSE(PreviewClient::decodeJpeg(tinyJpeg().substr(0, 40), width, height, rgba));
}

TEST_CASE("PreviewClient fetches every channel of one tick and hands it to the model") {
    FakePreviewServer server;
    BlockingQueue<model_update_t> inbox(64, overflow_policy::drop_oldest);
    DashboardModel model;
    PreviewClient client("http://127.0.0.1:8090/", server, std::chrono::milliseconds(20),
                         std::chrono::milliseconds(100), inbox);
    runUntil(client, inbox, model, server, 4);

    const preview_state& preview = model.get_preview();
    REQUIRE(preview.link == link_state::connected);
    REQUIRE(preview.tick == 42);
    REQUIRE(preview.offsetUs == 310);
    REQUIRE(preview.frames.size() == 4);
    REQUIRE(preview.frames[2].latchUs == 310);
    REQUIRE(preview.frames[3].sourceType == "synthetic");
    REQUIRE(preview.frames[0].width == 16);
    REQUIRE(preview.version == 1);
    // every image request named the tick, and the same tick is not fetched twice
    REQUIRE(server.count(".jpg?tick=42") == 4);
}

TEST_CASE("PreviewClient drops a set whose tick was replaced while fetching") {
    FakePreviewServer server;
    server.replaceTick = true;
    BlockingQueue<model_update_t> inbox(64, overflow_policy::drop_oldest);
    DashboardModel model;
    PreviewClient client("http://127.0.0.1:8090", server, std::chrono::milliseconds(20), std::chrono::milliseconds(100),
                         inbox);
    runUntil(client, inbox, model, server, 6);
    REQUIRE(model.get_preview().version == 0);
    REQUIRE(model.get_preview().frames.empty());
}

TEST_CASE("DashboardModel reports an unreachable preview once") {
    DashboardModel model;
    model.applyPreviewUnavailable(false, "Could not establish connection");
    model.applyPreviewUnavailable(false, "Could not establish connection");
    REQUIRE(model.get_preview().link == link_state::unreachable);
    int warnings = 0;
    for (const event_entry& event : model.get_events()) {
        warnings += event.text.find("video preview unreachable") != std::string::npos ? 1 : 0;
    }
    REQUIRE(warnings == 1);
    model.applyPreviewUnavailable(true, "no preview yet (not recording?)");
    REQUIRE(model.get_preview().link == link_state::connected);
}

TEST_CASE("DashboardModel clears the pictures when the preview goes away") {
    DashboardModel model;
    preview_update update;
    update.tick = 7;
    update.frames.resize(4);
    model.applyPreview(update);
    REQUIRE(model.get_preview().frames.size() == 4);
    const uint64_t version = model.get_preview().version;
    model.applyPreviewUnavailable(true, "no preview yet (not recording?)");
    REQUIRE(model.get_preview().frames.empty());
    REQUIRE(model.get_preview().version == version + 1);
}
