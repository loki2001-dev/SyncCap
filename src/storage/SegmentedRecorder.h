#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "core/BlockingQueue.h"
#include "storage/IRecordSink.h"
#include "storage/SegmentClock.h"

/**
 * @brief the storage thread. writes every record into the file of its segment (EX-P1-001)
 *
 * layout: <root>/<session>/seg_NNNN_<wall time>/{chN.h264, sync.csv, sensor.csv}.
 * a new segment directory starts when the first record of that segment arrives
 */
class SegmentedRecorder : public IRecordSink {
public:
    explicit SegmentedRecorder(size_t queueCapacity);
    ~SegmentedRecorder() override;

    SegmentedRecorder(const SegmentedRecorder&) = delete;
    SegmentedRecorder& operator=(const SegmentedRecorder&) = delete;

    bool beginSession(const session_info& session) override;
    void endSession() override;
    bool push(record_item item) override;
    recorder_stats get_stats() const override;

    // joins the storage thread after writing everything queued. idempotent
    void shutdown();

    static std::string segmentDirectoryName(int64_t segment, std::chrono::system_clock::time_point wallStart,
                                            int64_t segmentNs);

private:
    struct session_begin {
        session_info session;
    };
    struct session_end {};
    using entry_t = std::variant<record_item, session_begin, session_end>;

    // one open file of the current segment
    class SegmentFile {
    public:
        bool open(const std::filesystem::path& path, const std::string& header);
        bool write(const char* data, size_t size);
        void close();
        bool isOpen() const;
        int64_t segment = -1;
        std::filesystem::path path;

    private:
        std::ofstream _stream;
        std::vector<char> _buffer;
    };

    void run();
    void handle(const record_item& item);
    void handleBegin(const session_info& session);
    void handleEnd();
    SegmentFile* prepareFile(SegmentFile& file, int64_t segment, const std::string& name, const std::string& header);
    bool writeTo(SegmentFile* file, const char* data, size_t size);
    void noteFile(const std::filesystem::path& path);

    BlockingQueue<entry_t> _queue;
    std::thread _thread;
    std::atomic<bool> _sessionOpen;

    // storage thread only
    session_info _session;
    std::map<int, SegmentFile> _videoFiles;
    SegmentFile _syncFile;
    SegmentFile _sensorFile;
    int64_t _currentSegment;

    // shared with get_stats
    mutable std::mutex _statsMutex;
    recorder_stats _stats;
    std::deque<std::string> _recentFiles;
};
