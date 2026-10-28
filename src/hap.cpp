#include "hap.h"

#include <cstring>
#include <utility>

#include "snappy.h"

namespace {

struct Section {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint8_t type = 0;
    size_t total = 0;  // header plus data
};

// A section header is a 24-bit little-endian size and a type byte; a size of 0
// means a 32-bit size follows.
bool readSection(const uint8_t* data, size_t size, Section& section) {
    if (size < 4) return false;
    size_t length = data[0] | ((size_t)data[1] << 8) | ((size_t)data[2] << 16);
    size_t header = 4;
    if (length == 0) {
        if (size < 8) return false;
        length = data[4] | ((size_t)data[5] << 8) | ((size_t)data[6] << 16) | ((size_t)data[7] << 24);
        header = 8;
    }
    if (header + length > size) return false;
    section.type = data[3];
    section.data = data + header;
    section.size = length;
    section.total = header + length;
    return true;
}

uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

HapFormat formatOf(uint8_t type) {
    switch (type & 0x0F) {
        case 0xB: return HapFormat::Dxt1;
        case 0xE: return HapFormat::Dxt5;
        case 0xF: return HapFormat::YCoCgDxt5;
        case 0xC: return HapFormat::Bc7;
        case 0x1: return HapFormat::AlphaRgtc1;
        default: return HapFormat::None;
    }
}

bool decompressChunk(uint8_t compressor, const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (compressor == 0x0A) {
        out.insert(out.end(), data, data + size);
        return true;
    }
    if (compressor == 0x0B) return snappyDecompress(data, size, out);
    return false;
}

// "Complex" storage: a decode-instructions container lists the chunks, each
// stored or Snappy-compressed on its own, then the chunk data follows.
bool decodeChunked(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error) {
    Section container;
    if (!readSection(data, size, container) || container.type != 0x01) {
        error = "HAP chunk table missing";
        return false;
    }
    const uint8_t* compressors = nullptr;
    const uint8_t* sizes = nullptr;
    const uint8_t* offsets = nullptr;
    size_t chunks = 0;
    for (size_t pos = 0; pos < container.size;) {
        Section entry;
        if (!readSection(container.data + pos, container.size - pos, entry)) break;
        if (entry.type == 0x02) {
            compressors = entry.data;
            chunks = entry.size;
        } else if (entry.type == 0x03) {
            sizes = entry.data;
        } else if (entry.type == 0x04) {
            offsets = entry.data;
        }
        pos += entry.total;
    }
    if (!compressors || !sizes || chunks == 0) {
        error = "HAP chunk table incomplete";
        return false;
    }
    const uint8_t* frame = data + container.total;
    const size_t frameSize = size - container.total;
    size_t running = 0;
    for (size_t c = 0; c < chunks; ++c) {
        const size_t chunkSize = readU32(sizes + c * 4);
        const size_t offset = offsets ? readU32(offsets + c * 4) : running;
        running = offset + chunkSize;
        if (offset + chunkSize > frameSize || !decompressChunk(compressors[c], frame + offset, chunkSize, out)) {
            error = "HAP chunk " + std::to_string(c) + " is damaged";
            return false;
        }
    }
    return true;
}

bool decodeTexture(const Section& section, int width, int height, HapTexture& texture, std::string& error) {
    texture.format = formatOf(section.type);
    if (texture.format == HapFormat::None) {
        error = "unsupported HAP texture type 0x" + std::to_string(section.type);
        return false;
    }
    texture.blocks.clear();
    const uint8_t compressor = section.type >> 4;
    bool ok = false;
    if (compressor == 0xA || compressor == 0xB) ok = decompressChunk(compressor == 0xA ? 0x0A : 0x0B, section.data,
                                                                       section.size, texture.blocks);
    else if (compressor == 0xC) ok = decodeChunked(section.data, section.size, texture.blocks, error);
    if (!ok) {
        if (error.empty()) error = "HAP frame could not be decompressed";
        return false;
    }
    const size_t expected = (size_t)((width + 3) / 4) * ((height + 3) / 4) * hapBlockSize(texture.format);
    if (texture.blocks.size() < expected) {
        error = "HAP frame is smaller than the video size";
        return false;
    }
    texture.blocks.resize(expected);
    return true;
}

}  // namespace

int hapBlockSize(HapFormat format) {
    return format == HapFormat::Dxt1 || format == HapFormat::AlphaRgtc1 ? 8 : 16;
}

const char* hapFormatName(HapFormat format) {
    switch (format) {
        case HapFormat::Dxt1: return "HAP";
        case HapFormat::Dxt5: return "HAP Alpha";
        case HapFormat::YCoCgDxt5: return "HAP Q";
        case HapFormat::Bc7: return "HAP R";
        case HapFormat::AlphaRgtc1: return "HAP alpha plane";
        default: return "HAP";
    }
}

bool decodeHapFrame(const uint8_t* data, size_t size, int width, int height, std::vector<HapTexture>& textures,
                    std::string& error) {
    Section top;
    if (!readSection(data, size, top)) {
        error = "HAP frame header is damaged";
        return false;
    }
    if (top.type == 0x0D) {  // several textures: HAP Q Alpha
        textures.resize(2);
        size_t pos = 0, count = 0;
        while (pos < top.size && count < 2) {
            Section part;
            if (!readSection(top.data + pos, top.size - pos, part)) break;
            if (!decodeTexture(part, width, height, textures[count], error)) return false;
            pos += part.total;
            ++count;
        }
        if (count != 2) {
            error = "HAP Q Alpha frame is missing a texture";
            return false;
        }
        if (textures[0].format == HapFormat::AlphaRgtc1) std::swap(textures[0], textures[1]);
        return true;
    }
    textures.resize(1);
    return decodeTexture(top, width, height, textures[0], error);
}
