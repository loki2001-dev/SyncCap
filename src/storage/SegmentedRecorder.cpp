#include "storage/SegmentedRecorder.h"

#include <cstdio>

#include <array>
#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>

#include "util/TimeUtil.h"
#include "util/log/Logger.h"

namespace {

constexpr size_t FILE_BUFFER_BYTES = 1 << 20;
constexpr size_t RECENT_FILES = 16;
constexpr int64_t NANOS_PER_MICRO = 1000;
constexpr const char* SYNC_HEADER = "tick,segment,target_us,ch0_us,ch1_us,ch2_us,ch3_us,offset_us,verdict\n";
constexpr const char* SENSOR_HEADER = "time_us,temperature_c,humidity_pct,accel_x_g,accel_y_g,accel_z_g\n";
constexpr size_t LINE_SIZE = 256;

}  // namespace

bool SegmentedRecorder::SegmentFile::open(const std::filesystem::path& filePath, const std::string& header) {
    close();
    _buffer.resize(FILE_BUFFER_BYTES);
    _stream.rdbuf()->pubsetbuf(_buffer.data(), static_cast<std::streamsize>(_buffer.size()));
    _stream.open(filePath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!_stream) {
        return false;
    }
    path = filePath;
    if (!header.empty()) {
        _stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    }
    return static_cast<bool>(_stream);
}

bool SegmentedRecorder::SegmentFile::write(const char* data, size_t size) {
    _stream.write(data, static_cast<std::streamsize>(size));
    return static_cast<bool>(_stream);
}

void SegmentedRecorder::SegmentFile::close() {
    if (_stream.is_open()) {
        _stream.close();
    }
    _stream.clear();
    segment = -1;
}

bool SegmentedRecorder::SegmentFile::isOpen() const {
    return _stream.is_open();
}

SegmentedRecorder::SegmentedRecorder(size_t queueCapacity)
    : _queue(queueCapacity, overflow_policy::reject)
    , _sessionOpen(false)
    , _currentSegment(-1) {
    _thread = std::thread([this]() {
        run();
    });
}

SegmentedRecorder::~SegmentedRecorder() {
    shutdown();
}

void SegmentedRecorder::shutdown() {
    _queue.close();
    if (_thread.joinable()) {
        _thread.join();
    }
}

bool SegmentedRecorder::beginSession(const session_info& session) {
    std::error_code error;
    std::filesystem::create_directories(session.directory, error);
    if (error) {
        LOG_ERROR(store, life, "session directory create failed path={} message={}", session.directory,
                  error.message());
        return false;
    }
    if (_queue.push(session_begin{session}) == push_result::rejected) {
        LOG_ERROR(store, sys, "session begin rejected reason=queue_full session={}", session.sessionId);
        return false;
    }
    _sessionOpen = true;
    return true;
}

void SegmentedRecorder::endSession() {
    if (!_sessionOpen.exchange(false)) {
        return;
    }
    // the marker queues behind every record already pushed, so nothing of this session is lost
    while (_queue.push(session_end{}) == push_result::rejected && !_queue.isClosed()) {
        std::this_thread::yield();
    }
}

bool SegmentedRecorder::push(record_item item) {
    if (!_sessionOpen) {
        const std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.dropped += 1;
        return false;
    }
    if (_queue.push(entry_t(std::move(item))) == push_result::rejected) {
        uint64_t dropped = 0;
        {
            const std::lock_guard<std::mutex> lock(_statsMutex);
            _stats.dropped += 1;
            dropped = _stats.dropped;
        }
        if (dropped == 1 || dropped % 100 == 0) {
            LOG_WARN(store, sys, "record dropped reason=queue_full dropped_total={}", dropped);
        }
        return false;
    }
    return true;
}

recorder_stats SegmentedRecorder::get_stats() const {
    recorder_stats stats;
    {
        const std::lock_guard<std::mutex> lock(_statsMutex);
        stats = _stats;
        stats.recentFiles.assign(_recentFiles.begin(), _recentFiles.end());
    }
    stats.queueLength = _queue.get_size();
    stats.queueHighWater = _queue.get_high_water();
    stats.sessionOpen = _sessionOpen;
    return stats;
}

std::string SegmentedRecorder::segmentDirectoryName(int64_t segment, std::chrono::system_clock::time_point wallStart,
                                                    int64_t segmentNs) {
    const auto segmentStart = wallStart + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                              std::chrono::nanoseconds(segment * segmentNs));
    std::array<char, LINE_SIZE> name{};
    std::snprintf(name.data(), name.size(), "seg_%04lld_%s", static_cast<long long>(segment),
                  TimeUtil::formatCompactLocal(segmentStart).c_str());
    return name.data();
}

void SegmentedRecorder::run() {
    entry_t entry;
    while (_queue.pop(entry)) {
        // an exception must not leave the thread: it would std::terminate the process
        try {
            std::visit(
                [this](auto& value) {
                    using value_t = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<value_t, record_item>) {
                        handle(value);
                    } else if constexpr (std::is_same_v<value_t, session_begin>) {
                        handleBegin(value.session);
                    } else {
                        handleEnd();
                    }
                },
                entry);
        } catch (const std::exception& e) {
            LOG_ERROR(store, sys, "record write threw message={}", e.what());
            const std::lock_guard<std::mutex> lock(_statsMutex);
            _stats.writeErrors += 1;
        }
        entry = session_end{};
    }
    handleEnd();
}

