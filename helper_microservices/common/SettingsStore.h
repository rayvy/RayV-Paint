#pragma once
// Tiny key=value settings under %LOCALAPPDATA%\RayVPaint\helpers_settings.ini

#include <string>

namespace helpers {

struct Settings {
    std::string lastPngDir;
    std::string lastDdsFormat = "BC7_UNORM_SRGB";
    bool generateMips = true;
    std::string compressionSpeed = "Medium"; // Fast / Medium / Slow
    std::string mipFilter = "CUBIC";
    int atlasPadding = 2;
    int atlasMaxSize = 4096;
    bool atlasPowerOfTwo = true;
    bool atlasExportDds = false;
};

Settings LoadSettings();
void SaveSettings(const Settings& s);

// Default last folder: Documents if empty/missing.
std::string ResolveBrowseStartDir(const Settings& s);

} // namespace helpers
