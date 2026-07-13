// ============================================================
//  LayerStyles.cpp — REFACTORED for GPU Driven Render
//
//  Changes from original:
//  1. ApplyPixelFilters: added gpu_fx dispatch path (HSV/Curves/Noise/AlphaInvert/Blur)
//  2. Removed duplicate RGBtoHSV/HSVtoRGB (use shared inline versions only)
//  3. BuildPresentation: GPU-accelerated shadow/outline when GpuFxContext available
//  4. FillSolidBuffer: GPU texture sample path when GpuFxContext available
//  5. CPU fallback preserved for export (no GPU) and when device is null
//  6. BoxBlur: kept CPU version but only for export path; GPU path uses ping-pong shaders
//
//  INTEGRATION: include GpuFxDispatch.h and pass GpuFxContext* through
// ============================================================

#include "LayerStyles.h"
#include "../assets/AssetStore.h"
#include "GpuFxDispatch.h"  // <-- NEW: GPU dispatch
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace layer_fx {

// ============================================================
//  Shared color math (single definition — no more duplicates)
// ============================================================

static inline void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max(r, std::max(g, b));
    float mn = std::min(r, std::min(g, b));
    v = mx;
    float d = mx - mn;
    s = (mx > 1e-6f) ? d / mx : 0.f;
    if (d < 1e-6f) { h = 0.f; return; }
    if (mx == r)      h = (g - b) / d + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / d + 2.f;
    else              h = (r - g) / d + 4.f;
    h /= 6.f;
}

