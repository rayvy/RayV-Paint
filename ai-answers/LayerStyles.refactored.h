#pragma once
// CPU algorithms for layer styles (shadow / outline) and presentation bake.
// Now supports GPU-accelerated paths via optional GpuFxContext*.
// Operates on flat float RGBA buffers (0..1). Used by export + GPU presentation cache.

#include "LayerTypes.h"
#include <vector>

// Forward declaration for GPU context
namespace gpu_fx { struct GpuFxContext; }

namespace layer_fx {

// Box blur separable (3 passes recommended by caller for gaussian approx).
// CPU-only — for export path. GPU path: GpuFxDispatch::ApplyBoxBlur
void BoxBlurH(std::vector<float>& px, int w, int h, int r, int channels = 1);
void BoxBlurV(std::vector<float>& px, int w, int h, int r, int channels = 1);
void BoxBlur(std::vector<float>& px, int w, int h, int r, int channels = 1, int passes = 3);

// Extract single-channel alpha from RGBA float buffer.
void ExtractAlpha(const float* rgba, int w, int h, std::vector<float>& alphaOut);

// Offset alpha mask by (dx, dy) pixels (positive y = down).
void OffsetMask(const std::vector<float>& src, int w, int h, int dx, int dy, std::vector<float>& dst);

// Max-filter dilate (r in px) on single-channel mask.
void DilateMask(const std::vector<float>& src, int w, int h, int r, std::vector<float>& dst);
void ErodeMask(const std::vector<float>& src, int w, int h, int r, std::vector<float>& dst);

// Spread 0..100: threshold boost before blur (choke-like).
void ApplySpread(std::vector<float>& alpha, float spread01);

// Composite src over dest (straight alpha).
void CompositeOver(float* destRgba, const float* srcRgba, int nPixels);
void CompositeOverPixel(float* d, float sr, float sg, float sb, float sa);

// Build drop-shadow RGBA (straight) from content alpha silhouette. Does not include content.
// fillOpacity is NOT applied — shadow uses style.opacity only.
void BuildShadowRgba(const float* contentRgba, int w, int h,
                     const LayerStyle& style,
                     std::vector<float>& shadowRgbaOut,
                     bool previewQuality = false);

// Build outline RGBA from content alpha.
// Supports Solid, Gradient (multi-stop), Texture (RGBA8 wrap sample).
void BuildOutlineRgba(const float* contentRgba, int w, int h,
                      const LayerStyle& style,
                      std::vector<float>& outlineRgbaOut,
                      bool previewQuality = false);

// Apply pixel filters in-place on RGBA float buffer.
// NEW: optional gpuCtx parameter enables GPU-accelerated path for HSV/Blur/Curves/AlphaInvert.
void ApplyPixelFilters(std::vector<float>& rgba, int w, int h,
                       const std::vector<LayerFilter>& filters,
                       gpu_fx::GpuFxContext* gpuCtx = nullptr);

// Multiply RGB content alpha by mask (0..255) and optional fillOpacity.
void ApplyMaskToAlpha(std::vector<float>& rgba, int w, int h,
                      const uint8_t* mask, size_t maskBytes, float fillOpacity);

// Full presentation:
//   shadows (style.opacity)  UNDER
//   content * mask * fillOpacity
//   outlines (style.opacity) OVER
// NEW: optional gpuCtx for GPU-accelerated shadow/outline build.
struct PresentationParams {
    float fillOpacity = 1.f;
    bool bakeFillOpacity = false;
    const uint8_t* mask = nullptr;
    size_t maskBytes = 0;
    bool hasMask = false;
    bool previewQuality = false;
};

std::vector<float> BuildPresentation(const std::vector<float>& contentRgba, int w, int h,
                                     const std::vector<LayerFilter>& filters,
                                     const std::vector<LayerStyle>& styles,
                                     const PresentationParams& params,
                                     gpu_fx::GpuFxContext* gpuCtx = nullptr);

// Whether presentation must bake fillOpacity (styles present) so GPU can draw at opacity=1.
inline bool NeedsBakedPresentation(const std::vector<LayerStyle>& styles) {
    return LayerStyleListHasEnabled(styles);
}

// Resolve fill content buffer (full W×H): solid color and/or tiled texture × color.
// NEW: optional gpuCtx for GPU texture sampling path.
void FillSolidBuffer(std::vector<float>& out, int w, int h, const FillLayerParams& fill,
                     gpu_fx::GpuFxContext* gpuCtx = nullptr);

// Sample gradient stops at t in [0,1] → rgba
void SampleGradient(const std::vector<GradientStop>& stops, float t, float outRgba[4]);

// Shared color conversion (used by both LayerStyles and Canvas — single definition)
void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v);
void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b);

} // namespace layer_fx