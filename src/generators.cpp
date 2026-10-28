#include "generators.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

#include <glad/gl.h>

#include "gl_util.h"

namespace {

constexpr int kMaxTextLength = 48;

// A full-screen triangle; no vertex buffer needed.
const char* kVertexShader = R"(#version 330 core
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Every pattern works in output pixels, so it lines up with the pixels of the
// mapping (useful on LED). The result is premultiplied, like the image layers.
const char* kFragmentShader = R"(#version 330 core
uniform int uType;
uniform vec2 uResolution;
uniform float uTime;
uniform vec3 uColor;
uniform vec3 uBackground;
uniform bool uTransparent;
uniform float uSpeed;
uniform float uSize;
uniform float uDensity;
uniform float uTrail;
uniform float uAngle;     // radians
uniform vec2 uCenter;
uniform ivec2 uGrid;
uniform float uSeed;
uniform int uText[48];    // 3x5 glyph bitmasks, top row in the high bits
uniform int uTextLength;
uniform int uDigits[10];
out vec4 fragColor;

const float TAU = 6.28318530718;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21) + uSeed * 0.618);
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Lights the pixel when it falls on a set bit of a 3x5 glyph; `cell` is in glyph pixels.
bool glyphBit(int mask, vec2 cell) {
    if (cell.x < 0.0 || cell.x >= 3.0 || cell.y < 0.0 || cell.y >= 5.0) return false;
    int bit = (4 - int(cell.y)) * 3 + (2 - int(cell.x));
    return ((mask >> bit) & 1) == 1;
}

// `q` is measured from the top-left corner; text starts at `origin`, `scale` pixels per glyph pixel.
bool textAt(vec2 q, vec2 origin, float scale) {
    vec2 local = (q - origin) / scale;
    if (local.y < 0.0 || local.y >= 5.0 || local.x < 0.0) return false;
    int index = int(local.x / 4.0);
    if (index >= uTextLength) return false;
    int mask = 0;
    for (int i = 0; i < 48; ++i)
        if (i == index) mask = uText[i];
    return glyphBit(mask, vec2(local.x - float(index) * 4.0, local.y));
}

bool numberAt(vec2 q, vec2 origin, float scale, int value) {
    int digits = value >= 100 ? 3 : (value >= 10 ? 2 : 1);
    vec2 local = (q - origin) / scale;
    if (local.y < 0.0 || local.y >= 5.0 || local.x < 0.0) return false;
    int index = int(local.x / 4.0);
    if (index >= digits) return false;
    int place = digits - 1 - index;
    int digit = place == 2 ? value / 100 : (place == 1 ? (value / 10) % 10 : value % 10);
    int mask = 0;
    for (int d = 0; d < 10; ++d)
        if (d == digit) mask = uDigits[d];
    return glyphBit(mask, vec2(local.x - float(index) * 4.0, local.y));
}

float lineMask(float distance, float width) {
    return 1.0 - smoothstep(width * 0.5 - 0.5, width * 0.5 + 0.5, distance);
}

