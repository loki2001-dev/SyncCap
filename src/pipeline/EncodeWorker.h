#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>

#include "codec/IVideoEncoder.h"
#include "core/BlockingQueue.h"
#include "model/VideoFrame.h"
#include "storage/IRecordSink.h"

struct encode_stats {
    uint64_t framesEncoded = 0;
    uint64_t packetsDropped = 0;  // the record queue refused them
    uint64_t encodeErrors = 0;
    double encodeMsAvg = 0.0;
    double encodeMsWorst = 0.0;
    size_t queueLength = 0;
    size_t queueHighWater = 0;
};

/**
 * @brief encoder thread of one channel: frames from its queue → libx264 → record sink (EX-P1-001)
 *
 * the first frame of a new segment is forced to IDR so each segment file starts decodable.
 * on close of the input queue it drains, flushes the encoder and exits
 */
class EncodeWorker {
public:
    EncodeWorker(int channelId, std::unique_ptr<IVideoEncoder> encoder, size_t queueCapacity, IRecordSink& sink);
    ~EncodeWorker();

    EncodeWorker(const EncodeWorker&) = delete;
    EncodeWorker& operator=(const EncodeWorker&) = delete;

    void start(std::future<void>& done);
    // no more input: the thread drains, flushes and exits
    void closeInput();
    void join();

    BlockingQueue<video_frame>& get_queue();
    encode_stats get_stats() const;

private:
    void run();
    void forward(std::vector<encoded_packet>& packets);

    int _channelId;
    std::unique_ptr<IVideoEncoder> _encoder;
    BlockingQueue<video_frame> _queue;
    IRecordSink& _sink;
    std::thread _thread;

    std::atomic<uint64_t> _framesEncoded;
    std::atomic<uint64_t> _packetsDropped;
    std::atomic<uint64_t> _encodeErrors;
    std::atomic<int64_t> _encodeNsTotal;
    std::atomic<int64_t> _encodeNsWorst;
};
