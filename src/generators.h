#pragma once

#include <string>

// Built-in animated content, drawn on the GPU each frame like a media server's
// generative layers. A generator layer composites exactly like an image layer.
enum class GeneratorType { TestPattern, Bugs, Chevron, Radar, Scan, Gradient, Noise, Tiles, Count };
const char* generatorName(GeneratorType type);

struct GeneratorSettings {
    GeneratorType type = GeneratorType::TestPattern;
    int width = 1920, height = 1080;  // pixels drawn, normally the mapping's size
    float color[3] = {1, 1, 1};       // foreground
    float background[3] = {0, 0, 0};
    bool transparent = false;         // background lets the layers below show through
    float speed = 1.0f;
    float size = 16.0f;               // pixels: bug size, stripe width, line width, grid or ring spacing
    float density = 0.3f;             // Bugs: share of rows and columns with a bug
    float trail = 0.3f;               // Bugs, Radar, Scan: trail length
    float angle = 0.0f;               // degrees: direction of motion
    float center[2] = {0.5f, 0.5f};   // Radar: centre, as a share of width and height
    int columns = 8, rows = 4;        // Tiles
    int seed = 1;
    std::string label;                // Test pattern: text shown at the top
};

// Sensible starting values for each type.
GeneratorSettings defaultGenerator(GeneratorType type, int width, int height);

// The texture a generator layer draws into.
struct GeneratorTarget {
    unsigned framebuffer = 0;
    unsigned texture = 0;
    int width = 0, height = 0;
};

class Generators {
public:
    bool init();
    void shutdown();
    // Draws `settings` at `time` seconds into the target, resizing it when needed.
    void render(const GeneratorSettings& settings, double time, GeneratorTarget& target);
    static void release(GeneratorTarget& target);

private:
    unsigned program_ = 0;
    unsigned vao_ = 0;
};
