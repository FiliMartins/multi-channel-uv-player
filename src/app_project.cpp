// Project files: save and open the whole setup, relink moved media, and ask
// about unsaved changes before New, Open or Quit.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "app.h"
#include "file_dialog.h"
#include "paths.h"

// ordered_json keeps the keys in the order written, so the file reads top to bottom.
using json = nlohmann::ordered_json;

namespace {

constexpr int kProjectVersion = 1;
constexpr const char* kAppName = "Multi-Channel UV Player";
const ImVec4 kWarningColor(0.95f, 0.75f, 0.30f, 1.0f);
const ImVec4 kMutedColor(0.55f, 0.57f, 0.62f, 1.0f);

enum PendingAction { kNone = 0, kNew, kOpen, kQuit };

std::string lowerText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(toPath(path), ec);
}

std::string absolutePath(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(toPath(path), ec);
    return ec ? path : utf8String(absolute.lexically_normal());
}

// `path` relative to the project's folder with forward slashes, or empty when there is
// no such path (another drive on Windows).
std::string relativeTo(const std::string& path, const std::string& projectPath) {
    if (path.empty() || projectPath.empty()) return {};
    std::error_code ec;
    const std::filesystem::path relative =
        std::filesystem::relative(toPath(absolutePath(path)), toPath(absolutePath(projectPath)).parent_path(), ec);
    if (ec || relative.empty()) return {};
    const std::u8string text = relative.generic_u8string();
    return std::string(text.begin(), text.end());
}

// File names found under a folder (lower case) mapped to their first full path.
std::map<std::string, std::string> indexFolder(const std::string& folder) {
    std::map<std::string, std::string> index;
    if (folder.empty()) return index;
    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(
        toPath(folder), std::filesystem::directory_options::skip_permission_denied, ec);
    int visited = 0;
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (++visited > 200000) break;  // a whole drive picked by mistake
        if (!it->is_regular_file(ec)) continue;
        const std::string path = utf8String(it->path());
        index.emplace(lowerText(fileNameOf(path)), path);
    }
    return index;
}

// Where a saved file is now: its saved path, next to the project at the saved relative
// path, or anywhere under the search folder by name.
std::string locate(const std::string& absolute, const std::string& relative, const std::string& projectPath,
                   const std::map<std::string, std::string>& searchIndex) {
    if (fileExists(absolute)) return absolute;
    if (!relative.empty() && !projectPath.empty()) {
        const std::string candidate = utf8String(toPath(projectPath).parent_path() / toPath(relative));
        if (fileExists(candidate)) return candidate;
    }
    const std::string name = lowerText(fileNameOf(absolute.empty() ? relative : absolute));
    const auto found = searchIndex.find(name);
    return found != searchIndex.end() ? found->second : std::string();
}

const char* modeName(MappingMode mode) {
    return mode == MappingMode::Led ? "led" : mode == MappingMode::Mixed ? "mixed" : "projection";
}

const char* viewModeName(ViewMode mode) {
    switch (mode) {
        case ViewMode::MeshOnly: return "mesh";
        case ViewMode::TestGrid: return "grid";
        case ViewMode::Distortion: return "distortion";
        default: return "layers";
    }
}

ViewMode viewModeFromName(const std::string& name) {
    if (name == "mesh") return ViewMode::MeshOnly;
    if (name == "grid") return ViewMode::TestGrid;
    if (name == "distortion") return ViewMode::Distortion;
    return ViewMode::Layers;
}

template <typename T>
T read(const json& object, const char* key, T fallback) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const json::exception&) {
        return fallback;
    }
}

void readFloats(const json& object, const char* key, float* out, int count) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array()) return;
    for (int i = 0; i < count && i < (int)it->size(); ++i)
        if ((*it)[i].is_number()) out[i] = (*it)[i].get<float>();
}