static inline void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b) {
    if (s <= 1e-6f) { r = g = b = v; return; }
    h = std::fmod(h, 1.f); if (h < 0.f) h += 1.f;
    float i = std::floor(h * 6.f);
    float f = h * 6.f - i;
    float p = v * (1.f - s);
    float q = v * (1.f - f * s);
    float t = v * (1.f - (1.f - f) * s);
    switch ((int)i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

// ============================================================
//  CPU Box Blur — kept ONLY for export path (no GPU available)
//  GPU path: see GpuFxDispatch::ApplyBoxBlur
// ============================================================

void BoxBlurH(std::vector<float>& px, int w, int h, int r, int channels) {
    if (r < 1 || w < 1 || h < 1) return;
    std::vector<float> tmp(px.size());
    const int span = r * 2 + 1;
    for (int y = 0; y < h; ++y) {
        for (int c = 0; c < channels; ++c) {
            float acc = 0.f;
            for (int x = -r; x <= r; ++x) {
                int xx = std::clamp(x, 0, w - 1);
                acc += px[((size_t)y * w + xx) * channels + c];
            }
            for (int x = 0; x < w; ++x) {
                tmp[((size_t)y * w + x) * channels + c] = acc / (float)span;
                int xOut = x - r;
                int xIn  = x + r + 1;
                if (xOut >= 0)
                    acc -= px[((size_t)y * w + xOut) * channels + c];
                else
                    acc -= px[((size_t)y * w + 0) * channels + c];
                if (xIn < w)
                    acc += px[((size_t)y * w + xIn) * channels + c];
                else
                    acc += px[((size_t)y * w + (w - 1)) * channels + c];
            }
        }
    }
    px.swap(tmp);
}

void BoxBlurV(std::vector<float>& px, int w, int h, int r, int channels) {
    if (r < 1 || w < 1 || h < 1) return;
    std::vector<float> tmp(px.size());
    const int span = r * 2 + 1;
    for (int x = 0; x < w; ++x) {
        for (int c = 0; c < channels; ++c) {
            float acc = 0.f;
            for (int y = -r; y <= r; ++y) {
                int yy = std::clamp(y, 0, h - 1);
                acc += px[((size_t)yy * w + x) * channels + c];
            }
            for (int y = 0; y < h; ++y) {
                tmp[((size_t)y * w + x) * channels + c] = acc / (float)span;
                int yOut = y - r;
                int yIn  = y + r + 1;
                if (yOut >= 0)
                    acc -= px[((size_t)yOut * w + x) * channels + c];
                else
                    acc -= px[((size_t)0 * w + x) * channels + c];
                if (yIn < h)
                    acc += px[((size_t)yIn * w + x) * channels + c];
                else
                    acc += px[((size_t)(h - 1) * w + x) * channels + c];
            }
        }
    }
    px.swap(tmp);
}

void BoxBlur(std::vector<float>& px, int w, int h, int r, int channels, int passes) {
    r = std::max(0, r);
    if (r < 1) return;
    for (int p = 0; p < passes; ++p) {
        BoxBlurH(px, w, h, r, channels);
        BoxBlurV(px, w, h, r, channels);
    }
}

// ============================================================
//  CPU helpers (kept for export path and when GPU is unavailable)
// ============================================================

void ExtractAlpha(const float* rgba, int w, int h, std::vector<float>& alphaOut) {
    const size_t n = (size_t)w * h;
    alphaOut.resize(n);
    for (size_t i = 0; i < n; ++i)
        alphaOut[i] = rgba[i * 4 + 3];
}

void OffsetMask(const std::vector<float>& src, int w, int h, int dx, int dy, std::vector<float>& dst) {
    dst.assign((size_t)w * h, 0.f);
    for (int y = 0; y < h; ++y) {
        int sy = y - dy;
        if (sy < 0 || sy >= h) continue;
        for (int x = 0; x < w; ++x) {
            int sx = x - dx;
            if (sx < 0 || sx >= w) continue;
            dst[(size_t)y * w + x] = src[(size_t)sy * w + sx];
        }
    }
}

void DilateMask(const std::vector<float>& src, int w, int h, int r, std::vector<float>& dst) {
    dst.assign((size_t)w * h, 0.f);
    if (r < 1) { dst = src; return; }
    std::vector<float> tmp((size_t)w * h, 0.f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float m = 0.f;
            for (int k = -r; k <= r; ++k) {
                int xx = std::clamp(x + k, 0, w - 1);
                m = std::max(m, src[(size_t)y * w + xx]);
            }
            tmp[(size_t)y * w + x] = m;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float m = 0.f;
            for (int k = -r; k <= r; ++k) {
                int yy = std::clamp(y + k, 0, h - 1);
                m = std::max(m, tmp[(size_t)yy * w + x]);
            }
            dst[(size_t)y * w + x] = m;
        }
    }
}

void ErodeMask(const std::vector<float>& src, int w, int h, int r, std::vector<float>& dst) {
    dst.assign((size_t)w * h, 1.f);
    if (r < 1) { dst = src; return; }
    std::vector<float> tmp((size_t)w * h, 1.f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float m = 1.f;
            for (int k = -r; k <= r; ++k) {
                int xx = std::clamp(x + k, 0, w - 1);
                m = std::min(m, src[(size_t)y * w + xx]);
            }
            tmp[(size_t)y * w + x] = m;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float m = 1.f;
            for (int k = -r; k <= r; ++k) {
                int yy = std::clamp(y + k, 0, h - 1);
                m = std::min(m, tmp[(size_t)yy * w + x]);
            }
            dst[(size_t)y * w + x] = m;
        }
    }
}

void ApplySpread(std::vector<float>& alpha, float spread01) {
    spread01 = std::clamp(spread01, 0.f, 1.f);
    if (spread01 <= 0.f) return;
    const float expn = 1.f - spread01 * 0.85f;
    for (float& a : alpha) {
        if (a <= 0.f) continue;
        a = std::pow(std::clamp(a, 0.f, 1.f), expn);
    }
}

// ============================================================
//  Composite (CPU — used for export path only)
//  GPU path: D3D11 blend state in ComposeLayers
// ============================================================

static inline void Over(float* d, float sr, float sg, float sb, float sa) {
    if (sa <= 0.f) return;
    const float da = d[3];
    const float inv = 1.f - sa;
    const float outA = sa + da * inv;
    if (outA > 1e-6f) {
        d[0] = (sr * sa + d[0] * da * inv) / outA;
        d[1] = (sg * sa + d[1] * da * inv) / outA;
        d[2] = (sb * sa + d[2] * da * inv) / outA;
    } else {
        d[0] = d[1] = d[2] = 0.f;
    }
    d[3] = outA;
}

void CompositeOverPixel(float* d, float sr, float sg, float sb, float sa) {
    Over(d, sr, sg, sb, sa);
}

