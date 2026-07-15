#pragma once
// PNG load/save via Windows Imaging Component (no third-party libs).

#include <cstdint>
#include <string>
#include <vector>

namespace helpers {

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // w*h*4, premultiplied? no — straight alpha
};

bool LoadPng(const std::string& utf8Path, RgbaImage& out, std::string* err = nullptr);
bool SavePng(const std::string& utf8Path, const RgbaImage& img, std::string* err = nullptr);

// Simple multi-filter open dialog (PNG). startDir optional.
// Returns empty on cancel.
std::vector<std::string> BrowseOpenPngFiles(const std::string& startDirUtf8);
std::string BrowseSavePngFile(const std::string& startDirUtf8, const std::string& defaultName);

} // namespace helpers
