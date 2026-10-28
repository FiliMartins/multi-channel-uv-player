#include "snappy.h"

#include <cstring>

// Format: a varint with the uncompressed length, then elements. Each element's
// tag byte says, in its low two bits, whether it is a literal run or a copy of
// earlier output with a 1, 2 or 4 byte offset.
bool snappyDecompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    size_t pos = 0;
    uint64_t length = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (pos >= size) return false;
        const uint8_t byte = data[pos++];
        length |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
    }
    const size_t start = out.size();
    if (length > (size_t)1 << 30) return false;
    out.resize(start + (size_t)length);
    uint8_t* dst = out.data() + start;
    size_t written = 0;

    while (pos < size) {
        const uint8_t tag = data[pos++];
        size_t runLength = 0, offset = 0;
        switch (tag & 3) {
            case 0: {  // literal
                runLength = (tag >> 2) + 1;
                if (runLength > 60) {
                    const size_t bytes = runLength - 60;
                    if (pos + bytes > size) return false;
                    runLength = 0;
                    for (size_t i = 0; i < bytes; ++i) runLength |= (size_t)data[pos + i] << (8 * i);
                    runLength += 1;
                    pos += bytes;
                }
                if (pos + runLength > size || written + runLength > length) return false;
                std::memcpy(dst + written, data + pos, runLength);
                pos += runLength;
                written += runLength;
                continue;
            }
            case 1:
                if (pos >= size) return false;
                runLength = 4 + ((tag >> 2) & 7);
                offset = ((size_t)(tag >> 5) << 8) | data[pos++];
                break;
            case 2:
                if (pos + 2 > size) return false;
                runLength = (tag >> 2) + 1;
                offset = data[pos] | ((size_t)data[pos + 1] << 8);
                pos += 2;
                break;
            default:
                if (pos + 4 > size) return false;
                runLength = (tag >> 2) + 1;
                offset = data[pos] | ((size_t)data[pos + 1] << 8) | ((size_t)data[pos + 2] << 16) |
                         ((size_t)data[pos + 3] << 24);
                pos += 4;
                break;
        }
        if (offset == 0 || offset > written || written + runLength > length) return false;
        // Copies may overlap their own output (a repeating pattern), so go byte by byte then.
        uint8_t* to = dst + written;
        const uint8_t* from = to - offset;
        if (offset >= runLength) std::memcpy(to, from, runLength);
        else for (size_t i = 0; i < runLength; ++i) to[i] = from[i];
        written += runLength;
    }
    return written == length;
}
