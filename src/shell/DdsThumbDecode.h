#pragma once
// Lightweight DDS → RGBA8 decoder for Explorer thumbnail handler.
// No GPU, no OpenCV, no RayVPaint core deps — safe to load inside dllhost.exe.

#include <cstdint>
#include <vector>

namespace ddsthumb {

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // w*h*4, R,G,B,A
};

// Decode from a full (or partial) DDS file in memory.
// targetSize: preferred longest edge for mip selection (Explorer typically 32–256).
// Returns false on unsupported / corrupt data.
bool DecodeDdsToRgba8(const uint8_t* data, size_t size, int targetSize, RgbaImage& out);

// Box-filter scale to outW x outH (both > 0).
void ScaleRgba8(const RgbaImage& src, int outW, int outH, std::vector<uint8_t>& outRgba);

} // namespace ddsthumb
