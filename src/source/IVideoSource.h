#pragma once

#include <string>

#include "model/FrameBuffer.h"

struct video_source_info {
    std::string type;
    std::string uri;
    int width = 0;  // output size after conversion
    int height = 0;
    double nativeFps = 0.0;
    bool live = false;  // true: the source paces itself (camera, stream). false: the reader paces at nativeFps
};

enum class read_status {
    ok,
    end_of_stream,
    retry,  // transient failure, the caller may read again later
    error
};

/**
 * @brief one camera channel. simulators and real inputs live behind the same interface
 *
 * open() and read() may block (network connect, decode). they run on the channel reader thread only.
 * read() writes the next picture into out as I420 at the configured output size.
 * interrupt() is thread-safe and makes a blocked open()/read() return soon
 */
class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    virtual bool open() = 0;
    virtual read_status read(FrameBuffer& out) = 0;
    virtual void close() = 0;
    virtual void interrupt() = 0;
    virtual video_source_info get_info() const = 0;
};
