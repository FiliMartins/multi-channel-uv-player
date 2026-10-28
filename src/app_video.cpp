// Video layers, the performance readout and keyframes.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <imgui.h>

#include "app.h"
#include "paths.h"

namespace {

const ImVec4 kWarningColor(0.95f, 0.75f, 0.30f, 1.0f);
const ImVec4 kMutedColor(0.55f, 0.57f, 0.62f, 1.0f);

float evaluate(const std::vector<Keyframe>& keys, double time) {
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (time > keys[i].time) continue;
        const Keyframe& a = keys[i - 1];
        const Keyframe& b = keys[i];
        const double span = b.time - a.time;
        const float t = span > 0.0 ? (float)((time - a.time) / span) : 1.0f;
        return a.value + (b.value - a.value) * t;
    }
    return keys.back().value;
}

}  // namespace

const char* keyParamName(KeyParam param) {
    switch (param) {
        case KeyParam::Opacity: return "opacity";
        case KeyParam::Size: return "size";
        case KeyParam::Angle: return "angle";
        case KeyParam::Trail: return "trail";
        case KeyParam::Density: return "amount";
        default: return "?";
    }
}

bool App::loadMedia(const std::string& path, Layer& layer, std::string& error) {
    if (!isVideoFile(path)) {
        layer.video.reset();
        return loadImage(path, maxTextureSize_, layer.image, error);
    }
    auto video = std::make_shared<Video>();
    if (!video->open(path, maxTextureSize_, error)) return false;
    layer.video = video;
    layer.image = Image{};
    layer.image.path = path;
    layer.image.name = fileNameOf(path);
    layer.image.width = video->info().width;
    layer.image.height = video->info().height;
    return true;
}

void App::freeMedia(Layer& layer) {
    if (layer.video) {
        layer.video.reset();  // the video owns its texture
        layer.image = Image{};
    } else {
        freeImage(layer.image);
    }
}

// Each video shows the frame for its layer's time: frames count from the in point
// and, past the last one, loop, hold or (in layerActive) hide.
void App::updateVideos() {
    for (Layer& layer : layers_) {
        if (!layer.video) continue;
        const std::string error = layer.video->takeError();
        if (!error.empty()) log(layer.image.name + ": " + error);
        const VideoInfo& info = layer.video->info();
        const double local = std::max(0.0, time_ - layer.inTime);
        int64_t frame = (int64_t)std::floor(local * info.fps + 1e-6);
        if (frame >= info.frames) frame = layer.videoEnd == VideoEnd::Loop ? frame % info.frames : info.frames - 1;
        const bool active = layer.visible && layerActive(layer);
        layer.video->request(frame, playing_ && active, layer.videoEnd == VideoEnd::Loop);
        layer.video->update();
        layer.image.texture = layer.video->texture();
    }
}

