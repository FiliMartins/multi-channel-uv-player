#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glad/gl.h>

namespace {

constexpr float kPi = 3.14159265358979f;

const char* kMeshVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in float aSurface;
layout(location = 2) in vec2 aUv[8];
uniform mat4 uViewProjection;
uniform float uScale;
uniform int uLayoutChannel;  // -1: 3D view; otherwise draw flat in this channel's UV square
out vec3 vWorld;
flat out int vSurface;
out vec2 vUv[8];
void main() {
    vec3 world = aPosition * uScale;
    vWorld = world;
    vSurface = int(aSurface + 0.5);
    vec2 layoutUv = vec2(0.0);
    for (int i = 0; i < 8; ++i) {
        vUv[i] = aUv[i];
        if (i == uLayoutChannel) layoutUv = aUv[i];
    }
    gl_Position = uLayoutChannel >= 0 ? vec4(layoutUv * 2.0 - 1.0, 0.0, 1.0)
                                      : uViewProjection * vec4(world, 1.0);
}
)";

// Shared part of the mesh fragment shader. The per-layer code is appended by
// meshFragmentShader(), because GLSL 3.30 only indexes sampler arrays with constants.
const char* kMeshFragmentHeader = R"(#version 330 core
#define MAX_LAYERS 15
in vec3 vWorld;
flat in int vSurface;
in vec2 vUv[8];
uniform int uMode;  // 0 layers, 1 mesh only, 2 UV test grid
uniform int uGridChannel;
uniform bool uGridClamp;
uniform bool uWire;
uniform int uOnlySurface;  // -1 every surface, -2 projection surfaces only, else one surface
uniform bool uOutput;      // output preview: no mesh shading, black where no layer covers
uniform int uLayerCount;
uniform int uLayerChannel[MAX_LAYERS];
uniform int uLayerSurface[MAX_LAYERS];
uniform int uLayerBlend[MAX_LAYERS];
uniform float uLayerOpacity[MAX_LAYERS];
uniform bool uLayerClamp[MAX_LAYERS];
uniform sampler2D uTexture[MAX_LAYERS];
uniform sampler2D uLedSurfaces;  // one texel per surface, red = 1 for LED surfaces
out vec4 fragColor;

vec3 color = vec3(0.0);   // accumulated layer colour
float coverage = 0.0;     // how much of the base mesh the layers hide

vec2 channelUv(int c) {
    if (c == 0) return vUv[0];
    if (c == 1) return vUv[1];
    if (c == 2) return vUv[2];
    if (c == 3) return vUv[3];
    if (c == 4) return vUv[4];
    if (c == 5) return vUv[5];
    if (c == 6) return vUv[6];
    return vUv[7];
}

bool insideUnitSquare(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(-1e-4))) && all(lessThanEqual(uv, vec2(1.0 + 1e-4)));
}

// Shown on a channel with no image: a coloured checker so the layout is readable.
vec3 uvTestGrid(vec2 uv) {
    vec2 cells = uv * 8.0;
    float checker = mod(floor(cells.x) + floor(cells.y), 2.0);
    vec3 grid = vec3(0.25 + 0.6 * uv.x, 0.25 + 0.6 * uv.y, 0.55) * (0.75 + 0.25 * checker);
    vec2 distance = abs(fract(cells - 0.5) - 0.5) / max(fwidth(cells), vec2(1e-5));
    float line = 1.0 - min(min(distance.x, distance.y), 1.0);
    return mix(grid, vec3(0.95), line * 0.5);
}

// Samples with gradients from the unwrapped UV so repeating (clamp off) has no seams.
// Textures are stored premultiplied; this returns straight colour and alpha.
vec4 sampleLayer(sampler2D tex, vec2 uv, bool clampUv) {
    vec2 at = clampUv ? uv : fract(uv);
    vec4 texel = textureGrad(tex, at, dFdx(uv), dFdy(uv));
    return vec4(texel.a > 0.0 ? texel.rgb / texel.a : vec3(0.0), texel.a);
}

bool isLedSurface() {
    return texelFetch(uLedSurfaces, ivec2(vSurface, 0), 0).r > 0.5;
}

