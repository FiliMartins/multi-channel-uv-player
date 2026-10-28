#pragma once

#include <filesystem>
#include <string>

// All paths travel through the app as UTF-8 strings; these convert at the edges
// so names with accents work on Windows.

inline std::filesystem::path toPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string utf8String(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::string fileNameOf(const std::string& utf8) {
    return utf8String(toPath(utf8).filename());
}
