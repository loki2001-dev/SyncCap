#include "pipeline/PreviewWorker.h"

#include <exception>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "codec/JpegEncoder.h"
#include "util/log/Logger.h"

PreviewWorker::PreviewWorker(preview_config config)
    : _config(std::move(config))
    , _running(false)
    , _encoded(0)
    , _replaced(0) {
}

PreviewWorker::~PreviewWorker() {
    stop();
}

void PreviewWorker::start(std::future<void>& done) {
    {
        const std::lock_guard<std::mutex> lock(_jobMutex);
        _running = true;
        _job.reset();
    }
    std::promise<void> promise;
    done = promise.get_future();
    _thread = std::thread([this, promise = std::move(promise)]() mutable {
        try {
            run();
            promise.set_value();
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
}

void PreviewWorker::stop() {
    {
        const std::lock_guard<std::mutex> lock(_jobMutex);
        _running = false;
    }
    _jobReady.notify_all();
    if (_thread.joinable()) {
        _thread.join();
    }
    // the job may still hold pooled frames; release them now
    std::optional<preview_job> leftover;
    {
        const std::lock_guard<std::mutex> lock(_jobMutex);
        leftover.swap(_job);
    }
}

void PreviewWorker::offer(preview_job job) {
    std::optional<preview_job> replaced;
    {
        const std::lock_guard<std::mutex> lock(_jobMutex);
        if (!_running) {
            return;
        }
        if (_job) {
            replaced.swap(_job);
            _replaced += 1;
        }
        _job = std::move(job);
    }
    _jobReady.notify_one();
    // a replaced job returns its frames to the pools here, outside the lock
}

void PreviewWorker::clear() {
    const std::lock_guard<std::mutex> lock(_setMutex);
    _sets.clear();
}

void PreviewWorker::run() {
    // one encoder per channel index: each keeps its scaler for its own source size
    std::map<int, JpegEncoder> encoders;
    while (true) {
        preview_job job;
        {
            std::unique_lock<std::mutex> lock(_jobMutex);
            _jobReady.wait(lock, [this]() {
                return !_running || _job.has_value();
            });
            if (!_running) {
                return;
            }
            job = std::move(*_job);
            _job.reset();
        }
        auto set = std::make_shared<preview_set>();
        set->tick = job.tick;
        set->targetNs = job.targetNs;
        set->offsetNs = job.offsetNs;
        set->verdict = job.verdict;
        for (const preview_job::channel& channel : job.channels) {
            if (!channel.buffer) {
                continue;
            }
            auto found = encoders.find(channel.index);
            if (found == encoders.end()) {
                found = encoders
                            .emplace(std::piecewise_construct, std::forward_as_tuple(channel.index),
                                     std::forward_as_tuple())
                            .first;
                if (!found->second.open(_config.width, _config.height, _config.quality)) {
                    throw std::runtime_error("preview jpeg encoder open failed channel=" +
                                             std::to_string(channel.channelId));
                }
            }
            auto jpeg = std::make_shared<std::vector<uint8_t>>();
            if (!found->second.encode(*channel.buffer, *jpeg)) {
                continue;
            }
            preview_image image;
            image.index = channel.index;
            image.channelId = channel.channelId;
            image.sourceType = channel.sourceType;
            image.timestampNs = channel.timestampNs;
            image.width = _config.width;
            image.height = _config.height;
            image.jpeg = std::move(jpeg);
            set->images.push_back(std::move(image));
        }
        // the full-size frames are no longer needed: give them back to the pools before publishing
        job.channels.clear();
        {
            const std::lock_guard<std::mutex> lock(_setMutex);
            _sets.push_back(std::move(set));
            while (_sets.size() > KEPT_SETS) {
                _sets.pop_front();
            }
        }
        _encoded += 1;
    }
}

std::shared_ptr<const preview_set> PreviewWorker::get_latest_preview() const {
    const std::lock_guard<std::mutex> lock(_setMutex);
    return _sets.empty() ? nullptr : _sets.back();
}

std::shared_ptr<const preview_set> PreviewWorker::find_preview(uint64_t tick) const {
    const std::lock_guard<std::mutex> lock(_setMutex);
    for (const auto& set : _sets) {
        if (set->tick == tick) {
            return set;
        }
    }
    return nullptr;
}

uint64_t PreviewWorker::get_encoded_total() const {
    return _encoded;
}

uint64_t PreviewWorker::get_replaced_total() const {
    return _replaced;
}
