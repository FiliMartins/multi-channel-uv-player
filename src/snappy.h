#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decompresses a raw Snappy block (the format HAP uses for its texture chunks).
// Appends to `out`; returns false on corrupt input.
bool snappyDecompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
