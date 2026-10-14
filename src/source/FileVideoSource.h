#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "source/FfmpegVideoSource.h"

// MP4/AVI file played as a camera. paced by the reader at the file frame rate, optionally looped
class FileVideoSource : public FfmpegVideoSource {
public:
    FileVideoSource(std::string path, int width, int height, bool loop);

protected:
    std::map<std::string, std::string> get_open_options() const override;
    read_status on_end_of_stream() override;
    read_status on_read_error(int code) override;
    int64_t get_io_timeout_us() const override;

private:
    std::string _path;
    bool _loop;
};
