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
const char* kViewportWindow = "3D View";
const char* kLogWindow = "Log";

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

}  // namespace

bool App::init(int maxTextureSize) {
    maxTextureSize_ = maxTextureSize;
    if (!renderer_.init()) return false;
    log("GPU texture limit: " + std::to_string(maxTextureSize_) + " px per side.");
    return true;
}

void App::shutdown() {
    for (ChannelState& channel : channels_) freeImage(channel.image);
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

bool App::channelUsable(int channel) const {
    return channel >= 0 && channel < (int)channels_.size() && !channels_[channel].ignored;
}

void App::openMesh(const std::string& path) {
    Mesh loaded;
    std::string error;
    if (!loaded.load(path, error)) {
        log("Could not open " + fileNameOf(path) + ": " + error);
        return;
    }

    for (ChannelState& channel : channels_) freeImage(channel.image);
    mesh_ = std::move(loaded);
    channels_.assign(mesh_.channels.size(), ChannelState{});
    renderer_.uploadMesh(mesh_);
    scale_ = 1.0f;
    camera_.frame(mesh_.boundsMin, mesh_.boundsMax);
    viewChannel_ = mesh_.channels.empty() ? -1 : 0;

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
    const std::filesystem::path fsPath = toPath(path);
    if (std::filesystem::is_directory(fsPath, ec)) {
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
    std::string reason;
    if (mesh_.empty()) reason = "Open a mesh first";
    else if (tag < 0) reason = "No UV tag in the name";
    else if (tag < 1 || tag > (int)channels_.size()) reason = "UV" + std::to_string(tag) + " is not on this mesh";
    else if (channels_[tag - 1].ignored) reason = channelLabel(tag - 1) + " is ignored";

    if (!reason.empty()) {
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&](const PendingFile& p) { return p.path == path; }),
                       pending_.end());
        pending_.push_back({path, reason, 0});
        log(name + ": " + reason + ". Pick a channel under Unassigned files.");
        return;
    }
    assignImage(path, tag - 1);
}

void App::assignImage(const std::string& path, int channel) {
    Image image;
    std::string error;
    if (!loadImage(path, maxTextureSize_, image, error)) {
        log(fileNameOf(path) + ": " + error);
        return;
    }
    ChannelState& state = channels_[channel];
    if (state.image.texture)
        log(image.name + " replaces " + state.image.name + " on " + channelLabel(channel) +
            " (several files per channel arrive with layers in milestone 2).");
    freeImage(state.image);
    state.image = image;
    log(image.name + " -> " + channelLabel(channel) + " (" + std::to_string(image.width) + " x " +
        std::to_string(image.height) + " px)");
    if (viewChannel_ < 0) viewChannel_ = channel;
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
            ImGuiID left = 0, right = 0, bottom = 0;
            ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.34f, &left, &right);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.18f, &bottom, &right);
            ImGui::DockBuilderDockWindow(kProjectWindow, left);
            ImGui::DockBuilderDockWindow(kViewportWindow, right);
            ImGui::DockBuilderDockWindow(kLogWindow, bottom);
            ImGui::DockBuilderFinish(dockspace);
        }
    }

    drawProjectPanel();
    drawViewport();
    drawLog();
}

void App::drawProjectPanel() {
    ImGui::Begin(kProjectWindow);
    drawMeshSection();
    ImGui::Spacing();
    drawChannelSection();
    drawPendingSection();
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
            freeImage(state.image);
            if (viewChannel_ == (int)c) viewChannel_ = info.duplicateOf;
        }
        ImGui::PopID();
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("channels", 5, flags)) return;
    ImGui::TableSetupColumn("UV");
    ImGui::TableSetupColumn("Name in file");
    ImGui::TableSetupColumn("In 0-1");
    ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Clamp");
    ImGui::TableHeadersRow();

    for (size_t c = 0; c < mesh_.channels.size(); ++c) {
        const UvChannelInfo& info = mesh_.channels[c];
        ChannelState& state = channels_[c];
        ImGui::PushID((int)c);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        const bool selected = viewChannel_ == (int)c;
        if (state.ignored) ImGui::TextColored(kMutedColor, "%s", channelLabel((int)c).c_str());
        else if (ImGui::Selectable(channelLabel((int)c).c_str(), selected)) viewChannel_ = (int)c;

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
        if (state.ignored) {
            ImGui::TextColored(kMutedColor, "Ignored");
            ImGui::SameLine();
            if (ImGui::SmallButton("Restore")) state.ignored = false;
        } else {
            if (state.image.texture) {
                ImGui::Text("%s", state.image.name.c_str());
                ImGui::TextColored(kMutedColor, "%d x %d px", state.image.width, state.image.height);
            } else {
                ImGui::TextColored(kMutedColor, "No image (test grid)");
            }
            if (ImGui::SmallButton("Load...")) {
                const std::string path = openFileDialog(FileKind::Image);
                if (!path.empty()) assignImage(path, (int)c);
            }
            if (state.image.texture) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) freeImage(state.image);
            }
        }

        ImGui::TableNextColumn();
        ImGui::Checkbox("##clamp", &state.clamp);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: content shows only inside the 0-1 UV square.");

        ImGui::PopID();
    }
    ImGui::EndTable();
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
            ImGui::SetNextItemWidth(110);
            const std::string preview = channelLabel(file.choice);
            if (ImGui::BeginCombo("##channel", preview.c_str())) {
                for (int c = 0; c < (int)channels_.size(); ++c) {
                    if (!channelUsable(c)) continue;
                    if (ImGui::Selectable(channelLabel(c).c_str(), file.choice == c)) file.choice = c;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Assign") && channelUsable(file.choice)) {
                assignImage(file.path, file.choice);
                remove = true;
            }
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
    ImGui::SetNextItemWidth(200);
    const std::string preview = channelUsable(viewChannel_)
                                    ? channelLabel(viewChannel_) + "  " + mesh_.channels[viewChannel_].name
                                    : std::string("Mesh only");
    if (ImGui::BeginCombo("##show", preview.c_str())) {
        if (ImGui::Selectable("Mesh only", viewChannel_ < 0)) viewChannel_ = -1;
        for (int c = 0; c < (int)channels_.size(); ++c) {
            if (!channelUsable(c)) continue;
            const std::string label = channelLabel(c) + "  " + mesh_.channels[c].name;
            if (ImGui::Selectable(label.c_str(), viewChannel_ == c)) viewChannel_ = c;
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

    view_.scale = scale_;
    view_.channel = channelUsable(viewChannel_) ? viewChannel_ : -1;
    view_.texture = view_.channel >= 0 ? channels_[view_.channel].image.texture : 0;
    view_.clamp = view_.channel >= 0 ? channels_[view_.channel].clamp : true;
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
