#include "PaintEngine.h"
#include "TileCache.h"
#include "HalfFloat.h"
#include <cmath>
#include <algorithm>
#include <cstring>

static inline void ReadPixelRaw(const uint8_t* p, CanvasPixelFormat fmt, float out[4]) {
    if (fmt == CanvasPixelFormat::RGBA8) {
        out[0] = p[0] / 255.0f;
        out[1] = p[1] / 255.0f;
        out[2] = p[2] / 255.0f;
        out[3] = p[3] / 255.0f;
    } else if (fmt == CanvasPixelFormat::RGBA16F) {
        HalfFloat::LoadRGBA16F(p, out);
    } else {
        std::memcpy(out, p, 4 * sizeof(float));
    }
}

static inline void WritePixelRaw(uint8_t* p, CanvasPixelFormat fmt, const float in[4]) {
    if (fmt == CanvasPixelFormat::RGBA8) {
        // Only U8 clamps to 0..1 — float docs preserve HDR / height values.
        p[0] = HalfFloat::FloatToU8(in[0]);
        p[1] = HalfFloat::FloatToU8(in[1]);
        p[2] = HalfFloat::FloatToU8(in[2]);
        p[3] = HalfFloat::FloatToU8(in[3]);
    } else if (fmt == CanvasPixelFormat::RGBA16F) {
        HalfFloat::StoreRGBA16F(p, in);
    } else {
        std::memcpy(p, in, 4 * sizeof(float));
    }
}

// ---------------------------------------------------------------------------
// Tip sampling helpers
// ---------------------------------------------------------------------------
static float SampleTipBilinear(const BrushTip& tip, float u, float v) {
    const int s = tip.size;
    if (s <= 0 || tip.pixels.empty()) return 0.f;
    const float maxC = (float)(s - 1);
    u = std::clamp(u, 0.f, maxC);
    v = std::clamp(v, 0.f, maxC);
    int x0 = (int)std::floor(u);
    int y0 = (int)std::floor(v);
    int x1 = std::min(x0 + 1, s - 1);
    int y1 = std::min(y0 + 1, s - 1);
    float tx = u - (float)x0;
    float ty = v - (float)y0;
    auto at = [&](int x, int y) -> float {
        return tip.pixels[(size_t)y * s + x] / 255.0f;
    };
    float a = at(x0, y0), b = at(x1, y0), c = at(x0, y1), d = at(x1, y1);
    return a * (1.f - tx) * (1.f - ty) + b * tx * (1.f - ty)
         + c * (1.f - tx) * ty       + d * tx * ty;
}

// Deterministic hash → [0,1) for scatter / angle jitter (stable per dab position)
static float Hash01(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x & 0xFFFFFFu) / 16777216.0f;
}

static float SoftFalloff(float dist, float r, float hardness) {
    if (dist >= r) return 0.f;
    if (hardness >= 0.999f) return 1.f;
    float core = r * hardness;
    if (dist <= core) return 1.f;
    float denom = r - core;
    if (denom < 1e-6f) return 1.f;
    return 1.f - (dist - core) / denom;
}

