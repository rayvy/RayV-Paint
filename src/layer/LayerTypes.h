#pragma once
// Layer type system: filters, styles, fill params.
// Kept free of D3D/ImGui so CPU compose and serialization can share it.

#include "../texset/TextureSetTypes.h"

#include <cstdint>
#include <string>
#include <vector>

// ---- Blend Modes ----
enum class BlendMode : uint8_t {
    Normal = 0,
    Multiply,
    Screen,
    Overlay,
    Add,
    Subtract,
    Darken,
    Lighten,
    HardLight,
    SoftLight
};

// ---- Non-destructive Pixel Filters ----
enum class FilterType : uint8_t {
    Blur = 0,
    HSV,
    Curves,
    AlphaInvert,
    Noise
};

struct LayerFilter {
    FilterType type = FilterType::Blur;
    bool enabled    = true;
    float p[4]      = {};  // blur=p[0]; hsv=p[0..2]; noise=p[0] strength,p[1] colorNoise
    // Curves legacy: single lut applied to enabled RGB channels when lutR/G/B empty.
    std::vector<float> lut; // 256 floats [0..1]
    // Per-channel LUTs (optional; empty = fall back to lut for RGB, identity for A)
    std::vector<float> lutR, lutG, lutB, lutA;
    // Control points for UI (optional serialize); 0=RGB master, 1..4 = R,G,B,A
    std::vector<std::pair<float, float>> curvePts[5];
    // bit0=R bit1=G bit2=B bit3=A. Default RGB on, A OFF.
    uint8_t curvesChannels = 0x7;
};

// ---- Layer Styles (visual derivatives: independent opacity) ----
enum class StyleType : uint8_t {
    Shadow = 0,
    Outline = 1
};

enum class OutlinePosition : uint8_t { Outside = 0, Inside = 1, Center = 2 };
enum class OutlineFillMode : uint8_t { Solid = 0, Gradient = 1, Texture = 2 };

struct GradientStop {
    float t = 0.f;
    float rgba[4] = {0.f, 0.f, 0.f, 1.f};
};

struct LayerStyle {
    StyleType type = StyleType::Shadow;
    bool enabled = true;
    float opacity = 1.f; // independent of layer fill opacity

    // Shadow
    float shadowColor[4] = {0.f, 0.f, 0.f, 1.f};
    float distance = 8.f;
    float angleDeg = 120.f;
    float offsetX = 0.f;
    float offsetY = 0.f;
    float spread = 0.f; // 0..100
    float size = 8.f;   // blur radius px

    // Outline
    float outlineColor[4] = {0.f, 0.f, 0.f, 1.f};
    float outlineSize = 2.f;
    OutlinePosition outlinePos = OutlinePosition::Outside;
    OutlineFillMode outlineFill = OutlineFillMode::Solid;
    std::vector<GradientStop> outlineGradient;
    std::string outlineTexturePath;
    // Cached outline texture (RGBA8). Loaded by Canvas when path changes.
    std::vector<uint8_t> outlineTextureRgba;
    int outlineTextureW = 0;
    int outlineTextureH = 0;
    float outlineTexScale[2] = {1.f, 1.f};
    float outlineTexOffset[2] = {0.f, 0.f};
    // Gradient mapping: 0 = distance-from-edge (ring strength), 1 = horizontal, 2 = vertical
    uint8_t outlineGradientMap = 0;
};

// ---- Fill Layer (cross-map RGBA) ----
// Philosophy: maps are textures. Fill writes plain RGBA into any enabled maps.
// Shared mask shapes all of them. Roles/labels are soft (3D hints), not required.

enum class FillValueMode : uint8_t {
    RGB = 0,
    Grayscale01,
    GrayscaleSigned
};

// Legacy enum kept for .rayp migration only
enum class FillChannelTarget : uint8_t {
    Diffuse = 0,
    Transparency,
    Metallic,
    Roughness,
    ShadowRamp,
    Specular,
    Glossiness,
    AO,
    Emission,
    Height,
    Count
};

