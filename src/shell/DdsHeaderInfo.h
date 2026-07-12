#pragma once
// Fast DDS header parse (no pixel decode) for Explorer property handler / UI.

#include <cstdint>
#include <string>

namespace ddsinfo {

struct Info {
    int width = 0;
    int height = 0;
    int depth = 1;          // volume depth, usually 1
    int mipCount = 1;
    int arraySize = 1;
    bool isDx10 = false;
    bool isCube = false;
    bool isVolume = false;
    bool srgb = false;
    uint32_t dxgiFormat = 0; // DXGI_FORMAT numeric (0 if legacy unknown)
    char fourCC[5] = {};     // e.g. "DXT5", "DX10", ""
    std::string formatLabel; // "BC7_UNORM_SRGB", "DXT1", "R10G10B10A2_UNORM", …
    int bitDepth = 0;        // approximate bits per pixel (0 if unknown)
};

// Parse from memory (magic + header + optional DX10). Returns false if not DDS / truncated.
bool Parse(const uint8_t* data, size_t size, Info& out);

// Read first ~256 bytes of path and Parse.
bool ParseFile(const wchar_t* path, Info& out);

} // namespace ddsinfo