// ---------------------------------------------------------------------------
// Internal: single stamp at (px, py) into TileCache
// Supports tip rotation (rotationDeg) and hardness envelope on tips.
// ---------------------------------------------------------------------------
static void StampAt(TileCache& cache, float px, float py,
                    const BrushSettings& brush,
                    const std::vector<uint8_t>& selectionMask) {
    int width  = cache.GetWidth();
    int height = cache.GetHeight();
    CanvasPixelFormat fmt = cache.GetFormat();
    int bytesPerPixel = cache.GetBytesPerPixel();
    float r  = brush.radius;
    if (r < 0.5f) r = 0.5f;
    float h  = std::clamp(brush.hardness, 0.0f, 1.0f);
    float op = std::clamp(brush.opacity,  0.0f, 1.0f);

    // Effective tip rotation (radians). Pressure-rotation is folded into rotationDeg by Canvas.
    const float ang = brush.rotationDeg * (3.14159265358979323846f / 180.0f);
    const float cosA = std::cos(ang);
    const float sinA = std::sin(ang);
    const bool doRotate = std::fabs(ang) > 1e-5f;

    int startX = std::max(0, (int)std::floor(px - r));
    int endX   = std::min(width  - 1, (int)std::ceil(px + r));
    int startY = std::max(0, (int)std::floor(py - r));
    int endY   = std::min(height - 1, (int)std::ceil(py + r));

    if (startX > endX || startY > endY) return;

    int startTileX = startX / TILE_SIZE;
    int endTileX   = endX / TILE_SIZE;
    int startTileY = startY / TILE_SIZE;
    int endTileY   = endY / TILE_SIZE;

    const float r2 = r * r;
    const bool useTip = brush.tip && brush.tip->size > 0 &&
                        (int)brush.tip->pixels.size() >= brush.tip->size * brush.tip->size;
    const int tipSize = useTip ? brush.tip->size : 0;
    const float tipMax = useTip ? (float)(tipSize - 1) : 0.f;

    for (int ty = startTileY; ty <= endTileY; ++ty) {
        int tileY0 = ty * TILE_SIZE;
        int tileY1 = tileY0 + TILE_SIZE - 1;
        int py0 = std::max(startY, tileY0);
        int py1 = std::min(endY, tileY1);

        for (int tx = startTileX; tx <= endTileX; ++tx) {
            int tileX0 = tx * TILE_SIZE;
            int tileX1 = tileX0 + TILE_SIZE - 1;
            int px0 = std::max(startX, tileX0);
            int px1 = std::min(endX, tileX1);

            uint8_t* raw = cache.LockTile(tx, ty);

            for (int y = py0; y <= py1; ++y) {
                int ly = y - tileY0;
                uint8_t* row = raw + ((size_t)ly * TILE_SIZE) * bytesPerPixel;

                for (int x = px0; x <= px1; ++x) {
                    float dx = (float)x - px;
                    float dy = (float)y - py;
                    float dist2 = dx * dx + dy * dy;
                    if (dist2 >= r2) continue;

                    float intensity = 1.0f;
                    if (useTip) {
                        // Inverse-rotate offset into tip local space, then map to texture.
                        float lx = dx, ly2 = dy;
                        if (doRotate) {
                            lx  =  dx * cosA + dy * sinA;
                            ly2 = -dx * sinA + dy * cosA;
                        }
                        float u = (lx  / r + 1.0f) * 0.5f * tipMax;
                        float v = (ly2 / r + 1.0f) * 0.5f * tipMax;
                        // Outside tip square after rotation → no stamp
                        if (u < -0.5f || v < -0.5f || u > tipMax + 0.5f || v > tipMax + 0.5f)
                            continue;
                        intensity = SampleTipBilinear(*brush.tip, u, v);
                        if (intensity <= 1e-4f) continue;
                        // Soft hardness envelope on top of tip (A2)
                        if (h < 0.999f) {
                            float env = SoftFalloff(std::sqrt(dist2), r, h);
                            intensity *= env;
                            if (intensity <= 1e-4f) continue;
                        }
                    } else {
                        // Procedural soft circle — rotation is a no-op (radial).
                        if (h < 1.0f) {
                            intensity = SoftFalloff(std::sqrt(dist2), r, h);
                            if (intensity <= 1e-4f) continue;
                        }
                    }

                    float selVal = 1.0f;
                    if (!selectionMask.empty()) {
                        selVal = selectionMask[(size_t)y * width + x] / 255.0f;
                        if (selVal <= 0.0f) continue;
                    }

                    // Brush color.a is only used when writing alpha; in RGB-morph mode
                    // stamp strength comes from opacity × hardness × tip (color.a forced 1 by caller).
                    float stampAlpha = brush.color[3] * op * intensity * selVal;
                    if (stampAlpha <= 0.0f) continue;

                    int lxPix = x - tileX0;
                    uint8_t* p = row + (size_t)lxPix * bytesPerPixel;

                    float dest[4];
                    ReadPixelRaw(p, fmt, dest);
                    float out[4] = { dest[0], dest[1], dest[2], dest[3] };

                    // rgbMorphOnly: stamp is straight RGB lerp + optional A coverage
                    // (used when Channels→Alpha is OFF, or Alpha Rewrite is OFF).
                    // !writeA alone also uses morph so RGB-only channel locks stay simple.
                    const bool morphRgb = brush.rgbMorphOnly || !brush.writeA;

                    // Brush blend mode (Overlay, Multiply, …) vs destination RGB.
                    auto blendBrushRgb = [&](float sr, float sg, float sb,
                                             float dr, float dg, float db,
                                             float& or_, float& og, float& ob) {
                        or_ = sr; og = sg; ob = sb;
                        switch (brush.blendMode) {
                        case BlendMode::Multiply:
                            or_ = sr * dr; og = sg * dg; ob = sb * db; break;
                        case BlendMode::Screen:
                            or_ = 1.f - (1.f - sr) * (1.f - dr);
                            og = 1.f - (1.f - sg) * (1.f - dg);
                            ob = 1.f - (1.f - sb) * (1.f - db); break;
                        case BlendMode::Overlay:
                            or_ = (dr < 0.5f) ? 2.f * sr * dr : 1.f - 2.f * (1.f - sr) * (1.f - dr);
                            og = (dg < 0.5f) ? 2.f * sg * dg : 1.f - 2.f * (1.f - sg) * (1.f - dg);
                            ob = (db < 0.5f) ? 2.f * sb * db : 1.f - 2.f * (1.f - sb) * (1.f - db); break;
                        case BlendMode::Add:
                            or_ = std::min(sr + dr, 1.f); og = std::min(sg + dg, 1.f); ob = std::min(sb + db, 1.f); break;
                        case BlendMode::Subtract:
                            or_ = std::max(dr - sr, 0.f); og = std::max(dg - sg, 0.f); ob = std::max(db - sb, 0.f); break;
                        case BlendMode::Darken:
                            or_ = std::min(sr, dr); og = std::min(sg, dg); ob = std::min(sb, db); break;
                        case BlendMode::Lighten:
                            or_ = std::max(sr, dr); og = std::max(sg, dg); ob = std::max(sb, db); break;
                        case BlendMode::HardLight:
                            or_ = (sr < 0.5f) ? 2.f * sr * dr : 1.f - 2.f * (1.f - sr) * (1.f - dr);
                            og = (sg < 0.5f) ? 2.f * sg * dg : 1.f - 2.f * (1.f - sg) * (1.f - dg);
                            ob = (sb < 0.5f) ? 2.f * sb * db : 1.f - 2.f * (1.f - sb) * (1.f - db); break;
                        case BlendMode::SoftLight:
                            or_ = (sr < 0.5f) ? dr - (1.f - 2.f * sr) * dr * (1.f - dr)
                                              : dr + (2.f * sr - 1.f) * (std::sqrt(std::max(dr, 0.f)) - dr);
                            og = (sg < 0.5f) ? dg - (1.f - 2.f * sg) * dg * (1.f - dg)
                                              : dg + (2.f * sg - 1.f) * (std::sqrt(std::max(dg, 0.f)) - dg);
                            ob = (sb < 0.5f) ? db - (1.f - 2.f * sb) * db * (1.f - db)
                                              : db + (2.f * sb - 1.f) * (std::sqrt(std::max(db, 0.f)) - db); break;
                        case BlendMode::Normal:
                        default: break;
                        }
                        or_ = std::clamp(or_, 0.f, 1.f);
                        og = std::clamp(og, 0.f, 1.f);
                        ob = std::clamp(ob, 0.f, 1.f);
                    };

                    if (brush.erase) {
                        // Erase: pull written channels toward 0.
                        // writeA: fade coverage (rewrite ON) or morph strength (rewrite OFF).
                        float factor = 1.0f - stampAlpha;
                        if (brush.writeR) out[0] *= factor;
                        if (brush.writeG) out[1] *= factor;
                        if (brush.writeB) out[2] *= factor;
                        if (brush.writeA) out[3] *= factor;
                    } else {
                        // Source color: solid brush, or clone-stamp resample from offset
                        float br = brush.color[0], bg = brush.color[1], bb = brush.color[2];
                        float ba = brush.color[3];
                        if (brush.cloneStamp) {
                            int isx = (int)std::floor((float)x - brush.cloneOffsetX + 0.5f);
                            int isy = (int)std::floor((float)y - brush.cloneOffsetY + 0.5f);
                            if (isx < 0 || isy < 0 || isx >= width || isy >= height)
                                continue;
                            int stx = isx / TILE_SIZE, sty = isy / TILE_SIZE;
                            const uint8_t* sraw = cache.GetTileData(stx, sty);
                            if (!sraw) continue;
                            // Same-tile COW: dest tile is writable copy; source read is safe.
                            int slx = isx - stx * TILE_SIZE, sly = isy - sty * TILE_SIZE;
                            const uint8_t* sp = sraw + ((size_t)sly * TILE_SIZE + slx) * bytesPerPixel;
                            float srcPx[4];
                            ReadPixelRaw(sp, fmt, srcPx);
                            br = srcPx[0]; bg = srcPx[1]; bb = srcPx[2]; ba = srcPx[3];
                            stampAlpha *= std::clamp(ba, 0.f, 1.f);
                            if (stampAlpha <= 0.0f) continue;
                        }
                        if (brush.blendMode != BlendMode::Normal)
                            blendBrushRgb(br, bg, bb, dest[0], dest[1], dest[2], br, bg, bb);

                        if (morphRgb) {
                            // Brush: stamp = opacity×hardness×tip (color.a usually forced 1).
                            const float t = stampAlpha;
                            const float inv = 1.0f - t;
                            if (brush.writeR) out[0] = br * t + dest[0] * inv;
                            if (brush.writeG) out[1] = bg * t + dest[1] * inv;
                            if (brush.writeB) out[2] = bb * t + dest[2] * inv;
                            if (brush.writeA)
                                out[3] = stampAlpha + dest[3] * (1.0f - stampAlpha);
                        } else {
                            // Classic src-over (Alpha Rewrite ON + Channel A ON).
                            float outA = stampAlpha + dest[3] * (1.0f - stampAlpha);
                            if (outA > 0.0f) {
                                if (brush.writeR) out[0] = (br * stampAlpha + dest[0] * dest[3] * (1.0f - stampAlpha)) / outA;
                                if (brush.writeG) out[1] = (bg * stampAlpha + dest[1] * dest[3] * (1.0f - stampAlpha)) / outA;
                                if (brush.writeB) out[2] = (bb * stampAlpha + dest[2] * dest[3] * (1.0f - stampAlpha)) / outA;
                                if (brush.writeA) out[3] = outA;
                            }
                        }
                    }

                    WritePixelRaw(p, fmt, out);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
void PaintEngine::DrawStamp(TileCache& cache,
                             float cx, float cy, const BrushSettings& brush,
                             bool mirrorH, bool mirrorV,
                             const std::vector<uint8_t>& selectionMask) {
    int w = cache.GetWidth();
    int h = cache.GetHeight();

    StampAt(cache, cx, cy, brush, selectionMask);
    if (mirrorH)             StampAt(cache, (float)w - cx, cy,         brush, selectionMask);
    if (mirrorV)             StampAt(cache, cx,            (float)h-cy, brush, selectionMask);
    if (mirrorH && mirrorV)  StampAt(cache, (float)w - cx, (float)h-cy, brush, selectionMask);
}

// ---------------------------------------------------------------------------
void PaintEngine::DrawLine(TileCache& cache,
                            float x0, float y0, float x1, float y1,
                            const BrushSettings& brush,
                            bool mirrorH, bool mirrorV,
                            const std::vector<uint8_t>& selectionMask) {
    float dx = x1 - x0, dy = y1 - y0;
    float dist = std::sqrt(dx*dx + dy*dy);
    if (dist == 0.0f) {
        DrawStamp(cache, x0, y0, brush, mirrorH, mirrorV, selectionMask);
        return;
    }
    float step = std::max(1.0f, brush.radius * 0.1f);
    int   n    = (int)std::ceil(dist / step);
    for (int i = 0; i <= n; ++i) {
        float t = (float)i / n;
        DrawStamp(cache, x0 + dx*t, y0 + dy*t, brush, mirrorH, mirrorV, selectionMask);
    }
}

// ---------------------------------------------------------------------------
void PaintEngine::DrawStrokeSegment(TileCache& cache,
                                     float x0, float y0, float x1, float y1,
                                     const BrushSettings& brush,
                                     float& distanceAccumulator,
                                     float& lastDabX, float& lastDabY,
                                     bool mirrorH, bool mirrorV,
                                     const std::vector<uint8_t>& selectionMask) {
    float dx = x1 - x0, dy = y1 - y0;
    float segLen = std::sqrt(dx*dx + dy*dy);
    if (segLen == 0.0f) return;

    float spacingMul = (brush.tip) ? brush.tip->spacingMul : 1.0f;
    float spacing = std::max(1.0f, brush.radius * 2.0f * brush.spacing * spacingMul);
    float dirX    = dx / segLen, dirY = dy / segLen;
    float traveled = 0.0f;

    const float scatter = std::clamp(brush.scatter, 0.f, 1.f);
    const float angJit  = std::clamp(brush.angleJitter, 0.f, 1.f);
    const bool needDynamics = (scatter > 1e-4f) || (angJit > 1e-4f);

    while (traveled <= segLen) {
        float needed = spacing - distanceAccumulator;
        if (traveled + needed <= segLen) {
            traveled += needed;
            float dabX = x0 + dirX * traveled;
            float dabY = y0 + dirY * traveled;

            if (needDynamics) {
                // Deterministic noise from dab position (stable under same path).
                uint32_t hx = (uint32_t)std::lround(dabX * 64.f) * 73856093u
                            ^ (uint32_t)std::lround(dabY * 64.f) * 19349663u
                            ^ (uint32_t)std::lround(traveled * 32.f) * 83492791u;
                BrushSettings dab = brush;
                if (scatter > 1e-4f) {
                    float rx = Hash01(hx) * 2.f - 1.f;
                    float ry = Hash01(hx + 0x9E3779B9u) * 2.f - 1.f;
                    float rad = brush.radius * scatter;
                    dabX += rx * rad;
                    dabY += ry * rad;
                }
                if (angJit > 1e-4f) {
                    float j = Hash01(hx + 0x85EBCA6Bu) * 2.f - 1.f; // [-1,1]
                    dab.rotationDeg = brush.rotationDeg + j * angJit * 180.f;
                }
                DrawStamp(cache, dabX, dabY, dab, mirrorH, mirrorV, selectionMask);
            } else {
                DrawStamp(cache, dabX, dabY, brush, mirrorH, mirrorV, selectionMask);
            }
            lastDabX = dabX;
            lastDabY = dabY;
            distanceAccumulator = 0.0f;
        } else {
            float ex = x1 - lastDabX, ey = y1 - lastDabY;
            distanceAccumulator = std::sqrt(ex*ex + ey*ey);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Built-in tip presets (64x64 grayscale)
// ---------------------------------------------------------------------------
namespace {
BrushTip MakeRadialTip(int size, float hardness, float softnessPow, const char* name, float spacingMul) {
    BrushTip t;
    t.size = size;
    t.name = name;
    t.spacingMul = spacingMul;
    t.pixels.resize((size_t)size * size);
    float c = (size - 1) * 0.5f;
    float r = c;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = x - c, dy = y - c;
            float dist = std::sqrt(dx * dx + dy * dy);
            float v = 0.f;
            if (dist < r) {
                float nd = dist / r;
                if (nd <= hardness) v = 1.f;
                else {
                    float t01 = (nd - hardness) / std::max(1e-6f, 1.f - hardness);
                    v = std::pow(1.f - t01, softnessPow);
                }
            }
            t.pixels[(size_t)y * size + x] = (uint8_t)(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
        }
    }
    return t;
}
} // namespace

namespace BrushPresets {
const BrushTip& SoftRound() {
    static BrushTip tip = MakeRadialTip(64, 0.35f, 1.2f, "Soft Round", 1.0f);
    return tip;
}
const BrushTip& HardRound() {
    static BrushTip tip = MakeRadialTip(64, 0.92f, 4.0f, "Hard Round", 0.85f);
    return tip;
}
const BrushTip& Pencil() {
    static BrushTip tip = MakeRadialTip(32, 0.75f, 2.5f, "Pencil", 0.55f);
    return tip;
}
const BrushTip& Airbrush() {
    static BrushTip tip = MakeRadialTip(64, 0.05f, 0.65f, "Airbrush", 1.25f);
    return tip;
}
} // namespace BrushPresets
