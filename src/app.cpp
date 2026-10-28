#include "app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image_write.h>

#include "file_dialog.h"
#include "paths.h"

namespace {

const char* kProjectWindow = "Project";
const char* kLayersWindow = "Layers";
const char* kViewportWindow = "3D View";
const char* kUvWindow = "UV View";
const char* kLogWindow = "Log";
const char* kTimelineWindow = "Timeline";
const char* kLayerPayload = "LAYER_INDEX";

const ImVec4 kWarningColor(0.95f, 0.75f, 0.30f, 1.0f);
const ImVec4 kMutedColor(0.55f, 0.57f, 0.62f, 1.0f);

std::string unitName(double meters) {
    if (std::fabs(meters - 0.01) < 1e-9) return "centimetres";
    if (std::fabs(meters - 1.0) < 1e-9) return "metres";
    if (std::fabs(meters - 0.001) < 1e-9) return "millimetres";
    if (std::fabs(meters - 0.0254) < 1e-9) return "inches";
    if (std::fabs(meters - 0.3048) < 1e-9) return "feet";
    char text[64];
    std::snprintf(text, sizeof(text), "%g m per unit", meters);
    return text;
}

std::string lowerExtension(const std::string& path) {
    std::string extension = utf8String(toPath(path).extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return extension;
}

// Lower-case letters and digits only, so "Wall 1", "wall_1" and "WALL-1" all match.
std::string normalizedName(const std::string& text) {
    std::string result;
    for (unsigned char c : text)
        if (std::isalnum(c)) result += (char)std::tolower(c);
    return result;
}

// Drops UV tags such as "uv01" from a normalized name, so a surface called
// "Wall 1 uv01" still matches a file called "Wall_1_UV1.png".
std::string withoutUvTags(const std::string& name) {
    std::string result;
    for (size_t i = 0; i < name.size();) {
        size_t digits = 0;
        if (name.compare(i, 2, "uv") == 0)
            while (i + 2 + digits < name.size() && std::isdigit((unsigned char)name[i + 2 + digits])) ++digits;
        if (digits == 1 || digits == 2) {
            i += 2 + digits;
        } else {
            result += name[i];
            ++i;
        }
    }
    return result;
}

std::string sizeText(int width, int height) {
    return std::to_string(width) + " x " + std::to_string(height) + " px";
}

}  // namespace

bool App::init(int maxTextureSize) {
    maxTextureSize_ = maxTextureSize;
    if (!renderer_.init() || !generators_.init()) return false;
    log("GPU texture limit: " + std::to_string(maxTextureSize_) + " px per side.");
    return true;
}

void App::shutdown() {
    for (int i = (int)layers_.size() - 1; i >= 0; --i) removeLayer(i);
    generators_.shutdown();
    renderer_.shutdown();
}

void App::log(const std::string& message) {
    log_.push_back(message);
    std::fprintf(stderr, "%s\n", message.c_str());
}

std::string App::channelLabel(int channel) const {
    char label[16];
    std::snprintf(label, sizeof(label), "UV%02d", channel + 1);
    return label;
}

std::string App::surfaceLabel(int surface) const {
    if (surface < 0 || surface >= (int)mesh_.surfaces.size()) return "All surfaces";
    return mesh_.surfaces[surface].name;
}

std::string App::placementLabel(int channel, int surface) const {
    if (surface >= 0) return channelLabel(channel) + ", " + surfaceLabel(surface);
    if (channel == 0 && anyLedSurface() && anyProjectionSurface()) return channelLabel(channel) + ", projection surfaces";
    return channelLabel(channel);
}

bool App::channelUsable(int channel) const {
    return channel >= 0 && channel < (int)channels_.size() && !channels_[channel].ignored;
}

bool App::isLedSurface(int surface) const {
    if (surface < 0 || surface >= (int)mesh_.surfaces.size()) return false;
    if (mode_ == MappingMode::Led) return true;
    return mode_ == MappingMode::Mixed && surface < (int)mixedLed_.size() && mixedLed_[surface];
}

bool App::anyLedSurface() const {
    for (int s = 0; s < (int)mesh_.surfaces.size(); ++s)
        if (isLedSurface(s)) return true;
    return false;
}

bool App::anyProjectionSurface() const {
    for (int s = 0; s < (int)mesh_.surfaces.size(); ++s)
        if (!isLedSurface(s)) return true;
    return false;
}

// UV01 on an LED surface has its own canvas per surface.
bool App::perSurface(int channel, int surface) const {
    return channel == 0 && isLedSurface(surface);
}

// A UV01 layer must name its surface when no projection surface could take it.
bool App::needsSurface(int channel) const {
    return channel == 0 && anyLedSurface() && !anyProjectionSurface();
}

int App::selectedIndex() const {
    for (int i = 0; i < (int)layers_.size(); ++i)
        if (layers_[i].id == selectedLayer_) return i;
    return -1;
}

int App::matchSurface(const std::string& fileName) const {
    const std::string name = withoutUvTags(normalizedName(fileName));
    int best = -1;
    size_t bestLength = 0;
    for (int s = 0; s < (int)mesh_.surfaces.size(); ++s) {
        const std::string surface = withoutUvTags(normalizedName(mesh_.surfaces[s].name));
        // The longest match wins, so "Wall_10" picks "Wall 10" over "Wall 1".
        if (!surface.empty() && surface.size() > bestLength && name.find(surface) != std::string::npos) {
            best = s;
            bestLength = surface.size();
        }
    }
    return best;
}

void App::setMode(MappingMode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    if (mode_ == MappingMode::Led) log("LED mode: UV01 is per surface; UV02 and up are shared layouts.");
    else if (mode_ == MappingMode::Mixed) log("Mixed mode: choose Projection or LED for each surface under Surfaces.");
    else log("Projection mode: every UV channel is shared by the whole mesh.");
    placeLedLayers();
}

void App::setSurfaceLed(int surface, bool led) {
    if (surface < 0 || surface >= (int)mesh_.surfaces.size()) return;
    mixedLed_.resize(mesh_.surfaces.size(), 0);
    mixedLed_[surface] = led ? 1 : 0;
    placeLedLayers();
}

// After surfaces change between Projection and LED, UV01 layers whose file names
// a surface move onto it, and files waiting for a surface are tried again.
void App::placeLedLayers() {
    for (Layer& layer : layers_) {
        if (layer.channel != 0 || layer.surface >= 0) continue;
        const int surface = matchSurface(layer.image.name);
        if (isLedSurface(surface)) layer.surface = surface;
    }
    retryPending();
}

void App::showTestGrid(int channel) {
    gridChannel_ = channel;
    view_.mode = ViewMode::TestGrid;
}

// The mapping's canvas is set by the largest file on it. UV01 on an LED surface
// has one canvas per surface; projection surfaces share the rest.
void App::canvasSize(int channel, int surface, int& width, int& height) const {
    width = height = 0;
    const bool own = perSurface(channel, surface);
    for (const Layer& layer : layers_) {
        if (layer.channel != channel) continue;
        if (own ? layer.surface != surface : perSurface(channel, layer.surface)) continue;
        if ((long long)layer.image.width * layer.image.height > (long long)width * height) {
            width = layer.image.width;
            height = layer.image.height;
        }
    }
}

std::vector<std::string> App::layerWarnings(const Layer& layer) const {
    std::vector<std::string> warnings;
    if (!channelUsable(layer.channel)) warnings.push_back(channelLabel(layer.channel) + " is ignored");
    if (needsSurface(layer.channel) && layer.surface < 0) warnings.push_back("UV01 on LED surfaces needs a surface");

    int width = 0, height = 0;
    canvasSize(layer.channel, layer.surface, width, height);
    if (layer.image.width < width || layer.image.height < height)
        warnings.push_back("Lower resolution: " + sizeText(layer.image.width, layer.image.height) + " on a " +
                           sizeText(width, height) + " mapping");
    if (width > 0 && height > 0 && layer.image.height > 0) {
        const float aspect = (float)layer.image.width / (float)layer.image.height;
        const float canvasAspect = (float)width / (float)height;
        if (std::fabs(aspect / canvasAspect - 1.0f) > 0.01f) {
            char text[96];
            std::snprintf(text, sizeof(text), "Aspect %.2f:1 differs from the mapping's %.2f:1, so it is stretched",
                          aspect, canvasAspect);
            warnings.push_back(text);
        }
    }
    return warnings;
}

void App::openMesh(const std::string& path) {
    Mesh loaded;
    std::string error;
    if (!loaded.load(path, error)) {
        log("Could not open " + fileNameOf(path) + ": " + error);
        return;
    }

    mesh_ = std::move(loaded);
    channels_.assign(mesh_.channels.size(), ChannelState{});
    mixedLed_.assign(mesh_.surfaces.size(), 0);
    renderer_.uploadMesh(mesh_);
    scale_ = 1.0f;
    camera_.frame(mesh_.boundsMin, mesh_.boundsMax);
    gridChannel_ = 0;

    // Layers from a previous mesh keep their media; their channel and surface
    // may no longer exist, which shows as a warning on the layer.
    for (Layer& layer : layers_)
        if (layer.surface >= (int)mesh_.surfaces.size()) layer.surface = -1;

    log("Opened " + fileNameOf(path) + ": " + std::to_string(mesh_.surfaces.size()) + " surface(s), " +
        std::to_string(mesh_.vertexCount() / 3) + " triangles, " + std::to_string(mesh_.channels.size()) +
        " UV channel(s).");
    if (mesh_.droppedChannels > 0)
        log("Warning: " + std::to_string(mesh_.droppedChannels) + " UV channel(s) beyond " +
            std::to_string(kMaxUvChannels) + " were not loaded.");
    for (size_t c = 0; c < mesh_.channels.size(); ++c) {
        const int original = mesh_.channels[c].duplicateOf;
        if (original >= 0)
            log(mesh_.channels[c].name + " is an exact copy of " + mesh_.channels[original].name +
                ". Choose keep or ignore in the Project panel.");
    }
    retryPending();
}

void App::importPath(const std::string& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(toPath(path), ec)) {
        importFolder(path);
    } else if (lowerExtension(path) == ".fbx") {
        openMesh(path);
    } else if (isImageFile(path)) {
        importImage(path);
    } else {
        log("Skipped " + fileNameOf(path) + ": not an FBX, image or folder.");
    }
}

