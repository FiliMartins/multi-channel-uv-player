#include "media.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <glad/gl.h>
#include <stb_image.h>

#include "paths.h"

bool loadImage(const std::string& path, int maxTextureSize, Image& out, std::string& error) {
    // Image rows start at the top, UV space starts at the bottom.
    stbi_set_flip_vertically_on_load(1);
    int width = 0, height = 0, components = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &components, 4);
    if (!pixels) {
        error = std::string("Could not read image: ") + stbi_failure_reason();
        return false;
    }
    if (width > maxTextureSize || height > maxTextureSize) {
        stbi_image_free(pixels);
        error = "Image is " + std::to_string(width) + " x " + std::to_string(height) +
                " px, larger than this GPU's limit of " + std::to_string(maxTextureSize) + " px per side.";
        return false;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    out.path = path;
    out.name = fileNameOf(path);
    out.width = width;
    out.height = height;
    out.texture = texture;
    return true;
}

void freeImage(Image& image) {
    if (image.texture) glDeleteTextures(1, &image.texture);
    image = Image{};
}

bool isImageFile(const std::string& path) {
    std::string extension = utf8String(toPath(path).extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" ||
           extension == ".bmp";
}

int parseUvTag(const std::string& fileName) {
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i = 0; i + 2 < fileName.size(); ++i) {
        if (std::tolower((unsigned char)fileName[i]) != 'u' || std::tolower((unsigned char)fileName[i + 1]) != 'v')
            continue;
        size_t end = i + 2;
        while (end < fileName.size() && isDigit(fileName[end])) ++end;
        const size_t digits = end - (i + 2);
        if (digits == 1 || digits == 2) return std::stoi(fileName.substr(i + 2, digits));
    }
    return -1;
}