void SegmentedRecorder::handleBegin(const session_info& session) {
    handleEnd();
    _session = session;
    _currentSegment = -1;
    {
        const std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.segment = -1;
    }
    LOG_INFO(store, life, "session opened session={} path={} segment_s={}", session.sessionId, session.directory,
             session.segmentNs / (NANOS_PER_MICRO * NANOS_PER_MICRO * NANOS_PER_MICRO));
}

void SegmentedRecorder::handleEnd() {
    if (_session.directory.empty()) {
        return;
    }
    for (auto& entry : _videoFiles) {
        entry.second.close();
    }
    _videoFiles.clear();
    _syncFile.close();
    _sensorFile.close();
    uint64_t bytes = 0;
    {
        const std::lock_guard<std::mutex> lock(_statsMutex);
        bytes = _stats.bytesWritten;
    }
    LOG_INFO(store, life, "session closed session={} last_segment={} bytes_written_total={}", _session.sessionId,
             _currentSegment, bytes);
    _session = session_info{};
    _currentSegment = -1;
}

void SegmentedRecorder::handle(const record_item& item) {
    if (_session.directory.empty()) {
        const std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.dropped += 1;
        return;
    }
    if (const auto* packet = std::get_if<encoded_packet>(&item)) {
        SegmentFile& file = _videoFiles[packet->channel];
        SegmentFile* target =
            prepareFile(file, packet->segment, "ch" + std::to_string(packet->channel) + ".h264", std::string());
        writeTo(target, reinterpret_cast<const char*>(packet->data.data()), packet->data.size());
        return;
    }
    if (const auto* sync = std::get_if<sync_record>(&item)) {
        SegmentFile* target = prepareFile(_syncFile, sync->segment, "sync.csv", SYNC_HEADER);
        std::array<char, LINE_SIZE> line{};
        const int64_t epoch = _session.epochNs;
        auto relativeUs = [epoch, sync](int channel) -> long long {
            if (channel >= sync->channelCount || !sync->present[static_cast<size_t>(channel)]) {
                return -1;
            }
            return static_cast<long long>((sync->timestampNs[static_cast<size_t>(channel)] - epoch) / NANOS_PER_MICRO);
        };
        const int length =
            std::snprintf(line.data(), line.size(), "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%s\n",
                          static_cast<unsigned long long>(sync->tick), static_cast<long long>(sync->segment),
                          static_cast<long long>((sync->targetNs - epoch) / NANOS_PER_MICRO), relativeUs(0),
                          relativeUs(1), relativeUs(2), relativeUs(3),
                          static_cast<long long>(sync->offsetNs / NANOS_PER_MICRO), toString(sync->verdict));
        if (length > 0) {
            writeTo(target, line.data(), static_cast<size_t>(length));
        }
        return;
    }
    if (const auto* sensor = std::get_if<sensor_record>(&item)) {
        SegmentFile* target = prepareFile(_sensorFile, sensor->segment, "sensor.csv", SENSOR_HEADER);
        const sensor_sample& sample = sensor->sample;
        std::array<char, LINE_SIZE> line{};
        const int length =
            std::snprintf(line.data(), line.size(), "%lld,%.3f,%.3f,%.5f,%.5f,%.5f\n",
                          static_cast<long long>((sample.timestampNs - _session.epochNs) / NANOS_PER_MICRO),
                          sample.temperatureC, sample.humidityPct, sample.accelX, sample.accelY, sample.accelZ);
        if (length > 0) {
            writeTo(target, line.data(), static_cast<size_t>(length));
        }
    }
}

SegmentedRecorder::SegmentFile* SegmentedRecorder::prepareFile(SegmentFile& file, int64_t segment,
                                                               const std::string& name, const std::string& header) {
    if (file.isOpen() && file.segment == segment) {
        return &file;
    }
    if (file.isOpen() && segment < file.segment) {
        // a late record of a closed segment: write it into the current file rather than reopen the old one
        return &file;
    }
    file.close();
    const std::filesystem::path directory = std::filesystem::path(_session.directory) /
                                            segmentDirectoryName(segment, _session.wallStart, _session.segmentNs);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path path = directory / name;
    if (error || !file.open(path, header)) {
        LOG_ERROR(store, sys, "file open failed path={} message={}", path.string(), error.message());
        const std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.writeErrors += 1;
        return nullptr;
    }
    file.segment = segment;
    if (segment > _currentSegment) {
        _currentSegment = segment;
        LOG_INFO(store, life, "segment opened session={} segment={} path={}", _session.sessionId, segment,
                 directory.string());
        const std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.segment = segment;
    }
    noteFile(path);
    return &file;
}

bool SegmentedRecorder::writeTo(SegmentFile* file, const char* data, size_t size) {
    if (file == nullptr) {
        const std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.dropped += 1;
        return false;
    }
    const bool written = file->write(data, size);
    const std::lock_guard<std::mutex> lock(_statsMutex);
    if (written) {
        _stats.bytesWritten += size;
    } else {
        _stats.writeErrors += 1;
    }
    return written;
}

void SegmentedRecorder::noteFile(const std::filesystem::path& path) {
    std::string relative =
        path.lexically_relative(std::filesystem::path(_session.directory).parent_path()).generic_string();
    const std::lock_guard<std::mutex> lock(_statsMutex);
    _recentFiles.push_back(std::move(relative));
    while (_recentFiles.size() > RECENT_FILES) {
        _recentFiles.pop_front();
    }
}