// Returns colour and coverage of the foreground (0 = background only).
vec4 testPattern(vec2 p, vec2 q) {
    vec2 res = uResolution;
    vec3 color = uBackground;
    float alpha = 0.0;
    float spacing = max(uSize, 4.0);
    vec2 g = mod(p, spacing);
    if (min(g.x, g.y) < 1.0) { color = mix(uBackground, uColor, 0.35); alpha = 1.0; }

    // Corner-to-corner diagonals.
    vec2 n1 = normalize(vec2(res.y, -res.x));
    vec2 n2 = normalize(vec2(res.y, res.x));
    float diagonal = min(abs(dot(p, n1)), abs(dot(p - vec2(0.0, res.y), n2)));
    if (diagonal < 1.0) { color = mix(uBackground, uColor, 0.6); alpha = 1.0; }

    // Circles and a hand that turns once every 4 s at speed 1.
    vec2 c = res * 0.5;
    float r = length(p - c);
    float big = min(res.x, res.y) * 0.45;
    if (abs(r - big) < 1.5 || abs(r - big * 0.5) < 1.0) { color = uColor; alpha = 1.0; }
    float handAngle = uTime * uSpeed * TAU * 0.25;
    vec2 hand = vec2(sin(handAngle), cos(handAngle));
    if (r < big && dot(p - c, hand) > 0.0 && abs(dot(p - c, vec2(hand.y, -hand.x))) < 1.5) {
        color = vec3(1.0, 0.3, 0.2); alpha = 1.0;
    }

    // Colour bars and a grey ramp across the middle.
    vec2 uv = p / res;
    if (uv.x > 0.2 && uv.x < 0.8 && uv.y > 0.45 && uv.y < 0.55) {
        int bar = int((uv.x - 0.2) / 0.6 * 8.0);
        vec3 bars[8] = vec3[8](vec3(1), vec3(1, 1, 0), vec3(0, 1, 1), vec3(0, 1, 0),
                               vec3(1, 0, 1), vec3(1, 0, 0), vec3(0, 0, 1), vec3(0));
        color = bars[bar]; alpha = 1.0;
    }
    if (uv.x > 0.2 && uv.x < 0.8 && uv.y > 0.40 && uv.y <= 0.45) {
        color = vec3((uv.x - 0.2) / 0.6); alpha = 1.0;
    }

    // Border, and a block that runs along the bottom edge to show playback.
    if (p.x < 2.0 || p.y < 2.0 || p.x > res.x - 2.0 || p.y > res.y - 2.0) { color = uColor; alpha = 1.0; }
    float block = max(spacing * 0.5, 4.0);
    float runner = fract(uTime * uSpeed * 0.25) * (res.x - block);
    if (p.y > 4.0 && p.y < 4.0 + block && p.x > runner && p.x < runner + block) { color = uColor; alpha = 1.0; }

    // Name centred just below the grey ramp, kept within the ramp's width.
    float scale = max(1.0, floor(res.y / 160.0));
    while (scale > 1.0 && float(uTextLength) * 4.0 * scale > res.x * 0.6) scale -= 1.0;
    float textWidth = float(uTextLength) * 4.0 * scale - scale;
    vec2 textOrigin = vec2(floor((res.x - textWidth) * 0.5), floor(res.y * 0.60) + 2.0 * scale);
    if (textAt(q, textOrigin, scale)) { color = uColor; alpha = 1.0; }
    return vec4(color, alpha);
}

// Pixel dots that run only along rows or columns, each at its own speed, with a trail.
vec4 bugs(vec2 p) {
    float size = max(uSize, 1.0);
    vec2 cells = max(floor(uResolution / size), vec2(1.0));
    vec2 cell = floor(p / size);
    float trail = max(uTrail * 40.0, 1.0);
    float best = 0.0;
    for (int k = 0; k < 2; ++k) {
        for (int axis = 0; axis < 2; ++axis) {
            // axis 0: a bug in this row moving along x; axis 1: in this column moving along y.
            float lane = axis == 0 ? cell.y : cell.x;
            float along = axis == 0 ? cell.x : cell.y;
            float laneLength = axis == 0 ? cells.x : cells.y;
            vec2 id = vec2(lane + float(k) * 137.0, float(axis) * 71.0 + float(k) * 13.0);
            if (hash(id) > uDensity) continue;
            float speed = uSpeed * mix(3.0, 20.0, hash(id + 1.7));  // cells per second
            float direction = hash(id + 3.1) > 0.5 ? 1.0 : -1.0;
            float head = floor(fract(hash(id + 5.3) + uTime * speed / laneLength) * laneLength);
            float behind = mod(direction * (head - along), laneLength);
            float level = behind < 0.5 ? 1.0 : (behind < trail ? 0.75 * (1.0 - behind / trail) : 0.0);
            best = max(best, level);
        }
    }
    return vec4(uColor, best);
}

vec2 rotate(vec2 v, float a) {
    return vec2(cos(a) * v.x - sin(a) * v.y, sin(a) * v.x + cos(a) * v.y);
}

vec4 chevron(vec2 p) {
    vec2 d = rotate(p - uResolution * 0.5, -uAngle);
    float size = max(uSize, 2.0);
    float phase = (d.x - abs(d.y)) / (2.0 * size) - uTime * uSpeed;
    return vec4(uColor, fract(phase) < 0.5 ? 1.0 : 0.0);
}

