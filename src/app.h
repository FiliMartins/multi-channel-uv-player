#pragma once

#include <string>
#include <vector>

#include "media.h"
#include "mesh.h"
#include "renderer.h"

// Projection: every UV channel is a layout shared by the whole mesh.
// LED: UV channel 1 is per surface (each surface fills its own 0-1, like feed
// mapping); channels 2 and up stay shared layouts such as the output map.
// Mixed: each surface is set to Projection or LED on its own.
enum class MappingMode { Projection, Led, Mixed };

struct ChannelState {
    bool clamp = true;        // show content only inside the 0-1 UV square
    bool ignored = false;     // hidden by the user, e.g. a duplicate channel
    bool duplicateDecided = false;
};

// One entry of the global layer stack: a piece of media placed on a mapping.
struct Layer {
    int id = 0;
    Image image;
    int channel = 0;
    int surface = -1;  // -1 = every surface (on UV01: every projection surface)
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool visible = true;
};

// A file the player could not place by its name; the user picks where it goes.
struct PendingFile {
    std::string path;
    std::string reason;
    int channel = 0;
    int surface = -1;
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
    void setMode(MappingMode mode);
    void setSurfaceLed(int surface, bool led);  // used in Mixed mode
    void showTestGrid(int channel);
    void setWireframe(bool on) { view_.wireframe = on; }

private:
    void importFolder(const std::string& folder);
    void importImage(const std::string& path);
    bool addLayer(const std::string& path, int channel, int surface);
    void removeLayer(int index);
    void moveLayer(int from, int to);
    void retryPending();
    int matchSurface(const std::string& fileName) const;
    bool isLedSurface(int surface) const;
    bool anyLedSurface() const;
    bool anyProjectionSurface() const;
    bool perSurface(int channel, int surface) const;
    bool needsSurface(int channel) const;
    void placeLedLayers();
    bool channelUsable(int channel) const;
    std::string channelLabel(int channel) const;
    std::string surfaceLabel(int surface) const;
    std::string placementLabel(int channel, int surface) const;
    void canvasSize(int channel, int surface, int& width, int& height) const;
    std::vector<std::string> layerWarnings(const Layer& layer) const;
    int selectedIndex() const;
    void log(const std::string& message);

    void drawProjectPanel();
    void drawMeshSection();
    void drawSurfaceSection();
    void drawChannelSection();
    void drawLayersPanel();
    void drawLayerList();
    void drawLayerEditor(Layer& layer, int index);
    void drawPendingSection();
    bool channelCombo(const char* id, int& channel);
    bool surfaceCombo(const char* id, int& surface);
    void drawViewport();
    void drawLog();

    Mesh mesh_;
    MappingMode mode_ = MappingMode::Projection;
    std::vector<unsigned char> mixedLed_;  // per surface, the LED choice used in Mixed mode
    std::vector<ChannelState> channels_;
    std::vector<Layer> layers_;  // bottom to top
    std::vector<PendingFile> pending_;
    std::vector<std::string> log_;
    Renderer renderer_;
    OrbitCamera camera_;
    ViewSettings view_;
    float scale_ = 1.0f;
    int gridChannel_ = 0;
    int selectedLayer_ = -1;  // layer id
    int nextLayerId_ = 1;
    int maxTextureSize_ = 4096;
    bool layoutBuilt_ = false;
    char pathInput_[1024] = "";
};
