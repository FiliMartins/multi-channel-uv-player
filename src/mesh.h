#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The renderer passes every UV channel to the shader, so the count is capped.
constexpr int kMaxUvChannels = 8;

// Interleaved vertex: position (3 floats) followed by one UV pair per channel.
constexpr int kVertexFloats = 3 + 2 * kMaxUvChannels;

// UV written for surfaces that lack a channel; outside 0-1 so clamping hides it.
constexpr float kMissingUv = -10.0f;

// One object of the FBX that holds geometry, e.g. "Wall 1" in an LED setup.
struct Surface {
    std::string name;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
};

struct UvChannelInfo {
    std::string name;          // name stored in the file, e.g. "UVChannel_1"
    int duplicateOf = -1;      // index of an earlier channel with identical UVs, or -1
    int surfaceCount = 0;      // how many surfaces carry this channel
    float insideRatio = 0.0f;  // share of mesh corners whose UV lies inside 0-1
    float minU = 0, maxU = 0, minV = 0, maxV = 0;
};

// A triangulated, world-space copy of every mesh in an FBX file, in metres, Y up.
struct Mesh {
    std::string path;
    std::vector<float> vertices;  // kVertexFloats per vertex, three vertices per triangle
    std::vector<Surface> surfaces;
    std::vector<UvChannelInfo> channels;
    float boundsMin[3] = {0, 0, 0};
    float boundsMax[3] = {0, 0, 0};
    double fileUnitMeters = 0.01;  // unit of the file before conversion (FBX default: cm)
    int droppedChannels = 0;       // channels beyond kMaxUvChannels, not loaded

    size_t vertexCount() const { return vertices.size() / kVertexFloats; }
    bool empty() const { return vertices.empty(); }

    // Returns false and fills `error` when the file cannot be read.
    bool load(const std::string& filePath, std::string& error);
};
