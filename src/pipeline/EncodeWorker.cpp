#include "pipeline/EncodeWorker.h"

#include <chrono>
#include <exception>
#include <utility>
#include <vector>

#include "util/log/Logger.h"

namespace {

constexpr double NANOS_PER_MILLI = 1e6;
constexpr uint64_t DROP_LOG_EVERY = 100;

}  // namespace

EncodeWorker::EncodeWorker(int channelId, std::unique_ptr<IVideoEncoder> encoder, size_t queueCapacity,
                           IRecordSink& sink)
    : _channelId(channelId)
    , _encoder(std::move(encoder))
    , _queue(queueCapacity, overflow_policy::reject)
    , _sink(sink)
    , _framesEncoded(0)
    , _packetsDropped(0)
    , _encodeErrors(0)
    , _encodeNsTotal(0)
    , _encodeNsWorst(0) {
}

EncodeWorker::~EncodeWorker() {
    closeInput();
    join();
}

void EncodeWorker::start(std::future<void>& done) {
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

void EncodeWorker::closeInput() {
    _queue.close();
}

void EncodeWorker::join() {
    if (_thread.joinable()) {
        _thread.join();
    }
}

BlockingQueue<video_frame>& EncodeWorker::get_queue() {
    return _queue;
}

void EncodeWorker::run() {
    std::vector<encoded_packet> packets;
    video_frame frame;
    int64_t lastSegment = -1;
    while (_queue.pop(frame)) {
        const bool forceKey = frame.segment != lastSegment;
        lastSegment = frame.segment;
        const auto begin = std::chrono::steady_clock::now();
        const bool encoded = _encoder->encode(frame, forceKey, packets);
        const int64_t elapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
        frame.buffer.reset();
        if (!encoded) {
            _encodeErrors += 1;
            continue;
        }
        _framesEncoded += 1;
        _encodeNsTotal += elapsedNs;
        if (elapsedNs > _encodeNsWorst) {
            _encodeNsWorst = elapsedNs;
        }
        forward(packets);
    }
    _encoder->flush(packets);
    forward(packets);
    _encoder->close();
    LOG_INFO(enc, life, "stopped channel={} frames_encoded={} packets_dropped={} errors={}", _channelId,
             _framesEncoded.load(), _packetsDropped.load(), _encodeErrors.load());
}

void EncodeWorker::forward(std::vector<encoded_packet>& packets) {
    for (encoded_packet& packet : packets) {
        packet.channel = _channelId;
        if (!_sink.push(record_item(std::move(packet)))) {
            const uint64_t dropped = ++_packetsDropped;
            if (dropped == 1 || dropped % DROP_LOG_EVERY == 0) {
                LOG_WARN(enc, sys, "packet dropped reason=record_queue_full channel={} dropped_total={}", _channelId,
                         dropped);
            }
        }
    }
    packets.clear();
}

encode_stats EncodeWorker::get_stats() const {
    encode_stats stats;
    stats.framesEncoded = _framesEncoded;
    stats.packetsDropped = _packetsDropped;
    stats.encodeErrors = _encodeErrors;
    stats.encodeMsAvg = stats.framesEncoded == 0 ? 0.0
                                                 : static_cast<double>(_encodeNsTotal.load()) /
                                                       static_cast<double>(stats.framesEncoded) / NANOS_PER_MILLI;
    stats.encodeMsWorst = static_cast<double>(_encodeNsWorst.load()) / NANOS_PER_MILLI;
    stats.queueLength = _queue.get_size();
    stats.queueHighWater = _queue.get_high_water();
    return stats;
}
