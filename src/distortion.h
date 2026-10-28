#pragma once

#include <vector>

#include "mesh.h"

// How a UV channel spreads an output's pixels over the mesh: density in pixels
// per metre (the geometric mean of the two directions) and stretch, the ratio
// of the two (1 = square pixels).
struct DistortionSample {
    bool valid = false;
    float density = 0.0f;
    float stretch = 1.0f;
};

struct DistortionSummary {
    int triangles = 0;
    float meanDensity = 0.0f;  // weighted by surface area
    float minDensity = 0.0f, maxDensity = 0.0f;
    float meanStretch = 1.0f, maxStretch = 1.0f;
};

// Which triangles count: one surface, every surface (kAllSurfaces) or only the
// non-LED ones (kProjectionSurfaces); with clampInside, only those inside 0-1.
struct DistortionFilter {
    int channel = 0;
    int surface = -1;
    const std::vector<unsigned char>* ledSurfaces = nullptr;
    bool clampInside = true;
    int width = 1, height = 1;  // output pixels
    float scale = 1.0f;         // user scale applied to the mesh
};

DistortionSample triangleDistortion(const Mesh& mesh, size_t triangle, const DistortionFilter& filter);
DistortionSummary summarizeDistortion(const Mesh& mesh, const DistortionFilter& filter);
// The triangle under a UV position, or -1.
long findTriangleAt(const Mesh& mesh, const DistortionFilter& filter, float u, float v);