void App::importFolder(const std::string& folder) {
    std::vector<std::string> meshes, images;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(toPath(folder), ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string path = utf8String(entry.path());
        if (lowerExtension(path) == ".fbx") meshes.push_back(path);
        else if (isImageFile(path)) images.push_back(path);
    }
    if (ec) {
        log("Could not read folder " + folder + ": " + ec.message());
        return;
    }
    std::sort(meshes.begin(), meshes.end());
    std::sort(images.begin(), images.end());

    log("Importing folder " + fileNameOf(folder) + ": " + std::to_string(images.size()) + " image(s).");
    if (mesh_.empty() && !meshes.empty()) openMesh(meshes.front());
    for (const std::string& image : images) importImage(image);
}

void App::importImage(const std::string& path) {
    const std::string name = fileNameOf(path);
    const int tag = parseUvTag(name);
    const int channel = tag - 1;
    int surface = -1;
    std::string reason;
    if (mesh_.empty()) reason = "Open a mesh first";
    else if (tag < 0) reason = "No UV tag in the name";
    else if (channel < 0 || channel >= (int)channels_.size()) reason = "UV" + std::to_string(tag) + " is not on this mesh";
    else if (channels_[channel].ignored) reason = channelLabel(channel) + " is ignored";
    else if (channel == 0 && anyLedSurface() && (surface = matchSurface(name)) < 0 && needsSurface(channel))
        reason = "No surface name found in the file name (UV01 is per surface on LED surfaces)";

    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const PendingFile& p) { return p.path == path; }),
                   pending_.end());
    if (!reason.empty()) {
        PendingFile file{path, reason, std::clamp(channel, 0, std::max(0, (int)channels_.size() - 1)), -1};
        pending_.push_back(file);
        log(name + ": " + reason + ". Pick a mapping under Unassigned files.");
        return;
    }
    addLayer(path, channel, surface);
}

bool App::addLayer(const std::string& path, int channel, int surface) {
    Layer layer;
    std::string error;
    if (!loadImage(path, maxTextureSize_, layer.image, error)) {
        log(fileNameOf(path) + ": " + error);
        return false;
    }
    layer.id = nextLayerId_++;
    layer.inTime = 0.0;
    layer.outTime = length_;
    layer.channel = channel;
    layer.surface = surface;
    layers_.push_back(layer);
    selectedLayer_ = layer.id;

    log(layer.image.name + " -> " + placementLabel(channel, surface) + " (" + sizeText(layer.image.width, layer.image.height) + ")");
    if (layers_.size() == (size_t)kMaxLayers + 1)
        log("Warning: more than " + std::to_string(kMaxLayers) + " visible layers; only the top " +
            std::to_string(kMaxLayers) + " are drawn.");
    return true;
}

void App::removeLayer(int index) {
    Layer& layer = layers_[index];
    if (layer.generated) {
        Generators::release(layer.target);
        layer.image = Image{};
    } else {
        freeImage(layer.image);
    }
    layers_.erase(layers_.begin() + index);
}

void App::moveLayer(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= (int)layers_.size() || to >= (int)layers_.size()) return;
    Layer layer = layers_[from];
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + to, layer);
}

void App::retryPending() {
    std::vector<PendingFile> waiting;
    waiting.swap(pending_);
    for (const PendingFile& file : waiting) importImage(file.path);
}

void App::frame() {
    ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (!layoutBuilt_) {
        layoutBuilt_ = true;
        if (!ImGui::DockBuilderGetNode(dockspace) || ImGui::DockBuilderGetNode(dockspace)->IsLeafNode()) {
            ImGui::DockBuilderRemoveNode(dockspace);
            ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
            ImGuiID top = 0, bottom = 0, logNode = 0, left = 0, right = 0, leftBottom = 0, uv = 0;
            ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Down, 0.28f, &bottom, &top);
            ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.25f, &logNode, &bottom);
            ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.30f, &left, &right);
            ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.5f, &leftBottom, &left);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Right, 0.45f, &uv, &right);
            ImGui::DockBuilderDockWindow(kTimelineWindow, bottom);
            ImGui::DockBuilderDockWindow(kProjectWindow, left);
            ImGui::DockBuilderDockWindow(kLayersWindow, leftBottom);
            ImGui::DockBuilderDockWindow(kViewportWindow, right);
            ImGui::DockBuilderDockWindow(kUvWindow, uv);
            ImGui::DockBuilderDockWindow(kLogWindow, logNode);
            ImGui::DockBuilderFinish(dockspace);
        }
    }

    advanceTime();
    updateGenerators();
    drawProjectPanel();
    drawLayersPanel();
    drawViewport();
    drawUvView();
    drawTimeline();
    drawLog();
}

void App::drawProjectPanel() {
    ImGui::Begin(kProjectWindow);
    drawMeshSection();
    ImGui::Spacing();
    drawChannelSection();
    ImGui::End();
}