json generatorJson(const GeneratorSettings& g) {
    return {{"type", generatorName(g.type)},
            {"width", g.width},
            {"height", g.height},
            {"color", {g.color[0], g.color[1], g.color[2]}},
            {"background", {g.background[0], g.background[1], g.background[2]}},
            {"transparent", g.transparent},
            {"speed", g.speed},
            {"size", g.size},
            {"density", g.density},
            {"trail", g.trail},
            {"angle", g.angle},
            {"center", {g.center[0], g.center[1]}},
            {"columns", g.columns},
            {"rows", g.rows},
            {"seed", g.seed}};
}

GeneratorSettings generatorFromJson(const json& object) {
    GeneratorType type = GeneratorType::TestPattern;
    const std::string name = read<std::string>(object, "type", "");
    for (int t = 0; t < (int)GeneratorType::Count; ++t)
        if (name == generatorName((GeneratorType)t)) type = (GeneratorType)t;
    GeneratorSettings g = defaultGenerator(type, read(object, "width", 1920), read(object, "height", 1080));
    g.width = std::max(1, g.width);
    g.height = std::max(1, g.height);
    readFloats(object, "color", g.color, 3);
    readFloats(object, "background", g.background, 3);
    readFloats(object, "center", g.center, 2);
    g.transparent = read(object, "transparent", g.transparent);
    g.speed = read(object, "speed", g.speed);
    g.size = read(object, "size", g.size);
    g.density = read(object, "density", g.density);
    g.trail = read(object, "trail", g.trail);
    g.angle = read(object, "angle", g.angle);
    g.columns = read(object, "columns", g.columns);
    g.rows = read(object, "rows", g.rows);
    g.seed = read(object, "seed", g.seed);
    return g;
}

}  // namespace

// The project as JSON. For the unsaved-changes check, things that change just by
// looking around (playhead, camera) are left out.
std::string App::serializeProject(bool forChangeCheck) const {
    json project;
    project["app"] = kAppName;
    project["version"] = kProjectVersion;
    if (!mesh_.empty()) {
        project["mesh"] = {{"path", absolutePath(mesh_.path)}, {"relative", relativeTo(mesh_.path, projectPath_)}, {"scale", scale_}};
    }
    project["mode"] = modeName(mode_);
    json led = json::array();
    for (size_t s = 0; s < mixedLed_.size() && s < mesh_.surfaces.size(); ++s)
        if (mixedLed_[s]) led.push_back(mesh_.surfaces[s].name);
    project["mixedLedSurfaces"] = led;
    json channels = json::array();
    for (const ChannelState& channel : channels_)
        channels.push_back({{"clamp", channel.clamp}, {"ignored", channel.ignored}, {"decided", channel.duplicateDecided}});
    project["channels"] = channels;

    json timeline = {{"length", length_}, {"fps", fps_}, {"loop", loop_}};
    if (!forChangeCheck) timeline["time"] = time_;
    project["timeline"] = timeline;

    json view = {{"mode", viewModeName(view_.mode)},
                 {"wireframe", view_.wireframe},
                 {"grid", view_.grid},
                 {"gridChannel", gridChannel_},
                 {"distortionStretch", distortionStretch_}};
    if (!forChangeCheck)
        view["camera"] = {{"target", {camera_.target[0], camera_.target[1], camera_.target[2]}},
                          {"distance", camera_.distance},
                          {"yaw", camera_.yaw},
                          {"pitch", camera_.pitch}};
    project["view3d"] = view;

    const char* uvShowNames[] = {"output", "mapping", "density", "stretch"};
    json uv = {{"show", showUvView_},
               {"channel", uvChannel_},
               {"display", uvShowNames[(int)uvShow_]},
               {"wireframe", uvWireframe_}};
    if (uvSurface_ >= 0 && uvSurface_ < (int)mesh_.surfaces.size()) uv["surface"] = mesh_.surfaces[uvSurface_].name;
    else uv["surface"] = uvSurface_ == kProjectionSurfaces ? "projection" : "all";
    project["uvView"] = uv;

    json layers = json::array();
    for (const Layer& layer : layers_) {
        json entry = {{"name", layer.image.name},
                      {"channel", layer.channel},
                      {"blend", blendModeName(layer.blend)},
                      {"opacity", layer.opacity},
                      {"visible", layer.visible},
                      {"in", layer.inTime},
                      {"out", layer.outTime}};
        if (layer.surface >= 0 && layer.surface < (int)mesh_.surfaces.size())
            entry["surface"] = mesh_.surfaces[layer.surface].name;
        else
            entry["surface"] = nullptr;
        if (layer.generated) {
            entry["generator"] = generatorJson(layer.generator);
            entry["followMapping"] = layer.followMapping;
        } else {
            entry["path"] = absolutePath(layer.image.path);
            entry["relative"] = relativeTo(layer.image.path, projectPath_);
            entry["width"] = layer.image.width;
            entry["height"] = layer.image.height;
        }
        layers.push_back(entry);
    }
    project["layers"] = layers;
    return project.dump(2);
}

