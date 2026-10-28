#pragma once

#include <string>
#include <vector>

#include "media.h"
#include "mesh.h"
#include "renderer.h"

struct ChannelState {
    Image image;              // milestone 1: one image per channel
    bool clamp = true;        // show content only inside the 0-1 UV square
    bool ignored = false;     // hidden by the user, e.g. a duplicate channel
    bool duplicateDecided = false;
};

// A file the player could not place on a channel by its name.
struct PendingFile {
    std::string path;
    std::string reason;
    int choice = 0;  // channel picked in the UI
};

class App {
public:
    bool init(int maxTextureSize);
    void shutdown();

    // Builds the UI and draws the 3D view for one frame.
    void frame();

    // Accepts an .fbx, an image or a folder (from the command line, drag and drop or dialogs).
    void importPath(const std::string& path);
    void openMesh(const std::string& path);
    void setViewChannel(int channel) { viewChannel_ = channel; }
    void setWireframe(bool on) { view_.wireframe = on; }

private:
    void importFolder(const std::string& folder);
    void importImage(const std::string& path);
    void assignImage(const std::string& path, int channel);
    void retryPending();
    bool channelUsable(int channel) const;
    std::string channelLabel(int channel) const;
    void log(const std::string& message);

    void drawProjectPanel();
    void drawMeshSection();
    void drawChannelSection();
    void drawPendingSection();
    void drawViewport();
    void drawLog();

    Mesh mesh_;
    std::vector<ChannelState> channels_;
    std::vector<PendingFile> pending_;
    std::vector<std::string> log_;
    Renderer renderer_;
    OrbitCamera camera_;
    ViewSettings view_;
    float scale_ = 1.0f;
    int viewChannel_ = -1;
    int maxTextureSize_ = 4096;
    bool layoutBuilt_ = false;
    char pathInput_[1024] = "";
};
