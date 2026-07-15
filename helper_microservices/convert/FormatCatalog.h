#pragma once
// DDS format catalog for the helper converter (DXGI names for texconv -f).

#include <cstddef>
#include <cstring>

namespace helpers {

struct DdsFormatEntry {
    const char* uiLabel;   // shown in ImGui combo
    const char* texconv;   // -f argument
};

// Keep list practical for game/mod textures (Paint.NET-adjacent).
inline constexpr DdsFormatEntry kDdsFormats[] = {
    { "BC7 (sRGB)",              "BC7_UNORM_SRGB" },
    { "BC7 (Linear)",            "BC7_UNORM" },
    { "BC1 (sRGB)",              "BC1_UNORM_SRGB" },
    { "BC1 (Linear)",            "BC1_UNORM" },
    { "BC2 (sRGB)",              "BC2_UNORM_SRGB" },
    { "BC2 (Linear)",            "BC2_UNORM" },
    { "BC3 (sRGB)",              "BC3_UNORM_SRGB" },
    { "BC3 (Linear / DXT5)",     "BC3_UNORM" },
    { "BC4 (Unsigned)",          "BC4_UNORM" },
    { "BC4 (Signed)",            "BC4_SNORM" },
    { "BC5 (Unsigned)",          "BC5_UNORM" },
    { "BC5 (Signed)",            "BC5_SNORM" },
    { "BC6H (Unsigned)",         "BC6H_UF16" },
    { "BC6H (Signed)",           "BC6H_SF16" },
    { "B8G8R8A8 (sRGB)",         "B8G8R8A8_UNORM_SRGB" },
    { "B8G8R8A8 (Linear)",       "B8G8R8A8_UNORM" },
    { "R8G8B8A8 (sRGB)",         "R8G8B8A8_UNORM_SRGB" },
    { "R8G8B8A8 (Linear)",       "R8G8B8A8_UNORM" },
    { "R8G8 (Unsigned)",         "R8G8_UNORM" },
    { "R8 (Unsigned)",           "R8_UNORM" },
    { "R16G16B16A16 Float",      "R16G16B16A16_FLOAT" },
    { "R32G32B32A32 Float",      "R32G32B32A32_FLOAT" },
};

inline constexpr int kDdsFormatCount = (int)(sizeof(kDdsFormats) / sizeof(kDdsFormats[0]));

inline int FindFormatIndex(const char* texconvName) {
    if (!texconvName) return 0;
    for (int i = 0; i < kDdsFormatCount; ++i) {
        if (strcmp(kDdsFormats[i].texconv, texconvName) == 0)
            return i;
    }
    return 0;
}

} // namespace helpers
