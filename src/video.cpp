#include "video.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <glad/gl.h>

#include "gl_util.h"
#include "paths.h"

namespace {

// Compressed formats that are extensions in GL 3.3 core; every desktop driver has them.
constexpr GLenum kDxt1 = 0x83F0;  // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
constexpr GLenum kDxt5 = 0x83F3;  // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
constexpr GLenum kBc7 = 0x8E8C;   // GL_COMPRESSED_RGBA_BPTC_UNORM
constexpr int kDecodeAhead = 3;   // frames decoded past the playhead while playing

const char* kVertexShader = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Reads the decoded frame (top row first, possibly padded to whole 4x4 blocks)
// and writes premultiplied RGBA, bottom row first.
const char* kFragmentShader = R"(#version 330 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uColor;
uniform sampler2D uAlpha;
uniform int uMode;      // 0 RGB(A), 1 scaled YCoCg, 2 scaled YCoCg with an alpha plane
uniform vec2 uScale;    // share of the texture the picture covers
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y) * uScale;
    vec4 c = texture(uColor, uv);
    vec4 color = c;
    if (uMode >= 1) {
        float scale = c.z * (255.0 / 8.0) + 1.0;
        float co = (c.x - 0.5 * 256.0 / 255.0) / scale;
        float cg = (c.y - 0.5 * 256.0 / 255.0) / scale;
        float y = c.w;
        color = vec4(y + co - cg, y + cg, y - co - cg, 1.0);
        if (uMode == 2) color.a = texture(uAlpha, uv).r;
    }
    color = clamp(color, 0.0, 1.0);
    fragColor = vec4(color.rgb * color.a, color.a);
}
)";

GLuint gProgram = 0;
GLuint gVao = 0;

bool ensureProgram() {
    if (gProgram) return true;
    gProgram = linkProgram(kVertexShader, kFragmentShader);
    if (!gProgram) return false;
    glGenVertexArrays(1, &gVao);
    return true;
}

std::string fourcc(uint32_t tag) {
    std::string text;
    for (int i = 0; i < 4; ++i) text += (char)((tag >> (8 * i)) & 0xFF);
    return text;
}

std::string hapName(uint32_t tag) {
    const std::string code = fourcc(tag);
    if (code == "Hap1") return "HAP";
    if (code == "Hap5") return "HAP Alpha";
    if (code == "HapY") return "HAP Q";
    if (code == "HapM") return "HAP Q Alpha";
    if (code == "Hap7") return "HAP R";
    if (code == "HapH") return "HAP HDR";
    if (code == "HapA") return "HAP Alpha-only";
    return "HAP";
}

std::string codecName(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return "H.264";
        case AV_CODEC_ID_HEVC: return "H.265";
        case AV_CODEC_ID_NOTCHLC: return "NotchLC";
        case AV_CODEC_ID_PRORES: return "ProRes";
        case AV_CODEC_ID_DNXHD: return "DNxHD/HR";
        default: {
            const AVCodecDescriptor* descriptor = avcodec_descriptor_get(id);
            return descriptor ? descriptor->name : "unknown";
        }
    }
}

std::string ffmpegError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

}  // namespace

bool isVideoFile(const std::string& path) {
    std::string extension = utf8String(toPath(path).extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return extension == ".mov" || extension == ".mp4" || extension == ".m4v" || extension == ".avi" ||
           extension == ".mkv" || extension == ".mxf";
}

Video::~Video() {
    close();
}

void Video::close() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }
    if (sws_) sws_freeContext(sws_);
    if (codec_) avcodec_free_context(&codec_);
    if (format_) avformat_close_input(&format_);
    if (packet_) av_packet_free(&packet_);
    if (decoded_) av_frame_free(&decoded_);
    sws_ = nullptr;
    for (int i = 0; i < 2; ++i)
        if (staging_[i]) glDeleteTextures(1, &staging_[i]);
    if (output_) glDeleteTextures(1, &output_);
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    staging_[0] = staging_[1] = output_ = framebuffer_ = 0;
}