bool App::saveProject(const std::string& path) {
    const std::string previous = projectPath_;
    projectPath_ = path;  // relative paths are measured from the new location
    const std::string text = serializeProject(false);
    std::ofstream file(toPath(path), std::ios::binary | std::ios::trunc);
    file << text << '\n';
    file.close();
    if (!file) {
        projectPath_ = previous;
        log("Could not save the project to " + path + ".");
        return false;
    }
    savedState_ = serializeProject(true);
    dirtyCached_ = false;
    log("Saved project " + fileNameOf(path) + ".");
    return true;
}

bool App::openProject(const std::string& path, const std::string& searchFolder) {
    json project;
    try {
        std::ifstream file(toPath(path), std::ios::binary);
        if (!file) {
            log("Could not open project " + fileNameOf(path) + ".");
            return false;
        }
        project = json::parse(file);
    } catch (const json::exception& error) {
        log("Could not read project " + fileNameOf(path) + ": " + error.what());
        return false;
    }
    if (!project.is_object() || read<std::string>(project, "app", "") != kAppName) {
        log(fileNameOf(path) + " is not a project of this player.");
        return false;
    }
    if (read(project, "version", 0) > kProjectVersion)
        log("Warning: " + fileNameOf(path) + " was saved by a newer version; some settings may be lost.");

    const std::map<std::string, std::string> searchIndex = indexFolder(searchFolder);
    const json meshEntry = project.value("mesh", json::object());
    std::string meshPath;
    if (!meshEntry.empty()) {
        meshPath = locate(read<std::string>(meshEntry, "path", ""), read<std::string>(meshEntry, "relative", ""), path,
                          searchIndex);
        if (meshPath.empty()) {
            pendingProjectPath_ = path;
            missingMeshName_ = fileNameOf(read<std::string>(meshEntry, "path", ""));
            log("The mesh " + missingMeshName_ + " was not found. Locate the folder it is in.");
            return false;
        }
    }
    pendingProjectPath_.clear();
    missingMeshName_.clear();

    clearProject();
    projectPath_ = path;
    if (!meshPath.empty()) {
        openMesh(meshPath);
        if (mesh_.empty()) return false;
        scale_ = std::clamp(read(meshEntry, "scale", 1.0f), 0.0001f, 10000.0f);
    }

    auto surfaceByName = [&](const json& value) {
        if (!value.is_string()) return -1;
        const std::string name = value.get<std::string>();
        for (int s = 0; s < (int)mesh_.surfaces.size(); ++s)
            if (mesh_.surfaces[s].name == name) return s;
        return -1;
    };

    const std::string mode = read<std::string>(project, "mode", "projection");
    mode_ = mode == "led" ? MappingMode::Led : mode == "mixed" ? MappingMode::Mixed : MappingMode::Projection;
    mixedLed_.assign(mesh_.surfaces.size(), 0);
    for (const json& name : project.value("mixedLedSurfaces", json::array())) {
        const int s = surfaceByName(name);
        if (s >= 0) mixedLed_[s] = 1;
    }
    const json channels = project.value("channels", json::array());
    for (size_t c = 0; c < channels.size() && c < channels_.size(); ++c) {
        channels_[c].clamp = read(channels[c], "clamp", true);
        channels_[c].ignored = read(channels[c], "ignored", false);
        channels_[c].duplicateDecided = read(channels[c], "decided", false);
    }

    const json timeline = project.value("timeline", json::object());
    length_ = std::clamp(read(timeline, "length", 60.0), 1.0, 24.0 * 3600.0);
    fps_ = std::clamp(read(timeline, "fps", 30), 1, 240);
    loop_ = read(timeline, "loop", true);
    time_ = std::clamp(read(timeline, "time", 0.0), 0.0, length_);

    const json view = project.value("view3d", json::object());
    view_.mode = viewModeFromName(read<std::string>(view, "mode", "layers"));
    view_.wireframe = read(view, "wireframe", false);
    view_.grid = read(view, "grid", true);
    gridChannel_ = std::clamp(read(view, "gridChannel", 0), 0, std::max(0, (int)channels_.size() - 1));
    distortionStretch_ = read(view, "distortionStretch", false);
    if (view.contains("camera")) {
        const json& camera = view["camera"];
        readFloats(camera, "target", camera_.target, 3);
        camera_.distance = read(camera, "distance", camera_.distance);
        camera_.yaw = read(camera, "yaw", camera_.yaw);
        camera_.pitch = read(camera, "pitch", camera_.pitch);
    }

    const json uv = project.value("uvView", json::object());
    showUvView_ = read(uv, "show", true);
    uvChannel_ = std::clamp(read(uv, "channel", 0), 0, std::max(0, (int)channels_.size() - 1));
    const std::string display = read<std::string>(uv, "display", "output");
    uvShow_ = display == "mapping" ? UvShow::Mapping
              : display == "density" ? UvShow::Density
              : display == "stretch" ? UvShow::Stretch
                                     : UvShow::Output;
    uvWireframe_ = read(uv, "wireframe", false);
    const json surface = uv.value("surface", json("all"));
    uvSurface_ = surface == "projection" ? kProjectionSurfaces : surface == "all" ? kAllSurfaces : surfaceByName(surface);

    int missing = 0;
    for (const json& entry : project.value("layers", json::array())) {
        Layer layer;
        layer.id = nextLayerId_++;
        layer.channel = std::max(0, read(entry, "channel", 0));
        layer.surface = surfaceByName(entry.value("surface", json()));
        const std::string blend = read<std::string>(entry, "blend", "Normal");
        for (int b = 0; b < (int)BlendMode::Count; ++b)
            if (blend == blendModeName((BlendMode)b)) layer.blend = (BlendMode)b;
        layer.opacity = std::clamp(read(entry, "opacity", 1.0f), 0.0f, 1.0f);
        layer.visible = read(entry, "visible", true);
        layer.inTime = std::clamp(read(entry, "in", 0.0), 0.0, length_);
        layer.outTime = std::clamp(read(entry, "out", length_), layer.inTime, length_);
        if (entry.contains("generator")) {
            layer.generated = true;
            layer.generator = generatorFromJson(entry["generator"]);
            layer.followMapping = read(entry, "followMapping", true);
            layer.image.name = generatorName(layer.generator.type);
            layer.image.width = layer.generator.width;
            layer.image.height = layer.generator.height;
            layer.generator.label = generatorLabel(layer);
        } else {
            const std::string saved = read<std::string>(entry, "path", "");
            const std::string found = locate(saved, read<std::string>(entry, "relative", ""), path, searchIndex);
            std::string error;
            if (found.empty() || !loadImage(found, maxTextureSize_, layer.image, error)) {
                layer.missing = true;
                layer.image = Image{};
                layer.image.path = saved;
                layer.image.name = read<std::string>(entry, "name", fileNameOf(saved));
                layer.image.width = read(entry, "width", 0);
                layer.image.height = read(entry, "height", 0);
                if (!error.empty()) log(layer.image.name + ": " + error);
                ++missing;
            }
        }
        layers_.push_back(layer);
    }
    if (!layers_.empty()) selectedLayer_ = layers_.back().id;

    savedState_ = serializeProject(true);
    dirtyCached_ = false;
    log("Opened project " + fileNameOf(path) + ": " + std::to_string(layers_.size()) + " layer(s).");
    if (missing > 0)
        log(std::to_string(missing) + " file(s) were not found. Use Locate folder under Missing files to relink them.");
    return true;
}