vec4 radar(vec2 p) {
    vec2 c = uCenter * uResolution;
    vec2 d = p - c;
    float r = length(d);
    float radius = min(uResolution.x, uResolution.y) * 0.5;
    float angle = atan(d.x, d.y);
    float sweep = uTime * uSpeed * TAU * 0.25;
    float behind = mod(sweep - angle, TAU);
    float trail = max(uTrail, 0.01) * TAU;
    float level = behind < trail ? 0.85 * (1.0 - behind / trail) : 0.0;
    if (behind < 0.02 || behind > TAU - 0.002 * radius / max(r, 1.0)) level = 1.0;
    float spacing = max(uSize * 4.0, 8.0);
    if (abs(mod(r + spacing * 0.5, spacing) - spacing * 0.5) < 1.0) level = max(level, 0.45);
    if (min(abs(d.x), abs(d.y)) < 1.0) level = max(level, 0.45);
    if (r > radius) level = abs(r - radius) < 1.5 ? 0.45 : 0.0;
    return vec4(uColor, level);
}

vec4 scan(vec2 p) {
    vec2 dir = vec2(cos(uAngle), sin(uAngle));
    // Projections of the corners give the range the line travels across.
    vec2 res = uResolution;
    float lo = min(min(0.0, dot(vec2(res.x, 0.0), dir)), min(dot(vec2(0.0, res.y), dir), dot(res, dir)));
    float hi = max(max(0.0, dot(vec2(res.x, 0.0), dir)), max(dot(vec2(0.0, res.y), dir), dot(res, dir)));
    float width = max(uSize, 1.0);
    float travel = hi - lo + width;
    float position = lo - width * 0.5 + fract(uTime * uSpeed * 0.25) * travel;
    float u = dot(p, dir);
    float level = abs(u - position) <= width * 0.5 ? 1.0 : 0.0;
    float trail = uTrail * (hi - lo);
    float behind = position - width * 0.5 - u;
    if (level == 0.0 && trail > 0.0 && behind > 0.0 && behind < trail) level = 0.6 * (1.0 - behind / trail);
    return vec4(uColor, level);
}

vec4 gradient(vec2 p) {
    vec2 dir = vec2(cos(uAngle), sin(uAngle));
    float s = dot(p / uResolution - 0.5, dir) + 0.5;
    float wave = 0.5 + 0.5 * sin(TAU * (s - uTime * uSpeed * 0.25));
    return vec4(mix(uBackground, uColor, wave), 1.0);
}

float valueNoise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash(i.xy + i.z * 17.0), n100 = hash(i.xy + vec2(1, 0) + i.z * 17.0);
    float n010 = hash(i.xy + vec2(0, 1) + i.z * 17.0), n110 = hash(i.xy + vec2(1, 1) + i.z * 17.0);
    float n001 = hash(i.xy + (i.z + 1.0) * 17.0), n101 = hash(i.xy + vec2(1, 0) + (i.z + 1.0) * 17.0);
    float n011 = hash(i.xy + vec2(0, 1) + (i.z + 1.0) * 17.0), n111 = hash(i.xy + vec2(1, 1) + (i.z + 1.0) * 17.0);
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

vec4 noise(vec2 p) {
    vec3 x = vec3(p / max(uSize * 8.0, 4.0), uTime * uSpeed * 0.3);
    float total = 0.0, amplitude = 0.5;
    for (int octave = 0; octave < 4; ++octave) {
        total += amplitude * valueNoise(x);
        x = vec3(x.xy * 2.03, x.z * 1.7);
        amplitude *= 0.5;
    }
    return vec4(mix(uBackground, uColor, smoothstep(0.2, 0.8, total)), 1.0);
}

// Numbered tiles, counted from the top-left; one tile lights up in turn.
vec4 tiles(vec2 p, vec2 q) {
    vec2 grid = vec2(max(uGrid, ivec2(1)));
    vec2 tileSize = uResolution / grid;
    vec2 tile = min(floor(q / tileSize), grid - 1.0);
    int index = int(tile.y * grid.x + tile.x) + 1;
    int count = int(grid.x * grid.y);
    bool odd = mod(tile.x + tile.y, 2.0) > 0.5;
    vec3 color = mix(uBackground, uColor, odd ? 0.18 : 0.30);
    int lit = int(mod(floor(uTime * uSpeed), float(count))) + 1;
    if (index == lit) color = mix(uBackground, uColor, 0.6);
    vec2 local = q - tile * tileSize;
    if (local.x < 1.0 || local.y < 1.0) color = uColor;
    float scale = max(1.0, floor(min(tileSize.x, tileSize.y) / 12.0));
    int digits = index >= 100 ? 3 : (index >= 10 ? 2 : 1);
    vec2 textSize = vec2(float(digits) * 4.0 - 1.0, 5.0) * scale;
    vec2 origin = tile * tileSize + floor((tileSize - textSize) * 0.5);
    if (numberAt(q, origin, scale, index)) color = uColor;
    return vec4(color, 1.0);
}