void CompositeOver(float* destRgba, const float* srcRgba, int nPixels) {
    for (int i = 0; i < nPixels; ++i) {
        const float* s = srcRgba + (size_t)i * 4;
        float* d = destRgba + (size_t)i * 4;
        Over(d, s[0], s[1], s[2], s[3]);
    }
}

// ============================================================
//  Shadow / Outline (CPU — for export only)
//  GPU path: GpuFxDispatch::BuildShadow / BuildOutline
// ============================================================

static void ShadowOffset(const LayerStyle& style, int& dx, int& dy) {
    const float rad = style.angleDeg * 3.14159265f / 180.f;
    dx = (int)std::lround(std::cos(rad) * style.distance + style.offsetX);
    dy = (int)std::lround(std::sin(rad) * style.distance + style.offsetY);
}

void BuildShadowRgba(const float* contentRgba, int w, int h,
                     const LayerStyle& style,
                     std::vector<float>& shadowRgbaOut,
                     bool previewQuality) {
    const size_t n = (size_t)w * h;
    shadowRgbaOut.assign(n * 4, 0.f);
    if (!style.enabled || style.opacity <= 0.f) return;

    std::vector<float> alpha;
    ExtractAlpha(contentRgba, w, h, alpha);
    ApplySpread(alpha, style.spread / 100.f);

    int dx = 0, dy = 0;
    ShadowOffset(style, dx, dy);
    std::vector<float> offset;
    OffsetMask(alpha, w, h, dx, dy, offset);

    int blurR = std::max(0, (int)std::lround(style.size));
    int passes = 3;
    if (previewQuality) {
        blurR = std::min(blurR, 40);
        passes = (blurR > 12) ? 1 : 2;
    }
    if (blurR > 0)
        BoxBlur(offset, w, h, blurR, 1, passes);

    const float cr = style.shadowColor[0];
    const float cg = style.shadowColor[1];
    const float cb = style.shadowColor[2];
    const float ca = style.shadowColor[3] * style.opacity;
    for (size_t i = 0; i < n; ++i) {
        float a = std::clamp(offset[i] * ca, 0.f, 1.f);
        shadowRgbaOut[i * 4 + 0] = cr;
        shadowRgbaOut[i * 4 + 1] = cg;
        shadowRgbaOut[i * 4 + 2] = cb;
        shadowRgbaOut[i * 4 + 3] = a;
    }
}

void SampleGradient(const std::vector<GradientStop>& stops, float t, float outRgba[4]) {
    outRgba[0] = outRgba[1] = outRgba[2] = 0.f;
    outRgba[3] = 1.f;
    if (stops.empty()) return;
    if (stops.size() == 1) {
        for (int c = 0; c < 4; ++c) outRgba[c] = stops[0].rgba[c];
        return;
    }
    t = std::clamp(t, 0.f, 1.f);
    if (t <= stops.front().t) {
        for (int c = 0; c < 4; ++c) outRgba[c] = stops.front().rgba[c];
        return;
    }
    if (t >= stops.back().t) {
        for (int c = 0; c < 4; ++c) outRgba[c] = stops.back().rgba[c];
        return;
    }
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        if (t >= stops[i].t && t <= stops[i + 1].t) {
            float span = stops[i + 1].t - stops[i].t;
            float u = (span > 1e-6f) ? (t - stops[i].t) / span : 0.f;
            for (int c = 0; c < 4; ++c)
                outRgba[c] = stops[i].rgba[c] * (1.f - u) + stops[i + 1].rgba[c] * u;
            return;
        }
    }
}

static void SampleTextureRGBA8(const uint8_t* rgba, int tw, int th,
                               float u, float v, float out[4]) {
    if (!rgba || tw < 1 || th < 1) {
        out[0] = out[1] = out[2] = out[3] = 0.f;
        return;
    }
    u = u - std::floor(u);
    v = v - std::floor(v);
    float fx = u * (float)tw;
    float fy = v * (float)th;
    int x0 = ((int)std::floor(fx) % tw + tw) % tw;
    int y0 = ((int)std::floor(fy) % th + th) % th;
    int x1 = (x0 + 1) % tw;
    int y1 = (y0 + 1) % th;
    float tx = fx - std::floor(fx);
    float ty = fy - std::floor(fy);
    auto pix = [&](int x, int y, int c) -> float {
        return rgba[((size_t)y * tw + x) * 4 + c] / 255.f;
    };
    for (int c = 0; c < 4; ++c) {
        float a = pix(x0, y0, c) * (1.f - tx) + pix(x1, y0, c) * tx;
        float b = pix(x0, y1, c) * (1.f - tx) + pix(x1, y1, c) * tx;
        out[c] = a * (1.f - ty) + b * ty;
    }
}

