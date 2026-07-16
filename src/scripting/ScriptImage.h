#pragma once
// Image decode/encode for Python (no third-party pip required).
#include <cstdint>
#include <string>
#include <vector>

namespace script {

// Decode PNG/JPG/TGA/BMP/… from memory → RGBA8. Returns false + log on failure.
bool ImageDecodeMemory(const uint8_t* data, size_t size,
                       std::vector<uint8_t>& outRgba, int& outW, int& outH,
                       std::string* outError = nullptr);

// Encode RGBA8 → PNG bytes in memory.
bool ImageEncodePng(const uint8_t* rgba, int w, int h, int rowStride,
                    std::vector<uint8_t>& outPng, std::string* outError = nullptr);

// Convenience file load (UTF-8 path).
bool ImageLoadFile(const std::string& path,
                   std::vector<uint8_t>& outRgba, int& outW, int& outH,
                   std::string* outError = nullptr);

// Convenience file save PNG/JPG by extension.
bool ImageSaveFile(const std::string& path, const uint8_t* rgba, int w, int h,
                   std::string* outError = nullptr);

} // namespace script