void Video::shutdownGl() {
    if (gProgram) glDeleteProgram(gProgram);
    if (gVao) glDeleteVertexArrays(1, &gVao);
    gProgram = gVao = 0;
}

bool Video::open(const std::string& path, int maxTextureSize, std::string& error) {
    av_log_set_level(AV_LOG_ERROR);
    int result = avformat_open_input(&format_, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "Could not open video: " + ffmpegError(result);
        return false;
    }
    if ((result = avformat_find_stream_info(format_, nullptr)) < 0) {
        error = "Could not read the video's streams: " + ffmpegError(result);
        return false;
    }
    stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_ < 0) {
        error = "The file has no video stream.";
        return false;
    }
    AVStream* stream = format_->streams[stream_];
    for (unsigned i = 0; i < format_->nb_streams; ++i)
        if ((int)i != stream_) format_->streams[i]->discard = AVDISCARD_ALL;
    const AVCodecParameters* parameters = stream->codecpar;
    info_.width = parameters->width;
    info_.height = parameters->height;
    if (info_.width <= 0 || info_.height <= 0) {
        error = "The video has no picture size.";
        return false;
    }
    if (info_.width > maxTextureSize || info_.height > maxTextureSize) {
        error = "Video is " + std::to_string(info_.width) + " x " + std::to_string(info_.height) +
                " px, larger than this GPU's limit of " + std::to_string(maxTextureSize) + " px per side.";
        return false;
    }

    const AVRational rate = av_guess_frame_rate(format_, stream, nullptr);
    info_.fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 30.0;
    timeBase_ = av_q2d(stream->time_base);
    startTime_ = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    if (stream->duration > 0) info_.duration = stream->duration * timeBase_;
    else if (format_->duration > 0) info_.duration = (double)format_->duration / AV_TIME_BASE;
    info_.frames = stream->nb_frames > 0 ? stream->nb_frames : (int64_t)std::llround(info_.duration * info_.fps);
    info_.frames = std::max<int64_t>(info_.frames, 1);
    if (info_.duration <= 0.0) info_.duration = info_.frames / info_.fps;

    if (parameters->codec_id == AV_CODEC_ID_HAP) {
        info_.codec = hapName(parameters->codec_tag);
        info_.gpuDecoded = true;
        if (fourcc(parameters->codec_tag) == "HapH" || fourcc(parameters->codec_tag) == "HapA") {
            error = info_.codec + " is not supported yet.";
            return false;
        }
    } else {
        info_.codec = codecName(parameters->codec_id);
        const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
        if (!decoder) {
            error = "No decoder for " + info_.codec + ".";
            return false;
        }
        codec_ = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(codec_, parameters);
        codec_->thread_count = 0;  // as many as the CPU has
        if ((result = avcodec_open2(codec_, decoder, nullptr)) < 0) {
            error = "Could not start the " + info_.codec + " decoder: " + ffmpegError(result);
            return false;
        }
    }
    packet_ = av_packet_alloc();
    decoded_ = av_frame_alloc();
    worker_ = std::thread([this] { workerLoop(); });
    return true;
}

void Video::request(int64_t frame, bool playing, bool loop) {
    frame = std::clamp<int64_t>(frame, 0, info_.frames - 1);
    if (playing && frame != target_) {
        // Frames skipped over, and the one due before this if it never made it on screen.
        const int64_t step = frame > target_ ? frame - target_ : frame + info_.frames - target_;
        if (step < info_.frames / 2) dropped_ += (int)(step - 1) + (shown_ != target_ ? 1 : 0);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame == target_ && playing == playing_ && loop == loop_) return;
        target_ = frame;
        playing_ = playing;
        loop_ = loop;
    }
    wake_.notify_one();
}

float Video::decodeMilliseconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decodeMs_;
}

std::string Video::takeError() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    error.swap(error_);
    return error;
}

