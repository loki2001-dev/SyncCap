#include "catch_amalgamated.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "component/PreviewComponent.h"
#include "core/UvLoop.h"

namespace {

class FakePreviewProvider : public IPreviewProvider {
public:
    std::shared_ptr<const preview_set> get_latest_preview() const override {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _sets.empty() ? nullptr : _sets.back();
    }

    std::shared_ptr<const preview_set> find_preview(uint64_t tick) const override {
        const std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& set : _sets) {
            if (set->tick == tick) {
                return set;
            }
        }
        return nullptr;
    }

    void add(uint64_t tick) {
        auto set = std::make_shared<preview_set>();
        set->tick = tick;
        set->offsetNs = 250000;
        for (int index = 0; index < 4; ++index) {
            preview_image image;
            image.index = index;
            image.channelId = 10 + index;
            image.sourceType = "synthetic";
            image.timestampNs = 1000000 + index * 50000;
            image.width = 64;
            image.height = 36;
            image.jpeg =
                std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{0xFF, 0xD8, uint8_t(index), 0xFF, 0xD9});
            set->images.push_back(image);
        }
        const std::lock_guard<std::mutex> lock(_mutex);
        _sets.push_back(set);
        if (_sets.size() > 2) {
            _sets.erase(_sets.begin());
        }
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<const preview_set>> _sets;
};

class Harness {
public:
    Harness()
        : component(loop, provider, makeConfig()) {
        REQUIRE(component.initialize());
        REQUIRE(component.start());
        loopThread = std::thread([this]() {
            loop.run();
        });
        client = std::make_unique<httplib::Client>("127.0.0.1", component.get_bound_port());
    }

    ~Harness() {
        component.stop();
        loop.requestStop();
        loopThread.join();
    }

    static preview_config makeConfig() {
        preview_config config;
        config.bind = "127.0.0.1";
        config.port = 0;
        return config;
    }

    UvLoop loop;
    FakePreviewProvider provider;
    PreviewComponent component;
    std::thread loopThread;
    std::unique_ptr<httplib::Client> client;
};

}  // namespace

TEST_CASE("PreviewComponent answers 404 before the first preview") {
    Harness harness;
    REQUIRE(harness.client->Get("/preview")->status == 404);
    REQUIRE(harness.client->Get("/preview/0.jpg")->status == 404);
}

TEST_CASE("PreviewComponent serves the metadata and the JPEG of one tick") {
    Harness harness;
    harness.provider.add(100);
    auto response = harness.client->Get("/preview");
    REQUIRE(response->status == 200);
    const auto meta = nlohmann::json::parse(response->body);
    REQUIRE(meta["tick"] == 100);
    REQUIRE(meta["offset_us"] == 250);
    REQUIRE(meta["channels"].size() == 4);
    REQUIRE(meta["channels"][1]["id"] == 11);
    REQUIRE(meta["channels"][2]["latch_us"] == 100);

    response = harness.client->Get("/preview/2.jpg?tick=100");
    REQUIRE(response->status == 200);
    REQUIRE(response->get_header_value("Content-Type") == "image/jpeg");
    REQUIRE(response->get_header_value("X-Preview-Tick") == "100");
    REQUIRE(response->body.size() == 5);
    REQUIRE(static_cast<uint8_t>(response->body[2]) == 2);
}

TEST_CASE("PreviewComponent keeps serving a tick until it is replaced, then answers 409") {
    Harness harness;
    harness.provider.add(100);
    harness.provider.add(103);
    REQUIRE(harness.client->Get("/preview/0.jpg?tick=100")->status == 200);
    harness.provider.add(106);
    REQUIRE(harness.client->Get("/preview/0.jpg?tick=100")->status == 409);
    REQUIRE(harness.client->Get("/preview/0.jpg?tick=106")->status == 200);
}

TEST_CASE("PreviewComponent rejects bad indexes and ticks") {
    Harness harness;
    harness.provider.add(100);
    REQUIRE(harness.client->Get("/preview/9.jpg")->status == 404);
    REQUIRE(harness.client->Get("/preview/0.jpg?tick=abc")->status == 400);
    REQUIRE(harness.client->Get("/preview/123.jpg")->status == 404);
}