void App::drawMeshSection() {
    ImGui::SeparatorText("Mesh");
    if (ImGui::Button("Open FBX...")) {
        const std::string path = openFileDialog(FileKind::Fbx);
        if (!path.empty()) openMesh(path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Import folder...")) {
        const std::string folder = pickFolderDialog();
        if (!folder.empty()) importFolder(folder);
    }
    if (!nativeDialogsAvailable()) {
        ImGui::SetNextItemWidth(-80);
        ImGui::InputTextWithHint("##path", "Path to an FBX, image or folder", pathInput_, sizeof(pathInput_));
        ImGui::SameLine();
        if (ImGui::Button("Import") && pathInput_[0]) importPath(pathInput_);
    }
    ImGui::TextColored(kMutedColor, "Or drop files and folders onto the window.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mode");
    ImGui::SameLine();
    if (ImGui::RadioButton("Projection", mode_ == MappingMode::Projection)) setMode(MappingMode::Projection);
    ImGui::SameLine();
    if (ImGui::RadioButton("LED", mode_ == MappingMode::Led)) setMode(MappingMode::Led);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("LED: UV01 is per surface, like feed mapping. UV02 and up stay shared layouts.");
    ImGui::SameLine();
    if (ImGui::RadioButton("Mixed", mode_ == MappingMode::Mixed)) setMode(MappingMode::Mixed);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mixed: choose Projection or LED for each surface.");

    if (mesh_.empty()) return;

    ImGui::Text("%s", fileNameOf(mesh_.path).c_str());
    ImGui::TextColored(kMutedColor, "File units: %s. %d surface(s), %d triangles.",
                       unitName(mesh_.fileUnitMeters).c_str(), (int)mesh_.surfaces.size(),
                       (int)(mesh_.vertexCount() / 3));

    const float width = (mesh_.boundsMax[0] - mesh_.boundsMin[0]) * scale_;
    const float height = (mesh_.boundsMax[1] - mesh_.boundsMin[1]) * scale_;
    const float depth = (mesh_.boundsMax[2] - mesh_.boundsMin[2]) * scale_;
    ImGui::Text("Size: %.2f m wide x %.2f m high x %.2f m deep", width, height, depth);

    ImGui::SetNextItemWidth(140);
    if (ImGui::DragFloat("Scale", &scale_, 0.01f, 0.0001f, 10000.0f, "%.4g",
                         ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
        scale_ = std::max(scale_, 0.0001f);
    }
    for (float factor : {0.01f, 0.1f, 10.0f, 100.0f}) {
        ImGui::SameLine();
        char label[16];
        std::snprintf(label, sizeof(label), "x%g", factor);
        if (ImGui::SmallButton(label)) scale_ = std::clamp(scale_ * factor, 0.0001f, 10000.0f);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) scale_ = 1.0f;

    drawSurfaceSection();
}

// Lists the surfaces with their type and, for LED surfaces, the size of their own UV01 canvas.
void App::drawSurfaceSection() {
    const bool open = ImGui::TreeNodeEx("Surfaces", mode_ != MappingMode::Projection ? ImGuiTreeNodeFlags_DefaultOpen : 0,
                                        "Surfaces (%d)", (int)mesh_.surfaces.size());
    if (!open) return;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("surfaces", 3, flags)) {
        ImGui::TableSetupColumn("Surface", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("UV01 mapping");
        ImGui::TableHeadersRow();
        for (int s = 0; s < (int)mesh_.surfaces.size(); ++s) {
            const Surface& surface = mesh_.surfaces[s];
            ImGui::PushID(s);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(surface.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%u triangles", surface.vertexCount / 3);

            ImGui::TableNextColumn();
            const bool led = isLedSurface(s);
            if (mode_ == MappingMode::Mixed) {
                if (ImGui::RadioButton("Proj.", !led)) setSurfaceLed(s, false);
                ImGui::SameLine();
                if (ImGui::RadioButton("LED", led)) setSurfaceLed(s, true);
            } else {
                ImGui::TextUnformatted(led ? "LED" : "Projection");
            }

            ImGui::TableNextColumn();
            if (led) {
                int width = 0, height = 0;
                canvasSize(0, s, width, height);
                if (width > 0) ImGui::Text("%s", sizeText(width, height).c_str());
                else ImGui::TextColored(kMutedColor, "No media");
            } else {
                ImGui::TextColored(kMutedColor, "Shared");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
}

void App::drawChannelSection() {
    if (mesh_.empty()) return;
    ImGui::SeparatorText("UV channels");

    for (size_t c = 0; c < mesh_.channels.size(); ++c) {
        const UvChannelInfo& info = mesh_.channels[c];
        ChannelState& state = channels_[c];
        if (info.duplicateOf < 0 || state.duplicateDecided) continue;
        ImGui::PushID((int)c);
        ImGui::TextColored(kWarningColor, "%s is an exact copy of %s.", info.name.c_str(),
                           mesh_.channels[info.duplicateOf].name.c_str());
        if (ImGui::Button("Keep")) state.duplicateDecided = true;
        ImGui::SameLine();
        if (ImGui::Button("Ignore")) {
            state.duplicateDecided = true;
            state.ignored = true;
        }
        ImGui::PopID();
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("channels", 5, flags)) return;
    ImGui::TableSetupColumn("UV");
    ImGui::TableSetupColumn("Name in file");
    ImGui::TableSetupColumn("In 0-1");
    ImGui::TableSetupColumn("Mapping", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Clamp");
    ImGui::TableHeadersRow();

    for (size_t c = 0; c < mesh_.channels.size(); ++c) {
        const UvChannelInfo& info = mesh_.channels[c];
        ChannelState& state = channels_[c];
        ImGui::PushID((int)c);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextColored(state.ignored ? kMutedColor : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s",
                           channelLabel((int)c).c_str());

        ImGui::TableNextColumn();
        ImGui::TextColored(state.ignored ? kMutedColor : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s",
                           info.name.c_str());
        if (info.surfaceCount < (int)mesh_.surfaces.size())
            ImGui::TextColored(kMutedColor, "on %d of %d surfaces", info.surfaceCount, (int)mesh_.surfaces.size());

        ImGui::TableNextColumn();
        ImGui::Text("%.0f%%", info.insideRatio * 100.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Share of the mesh with UVs inside 0-1.\nU %.3f to %.3f, V %.3f to %.3f", info.minU,
                              info.maxU, info.minV, info.maxV);

        ImGui::TableNextColumn();
        int layerCount = 0;
        for (const Layer& layer : layers_) layerCount += layer.channel == (int)c;
        if (state.ignored) {
            ImGui::TextColored(kMutedColor, "Ignored");
            ImGui::SameLine();
            if (ImGui::SmallButton("Restore")) state.ignored = false;
        } else if (c == 0 && anyLedSurface()) {
            int width = 0, height = 0;
            if (anyProjectionSurface()) canvasSize(0, -1, width, height);
            if (width > 0) ImGui::Text("Shared %s", sizeText(width, height).c_str());
            ImGui::Text("Per LED surface, see Surfaces");
            ImGui::TextColored(kMutedColor, "%d layer(s)", layerCount);
        } else {
            int width = 0, height = 0;
            canvasSize((int)c, -1, width, height);
            if (width > 0) ImGui::Text("%s", sizeText(width, height).c_str());
            else ImGui::TextColored(kMutedColor, "No media");
            ImGui::TextColored(kMutedColor, "%d layer(s)", layerCount);
        }

        ImGui::TableNextColumn();
        ImGui::Checkbox("##clamp", &state.clamp);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: content shows only inside the 0-1 UV square.");

        ImGui::PopID();
    }
    ImGui::EndTable();
}

bool App::channelCombo(const char* id, int& channel) {
    bool changed = false;
    const std::string preview = channel >= 0 && channel < (int)channels_.size() ? channelLabel(channel) : "-";
    if (ImGui::BeginCombo(id, preview.c_str())) {
        for (int c = 0; c < (int)channels_.size(); ++c) {
            if (!channelUsable(c)) continue;
            const std::string label = channelLabel(c) + "  " + mesh_.channels[c].name;
            if (ImGui::Selectable(label.c_str(), channel == c)) {
                channel = c;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool App::surfaceCombo(const char* id, int& surface) {
    bool changed = false;
    if (ImGui::BeginCombo(id, surfaceLabel(surface).c_str())) {
        if (ImGui::Selectable("All surfaces", surface < 0)) {
            surface = -1;
            changed = true;
        }
        for (int s = 0; s < (int)mesh_.surfaces.size(); ++s) {
            if (ImGui::Selectable(mesh_.surfaces[s].name.c_str(), surface == s)) {
                surface = s;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void App::drawLayersPanel() {
    ImGui::Begin(kLayersWindow);
    const bool canAdd = !mesh_.empty();
    ImGui::BeginDisabled(!canAdd);
    if (ImGui::Button("Add image...")) {
        const std::string path = openFileDialog(FileKind::Image);
        if (!path.empty()) importImage(path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Import folder...##layers")) {
        const std::string folder = pickFolderDialog();
        if (!folder.empty()) importFolder(folder);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add generator...")) ImGui::OpenPopup("generators");
    if (ImGui::BeginPopup("generators")) {
        ImGui::TextColored(kMutedColor, "Added on %s", placementLabel(uvChannel_, resolvedUvSurface() >= 0 ? resolvedUvSurface() : -1).c_str());
        for (int t = 0; t < (int)GeneratorType::Count; ++t) {
            if (ImGui::Selectable(generatorName((GeneratorType)t))) {
                const int surface = resolvedUvSurface();
                addGenerator((GeneratorType)t, uvChannel_, surface >= 0 ? surface : -1);
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Animated patterns such as Bugs, Chevron or Radar. They go on the mapping shown in the UV view.");
    ImGui::EndDisabled();

    int visibleCount = 0;
    for (const Layer& layer : layers_) visibleCount += layer.visible;
    if (visibleCount > kMaxLayers)
        ImGui::TextColored(kWarningColor, "%d visible layers: only the top %d are drawn.", visibleCount, kMaxLayers);

    drawLayerList();
    const int index = selectedIndex();
    if (index >= 0) drawLayerEditor(layers_[index], index);
    drawPendingSection();
    ImGui::End();
}

void App::drawLayerList() {
    ImGui::SeparatorText("Stack (top first, drag to reorder)");
    if (layers_.empty()) {
        ImGui::TextColored(kMutedColor, "No layers yet. Add images or import a folder.");
        return;
    }

    int moveFrom = -1, moveTo = -1;
    for (int i = (int)layers_.size() - 1; i >= 0; --i) {
        Layer& layer = layers_[i];
        ImGui::PushID(layer.id);
        ImGui::Checkbox("##visible", &layer.visible);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show or hide this layer");
        ImGui::SameLine();

        std::string label = layer.image.name + "\n" + placementLabel(layer.channel, layer.surface);
        char detail[64];
        std::snprintf(detail, sizeof(detail), " · %s · %.0f%%", blendModeName(layer.blend), layer.opacity * 100.0f);
        label += detail;
        const bool warn = !layerWarnings(layer).empty();
        if (warn) label += "  (!)";

        if (!layer.visible) ImGui::PushStyleColor(ImGuiCol_Text, kMutedColor);
        if (ImGui::Selectable(label.c_str(), selectedLayer_ == layer.id, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2)))
            selectedLayer_ = layer.id;
        if (!layer.visible) ImGui::PopStyleColor();

        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kLayerPayload, &i, sizeof(int));
            ImGui::Text("Move %s", layer.image.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kLayerPayload)) {
                moveFrom = *(const int*)payload->Data;
                moveTo = i;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    if (moveFrom >= 0) moveLayer(moveFrom, moveTo);
}

void App::drawLayerEditor(Layer& layer, int index) {
    ImGui::SeparatorText("Selected layer");
    ImGui::Text("%s", layer.image.name.c_str());
    ImGui::TextColored(kMutedColor, "%s", sizeText(layer.image.width, layer.image.height).c_str());

    const float fieldWidth = std::max(120.0f, ImGui::GetContentRegionAvail().x - 110.0f);
    ImGui::SetNextItemWidth(fieldWidth);
    channelCombo("Mapping", layer.channel);
    ImGui::SetNextItemWidth(fieldWidth);
    surfaceCombo("Surface", layer.surface);

    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::BeginCombo("Blend", blendModeName(layer.blend))) {
        for (int b = 0; b < (int)BlendMode::Count; ++b) {
            if (ImGui::Selectable(blendModeName((BlendMode)b), (int)layer.blend == b)) layer.blend = (BlendMode)b;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Mask: this layer's brightness cuts out everything below it.");

    ImGui::SetNextItemWidth(fieldWidth);
    float percent = layer.opacity * 100.0f;
    if (ImGui::SliderFloat("Opacity", &percent, 0.0f, 100.0f, "%.0f%%")) layer.opacity = percent / 100.0f;

    const double frame = 1.0 / fps_;
    ImGui::SetNextItemWidth(fieldWidth * 0.5f);
    if (ImGui::InputDouble("##in", &layer.inTime, frame, 1.0, "%.3f s"))
        layer.inTime = std::clamp(snapToFrame(layer.inTime), 0.0, layer.outTime - frame);
    ImGui::SameLine();
    if (ImGui::Button("In = playhead")) layer.inTime = std::min(time_, layer.outTime - frame);
    ImGui::SetNextItemWidth(fieldWidth * 0.5f);
    if (ImGui::InputDouble("##out", &layer.outTime, frame, 1.0, "%.3f s"))
        layer.outTime = std::clamp(snapToFrame(layer.outTime), layer.inTime + frame, length_);
    ImGui::SameLine();
    if (ImGui::Button("Out = playhead")) layer.outTime = std::max(time_, layer.inTime + frame);
    ImGui::TextColored(kMutedColor, "On the timeline from %s to %s", timecode(layer.inTime).c_str(),
                       timecode(layer.outTime).c_str());

    if (layer.generated) drawGeneratorEditor(layer);

    for (const std::string& warning : layerWarnings(layer)) ImGui::TextColored(kWarningColor, "%s", warning.c_str());

    if (ImGui::Button("Up") && index + 1 < (int)layers_.size()) moveLayer(index, index + 1);
    ImGui::SameLine();
    if (ImGui::Button("Down") && index > 0) moveLayer(index, index - 1);
    ImGui::SameLine();
    if (!layer.generated && ImGui::Button("Replace image...")) {
        const std::string path = openFileDialog(FileKind::Image);
        Image image;
        std::string error;
        if (!path.empty()) {
            if (loadImage(path, maxTextureSize_, image, error)) {
                Layer& target = layers_[selectedIndex()];
                freeImage(target.image);
                target.image = image;
            } else {
                log(fileNameOf(path) + ": " + error);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        removeLayer(selectedIndex());
        selectedLayer_ = -1;
    }
}

void App::drawPendingSection() {
    if (pending_.empty()) return;
    ImGui::SeparatorText("Unassigned files");
    for (size_t i = 0; i < pending_.size(); ++i) {
        PendingFile& file = pending_[i];
        ImGui::PushID((int)i);
        ImGui::Text("%s", fileNameOf(file.path).c_str());
        ImGui::TextColored(kWarningColor, "%s", file.reason.c_str());
        bool remove = false;
        if (!channels_.empty()) {
            ImGui::SetNextItemWidth(90);
            channelCombo("##channel", file.channel);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130);
            surfaceCombo("##surface", file.surface);
            ImGui::SameLine();
            ImGui::BeginDisabled(!channelUsable(file.channel) || (needsSurface(file.channel) && file.surface < 0));
            if (ImGui::Button("Assign")) remove = addLayer(file.path, file.channel, file.surface);
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        if (ImGui::Button("Remove")) remove = true;
        ImGui::PopID();
        if (remove) {
            pending_.erase(pending_.begin() + i);
            break;
        }
    }
}

void App::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(kViewportWindow);
    ImGui::PopStyleVar();

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 8, ImGui::GetCursorPosY() + 6));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Show");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    std::string preview = "Layers";
    if (view_.mode == ViewMode::MeshOnly) preview = "Mesh only";
    if (view_.mode == ViewMode::Distortion)
        preview = std::string(distortionStretch_ ? "Stretch" : "Pixel density") + " on " + channelLabel(uvChannel_);
    if (view_.mode == ViewMode::TestGrid && channelUsable(gridChannel_))
        preview = "Test grid on " + channelLabel(gridChannel_);
    if (ImGui::BeginCombo("##show", preview.c_str())) {
        if (ImGui::Selectable("Layers", view_.mode == ViewMode::Layers)) view_.mode = ViewMode::Layers;
        if (ImGui::Selectable("Mesh only", view_.mode == ViewMode::MeshOnly)) view_.mode = ViewMode::MeshOnly;
        if (ImGui::Selectable("Distortion: pixel density", view_.mode == ViewMode::Distortion && !distortionStretch_))
            showDistortion(false);
        if (ImGui::Selectable("Distortion: stretch", view_.mode == ViewMode::Distortion && distortionStretch_))
            showDistortion(true);
        for (int c = 0; c < (int)channels_.size(); ++c) {
            if (!channelUsable(c)) continue;
            const std::string label = "Test grid on " + channelLabel(c) + "  " + mesh_.channels[c].name;
            if (ImGui::Selectable(label.c_str(), view_.mode == ViewMode::TestGrid && gridChannel_ == c))
                showTestGrid(c);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &view_.wireframe);
    ImGui::SameLine();
    ImGui::Checkbox("Floor grid", &view_.grid);
    ImGui::SameLine();
    bool frameRequested = ImGui::Button("Frame (F)");
    ImGui::SameLine();
    ImGui::Checkbox("UV view", &showUvView_);
    ImGui::Dummy(ImVec2(0, 2));

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const int width = std::max(1, (int)size.x), height = std::max(1, (int)size.y);

    ImGui::InvisibleButton("viewport", ImVec2((float)width, (float)height),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActive()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) camera_.orbit(io.MouseDelta.x, io.MouseDelta.y);
        else camera_.pan(io.MouseDelta.x, io.MouseDelta.y, height);
    }
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0.0f) camera_.zoom(io.MouseWheel);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frameRequested = true;
    }
    if (frameRequested && !mesh_.empty()) {
        float scaledMin[3], scaledMax[3];
        for (int axis = 0; axis < 3; ++axis) {
            scaledMin[axis] = mesh_.boundsMin[axis] * scale_;
            scaledMax[axis] = mesh_.boundsMax[axis] * scale_;
        }
        camera_.frame(scaledMin, scaledMax);
    }

    view_.layers = renderLayers(-1);
    if (view_.mode == ViewMode::TestGrid && !channelUsable(gridChannel_)) view_.mode = ViewMode::Layers;
    view_.gridChannel = gridChannel_;
    view_.gridClamp = channelUsable(gridChannel_) ? channels_[gridChannel_].clamp : true;
    view_.scale = scale_;
    view_.ledSurfaces = ledFlags();
    if (view_.mode == ViewMode::Distortion) {
        if (!channelUsable(uvChannel_)) view_.mode = ViewMode::Layers;
        const DistortionFilter filter = distortionFilter();
        view_.distortion.channel = filter.channel;
        view_.distortion.stretch = distortionStretch_;
        view_.distortion.resolution[0] = (float)filter.width;
        view_.distortion.resolution[1] = (float)filter.height;
        view_.distortion.meanDensity = summarizeDistortion(mesh_, filter).meanDensity;
        view_.distortion.clamp = filter.clampInside;
        view_.distortion.surface = filter.surface;
    }
    renderer_.render(width, height, camera_, view_);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddImage((ImTextureID)(intptr_t)renderer_.colorTexture(), origin,
                       ImVec2(origin.x + width, origin.y + height), ImVec2(0, 1), ImVec2(1, 0));
    if (mesh_.empty()) {
        const char* hint = "Open an FBX or drop one here";
        const ImVec2 textSize = ImGui::CalcTextSize(hint);
        drawList->AddText(ImVec2(origin.x + (width - textSize.x) * 0.5f, origin.y + (height - textSize.y) * 0.5f),
                          IM_COL32(150, 155, 165, 255), hint);
    }
    drawList->AddText(ImVec2(origin.x + 10, origin.y + height - 24), IM_COL32(120, 125, 135, 255),
                      "Left drag: orbit   Right or middle drag: pan   Wheel: zoom   F: frame");
    if (view_.mode == ViewMode::Distortion) drawDistortionLegend(drawList, ImVec2(origin.x + 10, origin.y + 10), distortionStretch_);
    ImGui::End();
}

// Visible layers as the renderer takes them, bottom to top; past the limit the top ones win.
std::vector<RenderLayer> App::renderLayers(int onlyChannel) const {
    std::vector<RenderLayer> result;
    for (const Layer& layer : layers_) {
        if (!layer.visible || !layer.image.texture || !channelUsable(layer.channel)) continue;
        if (onlyChannel >= 0 && layer.channel != onlyChannel) continue;
        if (!layerActive(layer)) continue;
        RenderLayer render;
        render.texture = layer.image.texture;
        render.channel = layer.channel;
        render.surface = layer.surface >= 0 ? layer.surface
                         : layer.channel == 0 ? kProjectionSurfaces
                                              : kAllSurfaces;
        render.blend = layer.blend;
        render.opacity = layer.opacity;
        render.clamp = channels_[layer.channel].clamp;
        result.push_back(render);
    }
    if ((int)result.size() > kMaxLayers) result.erase(result.begin(), result.end() - kMaxLayers);
    return result;
}

std::vector<unsigned char> App::ledFlags() const {
    std::vector<unsigned char> flags(mesh_.surfaces.size(), 0);
    for (int s = 0; s < (int)flags.size(); ++s) flags[s] = isLedSurface(s);
    return flags;
}

void App::showUvView(int channel, int surface) {
    showUvView_ = true;
    uvChannel_ = channel;
    uvSurface_ = surface;
}

// The surface the UV view draws. UV01 on LED surfaces is one surface at a time,
// since each LED surface fills its own 0-1 square.
int App::resolvedUvSurface() const {
    if (uvChannel_ == 0 && anyLedSurface()) {
        if (isLedSurface(uvSurface_)) return uvSurface_;
        if (anyProjectionSurface()) return kProjectionSurfaces;
        for (int s = 0; s < (int)mesh_.surfaces.size(); ++s)
            if (isLedSurface(s)) return s;
    }
    if (uvSurface_ >= 0 && uvSurface_ < (int)mesh_.surfaces.size()) return uvSurface_;
    return kAllSurfaces;
}

// The output takes the mapping's size, set by its largest file.
void App::outputSize(int& width, int& height, std::string& note) const {
    const int surface = resolvedUvSurface();
    canvasSize(uvChannel_, surface >= 0 ? surface : -1, width, height);
    note.clear();
    if (width <= 0 || height <= 0) {
        width = height = 1024;
        note = "No media on this mapping yet; previewing at 1024 x 1024 px.";
    }
    if (width > maxTextureSize_ || height > maxTextureSize_) {
        const float shrink = (float)maxTextureSize_ / (float)std::max(width, height);
        note = "Mapping is " + sizeText(width, height) + ", over this GPU's limit; previewing smaller.";
        width = std::max(1, (int)(width * shrink));
        height = std::max(1, (int)(height * shrink));
    }
}

std::string App::outputName() const {
    std::string name = utf8String(toPath(mesh_.path).stem());
    if (name.empty()) name = "output";
    name += "_" + channelLabel(uvChannel_);
    const int surface = resolvedUvSurface();
    if (surface >= 0) name += "_" + surfaceLabel(surface);
    if (uvShow_ == UvShow::Mapping) name += "_mapping";
    if (uvShow_ == UvShow::Density) name += "_density";
    if (uvShow_ == UvShow::Stretch) name += "_stretch";
    for (char& c : name)
        if (c == ' ' || c == '/' || c == '\\' || c == ':') c = '_';
    return name + ".png";
}

void App::renderOutput(bool wireframe, int& width, int& height, std::string& note) {
    UvViewSettings settings;
    settings.channel = uvChannel_;
    settings.surface = resolvedUvSurface();
    outputSize(settings.width, settings.height, note);
    settings.layers = renderLayers(uvShow_ == UvShow::Mapping ? uvChannel_ : -1);
    settings.ledSurfaces = ledFlags();
    settings.wireframe = wireframe;
    settings.scale = scale_;
    if (uvShow_ == UvShow::Density || uvShow_ == UvShow::Stretch) {
        const DistortionFilter filter = distortionFilter();
        settings.showDistortion = true;
        settings.distortion.channel = filter.channel;
        settings.distortion.stretch = uvShow_ == UvShow::Stretch;
        settings.distortion.resolution[0] = (float)filter.width;
        settings.distortion.resolution[1] = (float)filter.height;
        settings.distortion.meanDensity = summarizeDistortion(mesh_, filter).meanDensity;
        settings.distortion.clamp = filter.clampInside;
        settings.distortion.surface = filter.surface;
    }
    renderer_.renderUv(settings);
    width = settings.width;
    height = settings.height;
}

bool App::exportOutputPng(const std::string& path) {
    if (mesh_.empty() || !channelUsable(uvChannel_)) {
        log("Nothing to export: open a mesh and pick a UV channel in the UV view.");
        return false;
    }
    // Rendered without the wireframe overlay, so the PNG is the output itself.
    updateGenerators();
    int width = 0, height = 0;
    std::string note;
    renderOutput(false, width, height, note);
    std::vector<unsigned char> pixels;
    if (!renderer_.readUvPixels(pixels, width, height) ||
        !stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3)) {
        log("Could not save " + path + ".");
        return false;
    }
    log("Exported " + fileNameOf(path) + " (" + sizeText(width, height) + ").");
    return true;
}

bool App::uvSurfaceCombo() {
    const int current = resolvedUvSurface();
    const bool perSurfaceUv01 = uvChannel_ == 0 && anyLedSurface();
    std::string preview = current == kProjectionSurfaces ? "Projection surfaces" : surfaceLabel(current);
    bool changed = false;
    if (ImGui::BeginCombo("##uvsurface", preview.c_str())) {
        if (perSurfaceUv01) {
            if (anyProjectionSurface() && ImGui::Selectable("Projection surfaces", current == kProjectionSurfaces)) {
                uvSurface_ = kProjectionSurfaces;
                changed = true;
            }
        } else if (ImGui::Selectable("All surfaces", current == kAllSurfaces)) {
            uvSurface_ = kAllSurfaces;
            changed = true;
        }
        for (int s = 0; s < (int)mesh_.surfaces.size(); ++s) {
            if (perSurfaceUv01 && !isLedSurface(s)) continue;
            if (ImGui::Selectable(mesh_.surfaces[s].name.c_str(), current == s)) {
                uvSurface_ = s;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void App::drawUvView() {
    if (!showUvView_) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin(kUvWindow, &showUvView_);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 8, ImGui::GetCursorPosY() + 6));
    if (mesh_.empty()) {
        ImGui::TextColored(kMutedColor, "Open a mesh to see its UV layouts.");
        ImGui::End();
        return;
    }
    if (!channelUsable(uvChannel_)) {
        uvChannel_ = 0;
        while (uvChannel_ < (int)channels_.size() && !channelUsable(uvChannel_)) ++uvChannel_;
    }

    ImGui::SetNextItemWidth(80);
    channelCombo("##uvchannel", uvChannel_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    uvSurfaceCombo();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    const char* showNames[] = {"Output (all layers)", "This mapping only", "Distortion: pixel density",
                               "Distortion: stretch"};
    if (ImGui::BeginCombo("##uvshow", showNames[(int)uvShow_])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(showNames[i], (int)uvShow_ == i)) {
                uvShow_ = (UvShow)i;
                if (uvShow_ == UvShow::Density || uvShow_ == UvShow::Stretch)
                    distortionStretch_ = uvShow_ == UvShow::Stretch;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Output: every layer combined, as this UV layout would receive it.\n"
                          "This mapping only: just the layers placed on this mapping.\n"
                          "Pixel density: pixels per metre against the average (green even, blue fewer, red more).\n"
                          "Stretch: how far pixels are from square (green square, red stretched 2x or more).");

    if (!channelUsable(uvChannel_)) {
        ImGui::TextColored(kMutedColor, "Every UV channel is ignored.");
        ImGui::End();
        return;
    }

    int outputWidth = 0, outputHeight = 0;
    std::string note;
    renderOutput(uvWireframe_, outputWidth, outputHeight, note);
    const bool showingDistortion = uvShow_ == UvShow::Density || uvShow_ == UvShow::Stretch;

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s  %s", channelLabel(uvChannel_).c_str(), sizeText(outputWidth, outputHeight).c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe##uv", &uvWireframe_);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) uvZoom_ = 0.0f;
    ImGui::SameLine();
    if (ImGui::Button("1:1")) {
        uvZoom_ = 1.0f;
        uvPan_[0] = uvPan_[1] = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Export PNG...")) {
        const std::string path = savePngDialog(outputName());
        if (!path.empty()) exportOutputPng(path);
    }
    if (!note.empty()) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
        ImGui::TextColored(kWarningColor, "%s", note.c_str());
    }
    ImGui::Dummy(ImVec2(0, 2));

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const float areaWidth = std::max(1.0f, area.x), areaHeight = std::max(1.0f, area.y);
    ImGui::InvisibleButton("uvcanvas", ImVec2(areaWidth, areaHeight),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    const float fitZoom = std::min((areaWidth - 24.0f) / outputWidth, (areaHeight - 24.0f) / outputHeight);
    if (uvZoom_ <= 0.0f) {
        uvPan_[0] = uvPan_[1] = 0.0f;
    }
    float zoom = uvZoom_ > 0.0f ? uvZoom_ : std::max(fitZoom, 0.01f);
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 center(origin.x + areaWidth * 0.5f, origin.y + areaHeight * 0.5f);
    if (ImGui::IsItemActive()) {
        uvZoom_ = zoom;
        uvPan_[0] += io.MouseDelta.x;
        uvPan_[1] += io.MouseDelta.y;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        // Zoom around the cursor.
        const float newZoom = std::clamp(zoom * std::pow(1.2f, io.MouseWheel), 0.01f, 64.0f);
        const float mx = io.MousePos.x - center.x - uvPan_[0], my = io.MousePos.y - center.y - uvPan_[1];
        uvPan_[0] -= mx * (newZoom / zoom - 1.0f);
        uvPan_[1] -= my * (newZoom / zoom - 1.0f);
        zoom = uvZoom_ = newZoom;
    }

    const float imageWidth = outputWidth * zoom, imageHeight = outputHeight * zoom;
    const ImVec2 imageMin(center.x + uvPan_[0] - imageWidth * 0.5f, center.y + uvPan_[1] - imageHeight * 0.5f);
    const ImVec2 imageMax(imageMin.x + imageWidth, imageMin.y + imageHeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(origin, ImVec2(origin.x + areaWidth, origin.y + areaHeight), true);
    drawList->AddRectFilled(origin, ImVec2(origin.x + areaWidth, origin.y + areaHeight), IM_COL32(28, 29, 33, 255));
    drawList->AddImage((ImTextureID)(intptr_t)renderer_.uvTexture(), imageMin, imageMax, ImVec2(0, 1), ImVec2(1, 0));
    drawList->AddRect(ImVec2(imageMin.x - 1, imageMin.y - 1), ImVec2(imageMax.x + 1, imageMax.y + 1),
                      IM_COL32(120, 125, 135, 255));
    if (hovered && io.MousePos.x >= imageMin.x && io.MousePos.x < imageMax.x && io.MousePos.y >= imageMin.y &&
        io.MousePos.y < imageMax.y) {
        const float u = (io.MousePos.x - imageMin.x) / imageWidth;
        const float v = 1.0f - (io.MousePos.y - imageMin.y) / imageHeight;
        char text[160];
        int used = std::snprintf(text, sizeof(text), "Pixel %d, %d   UV %.4f, %.4f", (int)(u * outputWidth),
                                 (int)((1.0f - v) * outputHeight), u, v);
        if (showingDistortion) {
            const DistortionFilter filter = distortionFilter();
            const long triangle = findTriangleAt(mesh_, filter, u, v);
            const DistortionSample sample =
                triangle >= 0 ? triangleDistortion(mesh_, (size_t)triangle, filter) : DistortionSample{};
            if (sample.valid && used > 0 && used < (int)sizeof(text))
                std::snprintf(text + used, sizeof(text) - used, "   %.0f px/m (pitch %.2f mm)   stretch %.2f",
                              sample.density, 1000.0f / sample.density, sample.stretch);
        }
        drawList->AddText(ImVec2(origin.x + 10, origin.y + areaHeight - 44), IM_COL32(200, 205, 215, 255), text);
    }
    if (showingDistortion) {
        drawDistortionLegend(drawList, ImVec2(origin.x + 10, origin.y + 10), uvShow_ == UvShow::Stretch);
        const DistortionSummary summary = summarizeDistortion(mesh_, distortionFilter());
        char text[200];
        if (summary.triangles > 0)
            std::snprintf(text, sizeof(text),
                          "Average %.0f px/m (pitch %.2f mm), range %.0f to %.0f px/m\nStretch average %.2f, worst %.2f",
                          summary.meanDensity, 1000.0f / summary.meanDensity, summary.minDensity, summary.maxDensity,
                          summary.meanStretch, std::min(summary.maxStretch, 999.0f));
        else
            std::snprintf(text, sizeof(text), "No part of the mesh uses this mapping inside 0-1.");
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 textPos(origin.x + 10, origin.y + 62);
        drawList->AddRectFilled(ImVec2(textPos.x - 6, textPos.y - 4),
                                ImVec2(textPos.x + textSize.x + 6, textPos.y + textSize.y + 4), IM_COL32(20, 21, 25, 210),
                                4.0f);
        drawList->AddText(textPos, IM_COL32(200, 205, 215, 255), text);
    }
    char zoomText[64];
    std::snprintf(zoomText, sizeof(zoomText), "Zoom %.0f%%   Wheel: zoom   Drag: pan", zoom * 100.0f);
    drawList->AddText(ImVec2(origin.x + 10, origin.y + areaHeight - 24), IM_COL32(120, 125, 135, 255), zoomText);
    drawList->PopClipRect();
    ImGui::End();
}

// What the distortion colours measure: the UV view's mapping at its output size.
DistortionFilter App::distortionFilter() const {
    static std::vector<unsigned char> flags;
    flags = ledFlags();
    DistortionFilter filter;
    filter.channel = std::clamp(uvChannel_, 0, std::max(0, (int)channels_.size() - 1));
    filter.surface = resolvedUvSurface();
    filter.ledSurfaces = &flags;
    filter.clampInside = channelUsable(filter.channel) ? channels_[filter.channel].clamp : true;
    std::string note;
    outputSize(filter.width, filter.height, note);
    filter.scale = scale_;
    return filter;
}

void App::drawDistortionLegend(ImDrawList* drawList, ImVec2 corner, bool stretch) const {
    const ImU32 blue = IM_COL32(51, 115, 255, 255), green = IM_COL32(64, 204, 89, 255);
    const ImU32 yellow = IM_COL32(242, 217, 51, 255), red = IM_COL32(255, 64, 51, 255);
    const float width = 220.0f, height = 12.0f;
    const ImVec2 a(corner.x, corner.y + 18.0f), mid(corner.x + width * 0.5f, corner.y + 18.0f + height);
    drawList->AddRectFilled(ImVec2(corner.x - 6, corner.y - 4), ImVec2(corner.x + width + 6, corner.y + 46),
                            IM_COL32(20, 21, 25, 210), 4.0f);
    if (stretch) {
        drawList->AddText(corner, IM_COL32(220, 222, 230, 255), "Stretch (pixel aspect)");
        drawList->AddRectFilledMultiColor(a, mid, green, yellow, yellow, green);
        drawList->AddRectFilledMultiColor(ImVec2(mid.x, a.y), ImVec2(a.x + width, mid.y), yellow, red, red, yellow);
        drawList->AddText(ImVec2(a.x, mid.y + 1), IM_COL32(180, 185, 195, 255), "1.0");
        drawList->AddText(ImVec2(mid.x - 12, mid.y + 1), IM_COL32(180, 185, 195, 255), "1.5");
        drawList->AddText(ImVec2(a.x + width - 30, mid.y + 1), IM_COL32(180, 185, 195, 255), "2.0+");
    } else {
        drawList->AddText(corner, IM_COL32(220, 222, 230, 255), "Pixel density vs average");
        drawList->AddRectFilledMultiColor(a, mid, blue, green, green, blue);
        drawList->AddRectFilledMultiColor(ImVec2(mid.x, a.y), ImVec2(a.x + width, mid.y), green, red, red, green);
        drawList->AddText(ImVec2(a.x, mid.y + 1), IM_COL32(180, 185, 195, 255), "half");
        drawList->AddText(ImVec2(mid.x - 14, mid.y + 1), IM_COL32(180, 185, 195, 255), "even");
        drawList->AddText(ImVec2(a.x + width - 44, mid.y + 1), IM_COL32(180, 185, 195, 255), "double");
    }
}

void App::showDistortion(bool stretch) {
    distortionStretch_ = stretch;
    view_.mode = ViewMode::Distortion;
    showUvView_ = true;
    uvShow_ = stretch ? UvShow::Stretch : UvShow::Density;
}

std::string App::generatorLabel(const Layer& layer) const {
    std::string label = placementLabel(layer.channel, layer.surface);
    for (char& c : label)
        if (c == ',') c = ' ';
    return label + "  " + std::to_string(layer.generator.width) + "X" + std::to_string(layer.generator.height);
}

void App::addGenerator(GeneratorType type, int channel, int surface) {
    if (mesh_.empty() || !channelUsable(channel)) {
        log("Open a mesh and pick a usable UV channel before adding a generator.");
        return;
    }
    if (needsSurface(channel) && surface < 0) {
        for (int s = 0; s < (int)mesh_.surfaces.size() && surface < 0; ++s)
            if (isLedSurface(s)) surface = s;
    }
    int width = 0, height = 0;
    canvasSize(channel, surface, width, height);
    if (width <= 0 || height <= 0) width = 1920, height = 1080;
    Layer layer;
    layer.id = nextLayerId_++;
    layer.generated = true;
    layer.channel = channel;
    layer.surface = surface;
    layer.inTime = 0.0;
    layer.outTime = length_;
    layer.generator = defaultGenerator(type, std::min(width, maxTextureSize_), std::min(height, maxTextureSize_));
    layer.generator.label = generatorLabel(layer);
    layer.image.name = generatorName(type);
    layer.image.width = layer.generator.width;
    layer.image.height = layer.generator.height;
    layers_.push_back(layer);
    selectedLayer_ = layer.id;
    log(std::string(generatorName(type)) + " -> " + placementLabel(channel, surface) + " (" +
        sizeText(layer.generator.width, layer.generator.height) + ")");
}

// Generators draw before the views, at their layer's own time (zero at its in point).
void App::updateGenerators() {
    for (Layer& layer : layers_) {
        if (!layer.generated) continue;
        layer.image.width = layer.generator.width;
        layer.image.height = layer.generator.height;
        if (!layer.visible || !layerActive(layer)) continue;
        generators_.render(layer.generator, time_ - layer.inTime, layer.target);
        layer.image.texture = layer.target.texture;
    }
}

void App::drawGeneratorEditor(Layer& layer) {
    GeneratorSettings& g = layer.generator;
    ImGui::SeparatorText("Generator");
    const float fieldWidth = std::max(120.0f, ImGui::GetContentRegionAvail().x - 110.0f);
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::BeginCombo("Pattern", generatorName(g.type))) {
        for (int t = 0; t < (int)GeneratorType::Count; ++t) {
            if (ImGui::Selectable(generatorName((GeneratorType)t), (int)g.type == t)) {
                const std::string label = g.label;
                g = defaultGenerator((GeneratorType)t, g.width, g.height);
                g.label = label;
                layer.image.name = generatorName(g.type);
            }
        }
        ImGui::EndCombo();
    }

    int size[2] = {g.width, g.height};
    ImGui::SetNextItemWidth(fieldWidth * 0.6f);
    if (ImGui::InputInt2("##gensize", size, ImGuiInputTextFlags_EnterReturnsTrue)) {
        g.width = std::clamp(size[0], 1, maxTextureSize_);
        g.height = std::clamp(size[1], 1, maxTextureSize_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Match mapping")) {
        int width = 0, height = 0;
        canvasSize(layer.channel, layer.surface, width, height);
        if (width > 0) {
            g.width = std::min(width, maxTextureSize_);
            g.height = std::min(height, maxTextureSize_);
        }
        g.label = generatorLabel(layer);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draw at the mapping's pixel size, set by its largest file.");

    ImGui::ColorEdit3("Colour", g.color, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::ColorEdit3("Background", g.background, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::Checkbox("See-through", &g.transparent);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave the background empty so the layers below show through.");

    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::SliderFloat("Speed", &g.speed, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    const GeneratorType type = g.type;
    const char* sizeLabel = type == GeneratorType::Bugs ? "Bug size (px)"
                            : type == GeneratorType::Chevron ? "Stripe (px)"
                            : type == GeneratorType::Scan ? "Line width (px)"
                            : type == GeneratorType::Radar ? "Ring step"
                            : type == GeneratorType::Noise ? "Scale"
                                                           : "Grid (px)";
    if (type != GeneratorType::Gradient && type != GeneratorType::Tiles) {
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::DragFloat(sizeLabel, &g.size, 0.5f, 1.0f, 2000.0f, "%.0f");
    }
    if (type == GeneratorType::Bugs) {
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::SliderFloat("Amount", &g.density, 0.0f, 1.0f, "%.2f");
    }
    if (type == GeneratorType::Bugs || type == GeneratorType::Radar || type == GeneratorType::Scan) {
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::SliderFloat("Trail", &g.trail, 0.0f, 1.0f, "%.2f");
    }
    if (type == GeneratorType::Chevron || type == GeneratorType::Scan || type == GeneratorType::Gradient) {
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::SliderFloat("Direction", &g.angle, -180.0f, 180.0f, "%.0f deg");
    }
    if (type == GeneratorType::Radar) {
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::SliderFloat2("Centre", g.center, 0.0f, 1.0f, "%.2f");
    }
    if (type == GeneratorType::Tiles) {
        int grid[2] = {g.columns, g.rows};
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::InputInt2("Columns, rows", grid)) {
            g.columns = std::clamp(grid[0], 1, 64);
            g.rows = std::clamp(grid[1], 1, 64);
        }
    }
    if (type == GeneratorType::Bugs || type == GeneratorType::Noise) {
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputInt("Seed", &g.seed);
    }
    if (type == GeneratorType::TestPattern) {
        char label[64];
        std::snprintf(label, sizeof(label), "%s", g.label.c_str());
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::InputText("Label", label, sizeof(label))) g.label = label;
    }
}

double App::snapToFrame(double seconds) const {
    return std::round(seconds * fps_) / fps_;
}

// Hours:minutes:seconds:frames, as on a media server timeline.
std::string App::timecode(double seconds) const {
    const long long frames = std::llround(std::max(seconds, 0.0) * fps_);
    const long long totalSeconds = frames / fps_;
    char text[64];
    std::snprintf(text, sizeof(text), "%02lld:%02lld:%02lld:%02lld", totalSeconds / 3600, totalSeconds / 60 % 60,
                  totalSeconds % 60, frames % fps_);
    return text;
}

// A layer shows from its in point up to, not including, its out point; a layer
// that runs to the end of the timeline also shows on the last frame.
bool App::layerActive(const Layer& layer) const {
    if (time_ < layer.inTime) return false;
    return time_ < layer.outTime || (layer.outTime >= length_ && time_ >= length_);
}

void App::setTime(double seconds) {
    time_ = std::clamp(seconds, 0.0, length_);
}

void App::setTimelineLength(double seconds) {
    const double frame = 1.0 / fps_;
    const double length = std::clamp(snapToFrame(seconds), frame, 24.0 * 3600.0);
    for (Layer& layer : layers_) {
        // Layers that ran to the old end keep running to the new end.
        if (layer.outTime >= length_ - frame * 0.5) layer.outTime = length;
        layer.outTime = std::min(layer.outTime, length);
        layer.inTime = std::clamp(layer.inTime, 0.0, layer.outTime - frame);
        layer.inTime = std::max(layer.inTime, 0.0);
    }
    length_ = length;
    time_ = std::min(time_, length_);
}

void App::advanceTime() {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !io.WantTextInput && !ImGui::IsAnyItemActive())
        playing_ = !playing_;
    if (!playing_) return;
    time_ += std::min((double)io.DeltaTime, 0.25);
    if (time_ >= length_) {
        if (loop_) {
            time_ = std::fmod(time_, length_);
        } else {
            time_ = length_;
            playing_ = false;
        }
    }
}

void App::drawTransport() {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
    if (ImGui::Button("|<")) time_ = 0.0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to start");
    ImGui::SameLine();
    if (ImGui::Button(playing_ ? "Pause" : "Play", ImVec2(60, 0))) {
        if (!playing_ && !loop_ && time_ >= length_) time_ = 0.0;
        playing_ = !playing_;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play or pause (Space)");
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        playing_ = false;
        time_ = 0.0;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop_);
    ImGui::SameLine(0, 18);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", timecode(time_).c_str());
    ImGui::SameLine();
    ImGui::TextColored(kMutedColor, "/ %s", timecode(length_).c_str());

    ImGui::SameLine(0, 18);
    ImGui::TextUnformatted("Length");
    ImGui::SameLine();
    double length = length_;
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputDouble("##length", &length, 1.0, 10.0, "%.2f s", ImGuiInputTextFlags_EnterReturnsTrue))
        setTimelineLength(length);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Timeline length in seconds. Press Enter to apply.");

    ImGui::SameLine();
    ImGui::TextUnformatted("FPS");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::BeginCombo("##fps", std::to_string(fps_).c_str())) {
        for (int rate : {24, 25, 30, 50, 60}) {
            if (ImGui::Selectable(std::to_string(rate).c_str(), fps_ == rate)) fps_ = rate;
        }
        ImGui::EndCombo();
    }
}

void App::drawTimeline() {
    ImGui::Begin(kTimelineWindow);
    drawTransport();
    ImGui::Spacing();

    const float labelWidth = 200.0f, rulerHeight = 24.0f, rowHeight = 26.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float trackX = origin.x + labelWidth;
    const float trackWidth = std::max(50.0f, avail.x - labelWidth - 12.0f);
    const auto timeToX = [&](double t) { return trackX + (float)(t / length_) * trackWidth; };
    const auto xToTime = [&](float x) { return std::clamp((double)(x - trackX) / trackWidth * length_, 0.0, length_); };
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Ruler: click or drag to move the playhead.
    ImGui::SetCursorScreenPos(ImVec2(trackX, origin.y));
    ImGui::InvisibleButton("ruler", ImVec2(trackWidth, rulerHeight));
    if (ImGui::IsItemActive()) time_ = snapToFrame(xToTime(io.MousePos.x));
    drawList->AddRectFilled(ImVec2(trackX, origin.y), ImVec2(trackX + trackWidth, origin.y + rulerHeight),
                            IM_COL32(36, 38, 44, 255));
    double step = 1.0 / fps_;
    for (double candidate : {1.0 / fps_, 0.1, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0,
                             3600.0}) {
        step = candidate;
        if (candidate / length_ * trackWidth >= 80.0f) break;
    }
    for (int i = 0; i * step <= length_ + 1e-9; ++i) {
        const double t = i * step;
        const float x = timeToX(t);
        drawList->AddLine(ImVec2(x, origin.y + rulerHeight - 8), ImVec2(x, origin.y + rulerHeight),
                          IM_COL32(140, 145, 155, 255));
        // Whole-second steps read as minutes:seconds, finer steps as seconds and frames.
        const std::string code = timecode(t);
        std::string label = code.substr(t >= 3600.0 ? 0 : 3, t >= 3600.0 ? 8 : 5);
        if (step < 1.0) {
            const long long frames = std::llround(t * fps_);
            label = std::to_string(frames / fps_) + "s " + std::to_string(frames % fps_) + "f";
        }
        if (x + 3 + ImGui::CalcTextSize(label.c_str()).x <= trackX + trackWidth)
            drawList->AddText(ImVec2(x + 3, origin.y + 3), IM_COL32(150, 155, 165, 255), label.c_str());
        for (int minor = 1; minor < 5; ++minor) {
            const double tm = t + step * minor / 5.0;
            if (tm > length_) break;
            const float xm = timeToX(tm);
            drawList->AddLine(ImVec2(xm, origin.y + rulerHeight - 4), ImVec2(xm, origin.y + rulerHeight),
                              IM_COL32(90, 95, 105, 255));
        }
    }

    // One row per layer, top of the stack first, like the Layers panel.
    const ImU32 channelColors[] = {IM_COL32(86, 130, 225, 255), IM_COL32(60, 175, 125, 255),
                                   IM_COL32(215, 150, 60, 255), IM_COL32(180, 95, 200, 255),
                                   IM_COL32(205, 85, 95, 255),  IM_COL32(70, 170, 190, 255),
                                   IM_COL32(170, 170, 80, 255), IM_COL32(140, 140, 150, 255)};
    float y = origin.y + rulerHeight + 4.0f;
    const double frame = 1.0 / fps_;
    for (int i = (int)layers_.size() - 1; i >= 0; --i) {
        Layer& layer = layers_[i];
        ImGui::PushID(layer.id);
        const bool selected = layer.id == selectedLayer_;

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 4, y));
        if (!layer.visible) ImGui::PushStyleColor(ImGuiCol_Text, kMutedColor);
        if (ImGui::Selectable(layer.image.name.c_str(), selected, 0, ImVec2(labelWidth - 12, rowHeight - 4)))
            selectedLayer_ = layer.id;
        if (!layer.visible) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", placementLabel(layer.channel, layer.surface).c_str());

        drawList->AddRectFilled(ImVec2(trackX, y), ImVec2(trackX + trackWidth, y + rowHeight - 2),
                                IM_COL32(30, 31, 36, 255));
        const float x0 = timeToX(layer.inTime), x1 = std::max(timeToX(layer.outTime), x0 + 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(trackX, y));
        ImGui::InvisibleButton("track", ImVec2(trackWidth, rowHeight - 2));
        const bool hovered = ImGui::IsItemHovered();
        const float edge = 6.0f;
        int hoverPart = 0;
        if (hovered && io.MousePos.x >= x0 - edge && io.MousePos.x <= x1 + edge) {
            if (io.MousePos.x <= x0 + edge) hoverPart = 1;
            else if (io.MousePos.x >= x1 - edge) hoverPart = 3;
            else hoverPart = 2;
        }
        if (hoverPart == 1 || hoverPart == 3 || (dragLayer_ == layer.id && dragPart_ != 2))
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) {
            selectedLayer_ = layer.id;
            if (hoverPart > 0) {
                dragLayer_ = layer.id;
                dragPart_ = hoverPart;
                dragOffset_ = xToTime(io.MousePos.x) - layer.inTime;
            }
        }
        if (dragLayer_ == layer.id && ImGui::IsItemActive()) {
            const double t = xToTime(io.MousePos.x);
            if (dragPart_ == 1) {
                layer.inTime = std::clamp(snapToFrame(t), 0.0, layer.outTime - frame);
            } else if (dragPart_ == 3) {
                layer.outTime = std::clamp(snapToFrame(t), layer.inTime + frame, length_);
            } else {
                const double duration = layer.outTime - layer.inTime;
                layer.inTime = std::clamp(snapToFrame(t - dragOffset_), 0.0, std::max(0.0, length_ - duration));
                layer.outTime = layer.inTime + duration;
            }
        }
        if (dragLayer_ == layer.id && !ImGui::IsItemActive()) dragLayer_ = -1;

        ImU32 color = channelColors[std::clamp(layer.channel, 0, 7)];
        if (!layer.visible) color = (color & 0x00FFFFFF) | 0x60000000;
        drawList->AddRectFilled(ImVec2(x0, y + 2), ImVec2(x1, y + rowHeight - 4), color, 3.0f);
        if (selected)
            drawList->AddRect(ImVec2(x0, y + 2), ImVec2(x1, y + rowHeight - 4), IM_COL32(240, 240, 245, 255), 3.0f,
                              0, 2.0f);
        const std::string barText = channelLabel(layer.channel) + "  " + layer.image.name;
        drawList->PushClipRect(ImVec2(x0 + 4, y), ImVec2(x1 - 4, y + rowHeight), true);
        drawList->AddText(ImVec2(x0 + 6, y + 5), IM_COL32(255, 255, 255, 230), barText.c_str());
        drawList->PopClipRect();
        ImGui::PopID();
        y += rowHeight;
    }
    if (layers_.empty()) {
        drawList->AddText(ImVec2(trackX + 8, y + 4), IM_COL32(120, 125, 135, 255),
                          "Layers appear here once you add images.");
        y += rowHeight;
    }

    // Playhead across the ruler and every row.
    const float px = timeToX(time_);
    drawList->AddLine(ImVec2(px, origin.y), ImVec2(px, std::max(y, origin.y + avail.y)), IM_COL32(235, 80, 70, 255),
                      2.0f);
    drawList->AddTriangleFilled(ImVec2(px - 6, origin.y), ImVec2(px + 6, origin.y), ImVec2(px, origin.y + 8),
                                IM_COL32(235, 80, 70, 255));
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::End();
}

void App::drawLog() {
    ImGui::Begin(kLogWindow);
    for (const std::string& line : log_) ImGui::TextWrapped("%s", line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::End();
}