// Looks for every missing file under `folder` (and its subfolders) by name.
void App::relinkMissing(const std::string& folder) {
    const std::map<std::string, std::string> index = indexFolder(folder);
    int found = 0, still = 0;
    for (Layer& layer : layers_) {
        if (!layer.missing) continue;
        const auto it = index.find(lowerText(layer.image.name));
        Image image;
        std::string error;
        if (it != index.end() && loadImage(it->second, maxTextureSize_, image, error)) {
            layer.image = image;
            layer.missing = false;
            ++found;
        } else {
            if (!error.empty()) log(layer.image.name + ": " + error);
            ++still;
        }
    }
    log("Relinked " + std::to_string(found) + " file(s) from " + fileNameOf(folder) + "." +
        (still > 0 ? " " + std::to_string(still) + " still missing." : ""));
}

void App::clearProject() {
    for (int i = (int)layers_.size() - 1; i >= 0; --i) removeLayer(i);
    pending_.clear();
    mesh_ = Mesh{};
    renderer_.uploadMesh(mesh_);
    channels_.clear();
    mixedLed_.clear();
    mode_ = MappingMode::Projection;
    scale_ = 1.0f;
    gridChannel_ = 0;
    selectedLayer_ = -1;
    view_ = ViewSettings{};
    camera_ = OrbitCamera{};
    length_ = 60.0;
    fps_ = 30;
    time_ = 0.0;
    playing_ = false;
    loop_ = true;
    showUvView_ = true;
    uvChannel_ = 0;
    uvSurface_ = kAllSurfaces;
    uvShow_ = UvShow::Output;
    distortionStretch_ = false;
    uvWireframe_ = false;
    uvZoom_ = 0.0f;
    uvPan_[0] = uvPan_[1] = 0.0f;
    projectPath_.clear();
}

