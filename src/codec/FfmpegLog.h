#pragma once

// routes FFmpeg's av_log into the project logger so nothing leaks to stderr
class FfmpegLog {
public:
    static void install();
};