// The frames that should be in the cache: the target and, while playing, the
// next few. Drops the rest and returns the first one still missing, or -1.
int64_t Video::nextWanted() {
    std::vector<int64_t> wanted = {target_};
    if (playing_) {
        for (int i = 1; i <= kDecodeAhead; ++i) {
            int64_t index = target_ + i;
            if (index >= info_.frames) {
                if (!loop_) break;
                index %= info_.frames;
            }
            wanted.push_back(index);
        }
    }
    cache_.erase(std::remove_if(cache_.begin(), cache_.end(),
                                [&](const std::shared_ptr<Frame>& f) {
                                    return std::find(wanted.begin(), wanted.end(), f->index) == wanted.end();
                                }),
                 cache_.end());
    for (int64_t index : wanted) {
        const bool cached = std::any_of(cache_.begin(), cache_.end(),
                                        [&](const std::shared_ptr<Frame>& f) { return f->index == index; });
        if (!cached) return index;
    }
    return -1;
}

void Video::workerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        int64_t want = -1;
        wake_.wait(lock, [&] { return quit_ || (want = nextWanted()) >= 0; });
        if (quit_) return;
        lock.unlock();
        auto frame = std::make_shared<Frame>();
        const auto start = std::chrono::steady_clock::now();
        const bool ok = decode(want, *frame);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        lock.lock();
        if (ok) {
            decodeMs_ = decodeMs_ == 0.0f ? ms : decodeMs_ * 0.9f + ms * 0.1f;
            frame->index = want;  // a frame past the end or after a damaged one stands in for it
            cache_.push_back(frame);
        } else {
            // Leave a blank frame so the worker does not spin on the same failure.
            frame->index = want;
            frame->rgba.assign((size_t)info_.width * info_.height * 4, 0);
            cache_.push_back(frame);
        }
    }
}

int64_t Video::indexOf(int64_t timestamp) const {
    if (timestamp == AV_NOPTS_VALUE) return nextFrame_;
    return (int64_t)std::llround((timestamp - startTime_) * timeBase_ * info_.fps);
}

bool Video::seek(int64_t want) {
    const int64_t timestamp = startTime_ + (int64_t)std::llround(want / info_.fps / timeBase_);
    const int result = av_seek_frame(format_, stream_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (codec_) avcodec_flush_buffers(codec_);
    nextFrame_ = -1;
    if (result < 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Seek failed: " + ffmpegError(result);
        return false;
    }
    return true;
}

bool Video::convertPacket(const uint8_t* data, size_t size, int64_t index, Frame& frame) {
    std::string error;
    if (!decodeHapFrame(data, size, info_.width, info_.height, frame.hap, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Frame " + std::to_string(index) + ": " + error;
        return false;
    }
    return true;
}

// Reads on from where the file is, seeking first when the wanted frame is behind
// or far ahead, and keeps the first frame at or after `want`.
bool Video::decode(int64_t want, Frame& frame) {
    if (nextFrame_ < 0 || want < nextFrame_ || want > nextFrame_ + 30) seek(want);
    bool draining = false;
    for (int attempts = 0; attempts < 100000; ++attempts) {
        if (codec_) {
            const int received = avcodec_receive_frame(codec_, decoded_);
            if (received == 0) {
                const int64_t index = indexOf(decoded_->best_effort_timestamp);
                nextFrame_ = index + 1;
                if (index < want) continue;
                frame.rgba.resize((size_t)info_.width * info_.height * 4);
                sws_ = sws_getCachedContext(sws_, decoded_->width, decoded_->height, (AVPixelFormat)decoded_->format,
                                            info_.width, info_.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                            nullptr, nullptr);
                uint8_t* planes[1] = {frame.rgba.data()};
                const int strides[1] = {info_.width * 4};
                sws_scale(sws_, decoded_->data, decoded_->linesize, 0, decoded_->height, planes, strides);
                av_frame_unref(decoded_);
                return true;
            }
            if (received == AVERROR_EOF) {
                nextFrame_ = -1;
                return false;
            }
            if (draining) return false;
        }
        const int read = av_read_frame(format_, packet_);
        if (read < 0) {
            if (!codec_) {
                nextFrame_ = -1;
                return false;
            }
            avcodec_send_packet(codec_, nullptr);  // drain the frames still inside the decoder
            draining = true;
            continue;
        }
        if (packet_->stream_index != stream_) {
            av_packet_unref(packet_);
            continue;
        }
        if (codec_) {
            avcodec_send_packet(codec_, packet_);
            av_packet_unref(packet_);
            continue;
        }
        const int64_t index = indexOf(packet_->pts != AV_NOPTS_VALUE ? packet_->pts : packet_->dts);
        nextFrame_ = index + 1;
        if (index < want) {
            av_packet_unref(packet_);
            continue;
        }
        const bool ok = convertPacket(packet_->data, (size_t)packet_->size, index, frame);
        av_packet_unref(packet_);
        return ok;
    }
    return false;
}

void Video::update() {
    std::shared_ptr<Frame> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shown_ == target_) return;
        for (const auto& frame : cache_)
            if (frame->index == target_) ready = frame;
    }
    if (!ready) return;
    upload(*ready);
    shown_ = ready->index;
}

