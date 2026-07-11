#pragma once
// Layer type system: filters, styles, fill params.
// Kept free of D3D/ImGui so CPU compose and serialization can share it.

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

// ---- Fill Layer ----
enum class FillChannelTarget : uint8_t {
    Diffuse = 0,
    Transparency,
    Metallic,
    Roughness
};

enum class FillValueMode : uint8_t {
    RGB = 0,
    Grayscale01,
    GrayscaleSigned
};

struct FillLayerParams {
    FillChannelTarget target = FillChannelTarget::Diffuse;
    FillValueMode mode = FillValueMode::RGB;
    float color[4] = {1.f, 1.f, 1.f, 1.f};
    float gray = 1.f;
    bool useTexture = false;
    std::string texturePath;
    float texScale[2] = {1.f, 1.f};
    float texOffset[2] = {0.f, 0.f};
    // Cached fill texture (RGBA8). Loaded by Canvas when path set/changed.
    std::vector<uint8_t> textureRgba;
    int textureW = 0;
    int textureH = 0;

    // Resolve solid color to RGBA in [0,1] for Diffuse path (other targets store-only for now).
    void ResolveRgba(float out[4]) const {
        if (mode == FillValueMode::RGB) {
            out[0] = color[0]; out[1] = color[1]; out[2] = color[2]; out[3] = color[3];
            return;
        }
        float g = gray;
        if (mode == FillValueMode::GrayscaleSigned)
            g = (gray + 1.f) * 0.5f;
        g = g < 0.f ? 0.f : (g > 1.f ? 1.f : g);
        out[0] = out[1] = out[2] = g;
        out[3] = 1.f;
    }

    bool HasTexture() const {
        return useTexture && !textureRgba.empty() && textureW > 0 && textureH > 0;
    }
};

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
