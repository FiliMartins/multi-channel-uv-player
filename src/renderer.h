#pragma once

#include <vector>

#include "mesh.h"

// Layers drawn in one pass; each needs its own texture unit (OpenGL 3.3 guarantees 16).
constexpr int kMaxLayers = 15;

enum class BlendMode { Normal, Add, Multiply, Screen, Mask, Count };
const char* blendModeName(BlendMode mode);

// Orbit camera around a target point, in metres.
struct OrbitCamera {
    float target[3] = {0, 0, 0};
    float distance = 10.0f;
    float yaw = 0.6f;     // radians around Y
    float pitch = 0.35f;  // radians above the horizon
    float fovY = 0.8f;    // radians

    void frame(const float boundsMin[3], const float boundsMax[3]);
    void orbit(float dx, float dy);
    void pan(float dx, float dy, int viewportHeight);
    void zoom(float steps);
    void eye(float out[3]) const;
    void viewProjection(float aspect, float out[16]) const;
};

constexpr int kAllSurfaces = -1;
constexpr int kProjectionSurfaces = -2;  // every surface not marked as LED

// One layer as the renderer needs it. Layers are listed bottom to top.
struct RenderLayer {
    unsigned texture = 0;
    int channel = 0;          // UV channel the media is mapped through
    int surface = kAllSurfaces;  // a surface index, kAllSurfaces or kProjectionSurfaces
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool clamp = true;        // hide content outside the 0-1 UV square
};

enum class ViewMode { Layers, MeshOnly, TestGrid };

struct ViewSettings {
    ViewMode mode = ViewMode::Layers;
    std::vector<RenderLayer> layers;  // at most kMaxLayers, bottom to top
    int gridChannel = 0;              // channel shown by the UV test grid
    bool gridClamp = true;
    bool wireframe = false;
    bool grid = true;
    float scale = 1.0f;               // user scale factor applied to the mesh
    std::vector<unsigned char> ledSurfaces;  // 1 per LED surface, indexed by surface
};

// Draws the mesh into an offscreen colour buffer that the UI shows as an image.
class Renderer {
public:
    bool init();
    void shutdown();
    void uploadMesh(const Mesh& mesh);
    void render(int width, int height, const OrbitCamera& camera, const ViewSettings& view);
    unsigned colorTexture() const { return colorTexture_; }

private:
    void resizeTarget(int width, int height);
    void uploadLedSurfaces(const std::vector<unsigned char>& flags);

    unsigned meshProgram_ = 0;
    unsigned lineProgram_ = 0;
    unsigned meshVao_ = 0, meshVbo_ = 0;
    unsigned gridVao_ = 0, gridVbo_ = 0;
    int meshVertexCount_ = 0;
    int gridVertexCount_ = 0;
    unsigned ledTexture_ = 0;
    std::vector<unsigned char> ledUploaded_;
    bool ledValid_ = false;
    unsigned framebuffer_ = 0, colorTexture_ = 0, depthBuffer_ = 0;
    int targetWidth_ = 0, targetHeight_ = 0;
};
