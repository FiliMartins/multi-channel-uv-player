#pragma once

#include <string>

// A still image uploaded to the GPU.
struct Image {
    std::string path;
    std::string name;  // file name without folders
    int width = 0;
    int height = 0;
    unsigned texture = 0;
};

// Loads a PNG, JPG, TGA or BMP file into a texture. Fails, with a message in
// `error`, when the file cannot be decoded or exceeds `maxTextureSize` pixels per side.
bool loadImage(const std::string& path, int maxTextureSize, Image& out, std::string& error);
void freeImage(Image& image);

bool isImageFile(const std::string& path);

// Finds "UV" (any case) followed by one or two digits anywhere in a file name
// and returns that number, e.g. "logo_uv03_v2.png" -> 3 and "Wall_1_UV1.png" -> 1.
// Returns -1 when the name carries no such tag.
int parseUvTag(const std::string& fileName);
