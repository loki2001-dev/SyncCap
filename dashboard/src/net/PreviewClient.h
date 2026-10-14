#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model/DashboardModel.h"
#include "net/ServerClient.h"
#include "core/BlockingQueue.h"
#include "network/IHttpClient.h"

/**
 * @brief the dashboard's video thread (P2): fetches the collector's live preview and decodes it
 *
 * per round: GET /preview for the latest tick; when it is new, GET /preview/{index}.jpg?tick=T for every
 * channel so all tiles show the same instant; decode to RGBA; hand the set to the UI thread through the inbox.
 * a 409 (the tick was replaced while fetching) drops the round and the next one starts fresh
 */
class PreviewClient {
public:
    PreviewClient(std::string baseUrl, IHttpClient& http, std::chrono::milliseconds period,
                  std::chrono::milliseconds timeout, BlockingQueue<model_update_t>& inbox);
    ~PreviewClient();

    PreviewClient(const PreviewClient&) = delete;
    PreviewClient& operator=(const PreviewClient&) = delete;

    void start();
    void stop();

    // decodes a baseline JPEG to RGBA. false: not a JPEG stb can read
    static bool decodeJpeg(const std::string& bytes, int& width, int& height, std::vector<uint8_t>& rgba);

    static constexpr size_t MAX_JPEG_BYTES = 4 << 20;
    static constexpr int MAX_CHANNELS_IN_PREVIEW = 8;

private:
    void run();
    void fetchRound();
    bool isRunning();
    void publish(model_update_t update);

    std::string _baseUrl;
    IHttpClient& _http;
    std::chrono::milliseconds _period;
    std::chrono::milliseconds _timeout;
    BlockingQueue<model_update_t>& _inbox;
    std::mutex _stopMutex;
    std::condition_variable _stopSignal;
    bool _running;
    std::thread _thread;

    // worker thread only
    uint64_t _lastTick;
    bool _hasTick;
};