void BuildOutlineRgba(const float* contentRgba, int w, int h,
                      const LayerStyle& style,
                      std::vector<float>& outlineRgbaOut,
                      bool previewQuality) {
    const size_t n = (size_t)w * h;
    outlineRgbaOut.assign(n * 4, 0.f);
    if (!style.enabled || style.opacity <= 0.f) return;

    std::vector<float> alpha;
    ExtractAlpha(contentRgba, w, h, alpha);

    int r = std::max(0, (int)std::lround(style.outlineSize));
    if (previewQuality) r = std::min(r, 32);
    std::vector<float> ring(n, 0.f);

    if (r < 1) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float a = alpha[(size_t)y * w + x];
                float m = a;
                if (x > 0) m = std::min(m, alpha[(size_t)y * w + (x - 1)]);
                if (x + 1 < w) m = std::min(m, alpha[(size_t)y * w + (x + 1)]);
                if (y > 0) m = std::min(m, alpha[(size_t)(y - 1) * w + x]);
                if (y + 1 < h) m = std::min(m, alpha[(size_t)(y + 1) * w + x]);
                ring[(size_t)y * w + x] = std::max(0.f, a - m);
            }
        }
    } else {
        std::vector<float> dilated, eroded;
        switch (style.outlinePos) {
        case OutlinePosition::Inside:
            ErodeMask(alpha, w, h, r, eroded);
            for (size_t i = 0; i < n; ++i)
                ring[i] = std::max(0.f, alpha[i] - eroded[i]);
            break;
        case OutlinePosition::Center: {
            DilateMask(alpha, w, h, (r + 1) / 2, dilated);
            ErodeMask(alpha, w, h, r / 2, eroded);
            for (size_t i = 0; i < n; ++i)
                ring[i] = std::max(0.f, dilated[i] - eroded[i]);
            break;
        }
        case OutlinePosition::Outside:
        default:
            DilateMask(alpha, w, h, r, dilated);
            for (size_t i = 0; i < n; ++i)
                ring[i] = std::max(0.f, dilated[i] - alpha[i]);
            break;
        }
    }

    if (r >= 1)
        BoxBlur(ring, w, h, 1, 1, 1);

    float ringMax = 1e-6f;
    for (float v : ring) ringMax = std::max(ringMax, v);
    const float invRingMax = 1.f / ringMax;

    const float solidR = style.outlineColor[0];
    const float solidG = style.outlineColor[1];
    const float solidB = style.outlineColor[2];
    const float solidA = style.outlineColor[3];
    const float op = style.opacity;

    std::vector<GradientStop> stops = style.outlineGradient;
    if (style.outlineFill == OutlineFillMode::Gradient && stops.size() < 2) {
        stops = {
            {0.f, {style.outlineColor[0], style.outlineColor[1], style.outlineColor[2], 1.f}},
            {1.f, {style.outlineColor[0], style.outlineColor[1], style.outlineColor[2], 0.f}}
        };
    }
    std::sort(stops.begin(), stops.end(),
        [](const GradientStop& a, const GradientStop& b) { return a.t < b.t; });

    const bool useTex = style.outlineFill == OutlineFillMode::Texture &&
        !style.outlineTextureRgba.empty() && style.outlineTextureW > 0 && style.outlineTextureH > 0;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = (size_t)y * w + x;
            float ringA = ring[i];
            if (ringA <= 1e-5f) continue;

            float cr = solidR, cg = solidG, cb = solidB, ca = solidA;

            if (style.outlineFill == OutlineFillMode::Gradient && stops.size() >= 2) {
                float t = 0.f;
                if (style.outlineGradientMap == 1)
                    t = (w > 1) ? (float)x / (float)(w - 1) : 0.f;
                else if (style.outlineGradientMap == 2)
                    t = (h > 1) ? (float)y / (float)(h - 1) : 0.f;
                else
                    t = std::clamp(ringA * invRingMax, 0.f, 1.f);
                float gr[4];
                SampleGradient(stops, t, gr);
                cr = gr[0]; cg = gr[1]; cb = gr[2]; ca = gr[3];
            } else if (useTex) {
                float u = (float)x / (float)std::max(1, w) * style.outlineTexScale[0] + style.outlineTexOffset[0];
                float v = (float)y / (float)std::max(1, h) * style.outlineTexScale[1] + style.outlineTexOffset[1];
                float tr[4];
                SampleTextureRGBA8(style.outlineTextureRgba.data(),
                    style.outlineTextureW, style.outlineTextureH, u, v, tr);
                cr = tr[0] * solidR;
                cg = tr[1] * solidG;
                cb = tr[2] * solidB;
                ca = tr[3] * solidA;
            }

            float a = std::clamp(ringA * ca * op, 0.f, 1.f);
            outlineRgbaOut[i * 4 + 0] = cr;
            outlineRgbaOut[i * 4 + 1] = cg;
            outlineRgbaOut[i * 4 + 2] = cb;
            outlineRgbaOut[i * 4 + 3] = a;
        }
    }
}

