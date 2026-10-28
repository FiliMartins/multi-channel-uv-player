#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <glad/gl.h>

namespace {

constexpr float kPi = 3.14159265358979f;

const char* kMeshVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUv[8];
uniform mat4 uViewProjection;
uniform float uScale;
out vec3 vWorld;
out vec2 vUv[8];
void main() {
    vec3 world = aPosition * uScale;
    vWorld = world;
    for (int i = 0; i < 8; ++i) vUv[i] = aUv[i];
    gl_Position = uViewProjection * vec4(world, 1.0);
}
)";

const char* kMeshFragmentShader = R"(#version 330 core
in vec3 vWorld;
in vec2 vUv[8];
uniform int uChannel;
uniform sampler2D uTexture;
uniform bool uHasTexture;
uniform bool uClamp;
uniform bool uWire;
out vec4 fragColor;

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

// Shown on a channel with no image: a coloured checker so the layout is readable.
vec3 uvTestGrid(vec2 uv) {
    vec2 cells = uv * 8.0;
    float checker = mod(floor(cells.x) + floor(cells.y), 2.0);
    vec3 color = vec3(0.25 + 0.6 * uv.x, 0.25 + 0.6 * uv.y, 0.55) * (0.75 + 0.25 * checker);
    vec2 distance = abs(fract(cells - 0.5) - 0.5) / max(fwidth(cells), vec2(1e-5));
    float line = 1.0 - min(min(distance.x, distance.y), 1.0);
    return mix(color, vec3(0.95), line * 0.5);
}

void main() {
    if (uWire) {
        fragColor = vec4(0.9, 0.9, 0.9, 0.35);
        return;
    }
    vec3 normal = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    float shade = 0.55 + 0.45 * abs(dot(normal, normalize(vec3(0.3, 0.8, 0.5))));
    vec3 base = vec3(0.16, 0.17, 0.19) * shade;
    if (uChannel < 0) {
        fragColor = vec4(base * 1.6, 1.0);
        return;
    }
    vec2 uv = channelUv(uChannel);
    bool inside = all(greaterThanEqual(uv, vec2(-1e-4))) && all(lessThanEqual(uv, vec2(1.0 + 1e-4)));
    if (uClamp && !inside) {
        fragColor = vec4(base, 1.0);
        return;
    }
    vec2 sampleUv = uClamp ? uv : fract(uv);
    if (uHasTexture) {
        vec4 texel = texture(uTexture, sampleUv);
        fragColor = vec4(mix(base, texel.rgb, texel.a), 1.0);
    } else {
        fragColor = vec4(uvTestGrid(sampleUv), 1.0);
    }
}
)";

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
    meshProgram_ = linkProgram(kMeshVertexShader, kMeshFragmentShader);
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
    for (int c = 0; c < kMaxUvChannels; ++c) {
        glEnableVertexAttribArray(1 + c);
        glVertexAttribPointer(1 + c, 2, GL_FLOAT, GL_FALSE, stride, (void*)((3 + 2 * c) * sizeof(float)));
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
        glUniform1i(glGetUniformLocation(meshProgram_, "uChannel"), view.channel);
        glUniform1i(glGetUniformLocation(meshProgram_, "uHasTexture"), view.texture != 0);
        glUniform1i(glGetUniformLocation(meshProgram_, "uClamp"), view.clamp);
        glUniform1i(glGetUniformLocation(meshProgram_, "uTexture"), 0);
        glUniform1i(glGetUniformLocation(meshProgram_, "uWire"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, view.texture);
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
