#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "distortion.h"
#include "generators.h"
#include "media.h"
#include "mesh.h"
#include "renderer.h"
#include "video.h"

#include <array>
#include <functional>
#include <memory>

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

// What a video layer shows after its last frame while the layer is still on.
enum class VideoEnd { Loop, Hold, Hide };

// Values that can be animated with keyframes: the layer's opacity and some
// generator settings.
enum class KeyParam { Opacity, Size, Angle, Trail, Density, Count };
const char* keyParamName(KeyParam param);

struct Keyframe {
    double time = 0.0;  // seconds from the layer's in point, so keys move with the bar
    float value = 0.0f;
};

// One entry of the global layer stack: a piece of media placed on a mapping.
struct Layer {
    int id = 0;
    Image image;  // for a generator: its name, size and the texture it draws into
    bool generated = false;
    bool followMapping = false;  // generator size tracks its mapping's size
    GeneratorSettings generator;
    GeneratorTarget target;
    int channel = 0;
    int surface = -1;  // -1 = every surface (on UV01: every projection surface)
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool visible = true;
    double inTime = 0.0;   // seconds on the timeline where the layer starts
    double outTime = 0.0;  // and where it ends
    bool missing = false;  // the file was not found when the project opened
    std::shared_ptr<Video> video;  // set for video files; image.texture shows its current frame
    VideoEnd videoEnd = VideoEnd::Loop;
    std::array<std::vector<Keyframe>, (size_t)KeyParam::Count> keys;  // each sorted by time
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

    // Accepts a project, an .fbx, an image or a folder (from the command line, drag and drop or dialogs).
    void importPath(const std::string& path);

    // Projects (app_project.cpp). A project file holds the mesh, layers, timeline and
    // views; media is stored by path, also relative to the project, and can be relinked.
    bool saveProject(const std::string& path);
    bool openProject(const std::string& path, const std::string& searchFolder = "");
    void newProject();
    // Looks for missing media under a folder, by file name.
    void relinkMissing(const std::string& folder);
    // Opens a project after asking about unsaved changes.
    void requestOpenProject(const std::string& path);
    // Asks to save unsaved changes first; returns true when the app may close now.
    bool requestClose();
    bool quitConfirmed() const { return quitConfirmed_; }
    std::string windowTitle() const;
    void openMesh(const std::string& path);
    void setMode(MappingMode mode);
    void setSurfaceLed(int surface, bool led);  // used in Mixed mode
    void showTestGrid(int channel);
    void setWireframe(bool on) { view_.wireframe = on; }
    // Shows the UV view on a channel; surface is an index, kAllSurfaces or kProjectionSurfaces.
    void showUvView(int channel, int surface);
    // Renders the UV view's output at full size and saves it; returns false on failure.
    bool exportOutputPng(const std::string& path);
    void addGenerator(GeneratorType type, int channel, int surface);
    void showDistortion(bool stretch);
    void setTimelineLength(double seconds);
    void setTime(double seconds);
    void setPlaying(bool playing) { playing_ = playing; }
    // Waits up to `seconds` until every visible video shows its frame for the playhead.
    void waitForVideos(double seconds);

private:
    void importFolder(const std::string& folder);
    void importImage(const std::string& path);
    bool addLayer(const std::string& path, int channel, int surface);
    // Loads an image or video into a layer's media; false with `error` set on failure.
    bool loadMedia(const std::string& path, Layer& layer, std::string& error);
    void freeMedia(Layer& layer);
    void updateVideos();
    void drawVideoInfo(Layer& layer);
    void drawPerformance();
    void applyKeyframes();
    static float* keyTarget(Layer& layer, KeyParam param);
    // A diamond after a value: adds or removes a key at the playhead.
    void keyButton(Layer& layer, KeyParam param);
    // After a value was edited: updates its key at the playhead if it is animated.
    void keyEdited(Layer& layer, KeyParam param);
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
    void fileCanvasSize(int channel, int surface, int& width, int& height) const;
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
    void drawGeneratorEditor(Layer& layer);
    void updateGenerators();
    std::string generatorLabel(const Layer& layer) const;
    DistortionFilter distortionFilter() const;
    void drawDistortionLegend(ImDrawList* drawList, ImVec2 corner, bool stretch) const;
    void drawPendingSection();
    bool channelCombo(const char* id, int& channel);
    bool surfaceCombo(const char* id, int& surface);
    void drawViewport();
    void drawUvView();
    bool uvSurfaceCombo();
    int resolvedUvSurface() const;
    void outputSize(int& width, int& height, std::string& note) const;
    std::string outputName() const;
    std::vector<RenderLayer> renderLayers(int onlyChannel) const;
    std::vector<unsigned char> ledFlags() const;
    void renderOutput(bool wireframe, int& width, int& height, std::string& note);
    void drawLog();
    void drawMenuBar();
    void drawProjectPrompts();
    void drawMissingSection();
    void handleShortcuts();
    void runAfterSaveCheck(int action);
    void doPendingAction();
    bool saveOrSaveAs();
    std::string serializeProject(bool forChangeCheck) const;
    void clearProject();
    bool isDirty() const;
    void drawTimeline();
    void drawKeyDiamonds(Layer& layer, float y, float rowHeight, const std::function<float(double)>& timeToX,
                         const std::function<double(float)>& xToTime);
    void drawTransport();
    void advanceTime();
    bool layerActive(const Layer& layer) const;
    double snapToFrame(double seconds) const;
    std::string timecode(double seconds) const;