void applyLayer(int i, vec2 uv, vec4 texel) {
    float visible = uLayerClamp[i] && !insideUnitSquare(uv) ? 0.0 : 1.0;
    if (uLayerSurface[i] >= 0 && uLayerSurface[i] != vSurface) visible = 0.0;
    if (uLayerSurface[i] == -2 && isLedSurface()) visible = 0.0;
    float amount = visible * uLayerOpacity[i];
    float weight = amount * texel.a;
    int blend = uLayerBlend[i];
    if (blend == 0) {          // Normal
        color = mix(color, texel.rgb, weight);
        coverage += weight * (1.0 - coverage);
    } else if (blend == 1) {   // Add
        color += texel.rgb * weight;
        coverage += weight * (1.0 - coverage);
    } else if (blend == 2) {   // Multiply
        color = mix(color, color * texel.rgb, weight);
    } else if (blend == 3) {   // Screen
        color = mix(color, 1.0 - (1.0 - color) * (1.0 - texel.rgb), weight);
        coverage += weight * (1.0 - coverage);
    } else {                   // Mask: this layer's brightness cuts out everything below
        float luminance = dot(texel.rgb, vec3(0.2126, 0.7152, 0.0722)) * texel.a;
        float keep = mix(1.0, luminance, amount);
        color *= keep;
        coverage *= keep;
    }
}

void main() {
    if (uOnlySurface >= 0 && vSurface != uOnlySurface) discard;
    if (uOnlySurface == -2 && isLedSurface()) discard;
    if (uWire) {
        fragColor = vec4(0.9, 0.9, 0.9, 0.35);
        return;
    }
    vec3 normal = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    float shade = 0.55 + 0.45 * abs(dot(normal, normalize(vec3(0.3, 0.8, 0.5))));
    vec3 base = uOutput ? vec3(0.0) : vec3(0.16, 0.17, 0.19) * shade;
    if (uMode == 1) {
        fragColor = vec4(base * 1.6, 1.0);
        return;
    }
    if (uMode == 2) {
        vec2 uv = channelUv(uGridChannel);
        bool hidden = uGridClamp && !insideUnitSquare(uv);
        fragColor = vec4(hidden ? base : uvTestGrid(uGridClamp ? uv : fract(uv)), 1.0);
        return;
    }
    // @LAYER_CODE
    fragColor = vec4(color + base * (1.0 - coverage), 1.0);
}
)";

std::string meshFragmentShader() {
    std::string layers;
    for (int i = 0; i < kMaxLayers; ++i) {
        const std::string n = std::to_string(i);
        layers += "if (uLayerCount > " + n + ") { vec2 uv = channelUv(uLayerChannel[" + n +
                  "]); applyLayer(" + n + ", uv, sampleLayer(uTexture[" + n + "], uv, uLayerClamp[" + n +
                  "])); }\n    ";
    }
    std::string source = kMeshFragmentHeader;
    const std::string marker = "// @LAYER_CODE";
    source.replace(source.find(marker), marker.size(), layers);
    return source;
}

const char* kLineVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
uniform mat4 uViewProjection;
void main() { gl_Position = uViewProjection * vec4(aPosition, 1.0); }
)";

const char* kLineFragmentShader = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";

unsigned compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shader compile error:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

unsigned linkProgram(const char* vertexSource, const char* fragmentSource) {
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) return 0;
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shader link error:\n%s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void multiply(const float a[16], const float b[16], float out[16]) {
    float result[16];
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[column * 4 + k];
            result[column * 4 + row] = sum;
        }
    std::copy(result, result + 16, out);
}

void normalize(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0) for (int i = 0; i < 3; ++i) v[i] /= length;
}

void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

}  // namespace

const char* blendModeName(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal: return "Normal";
        case BlendMode::Add: return "Add";
        case BlendMode::Multiply: return "Multiply";
        case BlendMode::Screen: return "Screen";
        case BlendMode::Mask: return "Mask";
        default: return "?";
    }
}

void OrbitCamera::frame(const float boundsMin[3], const float boundsMax[3]) {
    float radius = 0;
    for (int axis = 0; axis < 3; ++axis) {
        target[axis] = 0.5f * (boundsMin[axis] + boundsMax[axis]);
        const float half = 0.5f * (boundsMax[axis] - boundsMin[axis]);
        radius += half * half;
    }
    radius = std::max(std::sqrt(radius), 0.01f);
    distance = radius / std::sin(fovY * 0.5f) * 1.05f;
}

void OrbitCamera::orbit(float dx, float dy) {
    yaw -= dx * 0.008f;
    pitch = std::clamp(pitch + dy * 0.008f, -1.55f, 1.55f);
}

