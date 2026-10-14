#include "source/FileVideoSource.h"

#include <utility>

#include "util/log/Logger.h"

namespace {

constexpr int64_t FILE_IO_TIMEOUT_US = 2000000;

}  // namespace

FileVideoSource::FileVideoSource(std::string path, int width, int height, bool loop)
    : FfmpegVideoSource("file", path, width, height, false)
    , _path(std::move(path))
    , _loop(loop) {
}

std::map<std::string, std::string> FileVideoSource::get_open_options() const {
    return {};
}

read_status FileVideoSource::on_end_of_stream() {
    if (!_loop) {
        LOG_INFO(cap, hw, "end of file path={}", _path);
        return read_status::end_of_stream;
    }
    return rewind() ? read_status::retry : read_status::error;
}

read_status FileVideoSource::on_read_error(int code) {
    LOG_ERROR(cap, hw, "file read failed path={} message={}", _path, describeError(code));
    return read_status::error;
}

int64_t FileVideoSource::get_io_timeout_us() const {
    return FILE_IO_TIMEOUT_US;
}
