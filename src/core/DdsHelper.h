#pragma once
#include <string>
#include <vector>

enum class DdsFormat {
    RGBA8_UNORM,
    RGBA16_UNORM,
    RGBA16_FLOAT,
    RGBA32_FLOAT,
    R8_UNORM,
    R8G8_UNORM, // RG only (B=0, A=1 on open); matches Paint.NET / texconv R8G8
    R16_FLOAT,
    R32_FLOAT
};

struct DdsImage {
    int width = 0;
    int height = 0;
    DdsFormat format = DdsFormat::RGBA8_UNORM;
    std::vector<float> pixels; // Internal representation always float RGBA (32-bit float per channel)
};

class TileCache;

class DdsHelper {
public:
    static bool LoadDDS(const std::string& filename, DdsImage& outImage);
    static bool LoadDDSToTileCache(const std::string& filename, TileCache& outCache, int& outWidth, int& outHeight, DdsFormat& outFormat);
    static bool SaveDDS(const std::string& filename, const DdsImage& image);
    // Header-only peek for UI (e.g. "BC7", "BC5", "R8G8", "BC6H_UF16"). Empty on failure.
    static std::string SniffFormatLabel(const std::string& filename);
};
