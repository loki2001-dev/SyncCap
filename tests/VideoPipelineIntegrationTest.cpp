#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "H264Inspect.h"
#include "codec/X264Encoder.h"
#include "pipeline/VideoPipeline.h"
#include "source/SyntheticVideoSource.h"
#include "storage/SegmentedRecorder.h"
#include "util/TimeUtil.h"

namespace {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;
constexpr int64_t SECOND_NS = 1000000000LL;

video_config makeVideo(int channels) {
    video_config video;
    video.fps = 30;
    video.width = WIDTH;
    video.height = HEIGHT;
    video.syncToleranceMs = 2.0;
    video.spinMarginUs = 1500;
    video.dropUnsynced = true;
    video.poolFrames = 12;
    video.encoderQueue = 4;
    video.maxPendingTicks = 3;
    for (int id = 0; id < channels; ++id) {
        channel_config channel;
        channel.id = id;
        channel.type = "synthetic";
        video.channels.push_back(channel);
    }
    return video;
}

encoder_config makeEncoder() {
    encoder_config encoder;
    encoder.preset = "ultrafast";
    encoder.tune = "zerolatency";
    encoder.bitrateKbps = 500;
    encoder.threads = 1;
    return encoder;
}

preview_config previewOff() {
    preview_config preview;
    preview.enabled = false;
    return preview;
}

preview_config previewOn() {
    preview_config preview;
    preview.enabled = true;
    preview.fps = 10;
    preview.width = 160;
    preview.height = 90;
    return preview;
}

// SOF0 dimensions of a baseline JPEG
bool jpegSize(const std::vector<uint8_t>& jpeg, int& width, int& height) {
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || jpeg[jpeg.size() - 2] != 0xFF ||
        jpeg[jpeg.size() - 1] != 0xD9) {
        return false;
    }
    for (size_t index = 2; index + 8 < jpeg.size(); ++index) {
        if (jpeg[index] == 0xFF && jpeg[index + 1] == 0xC0) {
            height = (jpeg[index + 5] << 8) | jpeg[index + 6];
            width = (jpeg[index + 7] << 8) | jpeg[index + 8];
            return true;
        }
    }
    return false;
}

video_encoder_factory_t x264Factory() {
    return []() {
        return std::make_unique<X264Encoder>();
    };
}

video_source_factory_t syntheticFactory() {
    return [](const channel_config& channel, std::string&) -> std::unique_ptr<IVideoSource> {
        return std::make_unique<SyntheticVideoSource>(channel.id, WIDTH, HEIGHT, 30.0);
    };
}

// a non-live source that fails after a few pictures
class FailingSource : public IVideoSource {
public:
    explicit FailingSource(int failAfter)
        : _failAfter(failAfter) {
    }
    bool open() override {
        return true;
    }
    read_status read(FrameBuffer&) override {
        return _reads++ < _failAfter ? read_status::ok : read_status::error;
    }
    void close() override {
    }
    void interrupt() override {
    }
    video_source_info get_info() const override {
        video_source_info info;
        info.type = "failing";
        info.uri = "failing://";
        info.width = WIDTH;
        info.height = HEIGHT;
        info.nativeFps = 30.0;
        return info;
    }

private:
    int _failAfter;
    int _reads = 0;
};

class UnopenableSource : public FailingSource {
public:
    UnopenableSource()
        : FailingSource(0) {
    }
    bool open() override {
        return false;
    }
};

std::filesystem::path makeTempDirectory(const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

// rows of sync.csv per verdict
std::map<std::string, int> countVerdicts(const std::filesystem::path& file) {
    std::map<std::string, int> counts;
    std::ifstream stream(file);
    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        counts[line.substr(line.rfind(',') + 1)] += 1;
    }
    return counts;
}

}  // namespace

