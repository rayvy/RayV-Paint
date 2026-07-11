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

    // ZZZ outline: view-space silhouette expand (game-like), not model-space balloon.
    // thickness = UI scale (1.0 ≈ game mid); albedoMul ~0.4 soft ink (not pure black).
    float outlineThickness = 1.0f;
    float outlineAlbedoMul = 0.42f;
    bool  outlineUseVertexColor = true; // COLOR.r thickness
    float outlineTint[3] = { 0.08f, 0.07f, 0.07f };
    bool  outlineUseFixedTint = false; // false = darkened diffuse

    // Which outline backend
    enum class OutlineMode : uint8_t { None = 0, ZZZ, GI_Reserved } outlineMode = OutlineMode::ZZZ;
};

const char* RenderPassName(RenderPassId id);

} // namespace preview3d
