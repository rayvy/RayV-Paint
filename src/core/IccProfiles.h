#pragma once
#include <cstdint>
#include <vector>
#include <string>

// Built-in ICC profile bytes for export presets (no free-text path required).
namespace IccProfiles {

enum class Preset : uint8_t {
    None = 0,
    sRGB = 1,
    DisplayP3 = 2,
    AdobeRGB = 3,
    Linear = 4   // scene-linear / gamma 1.0 (no sRGB transfer)
};

const char* Name(Preset p);

// Empty vector for None (caller skips iCCP). Non-empty for others.
const std::vector<uint8_t>& GetProfileBytes(Preset p);

// Human profile name written into iCCP chunk.
const char* ProfileTagName(Preset p);

} // namespace IccProfiles