void App::waitForVideos(double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (true) {
        updateVideos();
        bool ready = true;
        for (const Layer& layer : layers_)
            if (layer.video && layer.visible && layerActive(layer) && !layer.video->showingRequested()) ready = false;
        if (ready || std::chrono::steady_clock::now() > end) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void App::drawVideoInfo(Layer& layer) {
    Video& video = *layer.video;
    const VideoInfo& info = video.info();
    ImGui::SeparatorText("Video");
    ImGui::Text("%s, %.3g fps, %s (%lld frames)", info.codec.c_str(), info.fps, timecode(info.duration).c_str(),
                (long long)info.frames);
    ImGui::TextColored(kMutedColor, info.gpuDecoded ? "Decoded on the GPU" : "Decoded on the CPU");

    const char* endNames[] = {"Loop", "Hold last frame", "Hide"};
    const float fieldWidth = std::max(120.0f, ImGui::GetContentRegionAvail().x - 110.0f);
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::BeginCombo("At the end", endNames[(int)layer.videoEnd])) {
        for (int e = 0; e < 3; ++e)
            if (ImGui::Selectable(endNames[e], (int)layer.videoEnd == e)) layer.videoEnd = (VideoEnd)e;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the layer shows once the video has played to its end.");
    if (ImGui::Button("Fit bar to video length")) {
        layer.outTime = std::clamp(snapToFrame(layer.inTime + info.duration), layer.inTime + 1.0 / fps_, length_);
    }

    const float budget = 1000.0f / (float)info.fps;
    const float decodeMs = video.decodeMilliseconds();
    ImGui::Text("Frame %lld of %lld", (long long)video.shownFrame() + 1, (long long)info.frames);
    ImGui::SameLine(0, 16);
    if (decodeMs > budget) ImGui::TextColored(kWarningColor, "Decode %.1f ms per frame (too slow for %.3g fps)", decodeMs, info.fps);
    else ImGui::TextColored(kMutedColor, "Decode %.1f ms per frame", decodeMs);
    ImGui::Text("Dropped frames: %d", video.droppedFrames());
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##dropped")) video.resetStats();
}

// FPS of the app and dropped video frames, at the right of the transport bar.
void App::drawPerformance() {
    const ImGuiIO& io = ImGui::GetIO();
    int dropped = 0, videos = 0;
    bool slowDecode = false;
    for (const Layer& layer : layers_) {
        if (!layer.video) continue;
        ++videos;
        dropped += layer.video->droppedFrames();
        if (layer.visible && layerActive(layer) &&
            layer.video->decodeMilliseconds() > 1000.0f / (float)layer.video->info().fps)
            slowDecode = true;
    }
    const bool slowDraw = playing_ && io.Framerate < fps_ * 0.95f;
    ImGui::SameLine(0, 18);
    ImGui::BeginGroup();
    ImGui::TextColored(slowDraw ? kWarningColor : kMutedColor, "%.0f fps", io.Framerate);
    if (videos > 0) {
        ImGui::SameLine();
        ImGui::TextColored(dropped > 0 ? kWarningColor : kMutedColor, "dropped %d", dropped);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##perf"))
            for (Layer& layer : layers_)
                if (layer.video) layer.video->resetStats();
    }
    if (slowDraw || slowDecode) {
        ImGui::SameLine();
        ImGui::TextColored(kWarningColor, "Too heavy");
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Drawing at %.0f fps (%.1f ms per frame); the timeline runs at %d fps.", io.Framerate,
                    1000.0f / std::max(io.Framerate, 1.0f), fps_);
        if (slowDraw) ImGui::TextColored(kWarningColor, "Drawing is slower than the timeline, so video frames are skipped.");
        for (const Layer& layer : layers_) {
            if (!layer.video) continue;
            const float ms = layer.video->decodeMilliseconds();
            const float budget = 1000.0f / (float)layer.video->info().fps;
            ImGui::TextColored(ms > budget ? kWarningColor : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                               "%s: %s, decode %.1f of %.1f ms, dropped %d", layer.image.name.c_str(),
                               layer.video->info().codec.c_str(), ms, budget, layer.video->droppedFrames());
        }
        ImGui::EndTooltip();
    }
}

float* App::keyTarget(Layer& layer, KeyParam param) {
    switch (param) {
        case KeyParam::Opacity: return &layer.opacity;
        case KeyParam::Size: return layer.generated ? &layer.generator.size : nullptr;
        case KeyParam::Angle: return layer.generated ? &layer.generator.angle : nullptr;
        case KeyParam::Trail: return layer.generated ? &layer.generator.trail : nullptr;
        case KeyParam::Density: return layer.generated ? &layer.generator.density : nullptr;
        default: return nullptr;
    }
}

// Animated values follow their keys at the playhead, before anything draws.
void App::applyKeyframes() {
    for (Layer& layer : layers_) {
        const double local = time_ - layer.inTime;
        for (int p = 0; p < (int)KeyParam::Count; ++p) {
            const std::vector<Keyframe>& keys = layer.keys[p];
            float* target = keyTarget(layer, (KeyParam)p);
            if (!keys.empty() && target) *target = evaluate(keys, local);
        }
    }
}

namespace {

// The key on the playhead's frame, or -1.
int keyAt(const std::vector<Keyframe>& keys, double local, int fps) {
    for (size_t i = 0; i < keys.size(); ++i)
        if (std::fabs(keys[i].time - local) < 0.5 / fps) return (int)i;
    return -1;
}

void setKey(std::vector<Keyframe>& keys, double local, float value, int fps) {
    const int at = keyAt(keys, local, fps);
    if (at >= 0) {
        keys[at].value = value;
        return;
    }
    keys.push_back({local, value});
    std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

}  // namespace

void App::keyEdited(Layer& layer, KeyParam param) {
    std::vector<Keyframe>& keys = layer.keys[(int)param];
    float* target = keyTarget(layer, param);
    if (keys.empty() || !target) return;
    setKey(keys, snapToFrame(time_) - layer.inTime, *target, fps_);
}

void App::keyButton(Layer& layer, KeyParam param) {
    float* target = keyTarget(layer, param);
    if (!target) return;
    std::vector<Keyframe>& keys = layer.keys[(int)param];
    const double local = snapToFrame(time_) - layer.inTime;
    const int at = keyAt(keys, local, fps_);

    ImGui::SameLine(0, 4);
    ImGui::PushID((int)param + 7000);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("key", ImVec2(height * 0.8f, height))) {
        if (at >= 0) keys.erase(keys.begin() + at);
        else setKey(keys, local, *target, fps_);
    }
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 c(corner.x + height * 0.4f, corner.y + height * 0.5f);
    const float r = height * 0.28f;
    const ImVec2 points[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
    const ImU32 lit = IM_COL32(245, 180, 60, 255);
    const ImU32 color = !keys.empty() ? lit : hovered ? IM_COL32(200, 200, 210, 255) : IM_COL32(110, 115, 125, 255);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (at >= 0) drawList->AddConvexPolyFilled(points, 4, lit);
    else drawList->AddPolyline(points, 4, color, ImDrawFlags_Closed, 1.5f);
    if (hovered) {
        if (at >= 0) ImGui::SetTooltip("Remove the %s key at the playhead", keyParamName(param));
        else if (keys.empty()) ImGui::SetTooltip("Animate %s: add a key at the playhead.\nAfter that, changing the value sets a key.", keyParamName(param));
        else ImGui::SetTooltip("Add a %s key at the playhead (%zu keys so far)", keyParamName(param), keys.size());
    }
    ImGui::PopID();
}