void App::newProject() {
    clearProject();
    savedState_ = serializeProject(true);
    dirtyCached_ = false;
    log("New project.");
}

bool App::isDirty() const {
    if (++dirtyCheckFrame_ >= 30) {
        dirtyCheckFrame_ = 0;
        dirtyCached_ = serializeProject(true) != savedState_;
    }
    return dirtyCached_;
}

std::string App::windowTitle() const {
    std::string name = projectPath_.empty() ? "Untitled" : fileNameOf(projectPath_);
    if (isDirty()) name += " *";
    return name + " - " + kAppName;
}

bool App::requestClose() {
    if (quitConfirmed_) return true;
    dirtyCheckFrame_ = 30;  // check now, not on the next cached frame
    if (!isDirty()) return true;
    runAfterSaveCheck(kQuit);
    return false;
}

// Saves to the project's file, or asks where when it has none yet.
bool App::saveOrSaveAs() {
    std::string path = projectPath_;
    if (path.empty()) {
        const std::string suggested = mesh_.empty() ? "Untitled.mcuv" : utf8String(toPath(fileNameOf(mesh_.path)).stem()) + ".mcuv";
        path = saveProjectDialog(suggested);
    }
    return !path.empty() && saveProject(path);
}

void App::runAfterSaveCheck(int action) {
    pendingAction_ = action;
    dirtyCheckFrame_ = 30;
    if (!isDirty()) doPendingAction();  // otherwise drawProjectPrompts asks first
}

void App::requestOpenProject(const std::string& path) {
    pendingOpenPath_ = path;
    runAfterSaveCheck(kOpen);
}

void App::doPendingAction() {
    const int action = pendingAction_;
    pendingAction_ = kNone;
    if (action == kNew) {
        newProject();
    } else if (action == kOpen) {
        if (!pendingOpenPath_.empty()) openProject(pendingOpenPath_);
        pendingOpenPath_.clear();
    } else if (action == kQuit) {
        quitConfirmed_ = true;
    }
}