// Soft role slot (optional labels / old files) — not the primary Fill UX
struct FillRoleSlot {
    texset::ChannelRole role = texset::ChannelRole::BaseColor;
    bool enabled = false;
    FillValueMode mode = FillValueMode::RGB;
    float rgba[4] = {1.f, 1.f, 1.f, 1.f};
    float gray = 1.f;
    void ResolveColor(float out[4]) const {
        if (mode == FillValueMode::RGB) {
            out[0] = rgba[0]; out[1] = rgba[1]; out[2] = rgba[2]; out[3] = rgba[3];
            return;
        }
        float g = gray;
        if (mode == FillValueMode::GrayscaleSigned) g = (gray + 1.f) * 0.5f;
        g = g < 0.f ? 0.f : (g > 1.f ? 1.f : g);
        out[0] = out[1] = out[2] = g; out[3] = 1.f;
    }
};

// One map target: enable + solid RGBA (the law)
struct FillMapColor {
    bool enabled = false;
    float rgba[4] = {1.f, 1.f, 1.f, 1.f};
};

struct FillLayerParams {
    // Primary model: per-MapKind solid color (Diffuse/LightMap/Material/Normal/…)
    FillMapColor mapColor[(int)texset::MapKind::Count] = {};

    // Optional texture multiply
    bool useTexture = false;
    std::string texturePath;
    float texScale[2] = {1.f, 1.f};
    float texOffset[2] = {0.f, 0.f};
    std::vector<uint8_t> textureRgba;
    int textureW = 0;
    int textureH = 0;

    // Soft / legacy
    std::vector<FillRoleSlot> roles;
    FillChannelTarget target = FillChannelTarget::Diffuse;
    FillValueMode mode = FillValueMode::RGB;
    float color[4] = {1.f, 1.f, 1.f, 1.f};
    float gray = 1.f;

    void EnsureDefaults() {
        // At least Diffuse on if nothing enabled
        bool any = false;
        for (int i = 0; i < (int)texset::MapKind::Count; ++i)
            if (mapColor[i].enabled) { any = true; break; }
        if (!any) {
            mapColor[(int)texset::MapKind::Diffuse].enabled = true;
            mapColor[(int)texset::MapKind::Diffuse].rgba[0] = color[0];
            mapColor[(int)texset::MapKind::Diffuse].rgba[1] = color[1];
            mapColor[(int)texset::MapKind::Diffuse].rgba[2] = color[2];
            mapColor[(int)texset::MapKind::Diffuse].rgba[3] = color[3];
        }
    }

    bool HasAnyEnabledMap() const {
        for (int i = 0; i < (int)texset::MapKind::Count; ++i)
            if (mapColor[i].enabled) return true;
        return false;
    }

    void ResolveRgba(float out[4]) const {
        // Prefer Diffuse map color, else first enabled map, else legacy color
        const auto& d = mapColor[(int)texset::MapKind::Diffuse];
        if (d.enabled) {
            out[0] = d.rgba[0]; out[1] = d.rgba[1]; out[2] = d.rgba[2]; out[3] = d.rgba[3];
            return;
        }
        for (int i = 0; i < (int)texset::MapKind::Count; ++i) {
            if (mapColor[i].enabled) {
                out[0] = mapColor[i].rgba[0]; out[1] = mapColor[i].rgba[1];
                out[2] = mapColor[i].rgba[2]; out[3] = mapColor[i].rgba[3];
                return;
            }
        }
        out[0] = color[0]; out[1] = color[1]; out[2] = color[2]; out[3] = color[3];
    }

    // Direct map write — no role packing required
    bool ResolveForMap(const std::vector<texset::MapSlot>& /*maps*/, texset::MapKind mapKind,
                       float outRgba[4]) const {
        int i = (int)mapKind;
        if (i < 0 || i >= (int)texset::MapKind::Count) return false;
        if (!mapColor[i].enabled) return false;
        outRgba[0] = mapColor[i].rgba[0];
        outRgba[1] = mapColor[i].rgba[1];
        outRgba[2] = mapColor[i].rgba[2];
        outRgba[3] = mapColor[i].rgba[3];
        return true;
    }

    bool HasTexture() const {
        return useTexture && !textureRgba.empty() && textureW > 0 && textureH > 0;
    }