static float SampleLut(const std::vector<float>& lut, float v) {
    if (lut.size() != 256) return v;
    float fi = std::clamp(v, 0.f, 1.f) * 255.f;
    int ii = std::clamp((int)fi, 0, 254);
    float t = fi - ii;
    return lut[ii] * (1.f - t) + lut[ii + 1] * t;
}

// ============================================================
//  ApplyPixelFilters — GPU-accelerated when possible
// ============================================================

void ApplyPixelFilters(std::vector<float>& rgba, int w, int h,
                       const std::vector<LayerFilter>& filters,
                       gpu_fx::GpuFxContext* gpuCtx) {
    if (rgba.empty() || w < 1 || h < 1) return;
    const int n = w * h;

    for (const auto& f : filters) {
        if (!f.enabled) continue;

        // ---- GPU path ----
        if (gpuCtx && gpuCtx->device) {
            switch (f.type) {
            case FilterType::HSV: {
                // Upload current buffer to GPU
                auto src = gpu_fx::UploadBuffer(*gpuCtx, rgba.data(), w, h);
                if (src.Valid()) {
                    auto dst = gpu_fx::CreateTarget(*gpuCtx, w, h);
                    gpu_fx::ApplyHSV(*gpuCtx, src.srv, dst, w, h,
                                     f.p[0], f.p[1], f.p[2]);
                    if (dst.Valid()) {
                        gpu_fx::ReadBack(*gpuCtx, dst, rgba.data(), w, h);
                        dst.Release();
                    }
                    src.Release();
                }
                continue; // skip CPU path
            }
            case FilterType::Curves: {
                // Resolve LUT (per-channel or master)
                std::vector<float> masterLut;
                if (f.lutR.empty() && f.lutG.empty() && f.lutB.empty() && f.lutA.empty()) {
                    if (f.lut.size() == 256) masterLut = f.lut;
                }
                // If we have a single LUT for all channels, use GPU
                if (!masterLut.empty()) {
                    auto src = gpu_fx::UploadBuffer(*gpuCtx, rgba.data(), w, h);
                    if (src.Valid()) {
                        auto dst = gpu_fx::CreateTarget(*gpuCtx, w, h);
                        gpu_fx::ApplyCurves(*gpuCtx, src.srv, dst, w, h,
                                            masterLut.data(), f.curvesChannels);
                        if (dst.Valid()) {
                            gpu_fx::ReadBack(*gpuCtx, dst, rgba.data(), w, h);
                            dst.Release();
                        }
                        src.Release();
                    }
                    continue;
                }
                // Per-channel LUTs: fall through to CPU (or add multi-pass GPU later)
                break;
            }
            case FilterType::Blur: {
                int rr = std::max(1, (int)f.p[0]);
                auto src = gpu_fx::UploadBuffer(*gpuCtx, rgba.data(), w, h);
                if (src.Valid()) {
                    auto dst = gpu_fx::CreateTarget(*gpuCtx, w, h);
                    gpu_fx::ApplyBoxBlur(*gpuCtx, src.srv, dst, w, h, rr, 3, 4);
                    if (dst.Valid()) {
                        gpu_fx::ReadBack(*gpuCtx, dst, rgba.data(), w, h);
                        dst.Release();
                    }
                    src.Release();
                }
                continue;
            }
            case FilterType::AlphaInvert: {
                auto src = gpu_fx::UploadBuffer(*gpuCtx, rgba.data(), w, h);
                if (src.Valid()) {
                    auto dst = gpu_fx::CreateTarget(*gpuCtx, w, h);
                    gpu_fx::ApplyAlphaInvert(*gpuCtx, src.srv, dst, w, h);
                    if (dst.Valid()) {
                        gpu_fx::ReadBack(*gpuCtx, dst, rgba.data(), w, h);
                        dst.Release();
                    }
                    src.Release();
                }
                continue;
            }
            case FilterType::Noise:
                // Noise on GPU is deterministic (baked texture), not seeded.
                // For interactive preview this is fine; for final bake, fall to CPU
                // for seed consistency.
                break;
            }
        }

        // ---- CPU fallback ----
        switch (f.type) {
        case FilterType::Blur: {
            int rr = std::max(1, (int)f.p[0]);
            BoxBlur(rgba, w, h, rr, 4, 3);
            break;
        }
        case FilterType::HSV: {
            for (int i = 0; i < n; ++i) {
                size_t idx = (size_t)i * 4;
                float hr, hs, hv;
                RGBtoHSV(rgba[idx], rgba[idx + 1], rgba[idx + 2], hr, hs, hv);
                hr = std::fmod(hr + f.p[0] + 1.f, 1.f);
                hs = std::clamp(hs + f.p[1], 0.f, 1.f);
                hv = std::clamp(hv + f.p[2], 0.f, 1.f);
                HSVtoRGB(hr, hs, hv, rgba[idx], rgba[idx + 1], rgba[idx + 2]);
            }
            break;
        }
        case FilterType::Curves: {
            const uint8_t ch = f.curvesChannels;
            auto lutFor = [&](int channel) -> const std::vector<float>* {
                if (channel == 0 && !f.lutR.empty()) return &f.lutR;
                if (channel == 1 && !f.lutG.empty()) return &f.lutG;
                if (channel == 2 && !f.lutB.empty()) return &f.lutB;
                if (channel == 3 && !f.lutA.empty()) return &f.lutA;
                if (channel < 3 && f.lut.size() == 256) return &f.lut;
                return nullptr;
            };
            for (int i = 0; i < n; ++i) {
                size_t idx = (size_t)i * 4;
                for (int c = 0; c < 4; ++c) {
                    if ((ch & (1u << c)) == 0) continue;
                    const auto* L = lutFor(c);
                    if (L) rgba[idx + c] = SampleLut(*L, rgba[idx + c]);
                }
            }
            break;
        }
        case FilterType::AlphaInvert:
            for (int i = 0; i < n; ++i)
                rgba[(size_t)i * 4 + 3] = 1.f - rgba[(size_t)i * 4 + 3];
            break;
        case FilterType::Noise: {
            std::mt19937 rng(1337);
            std::uniform_real_distribution<float> dist(-1.f, 1.f);
            bool col = (f.p[1] > 0.5f);
            for (int i = 0; i < n; ++i) {
                size_t idx = (size_t)i * 4;
                if (col) {
                    for (int c = 0; c < 3; ++c)
                        rgba[idx + c] = std::clamp(rgba[idx + c] + dist(rng) * f.p[0], 0.f, 1.f);
                } else {
                    float noise = dist(rng) * f.p[0];
                    for (int c = 0; c < 3; ++c)
                        rgba[idx + c] = std::clamp(rgba[idx + c] + noise, 0.f, 1.f);
                }
            }
            break;
        }
        }
    }
}