void OrbitCamera::pan(float dx, float dy, int viewportHeight) {
    float eyePosition[3];
    eye(eyePosition);
    float forward[3] = {target[0] - eyePosition[0], target[1] - eyePosition[1], target[2] - eyePosition[2]};
    normalize(forward);
    const float worldUp[3] = {0, 1, 0};
    float right[3], up[3];
    cross(forward, worldUp, right);
    normalize(right);
    cross(right, forward, up);
    const float unitsPerPixel = 2.0f * distance * std::tan(fovY * 0.5f) / (float)std::max(viewportHeight, 1);
    for (int axis = 0; axis < 3; ++axis) target[axis] += (-dx * right[axis] + dy * up[axis]) * unitsPerPixel;
}

void OrbitCamera::zoom(float steps) {
    distance = std::clamp(distance * std::pow(0.88f, steps), 0.01f, 100000.0f);
}

void OrbitCamera::eye(float out[3]) const {
    out[0] = target[0] + distance * std::cos(pitch) * std::sin(yaw);
    out[1] = target[1] + distance * std::sin(pitch);
    out[2] = target[2] + distance * std::cos(pitch) * std::cos(yaw);
}

void OrbitCamera::viewProjection(float aspect, float out[16]) const {
    float eyePosition[3];
    eye(eyePosition);
    float forward[3] = {target[0] - eyePosition[0], target[1] - eyePosition[1], target[2] - eyePosition[2]};
    normalize(forward);
    const float worldUp[3] = {0, 1, 0};
    float side[3], up[3];
    cross(forward, worldUp, side);
    normalize(side);
    cross(side, forward, up);

    float view[16] = {side[0], up[0], -forward[0], 0,
                      side[1], up[1], -forward[1], 0,
                      side[2], up[2], -forward[2], 0,
                      0,       0,     0,           1};
    view[12] = -(side[0] * eyePosition[0] + side[1] * eyePosition[1] + side[2] * eyePosition[2]);
    view[13] = -(up[0] * eyePosition[0] + up[1] * eyePosition[1] + up[2] * eyePosition[2]);
    view[14] = forward[0] * eyePosition[0] + forward[1] * eyePosition[1] + forward[2] * eyePosition[2];

    const float nearPlane = distance * 0.005f, farPlane = distance * 200.0f;
    const float f = 1.0f / std::tan(fovY * 0.5f);
    float projection[16] = {};
    projection[0] = f / aspect;
    projection[5] = f;
    projection[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    projection[11] = -1.0f;
    projection[14] = 2.0f * farPlane * nearPlane / (nearPlane - farPlane);
    multiply(projection, view, out);
}

bool Renderer::init() {
    meshProgram_ = linkProgram(kMeshVertexShader, meshFragmentShader().c_str());
    lineProgram_ = linkProgram(kLineVertexShader, kLineFragmentShader);
    if (!meshProgram_ || !lineProgram_) return false;

    // Floor grid: 1 m spacing across 50 m, centred on the origin.
    std::vector<float> lines;
    const int half = 25;
    for (int i = -half; i <= half; ++i) {
        const float a = (float)i, b = (float)half;
        lines.insert(lines.end(), {a, 0, -b, a, 0, b, -b, 0, a, b, 0, a});
    }
    gridVertexCount_ = (int)lines.size() / 3;
    glGenVertexArrays(1, &gridVao_);
    glGenBuffers(1, &gridVbo_);
    glBindVertexArray(gridVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    glBufferData(GL_ARRAY_BUFFER, lines.size() * sizeof(float), lines.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);

    glGenVertexArrays(1, &meshVao_);
    glGenBuffers(1, &meshVbo_);
    glBindVertexArray(meshVao_);
    glBindBuffer(GL_ARRAY_BUFFER, meshVbo_);
    const GLsizei stride = kVertexFloats * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void*)(kSurfaceOffset * sizeof(float)));
    for (int c = 0; c < kMaxUvChannels; ++c) {
        glEnableVertexAttribArray(2 + c);
        glVertexAttribPointer(2 + c, 2, GL_FLOAT, GL_FALSE, stride, (void*)((kUvOffset + 2 * c) * sizeof(float)));
    }
    glBindVertexArray(0);
    return true;
}

