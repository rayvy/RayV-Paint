#pragma once
#include <string>

struct ExportSettings {
    bool isDds = true;
    std::string ddsFormatStr = "BC7_UNORM_SRGB";
    bool advancedMode = false;
    std::string compressionSpeed = "Medium"; // "Fast", "Medium", "Slow"
    bool generateMipMaps = true;
    std::string mipFilter = "Bicubic"; // "Bicubic", "Bicubic (Smooth)", "Bilinear", "Bilinear (Low Quality)", "Adaptive (Sharp)", "Lanczos", "Fant", "Nearest Neighbor"
    std::string pngColorSpace = "sRGB"; // "None", "sRGB", "Linear"
    std::string exportPath = "";
};

class TexconvHelper {
public:
    static bool CompressDDS(const std::string& srcImage, const std::string& destDds, const ExportSettings& settings);
};
