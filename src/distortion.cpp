#include "distortion.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "renderer.h"

namespace {

const float* vertexAt(const Mesh& mesh, size_t vertex) {
    return mesh.vertices.data() + vertex * kVertexFloats;
}

bool counts(const Mesh& mesh, size_t triangle, const DistortionFilter& filter) {
    const float* v0 = vertexAt(mesh, triangle * 3);
    const int surface = (int)(v0[kSurfaceOffset] + 0.5f);
    if (filter.surface >= 0 && surface != filter.surface) return false;
    if (filter.surface == kProjectionSurfaces && filter.ledSurfaces && surface < (int)filter.ledSurfaces->size() &&
        (*filter.ledSurfaces)[surface])
        return false;
    float u = 0, v = 0;
    for (int i = 0; i < 3; ++i) {
        const float* uv = vertexAt(mesh, triangle * 3 + i) + kUvOffset + 2 * filter.channel;
        if (uv[0] <= kMissingUv + 1.0f) return false;
        u += uv[0] / 3.0f;
        v += uv[1] / 3.0f;
    }
    return !filter.clampInside || (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
}

}  // namespace

DistortionSample triangleDistortion(const Mesh& mesh, size_t triangle, const DistortionFilter& filter) {
    DistortionSample sample;
    const float* p[3];
    const float* uv[3];
    for (int i = 0; i < 3; ++i) {
        p[i] = vertexAt(mesh, triangle * 3 + i);
        uv[i] = p[i] + kUvOffset + 2 * filter.channel;
    }
    // Triangle edges in metres, expressed in a 2D frame on the triangle's plane.
    float e1[3], e2[3];
    for (int a = 0; a < 3; ++a) {
        e1[a] = (p[1][a] - p[0][a]) * filter.scale;
        e2[a] = (p[2][a] - p[0][a]) * filter.scale;
    }
    const float length1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    if (length1 < 1e-9f) return sample;
    const float x[3] = {e1[0] / length1, e1[1] / length1, e1[2] / length1};
    const float ax = length1;
    const float bx = e2[0] * x[0] + e2[1] * x[1] + e2[2] * x[2];
    const float by2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2] - bx * bx;
    if (by2 < 1e-18f) return sample;
    const float by = std::sqrt(by2);

    // The same edges in output pixels.
    const float du1 = (uv[1][0] - uv[0][0]) * filter.width, dv1 = (uv[1][1] - uv[0][1]) * filter.height;
    const float du2 = (uv[2][0] - uv[0][0]) * filter.width, dv2 = (uv[2][1] - uv[0][1]) * filter.height;

    // Jacobian J = [pixel edges] * inverse([surface edges]), with surface edges (ax, 0) and (bx, by).
    const float j00 = du1 / ax, j10 = dv1 / ax;
    const float j01 = (du2 - j00 * bx) / by, j11 = (dv2 - j10 * bx) / by;

    // Singular values of the 2x2 Jacobian.
    const float e = (j00 + j11) * 0.5f, f = (j00 - j11) * 0.5f;
    const float g = (j10 + j01) * 0.5f, h = (j10 - j01) * 0.5f;
    const float q = std::sqrt(e * e + h * h), r = std::sqrt(f * f + g * g);
    const float s1 = q + r, s2 = std::fabs(q - r);
    if (s1 <= 0.0f) return sample;
    sample.valid = true;
    sample.density = std::sqrt(s1 * s2);
    sample.stretch = s2 > 1e-9f ? s1 / s2 : 1e9f;
    return sample;
}

DistortionSummary summarizeDistortion(const Mesh& mesh, const DistortionFilter& filter) {
    DistortionSummary summary;
    double areaSum = 0.0, densitySum = 0.0, stretchSum = 0.0;
    float minDensity = FLT_MAX, maxDensity = 0.0f, maxStretch = 1.0f;
    const size_t triangles = mesh.vertexCount() / 3;
    for (size_t t = 0; t < triangles; ++t) {
        if (!counts(mesh, t, filter)) continue;
        const DistortionSample sample = triangleDistortion(mesh, t, filter);
        if (!sample.valid) continue;
        const float* p0 = vertexAt(mesh, t * 3);
        const float* p1 = vertexAt(mesh, t * 3 + 1);
        const float* p2 = vertexAt(mesh, t * 3 + 2);
        float a[3], b[3];
        for (int i = 0; i < 3; ++i) {
            a[i] = (p1[i] - p0[i]) * filter.scale;
            b[i] = (p2[i] - p0[i]) * filter.scale;
        }
        const double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
        const double area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        areaSum += area;
        densitySum += area * sample.density;
        stretchSum += area * std::min(sample.stretch, 100.0f);
        minDensity = std::min(minDensity, sample.density);
        maxDensity = std::max(maxDensity, sample.density);
        maxStretch = std::max(maxStretch, sample.stretch);
        ++summary.triangles;
    }
    if (summary.triangles == 0 || areaSum <= 0.0) return summary;
    summary.meanDensity = (float)(densitySum / areaSum);
    summary.meanStretch = (float)(stretchSum / areaSum);
    summary.minDensity = minDensity;
    summary.maxDensity = maxDensity;
    summary.maxStretch = maxStretch;
    return summary;
}

long findTriangleAt(const Mesh& mesh, const DistortionFilter& filter, float u, float v) {
    const size_t triangles = mesh.vertexCount() / 3;
    for (size_t t = 0; t < triangles; ++t) {
        const float* a = vertexAt(mesh, t * 3) + kUvOffset + 2 * filter.channel;
        const float* b = vertexAt(mesh, t * 3 + 1) + kUvOffset + 2 * filter.channel;
        const float* c = vertexAt(mesh, t * 3 + 2) + kUvOffset + 2 * filter.channel;
        const float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (std::fabs(d) < 1e-12f) continue;
        const float w0 = ((b[1] - c[1]) * (u - c[0]) + (c[0] - b[0]) * (v - c[1])) / d;
        const float w1 = ((c[1] - a[1]) * (u - c[0]) + (a[0] - c[0]) * (v - c[1])) / d;
        if (w0 < 0.0f || w1 < 0.0f || w0 + w1 > 1.0f) continue;
        DistortionFilter any = filter;
        any.clampInside = false;
        if (counts(mesh, t, any)) return (long)t;
    }
    return -1;
}