    Mesh mesh_;
    MappingMode mode_ = MappingMode::Projection;
    std::vector<unsigned char> mixedLed_;  // per surface, the LED choice used in Mixed mode
    std::vector<ChannelState> channels_;
    std::vector<Layer> layers_;  // bottom to top
    std::vector<PendingFile> pending_;
    std::vector<std::string> log_;
    Renderer renderer_;
    Generators generators_;
    OrbitCamera camera_;
    ViewSettings view_;
    float scale_ = 1.0f;
    int gridChannel_ = 0;
    int selectedLayer_ = -1;  // layer id
    int nextLayerId_ = 1;
    int maxTextureSize_ = 4096;
    bool layoutBuilt_ = false;

    // Project
    std::string projectPath_;
    std::string savedState_;           // project as last saved or opened, to spot unsaved changes
    std::string pendingProjectPath_;   // project waiting for its mesh to be located
    std::string missingMeshName_;
    int pendingAction_ = 0;            // what to do after the unsaved-changes question
    std::string pendingOpenPath_;
    bool quitConfirmed_ = false;
    mutable int dirtyCheckFrame_ = 0;
    mutable bool dirtyCached_ = false;

    // Timeline
    double length_ = 60.0;  // seconds
    int fps_ = 30;
    double time_ = 0.0;     // playhead, seconds
    bool playing_ = false;
    bool loop_ = true;
    int dragLayer_ = -1;    // layer id being dragged on the timeline
    int dragPart_ = 0;      // 1 start edge, 2 body, 3 end edge, 4 playhead
    double dragOffset_ = 0.0;
    int dragKeyLayer_ = -1;    // layer id whose keyframes are being dragged
    double dragKeyTime_ = 0.0; // their current time

    // UV view
    bool showUvView_ = true;
    int uvChannel_ = 0;
    int uvSurface_ = kAllSurfaces;
    enum class UvShow { Output, Mapping, Density, Stretch };
    UvShow uvShow_ = UvShow::Output;
    bool distortionStretch_ = false;  // the 3D view's distortion shows stretch rather than density
    bool uvWireframe_ = false;
    float uvZoom_ = 0.0f;         // screen pixels per output pixel; 0 = fit to the window
    float uvPan_[2] = {0, 0};     // offset of the image centre from the view centre, in screen pixels
    char pathInput_[1024] = "";
};
