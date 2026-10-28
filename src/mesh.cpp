#include "mesh.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <ufbx.h>

namespace {

constexpr float kDuplicateEpsilon = 1e-5f;
// Tolerance for UVs exported a hair outside 0-1 (e.g. -1.5e-6), matching the shader.
constexpr float kInsideEpsilon = 1e-4f;

bool insideUnitSquare(float u, float v) {
    return u >= -kInsideEpsilon && u <= 1.0f + kInsideEpsilon &&
           v >= -kInsideEpsilon && v <= 1.0f + kInsideEpsilon;
}

}  // namespace

bool Mesh::load(const std::string& filePath, std::string& error) {
    *this = Mesh{};
    path = filePath;

    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;

    ufbx_error loadError;
    ufbx_scene* scene = ufbx_load_file(filePath.c_str(), &opts, &loadError);
    if (!scene) {
        char buffer[1024];
        ufbx_format_error(buffer, sizeof(buffer), &loadError);
        error = buffer;
        return false;
    }

    fileUnitMeters = scene->settings.original_unit_meters > 0.0
                         ? scene->settings.original_unit_meters
                         : scene->settings.unit_meters;

    // Channels are matched by position in each mesh's UV set list; names come from
    // the first mesh that has the channel.
    int channelCount = 0;
    for (const ufbx_node* node : scene->nodes) {
        if (node->mesh) channelCount = std::max(channelCount, (int)node->mesh->uv_sets.count);
    }
    droppedChannels = std::max(0, channelCount - kMaxUvChannels);
    channelCount = std::min(channelCount, kMaxUvChannels);
    channels.resize(channelCount);

    std::vector<int> insideCount(channelCount, 0);
    std::vector<int> cornerCount(channelCount, 0);
    for (UvChannelInfo& channel : channels) {
        channel.minU = channel.minV = FLT_MAX;
        channel.maxU = channel.maxV = -FLT_MAX;
    }
    for (int axis = 0; axis < 3; ++axis) {
        boundsMin[axis] = FLT_MAX;
        boundsMax[axis] = -FLT_MAX;
    }

    std::vector<uint32_t> triangleIndices;
    for (const ufbx_node* node : scene->nodes) {
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh) continue;

        const std::string nodeName = node->name.length > 0
                                         ? std::string(node->name.data, node->name.length)
                                         : "Surface " + std::to_string(surfaces.size() + 1);
        const int meshChannels = std::min((int)mesh->uv_sets.count, channelCount);
        for (int c = 0; c < meshChannels; ++c) {
            UvChannelInfo& channel = channels[c];
            if (channel.name.empty()) {
                const ufbx_string& name = mesh->uv_sets.data[c].name;
                channel.name = std::string(name.data, name.length);
            }
        }
        triangleIndices.resize(mesh->max_face_triangles * 3);

        // Each object is a surface; an object with several materials is split
        // into one surface per material.
        const bool splitByMaterial = mesh->material_parts.count > 1;
        const size_t partCount = splitByMaterial ? mesh->material_parts.count : 1;
        for (size_t part = 0; part < partCount; ++part) {
            Surface surface;
            surface.name = nodeName;
            if (splitByMaterial) {
                const ufbx_material* material =
                    part < mesh->materials.count ? mesh->materials.data[part] : nullptr;
                surface.name += " / " + (material && material->name.length > 0
                                             ? std::string(material->name.data, material->name.length)
                                             : "Material " + std::to_string(part + 1));
            }
            surface.firstVertex = (uint32_t)vertexCount();
            const float surfaceIndex = (float)surfaces.size();

            const size_t faceCount = splitByMaterial ? mesh->material_parts.data[part].face_indices.count
                                                     : mesh->faces.count;
            for (size_t f = 0; f < faceCount; ++f) {
                const ufbx_face face =
                    mesh->faces.data[splitByMaterial ? mesh->material_parts.data[part].face_indices.data[f] : f];
                const uint32_t triangles =
                    ufbx_triangulate_face(triangleIndices.data(), triangleIndices.size(), mesh, face);
                for (uint32_t i = 0; i < triangles * 3; ++i) {
                    const uint32_t index = triangleIndices[i];
                    const ufbx_vec3 local = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                    const ufbx_vec3 world = ufbx_transform_position(&node->geometry_to_world, local);
                    const float position[3] = {(float)world.x, (float)world.y, (float)world.z};
                    for (int axis = 0; axis < 3; ++axis) {
                        boundsMin[axis] = std::min(boundsMin[axis], position[axis]);
                        boundsMax[axis] = std::max(boundsMax[axis], position[axis]);
                        vertices.push_back(position[axis]);
                    }
                    vertices.push_back(surfaceIndex);
                    for (int c = 0; c < kMaxUvChannels; ++c) {
                        if (c >= meshChannels) {
                            vertices.push_back(kMissingUv);
                            vertices.push_back(kMissingUv);
                            continue;
                        }
                        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->uv_sets.data[c].vertex_uv, index);
                        const float u = (float)uv.x, v = (float)uv.y;
                        vertices.push_back(u);
                        vertices.push_back(v);

                        UvChannelInfo& channel = channels[c];
                        channel.minU = std::min(channel.minU, u);
                        channel.maxU = std::max(channel.maxU, u);
                        channel.minV = std::min(channel.minV, v);
                        channel.maxV = std::max(channel.maxV, v);
                        cornerCount[c]++;
                        if (insideUnitSquare(u, v)) insideCount[c]++;
                    }
                }
            }

            surface.vertexCount = (uint32_t)vertexCount() - surface.firstVertex;
            if (surface.vertexCount == 0) continue;
            for (int c = 0; c < meshChannels; ++c) channels[c].surfaceCount++;
            surfaces.push_back(surface);
        }
    }
    ufbx_free_scene(scene);

    if (vertices.empty()) {
        error = "The file has no mesh geometry.";
        *this = Mesh{};
        return false;
    }

    for (int c = 0; c < channelCount; ++c) {
        UvChannelInfo& channel = channels[c];
        if (channel.name.empty()) channel.name = "UV channel " + std::to_string(c + 1);
        channel.insideRatio = cornerCount[c] > 0 ? (float)insideCount[c] / (float)cornerCount[c] : 0.0f;
    }

    // A channel is a duplicate when every vertex has the same UV as an earlier channel.
    const size_t count = vertexCount();
    for (int c = 1; c < channelCount; ++c) {
        for (int earlier = 0; earlier < c && channels[c].duplicateOf < 0; ++earlier) {
            if (channels[earlier].duplicateOf >= 0) continue;
            bool same = true;
            for (size_t i = 0; i < count && same; ++i) {
                const float* uvs = &vertices[i * kVertexFloats + kUvOffset];
                same = std::fabs(uvs[c * 2] - uvs[earlier * 2]) <= kDuplicateEpsilon &&
                       std::fabs(uvs[c * 2 + 1] - uvs[earlier * 2 + 1]) <= kDuplicateEpsilon;
            }
            if (same) channels[c].duplicateOf = earlier;
        }
    }
    return true;
}