void Video::upload(const Frame& frame) {
    if (!ensureProgram()) return;
    const int width = info_.width, height = info_.height;
    const int paddedWidth = (width + 3) / 4 * 4, paddedHeight = (height + 3) / 4 * 4;
    int mode = 0;
    int textures = 1;
    float scale[2] = {1.0f, 1.0f};

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < 2; ++i)
        if (!staging_[i]) {
            glGenTextures(1, &staging_[i]);
            glBindTexture(GL_TEXTURE_2D, staging_[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }

    if (!frame.hap.empty()) {
        textures = (int)frame.hap.size();
        scale[0] = (float)width / paddedWidth;
        scale[1] = (float)height / paddedHeight;
        for (int i = 0; i < textures; ++i) {
            const HapTexture& texture = frame.hap[i];
            const GLenum format = texture.format == HapFormat::Dxt1         ? kDxt1
                                  : texture.format == HapFormat::Bc7        ? kBc7
                                  : texture.format == HapFormat::AlphaRgtc1 ? GL_COMPRESSED_RED_RGTC1
                                                                            : kDxt5;
            glBindTexture(GL_TEXTURE_2D, staging_[i]);
            if (stagingFormat_[i] != format || stagingWidth_[i] != paddedWidth || stagingHeight_[i] != paddedHeight) {
                glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, paddedWidth, paddedHeight, 0,
                                       (GLsizei)texture.blocks.size(), texture.blocks.data());
                stagingFormat_[i] = format;
                stagingWidth_[i] = paddedWidth;
                stagingHeight_[i] = paddedHeight;
            } else {
                glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, paddedWidth, paddedHeight, format,
                                          (GLsizei)texture.blocks.size(), texture.blocks.data());
            }
        }
        if (frame.hap[0].format == HapFormat::YCoCgDxt5) mode = textures == 2 ? 2 : 1;
    } else {
        glBindTexture(GL_TEXTURE_2D, staging_[0]);
        if (stagingFormat_[0] != GL_RGBA8 || stagingWidth_[0] != width || stagingHeight_[0] != height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
            stagingFormat_[0] = GL_RGBA8;
            stagingWidth_[0] = width;
            stagingHeight_[0] = height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
        }
    }

    if (!output_) {
        glGenTextures(1, &output_);
        glBindTexture(GL_TEXTURE_2D, output_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_, 0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(gProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, staging_[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, staging_[1]);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(gProgram, "uColor"), 0);
    glUniform1i(glGetUniformLocation(gProgram, "uAlpha"), 1);
    glUniform1i(glGetUniformLocation(gProgram, "uMode"), mode);
    glUniform2fv(glGetUniformLocation(gProgram, "uScale"), 1, scale);
    glBindVertexArray(gVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glBindTexture(GL_TEXTURE_2D, output_);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}