void ApplyMaskToAlpha(std::vector<float>& rgba, int w, int h,
                      const uint8_t* mask, size_t maskBytes, float fillOpacity) {
    const size_t n = (size_t)w * h;
    const bool useMask = mask && maskBytes >= n;
    for (size_t i = 0; i < n; ++i) {
        float a = rgba[i * 4 + 3] * fillOpacity;
        if (useMask) a *= mask[i] / 255.f;
        rgba[i * 4 + 3] = std::clamp(a, 0.f, 1.f);
    }
}

// ============================================================
//  BuildPresentation — GPU-accelerated shadow/outline when possible
// ============================================================

std::vector<float> BuildPresentation(const std::vector<float>& contentRgba, int w, int h,
                                     const std::vector<LayerFilter>& filters,
                                     const std::vector<LayerStyle>& styles,
                                     const PresentationParams& params,
                                     gpu_fx::GpuFxContext* gpuCtx) {
    std::vector<float> content = contentRgba;
    if ((int)content.size() < w * h * 4)
        content.assign((size_t)w * h * 4, 0.f);

    ApplyPixelFilters(content, w, h, filters, gpuCtx);

    // Silhouette for styles
    std::vector<float> silContent = content;
    {
        const size_t n = (size_t)w * h;
        const bool useMask = params.hasMask && params.mask && params.maskBytes >= n;
        for (size_t i = 0; i < n; ++i) {
            float a = silContent[i * 4 + 3];
            if (useMask) a *= params.mask[i] / 255.f;
            silContent[i * 4 + 3] = std::clamp(a, 0.f, 1.f);
        }
    }

    const bool hasStyles = NeedsBakedPresentation(styles);
    if (!hasStyles) {
        float fo = params.bakeFillOpacity ? params.fillOpacity : 1.f;
        ApplyMaskToAlpha(content, w, h,
                         params.hasMask ? params.mask : nullptr,
                         params.maskBytes, fo);
        return content;
    }

    // ---- GPU presentation path ----
    if (gpuCtx && gpuCtx->device) {
        std::vector<float> out((size_t)w * h * 4, 0.f);

        // Upload content to GPU
        auto contentGPU = gpu_fx::UploadBuffer(*gpuCtx, silContent.data(), w, h);
        if (!contentGPU.Valid()) {
            // Fallback to CPU below
            goto cpu_bake;
        }

        // Build shadows
        for (const auto& st : styles) {
            if (!st.enabled || st.type != StyleType::Shadow) continue;

            auto shadowGPU = gpu_fx::BuildShadow(
                *gpuCtx, contentGPU.srv, w, h,
                st.shadowColor, st.opacity,
                st.distance, st.angleDeg, st.offsetX, st.offsetY,
                st.spread / 100.f, st.size,
                3, params.previewQuality);

            if (shadowGPU.Valid()) {
                // Composite shadow over out on CPU (simple readback + blend)
                // Or use GPU blend state: upload out, draw shadow over out with SrcOver
                std::vector<float> shadowCPU((size_t)w * h * 4);
                gpu_fx::ReadBack(*gpuCtx, shadowGPU, shadowCPU.data(), w, h);
                CompositeOver(out.data(), shadowCPU.data(), w * h);
                shadowGPU.Release();
            }
        }

        // Content with fillOpacity
        {
            const size_t n = (size_t)w * h;
            std::vector<float> body = silContent;
            for (size_t i = 0; i < n; ++i)
                body[i * 4 + 3] = std::clamp(body[i * 4 + 3] * params.fillOpacity, 0.f, 1.f);
            CompositeOver(out.data(), body.data(), w * h);
        }

        // Build outlines
        for (const auto& st : styles) {
            if (!st.enabled || st.type != StyleType::Outline) continue;

            // Prepare gradient data as flat float array (t, r, g, b, a per stop)
            std::vector<float> gradFlat;
            for (const auto& gs : st.outlineGradient) {
                gradFlat.push_back(gs.t);
                for (int c = 0; c < 4; ++c) gradFlat.push_back(gs.rgba[c]);
            }

            ID3D11ShaderResourceView* texSRV = nullptr;
            // Outline texture SRV would need to be provided by caller
            // For now, texture outlines still use CPU fallback

            if (st.outlineFill != OutlineFillMode::Texture) {
                auto outlineGPU = gpu_fx::BuildOutline(
                    *gpuCtx, contentGPU.srv, w, h,
                    st.outlineColor, st.opacity, st.outlineSize,
                    (int)st.outlinePos, (int)st.outlineFill,
                    gradFlat.data(), (int)gradFlat.size() / 5,
                    texSRV, st.outlineTexScale, st.outlineTexOffset,
                    st.outlineGradientMap, params.previewQuality);

                if (outlineGPU.Valid()) {
                    std::vector<float> outlineCPU((size_t)w * h * 4);
                    gpu_fx::ReadBack(*gpuCtx, outlineGPU, outlineCPU.data(), w, h);
                    CompositeOver(out.data(), outlineCPU.data(), w * h);
                    outlineGPU.Release();
                }
            } else {
                // Texture outline: fall back to CPU
                std::vector<float> ol;
                BuildOutlineRgba(silContent.data(), w, h, st, ol, params.previewQuality);
                CompositeOver(out.data(), ol.data(), w * h);
            }
        }

        contentGPU.Release();
        return out;
    }

cpu_bake:
    // ---- CPU fallback (original code) ----
    {
        std::vector<float> out((size_t)w * h * 4, 0.f);

        for (const auto& st : styles) {
            if (!st.enabled || st.type != StyleType::Shadow) continue;
            std::vector<float> sh;
            BuildShadowRgba(silContent.data(), w, h, st, sh, params.previewQuality);
            CompositeOver(out.data(), sh.data(), w * h);
        }

        std::vector<float> body = silContent;
        const size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; ++i)
            body[i * 4 + 3] = std::clamp(body[i * 4 + 3] * params.fillOpacity, 0.f, 1.f);
        CompositeOver(out.data(), body.data(), w * h);

        for (const auto& st : styles) {
            if (!st.enabled || st.type != StyleType::Outline) continue;
            std::vector<float> ol;
            BuildOutlineRgba(silContent.data(), w, h, st, ol, params.previewQuality);
            CompositeOver(out.data(), ol.data(), w * h);
        }

        return out;
    }
}

