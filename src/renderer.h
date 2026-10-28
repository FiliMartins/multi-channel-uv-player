#pragma once

#include "mesh.h"

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

struct ViewSettings {
    int channel = -1;         // UV channel to show, or -1 for the bare mesh
    unsigned texture = 0;     // image on that channel, or 0 for the UV test grid
    bool clamp = true;        // hide content outside the 0-1 UV square
    bool wireframe = false;
    bool grid = true;
    float scale = 1.0f;       // user scale factor applied to the mesh
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

    unsigned meshProgram_ = 0;
    unsigned lineProgram_ = 0;
    unsigned meshVao_ = 0, meshVbo_ = 0;
    unsigned gridVao_ = 0, gridVbo_ = 0;
    int meshVertexCount_ = 0;
    int gridVertexCount_ = 0;
    unsigned framebuffer_ = 0, colorTexture_ = 0, depthBuffer_ = 0;
    int targetWidth_ = 0, targetHeight_ = 0;
};