TEST_CASE("VideoPipeline records synchronized, aligned, decodable segments for four channels") {
    const auto root = makeTempDirectory("synccap_pipeline_test");
    SegmentedRecorder recorder(512);
    session_info session;
    session.sessionId = "test";
    session.directory = (root / "session").string();
    session.epochNs = TimeUtil::steadyNowNs();
    session.segmentNs = SECOND_NS;
    session.wallStart = std::chrono::system_clock::now();
    REQUIRE(recorder.beginSession(session));

    VideoPipeline pipeline(makeVideo(4), makeEncoder(), previewOn(), recorder, syntheticFactory(), x264Factory());
    std::string error;
    REQUIRE(pipeline.start(session.epochNs, session.segmentNs, error));
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    REQUIRE_FALSE(pipeline.poll_failure().has_value());
    const pipeline_stats stats = pipeline.get_stats();
    REQUIRE(stats.running);
    REQUIRE(stats.channels.size() == 4);
    REQUIRE(stats.previewsEncoded >= 20);

    // the preview holds every channel of one locked tick, as small JPEGs
    const auto preview = pipeline.get_latest_preview();
    REQUIRE(preview);
    REQUIRE(preview->verdict == sync_verdict::locked);
    REQUIRE(preview->images.size() == 4);
    REQUIRE(pipeline.find_preview(preview->tick) == preview);
    for (const preview_image& image : preview->images) {
        int width = 0;
        int height = 0;
        REQUIRE(jpegSize(*image.jpeg, width, height));
        REQUIRE(width == 160);
        REQUIRE(height == 90);
    }
    pipeline.stop();
    REQUIRE_FALSE(pipeline.get_latest_preview());
    recorder.endSession();
    recorder.shutdown();

    const sync_report report = pipeline.get_sync_report();
    INFO("max_offset_ms=" << report.totalMaxOffsetMs << " violations=" << report.violations
                          << " incomplete=" << report.incomplete);
    REQUIRE(report.setsChecked >= 80);
    REQUIRE(report.violations == 0);
    REQUIRE(report.incomplete == 0);
    REQUIRE(report.totalMaxOffsetMs <= 2.0);
    REQUIRE(pipeline.isSynchronized());

    int segments = 0;
    for (const auto& entry : std::filesystem::directory_iterator(session.directory)) {
        const std::filesystem::path segment = entry.path();
        INFO("segment=" << segment.filename().string());
        const auto verdicts = countVerdicts(segment / "sync.csv");
        const int locked = verdicts.count("locked") != 0 ? verdicts.at("locked") : 0;
        for (int channel = 0; channel < 4; ++channel) {
            const auto file = segment / ("ch" + std::to_string(channel) + ".h264");
            REQUIRE(std::filesystem::exists(file));
            const h264_summary summary = inspectH264(readBinary(file.string()));
            INFO("channel=" << channel);
            // every channel holds exactly the locked sets of this segment, and each file starts decodable
            REQUIRE(summary.pictures == locked);
            REQUIRE(summary.firstNalType == 7);
            REQUIRE(summary.firstPictureIsIdr);
        }
        segments += 1;
    }
    REQUIRE(segments >= 3);
    std::filesystem::remove_all(root);
}

TEST_CASE("VideoPipeline reports a failed thread through poll_failure") {
    SegmentedRecorder recorder(64);
    VideoPipeline pipeline(
        makeVideo(3), makeEncoder(), previewOff(), recorder,
        [](const channel_config& channel, std::string&) -> std::unique_ptr<IVideoSource> {
            if (channel.id == 1) {
                return std::make_unique<FailingSource>(10);
            }
            return std::make_unique<SyntheticVideoSource>(channel.id, WIDTH, HEIGHT, 30.0);
        },
        x264Factory());
    std::string error;
    REQUIRE(pipeline.start(TimeUtil::steadyNowNs(), SECOND_NS, error));
    std::optional<std::string> failure;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!failure && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        failure = pipeline.poll_failure();
    }
    pipeline.stop();
    recorder.shutdown();
    REQUIRE(failure.has_value());
    REQUIRE(failure->find("reader ch1") != std::string::npos);
    REQUIRE(failure->find("source read failed") != std::string::npos);
}

TEST_CASE("VideoPipeline start fails cleanly when a source cannot open") {
    SegmentedRecorder recorder(64);
    VideoPipeline pipeline(
        makeVideo(3), makeEncoder(), previewOff(), recorder,
        [](const channel_config& channel, std::string&) -> std::unique_ptr<IVideoSource> {
            if (channel.id == 2) {
                return std::make_unique<UnopenableSource>();
            }
            return std::make_unique<SyntheticVideoSource>(channel.id, WIDTH, HEIGHT, 30.0);
        },
        x264Factory());
    std::string error;
    REQUIRE_FALSE(pipeline.start(TimeUtil::steadyNowNs(), SECOND_NS, error));
    REQUIRE(error.find("source open failed channel=2") != std::string::npos);
    REQUIRE_FALSE(pipeline.isRunning());
    // a second start after the failure works
    VideoPipeline healthy(makeVideo(3), makeEncoder(), previewOff(), recorder, syntheticFactory(), x264Factory());
    REQUIRE(healthy.start(TimeUtil::steadyNowNs(), SECOND_NS, error));
    healthy.stop();
    recorder.shutdown();
}

TEST_CASE("VideoPipeline survives repeated start and stop") {
    SegmentedRecorder recorder(64);
    VideoPipeline pipeline(makeVideo(4), makeEncoder(), previewOn(), recorder, syntheticFactory(), x264Factory());
    for (int round = 0; round < 10; ++round) {
        std::string error;
        REQUIRE(pipeline.start(TimeUtil::steadyNowNs(), SECOND_NS, error));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pipeline.stop();
        REQUIRE_FALSE(pipeline.isRunning());
    }
    recorder.shutdown();
}