    // Migrate old role-based fills → map colors
    void MigrateFromLegacy() {
        if (HasAnyEnabledMap()) return;
        if (!roles.empty()) {
            for (const auto& r : roles) {
                if (!r.enabled) continue;
                texset::MapKind mk = texset::MapKind::Diffuse;
                switch (r.role) {
                case texset::ChannelRole::ShadowRamp:
                case texset::ChannelRole::Metallic:
                case texset::ChannelRole::Roughness:
                case texset::ChannelRole::Glossiness:
                    mk = texset::MapKind::LightMap; break;
                case texset::ChannelRole::Opacity:
                case texset::ChannelRole::Specular:
                    mk = texset::MapKind::MaterialMap; break;
                case texset::ChannelRole::NormalX:
                case texset::ChannelRole::NormalY:
                case texset::ChannelRole::NormalZ:
                case texset::ChannelRole::AO:
                    mk = texset::MapKind::NormalMap; break;
                case texset::ChannelRole::Emission:
                    mk = texset::MapKind::GlowMap; break;
                case texset::ChannelRole::Height:
                    mk = texset::MapKind::ExtraMap; break;
                default: mk = texset::MapKind::Diffuse; break;
                }
                auto& mc = mapColor[(int)mk];
                mc.enabled = true;
                float col[4];
                r.ResolveColor(col);
                // merge: if already set keep RGB, overwrite if first
                if (mc.rgba[0] == 1.f && mc.rgba[1] == 1.f && mc.rgba[2] == 1.f) {
                    mc.rgba[0] = col[0]; mc.rgba[1] = col[1];
                    mc.rgba[2] = col[2]; mc.rgba[3] = col[3];
                }
            }
        }
        if (!HasAnyEnabledMap()) {
            mapColor[(int)texset::MapKind::Diffuse].enabled = true;
            mapColor[(int)texset::MapKind::Diffuse].rgba[0] = color[0];
            mapColor[(int)texset::MapKind::Diffuse].rgba[1] = color[1];
            mapColor[(int)texset::MapKind::Diffuse].rgba[2] = color[2];
            mapColor[(int)texset::MapKind::Diffuse].rgba[3] = color[3];
        }
    }
};

// Display / serialize name for fill target (legacy)
inline const char* FillChannelTargetName(FillChannelTarget t) {
    switch (t) {
    case FillChannelTarget::Transparency: return "Transparency";
    case FillChannelTarget::Metallic:     return "Metallic";
    case FillChannelTarget::Roughness:    return "Roughness";
    case FillChannelTarget::ShadowRamp:   return "ShadowRamp";
    case FillChannelTarget::Specular:     return "Specular";
    case FillChannelTarget::Glossiness:   return "Glossiness";
    case FillChannelTarget::AO:           return "AO";
    case FillChannelTarget::Emission:     return "Emission";
    case FillChannelTarget::Height:       return "Height";
    default: return "Diffuse";
    }
}
inline FillChannelTarget FillChannelTargetFromName(const std::string& s) {
    if (s == "Transparency" || s == "Opacity") return FillChannelTarget::Transparency;
    if (s == "Metallic")   return FillChannelTarget::Metallic;
    if (s == "Roughness")  return FillChannelTarget::Roughness;
    if (s == "ShadowRamp") return FillChannelTarget::ShadowRamp;
    if (s == "Specular")   return FillChannelTarget::Specular;
    if (s == "Glossiness") return FillChannelTarget::Glossiness;
    if (s == "AO")         return FillChannelTarget::AO;
    if (s == "Emission")   return FillChannelTarget::Emission;
    if (s == "Height")     return FillChannelTarget::Height;
    return FillChannelTarget::Diffuse;
}

// Helpers
inline bool LayerStyleListHasEnabled(const std::vector<LayerStyle>& styles) {
    for (const auto& s : styles)
        if (s.enabled) return true;
    return false;
}

inline bool LayerFilterListHasEnabled(const std::vector<LayerFilter>& filters) {
    for (const auto& f : filters)
        if (f.enabled) return true;
    return false;
}