void Renderer::shutdown() {
    glDeleteProgram(meshProgram_);
    glDeleteProgram(lineProgram_);
    glDeleteVertexArrays(1, &meshVao_);
    glDeleteBuffers(1, &meshVbo_);
    glDeleteVertexArrays(1, &gridVao_);
    glDeleteBuffers(1, &gridVbo_);
    glDeleteFramebuffers(1, &framebuffer_);
    glDeleteTextures(1, &colorTexture_);
    glDeleteTextures(1, &ledTexture_);
    glDeleteFramebuffers(1, &uvFramebuffer_);
    glDeleteTextures(1, &uvTexture_);
    glDeleteRenderbuffers(1, &depthBuffer_);
}

void Renderer::uploadMesh(const Mesh& mesh) {
    glBindBuffer(GL_ARRAY_BUFFER, meshVbo_);
    glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(float), mesh.vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    meshVertexCount_ = (int)mesh.vertexCount();
}

void Renderer::resizeTarget(int width, int height) {
    if (width == targetWidth_ && height == targetHeight_ && framebuffer_) return;
    targetWidth_ = width;
    targetHeight_ = height;
    if (!framebuffer_) {
        glGenFramebuffers(1, &framebuffer_);
        glGenTextures(1, &colorTexture_);
        glGenRenderbuffers(1, &depthBuffer_);
    }
    glBindTexture(GL_TEXTURE_2D, colorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::uploadLedSurfaces(const std::vector<unsigned char>& flags) {
    if (ledValid_ && flags == ledUploaded_) return;
    if (!ledTexture_) glGenTextures(1, &ledTexture_);
    std::vector<unsigned char> texels(std::max<size_t>(flags.size(), 1), 0);
    for (size_t i = 0; i < flags.size(); ++i) texels[i] = flags[i] ? 255 : 0;
    glBindTexture(GL_TEXTURE_2D, ledTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (int)texels.size(), 1, 0, GL_RED, GL_UNSIGNED_BYTE, texels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    ledUploaded_ = flags;
    ledValid_ = true;
}

void Renderer::bindLayers(const std::vector<RenderLayer>& layers, const std::vector<unsigned char>& ledSurfaces) {
    const int layerCount = std::min((int)layers.size(), kMaxLayers);
    int channel[kMaxLayers] = {}, surface[kMaxLayers] = {}, blend[kMaxLayers] = {}, clampUv[kMaxLayers] = {};
    int units[kMaxLayers];
    float opacity[kMaxLayers] = {};
    for (int i = 0; i < kMaxLayers; ++i) {
        units[i] = i;
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, i < layerCount ? layers[i].texture : 0);
        if (i >= layerCount) continue;
        const RenderLayer& layer = layers[i];
        channel[i] = layer.channel;
        surface[i] = layer.surface;
        blend[i] = (int)layer.blend;
        clampUv[i] = layer.clamp;
        opacity[i] = layer.opacity;
    }
    uploadLedSurfaces(ledSurfaces);
    glActiveTexture(GL_TEXTURE0 + kMaxLayers);
    glBindTexture(GL_TEXTURE_2D, ledTexture_);
    glUniform1i(glGetUniformLocation(meshProgram_, "uLedSurfaces"), kMaxLayers);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(meshProgram_, "uLayerCount"), layerCount);
    glUniform1iv(glGetUniformLocation(meshProgram_, "uLayerChannel"), kMaxLayers, channel);
    glUniform1iv(glGetUniformLocation(meshProgram_, "uLayerSurface"), kMaxLayers, surface);
    glUniform1iv(glGetUniformLocation(meshProgram_, "uLayerBlend"), kMaxLayers, blend);
    glUniform1iv(glGetUniformLocation(meshProgram_, "uLayerClamp"), kMaxLayers, clampUv);
    glUniform1fv(glGetUniformLocation(meshProgram_, "uLayerOpacity"), kMaxLayers, opacity);
    glUniform1iv(glGetUniformLocation(meshProgram_, "uTexture"), kMaxLayers, units);
}

void Renderer::render(int width, int height, const OrbitCamera& camera, const ViewSettings& view) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    resizeTarget(width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, width, height);
    glClearColor(0.07f, 0.075f, 0.085f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    float viewProjection[16];
    camera.viewProjection((float)width / (float)height, viewProjection);

    if (view.grid) {
        glUseProgram(lineProgram_);
        glUniformMatrix4fv(glGetUniformLocation(lineProgram_, "uViewProjection"), 1, GL_FALSE, viewProjection);
        glUniform4f(glGetUniformLocation(lineProgram_, "uColor"), 0.22f, 0.23f, 0.26f, 1.0f);
        glBindVertexArray(gridVao_);
        glDrawArrays(GL_LINES, 0, gridVertexCount_);
    }

    if (meshVertexCount_ > 0) {
        glUseProgram(meshProgram_);
        glUniformMatrix4fv(glGetUniformLocation(meshProgram_, "uViewProjection"), 1, GL_FALSE, viewProjection);
        glUniform1f(glGetUniformLocation(meshProgram_, "uScale"), view.scale);
        glUniform1i(glGetUniformLocation(meshProgram_, "uMode"), (int)view.mode);
        glUniform1i(glGetUniformLocation(meshProgram_, "uGridChannel"), view.gridChannel);
        glUniform1i(glGetUniformLocation(meshProgram_, "uGridClamp"), view.gridClamp);
        glUniform1i(glGetUniformLocation(meshProgram_, "uWire"), 0);

        glUniform1i(glGetUniformLocation(meshProgram_, "uLayoutChannel"), -1);
        glUniform1i(glGetUniformLocation(meshProgram_, "uOnlySurface"), kAllSurfaces);
        glUniform1i(glGetUniformLocation(meshProgram_, "uOutput"), 0);
        bindLayers(view.layers, view.ledSurfaces);
        glBindVertexArray(meshVao_);

        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, meshVertexCount_);
        glDisable(GL_POLYGON_OFFSET_FILL);

        if (view.wireframe) {
            glUniform1i(glGetUniformLocation(meshProgram_, "uWire"), 1);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glDrawArrays(GL_TRIANGLES, 0, meshVertexCount_);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glDisable(GL_BLEND);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::renderUv(const UvViewSettings& view) {
    const int width = std::max(view.width, 1), height = std::max(view.height, 1);
    if (!uvFramebuffer_) {
        glGenFramebuffers(1, &uvFramebuffer_);
        glGenTextures(1, &uvTexture_);
    }
    if (width != uvWidth_ || height != uvHeight_) {
        uvWidth_ = width;
        uvHeight_ = height;
        glBindTexture(GL_TEXTURE_2D, uvTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // Nearest when magnified, so zooming in shows the real output pixels.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, uvFramebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, uvTexture_, 0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, uvFramebuffer_);
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    if (meshVertexCount_ > 0) {
        glUseProgram(meshProgram_);
        glUniform1f(glGetUniformLocation(meshProgram_, "uScale"), 1.0f);
        glUniform1i(glGetUniformLocation(meshProgram_, "uLayoutChannel"), view.channel);
        glUniform1i(glGetUniformLocation(meshProgram_, "uOnlySurface"), view.surface);
        glUniform1i(glGetUniformLocation(meshProgram_, "uOutput"), 1);
        glUniform1i(glGetUniformLocation(meshProgram_, "uMode"), (int)ViewMode::Layers);
        glUniform1i(glGetUniformLocation(meshProgram_, "uWire"), 0);
        bindLayers(view.layers, view.ledSurfaces);
        glBindVertexArray(meshVao_);
        glDrawArrays(GL_TRIANGLES, 0, meshVertexCount_);

        if (view.wireframe) {
            glUniform1i(glGetUniformLocation(meshProgram_, "uWire"), 1);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glDrawArrays(GL_TRIANGLES, 0, meshVertexCount_);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glDisable(GL_BLEND);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool Renderer::readUvPixels(std::vector<unsigned char>& rgb, int& width, int& height) const {
    if (!uvFramebuffer_ || uvWidth_ <= 0 || uvHeight_ <= 0) return false;
    width = uvWidth_;
    height = uvHeight_;
    std::vector<unsigned char> rgba((size_t)width * height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, uvFramebuffer_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // OpenGL rows start at the bottom (V = 0); images start at the top.
    rgb.resize((size_t)width * height * 3);
    for (int y = 0; y < height; ++y) {
        const unsigned char* source = rgba.data() + (size_t)(height - 1 - y) * width * 4;
        unsigned char* target = rgb.data() + (size_t)y * width * 3;
        for (int x = 0; x < width; ++x) {
            target[x * 3 + 0] = source[x * 4 + 0];
            target[x * 3 + 1] = source[x * 4 + 1];
            target[x * 3 + 2] = source[x * 4 + 2];
        }
    }
    return true;
}
