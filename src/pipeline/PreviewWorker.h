#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config/AppConfig.h"
#include "model/PreviewSet.h"
#include "pipeline/IPreviewProvider.h"

// the frames of one locked set handed to the preview thread. buffers are shared, nothing is copied
struct preview_job {
    uint64_t tick = 0;
    int64_t targetNs = 0;
    int64_t offsetNs = 0;
    sync_verdict verdict = sync_verdict::locked;
    struct channel {
        int index = 0;
        int channelId = 0;
        std::string sourceType;
        int64_t timestampNs = 0;
        std::shared_ptr<const FrameBuffer> buffer;
    };
    std::vector<channel> channels;
};

/**
 * @brief preview thread: downscales and JPEG-encodes the latest offered set (EX-P1-001)
 *
 * offer() never blocks the synchronizer: a one-slot mailbox where a newer set replaces an unprocessed one.
 * finished sets are kept (the last KEPT_SETS) so a client can fetch every channel of the same tick
 */
class PreviewWorker : public IPreviewProvider {
public:
    explicit PreviewWorker(preview_config config);
    ~PreviewWorker() override;

    PreviewWorker(const PreviewWorker&) = delete;
    PreviewWorker& operator=(const PreviewWorker&) = delete;

    void start(std::future<void>& done);
    void stop();
    // synchronizer thread. never blocks
    void offer(preview_job job);
    // forget the sets of the previous session
    void clear();

    std::shared_ptr<const preview_set> get_latest_preview() const override;
    std::shared_ptr<const preview_set> find_preview(uint64_t tick) const override;

    uint64_t get_encoded_total() const;
    uint64_t get_replaced_total() const;

    static constexpr size_t KEPT_SETS = 3;

private:
    void run();

    preview_config _config;
    std::mutex _jobMutex;
    std::condition_variable _jobReady;
    std::optional<preview_job> _job;
    bool _running;
    std::thread _thread;

    mutable std::mutex _setMutex;
    std::deque<std::shared_ptr<const preview_set>> _sets;

    std::atomic<uint64_t> _encoded;
    std::atomic<uint64_t> _replaced;
};