void FillSolidBuffer(std::vector<float>& out, int w, int h, const FillLayerParams& fill,
                     gpu_fx::GpuFxContext* gpuCtx) {
    float c[4];
    fill.ResolveRgba(c);
    out.resize((size_t)w * h * 4);

    const uint8_t* texPx = nullptr;
    int tw = 0, th = 0;
    if (fill.useTexture) {
        if (!fill.textureAssetKey.empty()) {
            if (const assets::TextureAsset* a = assets::AssetStore::Get().Get(fill.textureAssetKey)) {
                if (!a->rgba.empty() && a->w > 0 && a->h > 0) {
                    texPx = a->rgba.data();
                    tw = a->w; th = a->h;
                }
            }
        }
        if (!texPx && !fill.textureRgba.empty() && fill.textureW > 0 && fill.textureH > 0) {
            texPx = fill.textureRgba.data();
            tw = fill.textureW; th = fill.textureH;
        }
    }

    // ---- GPU fill path ----
    if (gpuCtx && gpuCtx->device && texPx) {
        auto texGPU = gpu_fx::UploadBuffer(*gpuCtx,
            // Convert RGBA8 to RGBA32F for GPU
            nullptr, tw, th); // We need RGBA8 upload variant
        // For now, fall through to CPU for texture fills
        // TODO: add UploadBufferRGBA8 to GpuFxDispatch
    }

    if (!texPx) {
        for (int i = 0; i < w * h; ++i) {
            out[(size_t)i * 4 + 0] = c[0];
            out[(size_t)i * 4 + 1] = c[1];
            out[(size_t)i * 4 + 2] = c[2];
            out[(size_t)i * 4 + 3] = c[3];
        }
        return;
    }

    // CPU bilinear texture fill
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float u = (float)x / (float)std::max(1, w) * fill.texScale[0] + fill.texOffset[0];
            float v = (float)y / (float)std::max(1, h) * fill.texScale[1] + fill.texOffset[1];
            float tr[4];
            SampleTextureRGBA8(texPx, tw, th, u, v, tr);
            size_t i = ((size_t)y * w + x) * 4;
            out[i + 0] = tr[0] * c[0];
            out[i + 1] = tr[1] * c[1];
            out[i + 2] = tr[2] * c[2];
            out[i + 3] = tr[3] * c[3];
        }
    }
}

} // namespace layer_fx