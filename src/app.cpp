#include "app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h>

#include "file_dialog.h"
#include "paths.h"

namespace {

const char* kProjectWindow = "Project";
const char* kLayersWindow = "Layers";
const char* kViewportWindow = "3D View";
const char* kLogWindow = "Log";
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

std::string sizeText(int width, int height) {
    return std::to_string(width) + " x " + std::to_string(height) + " px";
}

}  // namespace

bool App::init(int maxTextureSize) {
    maxTextureSize_ = maxTextureSize;
    if (!renderer_.init()) return false;
    log("GPU texture limit: " + std::to_string(maxTextureSize_) + " px per side.");
    return true;
}

void App::shutdown() {
    for (Layer& layer : layers_) freeImage(layer.image);
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

bool App::channelUsable(int channel) const {
    return channel >= 0 && channel < (int)channels_.size() && !channels_[channel].ignored;
}

bool App::needsSurface(int channel) const {
    return mode_ == MappingMode::Led && channel == 0;
}

int App::selectedIndex() const {
    for (int i = 0; i < (int)layers_.size(); ++i)
        if (layers_[i].id == selectedLayer_) return i;
    return -1;
}

int App::matchSurface(const std::string& fileName) const {
    const std::string name = normalizedName(fileName);
    int best = -1;
    size_t bestLength = 0;
    for (int s = 0; s < (int)mesh_.surfaces.size(); ++s) {
        const std::string surface = normalizedName(mesh_.surfaces[s].name);
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
    if (mode_ == MappingMode::Led) {
        log("LED mode: UV01 is per surface; UV02 and up are shared layouts.");
        for (Layer& layer : layers_)
            if (layer.channel == 0 && layer.surface < 0) layer.surface = matchSurface(layer.image.name);
    } else {
        log("Projection mode: every UV channel is shared by the whole mesh.");
    }
}

void App::showTestGrid(int channel) {
    gridChannel_ = channel;
    view_.mode = ViewMode::TestGrid;
}

// The mapping's canvas is set by the largest file on it. In LED mode UV01 has
// one canvas per surface.
void App::canvasSize(int channel, int surface, int& width, int& height) const {
    width = height = 0;
    const bool perSurface = needsSurface(channel);
    for (const Layer& layer : layers_) {
        if (layer.channel != channel || (perSurface && layer.surface != surface)) continue;
        if ((long long)layer.image.width * layer.image.height > (long long)width * height) {
            width = layer.image.width;
            height = layer.image.height;
        }
    }
}

std::vector<std::string> App::layerWarnings(const Layer& layer) const {
    std::vector<std::string> warnings;
    if (!channelUsable(layer.channel)) warnings.push_back(channelLabel(layer.channel) + " is ignored");
    if (needsSurface(layer.channel) && layer.surface < 0) warnings.push_back("UV01 in LED mode needs a surface");

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
    else if (needsSurface(channel) && (surface = matchSurface(name)) < 0)
        reason = "No surface name found in the file name (UV01 is per surface in LED mode)";

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
    layer.channel = channel;
    layer.surface = surface;
    layers_.push_back(layer);
    selectedLayer_ = layer.id;

    std::string placement = channelLabel(channel);
    if (surface >= 0) placement += ", " + surfaceLabel(surface);
    log(layer.image.name + " -> " + placement + " (" + sizeText(layer.image.width, layer.image.height) + ")");
    if (layers_.size() == (size_t)kMaxLayers + 1)
        log("Warning: more than " + std::to_string(kMaxLayers) + " visible layers; only the top " +
            std::to_string(kMaxLayers) + " are drawn.");
    return true;
}

void App::removeLayer(int index) {
    freeImage(layers_[index].image);
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
            ImGuiID left = 0, right = 0, bottom = 0, leftBottom = 0;
            ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.34f, &left, &right);
            ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.5f, &leftBottom, &left);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.18f, &bottom, &right);
            ImGui::DockBuilderDockWindow(kProjectWindow, left);
            ImGui::DockBuilderDockWindow(kLayersWindow, leftBottom);
            ImGui::DockBuilderDockWindow(kViewportWindow, right);
            ImGui::DockBuilderDockWindow(kLogWindow, bottom);
            ImGui::DockBuilderFinish(dockspace);
        }
    }

    drawProjectPanel();
    drawLayersPanel();
    drawViewport();
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

    if (mesh_.surfaces.size() > 1 && ImGui::TreeNode("Surfaces")) {
        for (const Surface& surface : mesh_.surfaces)
            ImGui::BulletText("%s (%u triangles)", surface.name.c_str(), surface.vertexCount / 3);
        ImGui::TreePop();
    }
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
        } else if (needsSurface((int)c)) {
            ImGui::Text("Per surface");
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

        std::string label = layer.image.name + "\n" + channelLabel(layer.channel);
        if (layer.surface >= 0) label += " · " + surfaceLabel(layer.surface);
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

    for (const std::string& warning : layerWarnings(layer)) ImGui::TextColored(kWarningColor, "%s", warning.c_str());

    if (ImGui::Button("Up") && index + 1 < (int)layers_.size()) moveLayer(index, index + 1);
    ImGui::SameLine();
    if (ImGui::Button("Down") && index > 0) moveLayer(index, index - 1);
    ImGui::SameLine();
    if (ImGui::Button("Replace image...")) {
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
    ImGui::SetNextItemWidth(230);
    std::string preview = "Layers";
    if (view_.mode == ViewMode::MeshOnly) preview = "Mesh only";
    if (view_.mode == ViewMode::TestGrid && channelUsable(gridChannel_))
        preview = "Test grid on " + channelLabel(gridChannel_);
    if (ImGui::BeginCombo("##show", preview.c_str())) {
        if (ImGui::Selectable("Layers", view_.mode == ViewMode::Layers)) view_.mode = ViewMode::Layers;
        if (ImGui::Selectable("Mesh only", view_.mode == ViewMode::MeshOnly)) view_.mode = ViewMode::MeshOnly;
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

    // The renderer takes visible layers bottom to top; past the limit the top ones win.
    view_.layers.clear();
    for (const Layer& layer : layers_) {
        if (!layer.visible || !layer.image.texture || !channelUsable(layer.channel)) continue;
        RenderLayer render;
        render.texture = layer.image.texture;
        render.channel = layer.channel;
        render.surface = layer.surface;
        render.blend = layer.blend;
        render.opacity = layer.opacity;
        render.clamp = channels_[layer.channel].clamp;
        view_.layers.push_back(render);
    }
    if ((int)view_.layers.size() > kMaxLayers)
        view_.layers.erase(view_.layers.begin(), view_.layers.end() - kMaxLayers);
    if (view_.mode == ViewMode::TestGrid && !channelUsable(gridChannel_)) view_.mode = ViewMode::Layers;
    view_.gridChannel = gridChannel_;
    view_.gridClamp = channelUsable(gridChannel_) ? channels_[gridChannel_].clamp : true;
    view_.scale = scale_;
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
    ImGui::End();
}

void App::drawLog() {
    ImGui::Begin(kLogWindow);
    for (const std::string& line : log_) ImGui::TextWrapped("%s", line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::End();
}