void main() {
    vec2 p = gl_FragCoord.xy;
    vec2 q = vec2(p.x, uResolution.y - p.y);  // from the top-left, for text and tile numbers
    vec4 layer;
    if (uType == 0) layer = testPattern(p, q);
    else if (uType == 1) layer = bugs(p);
    else if (uType == 2) layer = chevron(p);
    else if (uType == 3) layer = radar(p);
    else if (uType == 4) layer = scan(p);
    else if (uType == 5) layer = gradient(p);
    else if (uType == 6) layer = noise(p);
    else layer = tiles(p, q);

    // Patterns that fill every pixel return coverage 1 with their own colour.
    vec3 color = mix(uBackground, layer.rgb, layer.a);
    float alpha = uTransparent ? layer.a : 1.0;
    if (uTransparent) color = layer.rgb;
    fragColor = vec4(color * alpha, alpha);
}
)";

// 3x5 glyphs, rows top to bottom, three bits per row.
int glyph(char c) {
    static const struct { char c; const char* rows; } kFont[] = {
        {'0', "111101101101111"}, {'1', "010110010010111"}, {'2', "111001111100111"}, {'3', "111001111001111"},
        {'4', "101101111001001"}, {'5', "111100111001111"}, {'6', "111100111101111"}, {'7', "111001001001001"},
        {'8', "111101111101111"}, {'9', "111101111001111"}, {'A', "010101111101101"}, {'B', "110101110101110"},
        {'C', "011100100100011"}, {'D', "110101101101110"}, {'E', "111100110100111"}, {'F', "111100110100100"},
        {'G', "011100101101011"}, {'H', "101101111101101"}, {'I', "111010010010111"}, {'J', "001001001101010"},
        {'K', "101101110101101"}, {'L', "100100100100111"}, {'M', "101111111101101"}, {'N', "110101101101101"},
        {'O', "010101101101010"}, {'P', "110101110100100"}, {'Q', "010101101110011"}, {'R', "110101110101101"},
        {'S', "011100010001110"}, {'T', "111010010010010"}, {'U', "101101101101111"}, {'V', "101101101101010"},
        {'W', "101101111111101"}, {'X', "101101010101101"}, {'Y', "101101010010010"}, {'Z', "111001010100111"},
        {':', "000010000010000"}, {'-', "000000111000000"}, {'.', "000000000000010"}, {'/', "001001010100100"},
        {'_', "000000000000111"}, {'(', "010100100100010"}, {')', "010001001001010"},
    };
    const char upper = (char)std::toupper((unsigned char)c);
    for (const auto& entry : kFont) {
        if (entry.c != upper) continue;
        int mask = 0;
        for (int i = 0; i < 15; ++i) mask = (mask << 1) | (entry.rows[i] == '1');
        return mask;
    }
    return 0;  // space and anything unknown
}

}  // namespace

const char* generatorName(GeneratorType type) {
    switch (type) {
        case GeneratorType::TestPattern: return "Test pattern";
        case GeneratorType::Bugs: return "Bugs";
        case GeneratorType::Chevron: return "Chevron";
        case GeneratorType::Radar: return "Radar";
        case GeneratorType::Scan: return "Scan";
        case GeneratorType::Gradient: return "Gradient";
        case GeneratorType::Noise: return "Noise";
        case GeneratorType::Tiles: return "Numbered tiles";
        default: return "?";
    }
}