void App::handleShortcuts() {
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) runAfterSaveCheck(kNew);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) {
        const std::string path = openFileDialog(FileKind::Project);
        if (!path.empty()) requestOpenProject(path);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) saveOrSaveAs();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global)) {
        const std::string path = saveProjectDialog(projectPath_.empty() ? "Untitled.mcuv" : fileNameOf(projectPath_));
        if (!path.empty()) saveProject(path);
    }
}

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New project", "Ctrl+N")) runAfterSaveCheck(kNew);
        if (ImGui::MenuItem("Open project...", "Ctrl+O")) {
            const std::string path = openFileDialog(FileKind::Project);
            if (!path.empty()) requestOpenProject(path);
        }
        if (ImGui::MenuItem("Save project", "Ctrl+S")) saveOrSaveAs();
        if (ImGui::MenuItem("Save project as...", "Ctrl+Shift+S")) {
            const std::string path = saveProjectDialog(projectPath_.empty() ? "Untitled.mcuv" : fileNameOf(projectPath_));
            if (!path.empty()) saveProject(path);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open FBX...")) {
            const std::string path = openFileDialog(FileKind::Fbx);
            if (!path.empty()) openMesh(path);
        }
        if (ImGui::MenuItem("Import folder...")) {
            const std::string folder = pickFolderDialog();
            if (!folder.empty()) importFolder(folder);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) runAfterSaveCheck(kQuit);
        ImGui::EndMenu();
    }
    ImGui::TextColored(kMutedColor, "  %s", projectPath_.empty() ? "Untitled project" : fileNameOf(projectPath_).c_str());
    if (isDirty()) {
        ImGui::SameLine(0, 4);
        ImGui::TextColored(kWarningColor, "(unsaved changes)");
    }
    ImGui::EndMainMenuBar();
}

// The modal questions: unsaved changes, and a project whose mesh has moved.
void App::drawProjectPrompts() {
    if (pendingAction_ != kNone && !ImGui::IsPopupOpen("Unsaved changes")) ImGui::OpenPopup("Unsaved changes");
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save the changes to %s first?", projectPath_.empty() ? "this project" : fileNameOf(projectPath_).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(110, 0))) {
            if (saveOrSaveAs()) {
                ImGui::CloseCurrentPopup();
                doPendingAction();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't save", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
            doPendingAction();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            pendingAction_ = kNone;
            pendingOpenPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!pendingProjectPath_.empty() && !ImGui::IsPopupOpen("Mesh not found")) ImGui::OpenPopup("Mesh not found");
    if (ImGui::BeginPopupModal("Mesh not found", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s could not find its mesh, %s.", fileNameOf(pendingProjectPath_).c_str(), missingMeshName_.c_str());
        ImGui::TextColored(kMutedColor, "Pick the folder it is in now. Media is searched there too, including subfolders.");
        ImGui::Spacing();
        std::string folder;
        if (ImGui::Button("Locate folder...")) folder = pickFolderDialog();
        if (!nativeDialogsAvailable()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(320);
            ImGui::InputTextWithHint("##meshfolder", "Folder path", pathInput_, sizeof(pathInput_));
            ImGui::SameLine();
            if (ImGui::Button("Search") && pathInput_[0]) folder = pathInput_;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pendingProjectPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        if (!folder.empty()) {
            const std::string project = pendingProjectPath_;
            pendingProjectPath_.clear();
            ImGui::CloseCurrentPopup();
            openProject(project, folder);  // asks again if the mesh is still not there
        }
        ImGui::EndPopup();
    }
}

void App::drawMissingSection() {
    int missing = 0;
    for (const Layer& layer : layers_) missing += layer.missing;
    if (missing == 0) return;
    ImGui::SeparatorText("Missing files");
    ImGui::TextColored(kWarningColor, "%d file(s) were not found where the project expects them.", missing);
    for (const Layer& layer : layers_)
        if (layer.missing) ImGui::BulletText("%s", layer.image.name.c_str());
    if (ImGui::Button("Locate folder...##missing")) {
        const std::string folder = pickFolderDialog();
        if (!folder.empty()) relinkMissing(folder);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pick the folder the media is in now; files are matched by name, including in subfolders.");
    if (!nativeDialogsAvailable()) {
        ImGui::SameLine();
        if (ImGui::Button("Search path field") && pathInput_[0]) relinkMissing(pathInput_);
    }
}
