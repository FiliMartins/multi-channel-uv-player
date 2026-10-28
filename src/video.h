#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hap.h"

struct VideoInfo {
    std::string codec;       // e.g. "HAP Q", "H.264", "NotchLC"
    int width = 0, height = 0;
    double fps = 0.0;
    double duration = 0.0;   // seconds
    int64_t frames = 0;
    bool gpuDecoded = false; // HAP: the GPU reads the compressed frames directly
};

// A video file played onto a texture. A worker thread reads and decodes frames
// ahead of the playhead; the main thread asks for a frame each UI frame and
// uploads it when it is ready. The texture is premultiplied RGBA with mipmaps,
// bottom row first, like an image layer's.
class Video {
public:
    Video() = default;
    ~Video();
    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    bool open(const std::string& path, int maxTextureSize, std::string& error);
    const VideoInfo& info() const { return info_; }

    // Main thread. `frame` is the one that should be on screen; while playing,
    // the frames after it are decoded ahead (wrapping to the start when looping).
    void request(int64_t frame, bool playing, bool loop);
    // Main thread: uploads the requested frame once it is decoded.
    void update();
    unsigned texture() const { return output_; }
    int64_t shownFrame() const { return shown_; }
    bool showingRequested() const { return shown_ == target_; }

    // Average time to read and decode one frame, in milliseconds.
    float decodeMilliseconds() const;
    // Frames that were due while playing but never shown.
    int droppedFrames() const { return dropped_; }
    void resetStats() { dropped_ = 0; }
    // An error from the worker since the last call, or empty.
    std::string takeError();

    // Frees the shared shader; call once before the GL context goes away.
    static void shutdownGl();

private:
    struct Frame {
        int64_t index = -1;
        std::vector<uint8_t> rgba;          // top row first, straight alpha
        std::vector<HapTexture> hap;
    };

    void close();
    void workerLoop();
    int64_t nextWanted();  // under mutex_
    bool decode(int64_t want, Frame& frame);
    bool seek(int64_t want);
    int64_t indexOf(int64_t timestamp) const;
    bool convertPacket(const uint8_t* data, size_t size, int64_t index, Frame& frame);
    void upload(const Frame& frame);

    VideoInfo info_;

    // FFmpeg state, used only by the worker after open().
    struct AVFormatContext* format_ = nullptr;
    struct AVCodecContext* codec_ = nullptr;  // null for HAP
    struct SwsContext* sws_ = nullptr;
    struct AVPacket* packet_ = nullptr;
    struct AVFrame* decoded_ = nullptr;
    int stream_ = -1;
    int64_t startTime_ = 0;
    double timeBase_ = 0.0;
    int64_t nextFrame_ = -1;  // what reading on would give next, -1 unknown

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    int64_t target_ = 0;
    bool playing_ = false;
    bool loop_ = true;
    std::deque<std::shared_ptr<Frame>> cache_;
    float decodeMs_ = 0.0f;
    std::string error_;

    // Main thread only.
    int64_t shown_ = -1;
    int dropped_ = 0;
    unsigned staging_[2] = {0, 0};
    unsigned stagingFormat_[2] = {0, 0};
    int stagingWidth_[2] = {0, 0}, stagingHeight_[2] = {0, 0};
    unsigned framebuffer_ = 0;
    unsigned output_ = 0;
};

bool isVideoFile(const std::string& path);