GeneratorSettings defaultGenerator(GeneratorType type, int width, int height) {
    GeneratorSettings settings;
    settings.type = type;
    settings.width = width;
    settings.height = height;
    const int shortSide = std::max(1, std::min(width, height));
    switch (type) {
        case GeneratorType::TestPattern:
            settings.size = (float)std::max(8, shortSide / 8);
            break;
        case GeneratorType::Bugs:
            settings.size = (float)std::max(1, shortSide / 100);
            settings.color[0] = 0.2f, settings.color[1] = 1.0f, settings.color[2] = 0.4f;
            settings.transparent = true;
            break;
        case GeneratorType::Chevron:
            settings.size = (float)std::max(4, shortSide / 16);
            settings.color[0] = 1.0f, settings.color[1] = 0.8f, settings.color[2] = 0.1f;
            settings.transparent = true;
            break;
        case GeneratorType::Radar:
            settings.size = (float)std::max(2, shortSide / 40);
            settings.color[0] = 0.2f, settings.color[1] = 1.0f, settings.color[2] = 0.3f;
            settings.trail = 0.15f;
            settings.transparent = true;
            break;
        case GeneratorType::Scan:
            settings.size = (float)std::max(2, shortSide / 50);
            settings.trail = 0.1f;
            settings.transparent = true;
            break;
        case GeneratorType::Gradient:
            settings.color[0] = 0.2f, settings.color[1] = 0.5f, settings.color[2] = 1.0f;
            settings.background[0] = 0.9f, settings.background[1] = 0.1f, settings.background[2] = 0.5f;
            break;
        case GeneratorType::Noise:
            settings.size = (float)std::max(4, shortSide / 40);
            settings.speed = 0.5f;
            break;
        case GeneratorType::Tiles:
            settings.color[0] = 0.9f, settings.color[1] = 0.9f, settings.color[2] = 1.0f;
            settings.background[0] = 0.1f, settings.background[1] = 0.12f, settings.background[2] = 0.2f;
            break;
        default:
            break;
    }
    return settings;
}

bool Generators::init() {
    program_ = linkProgram(kVertexShader, kFragmentShader);
    glGenVertexArrays(1, &vao_);
    return program_ != 0;
}

void Generators::shutdown() {
    glDeleteProgram(program_);
    glDeleteVertexArrays(1, &vao_);
}

void Generators::release(GeneratorTarget& target) {
    if (target.framebuffer) glDeleteFramebuffers(1, &target.framebuffer);
    if (target.texture) glDeleteTextures(1, &target.texture);
    target = GeneratorTarget{};
}

void Generators::render(const GeneratorSettings& settings, double time, GeneratorTarget& target) {
    const int width = std::max(1, settings.width), height = std::max(1, settings.height);
    if (!target.framebuffer) {
        glGenFramebuffers(1, &target.framebuffer);
        glGenTextures(1, &target.texture);
    }
    if (width != target.width || height != target.height) {
        target.width = width;
        target.height = height;
        glBindTexture(GL_TEXTURE_2D, target.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    }

    int text[kMaxTextLength] = {};
    const int textLength = std::min((int)settings.label.size(), kMaxTextLength);
    for (int i = 0; i < textLength; ++i) text[i] = glyph(settings.label[i]);
    int digits[10];
    for (int d = 0; d < 10; ++d) digits[d] = glyph((char)('0' + d));

    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(program_);
    const auto location = [&](const char* name) { return glGetUniformLocation(program_, name); };
    glUniform1i(location("uType"), (int)settings.type);
    glUniform2f(location("uResolution"), (float)width, (float)height);
    glUniform1f(location("uTime"), (float)time);
    glUniform3fv(location("uColor"), 1, settings.color);
    glUniform3fv(location("uBackground"), 1, settings.background);
    glUniform1i(location("uTransparent"), settings.transparent);
    glUniform1f(location("uSpeed"), settings.speed);
    glUniform1f(location("uSize"), settings.size);
    glUniform1f(location("uDensity"), settings.density);
    glUniform1f(location("uTrail"), settings.trail);
    glUniform1f(location("uAngle"), settings.angle * 3.14159265f / 180.0f);
    glUniform2fv(location("uCenter"), 1, settings.center);
    glUniform2i(location("uGrid"), settings.columns, settings.rows);
    glUniform1f(location("uSeed"), (float)settings.seed);
    glUniform1iv(location("uText"), kMaxTextLength, text);
    glUniform1i(location("uTextLength"), textLength);
    glUniform1iv(location("uDigits"), 10, digits);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glBindTexture(GL_TEXTURE_2D, target.texture);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}
