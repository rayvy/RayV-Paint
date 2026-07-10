#pragma once

#include <cstdint>

namespace preview3d {

// Multi-pass foundation for game-like character preview.
// ZZZ pipeline (target): Main → (Glow later) → Outline (global inverted hull)
// GI will use a different outline pass id later.

enum class RenderPassId : uint8_t {
    Main = 0,
    Glow,        // reserved (emissive / RabbitFX-style)
    OutlineZZZ,  // TEXCOORD1 packing + Cull Front
    OutlineGI,   // reserved — different math
    Bloom,       // reserved post
    Count
};

struct PassConfig {
    bool enableMain = true;
    bool enableOutline = true;
    bool enableGlow = false;
    bool enableBloom = false;

    // ZZZ outline knobs (preview-owned). Game uses ~0.001–0.005 view-space-ish;
    // we expand in model space — keep default thin, scale with character size.
    float outlineThickness = 0.0035f;
    float outlineAlbedoMul = 0.22f;
    bool  outlineUseVertexColor = true; // COLOR.r thickness (ZZZ community)
    float outlineTint[3] = { 0.05f, 0.05f, 0.06f };
    bool  outlineUseFixedTint = false; // false = diffuse * mul

    // Which outline backend
    enum class OutlineMode : uint8_t { None = 0, ZZZ, GI_Reserved } outlineMode = OutlineMode::ZZZ;
};

const char* RenderPassName(RenderPassId id);

} // namespace preview3d
