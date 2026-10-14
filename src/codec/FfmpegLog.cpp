#include "codec/FfmpegLog.h"

#include <cstdarg>

#include <array>
#include <string>

extern "C" {
#include <libavutil/log.h>
}

#include "util/log/Logger.h"

namespace {

constexpr size_t FFMPEG_LINE_SIZE = 1024;

void onAvLog(void* object, int level, const char* format, va_list arguments) {
    if (level > AV_LOG_WARNING) {
        return;
    }
    std::array<char, FFMPEG_LINE_SIZE> line{};
    int printPrefix = 1;
    av_log_format_line2(object, level, format, arguments, line.data(), static_cast<int>(line.size()), &printPrefix);
    std::string text(line.data());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    if (level <= AV_LOG_ERROR) {
        LOG_ERROR(ffmpeg, sys, "{}", text);
    } else {
        LOG_WARN(ffmpeg, sys, "{}", text);
    }
}

}  // namespace

void FfmpegLog::install() {
    av_log_set_callback(onAvLog);
}
