#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// HAP frames hold GPU texture blocks (DXT1, DXT5, scaled YCoCg DXT5, BC7 or an
// RGTC1 alpha plane), optionally Snappy-compressed in chunks. Decoding only
// undoes the compression; the GPU reads the blocks directly.
enum class HapFormat { None, Dxt1, Dxt5, YCoCgDxt5, Bc7, AlphaRgtc1 };

struct HapTexture {
    HapFormat format = HapFormat::None;
    std::vector<uint8_t> blocks;
};

// Bytes per 4x4 block for a format.
int hapBlockSize(HapFormat format);
const char* hapFormatName(HapFormat format);

// Decodes one frame into one texture, or two for HAP Q Alpha (colour then alpha).
// `width` and `height` are the video's size; returns false with `error` set on failure.
bool decodeHapFrame(const uint8_t* data, size_t size, int width, int height, std::vector<HapTexture>& textures,
                    std::string& error);
