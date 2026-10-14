#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#include "source/IVideoSource.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * @brief demux + decode + convert-to-I420 skeleton shared by file and network sources
 *
 * subclasses only decide the open options, whether to loop at the end and how to react to errors
 */
class FfmpegVideoSource : public IVideoSource {
public:
    FfmpegVideoSource(std::string type, std::string uri, int width, int height, bool live);
    ~FfmpegVideoSource() override;

    FfmpegVideoSource(const FfmpegVideoSource&) = delete;
    FfmpegVideoSource& operator=(const FfmpegVideoSource&) = delete;

    bool open() override;
    read_status read(FrameBuffer& out) override;
    void close() override;
    void interrupt() override;
    video_source_info get_info() const override;

protected:
    // subclass policy
    virtual std::map<std::string, std::string> get_open_options() const = 0;
    virtual read_status on_end_of_stream() = 0;
    virtual read_status on_read_error(int code) = 0;
    virtual int64_t get_io_timeout_us() const = 0;

    bool rewind();
    std::string describeError(int code) const;

private:
    static int on_interrupt(void* opaque);

    bool openDecoder();
    bool convert(const AVFrame* frame, FrameBuffer& out);
    read_status receiveFrame(FrameBuffer& out, bool& produced);

    std::string _type;
    std::string _uri;
    int _width;
    int _height;
    bool _live;
    double _nativeFps;
    AVFormatContext* _format;
    AVCodecContext* _decoder;
    AVFrame* _frame;
    AVPacket* _packet;
    SwsContext* _scaler;
    int _streamIndex;
    bool _draining;
    std::atomic<bool> _interrupted;
    std::atomic<int64_t> _ioDeadlineNs;
};
