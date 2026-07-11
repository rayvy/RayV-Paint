#pragma once
// CPU algorithms for layer styles (shadow / outline) and presentation bake.
// Operates on flat float RGBA buffers (0..1). Used by export + GPU presentation cache.

#include "LayerTypes.h"
#include <vector>

namespace layer_fx {

// Box blur separable (3 passes recommended by caller for gaussian approx).
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

// Build outline RGBA from content alpha. Solid color only for now (gradient/texture → solid fallback).
void BuildOutlineRgba(const float* contentRgba, int w, int h,
                      const LayerStyle& style,
                      std::vector<float>& outlineRgbaOut,
                      bool previewQuality = false);

// Apply pixel filters in-place on RGBA float buffer.
void ApplyPixelFilters(std::vector<float>& rgba, int w, int h, const std::vector<LayerFilter>& filters);

// Multiply RGB content alpha by mask (0..255) and optional fillOpacity.
// silhouetteMode: if true, write alpha = contentA * mask only (no fillOpacity) into outAlphaOnly...
void ApplyMaskToAlpha(std::vector<float>& rgba, int w, int h,
                      const uint8_t* mask, size_t maskBytes, float fillOpacity);

// Full presentation:
//   shadows (style.opacity)  UNDER
//   content * mask * fillOpacity
//   outlines (style.opacity) OVER
// If styles empty: content * mask (fillOpacity left to caller/shader) — actually applies mask only.
// When bakeFillOpacity=true, multiplies content alpha by fillOpacity (for single-pass draw at opacity=1).
struct PresentationParams {
    float fillOpacity = 1.f;
    bool bakeFillOpacity = false; // true when styles force single GPU texture at opacity=1
    const uint8_t* mask = nullptr;
    size_t maskBytes = 0;
    bool hasMask = false;
    // Viewport preview: fewer blur passes + radius caps (much faster on large docs)
    bool previewQuality = false;
};

// contentRgba: raw layer content (fill solid or tiles), no mask yet.
// filters applied first, then styles.
std::vector<float> BuildPresentation(const std::vector<float>& contentRgba, int w, int h,
                                     const std::vector<LayerFilter>& filters,
                                     const std::vector<LayerStyle>& styles,
                                     const PresentationParams& params);

// Whether presentation must bake fillOpacity (styles present) so GPU can draw at opacity=1.
inline bool NeedsBakedPresentation(const std::vector<LayerStyle>& styles) {
    return LayerStyleListHasEnabled(styles);
}

// Resolve fill solid content buffer (full W×H). Prefer GPU 1×1 path when no filters/styles.
void FillSolidBuffer(std::vector<float>& out, int w, int h, const FillLayerParams& fill);

} // namespace layer_fx
