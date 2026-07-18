#include "VectorRasterizer.h"
#include "PathMath.h"
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstring>

namespace vec {
namespace {

struct Edge {
    float x0, y0, x1, y1;
    float dxdy;
};

inline void PremulBlend(uint8_t* dst, float r, float g, float b, float a) {
    if (a <= 0.f) return;
    a = std::clamp(a, 0.f, 1.f);
    float dr = dst[0] / 255.f, dg = dst[1] / 255.f, db = dst[2] / 255.f, da = dst[3] / 255.f;
    // src over
    float outA = a + da * (1.f - a);
    if (outA < 1e-6f) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }
    float outR = (r * a + dr * da * (1.f - a)) / outA;
    float outG = (g * a + dg * da * (1.f - a)) / outA;
    float outB = (b * a + db * da * (1.f - a)) / outA;
    dst[0] = (uint8_t)std::clamp(outR * 255.f + 0.5f, 0.f, 255.f);
    dst[1] = (uint8_t)std::clamp(outG * 255.f + 0.5f, 0.f, 255.f);
    dst[2] = (uint8_t)std::clamp(outB * 255.f + 0.5f, 0.f, 255.f);
    dst[3] = (uint8_t)std::clamp(outA * 255.f + 0.5f, 0.f, 255.f);
}

void ClearRect(TileCache& tiles, int x0, int y0, int x1, int y1) {
    // x1/y1 exclusive → FillRect inclusive
    if (x1 <= x0 || y1 <= y0) return;
    const float z[4] = {0, 0, 0, 0};
    tiles.FillRect(x0, y0, x1 - 1, y1 - 1, z);
}

// Scanline fill; optional linear gradient via c0→c1 along (gx0,gy0)→(gx1,gy1)
void FillLoops(TileCache& tiles, const std::vector<std::vector<V2>>& loops,
               int bx0, int by0, int bw, int bh,
               const float c0[4], const float c1[4], bool useGrad,
               float gx0, float gy0, float gx1, float gy1, bool /*evenOdd*/) {
    if (bw <= 0 || bh <= 0) return;
    std::vector<uint8_t> cover((size_t)bw * bh, 0);

    for (int y = 0; y < bh; ++y) {
        float fy = (float)(by0 + y) + 0.5f;
        std::vector<float> xs;
        xs.reserve(16);
        for (const auto& loop : loops) {
            if (loop.size() < 3) continue;
            for (size_t i = 0, n = loop.size(); i < n; ++i) {
                const V2& a0 = loop[i];
                const V2& a1 = loop[(i + 1) % n];
                if (std::abs(a0.y - a1.y) < 1e-12f) continue;
                if ((a0.y <= fy && a1.y > fy) || (a1.y <= fy && a0.y > fy)) {
                    float t = (fy - a0.y) / (a1.y - a0.y);
                    xs.push_back(a0.x + t * (a1.x - a0.x));
                }
            }
        }
        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            int xStart = std::max(0, (int)std::floor(xs[k]) - bx0);
            int xEnd = std::min(bw, (int)std::ceil(xs[k + 1]) - bx0);
            for (int x = xStart; x < xEnd; ++x)
                cover[(size_t)y * bw + x] = 255;
        }
    }

    float gdx = gx1 - gx0, gdy = gy1 - gy0;
    float glen2 = gdx * gdx + gdy * gdy;
    if (glen2 < 1e-8f) useGrad = false;

    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            if (!cover[(size_t)y * bw + x]) continue;
            float r = c0[0], g = c0[1], b = c0[2], a = c0[3];
            if (useGrad) {
                float px = (float)(bx0 + x) + 0.5f;
                float py = (float)(by0 + y) + 0.5f;
                float t = ((px - gx0) * gdx + (py - gy0) * gdy) / glen2;
                t = std::clamp(t, 0.f, 1.f);
                r = c0[0] + (c1[0] - c0[0]) * t;
                g = c0[1] + (c1[1] - c0[1]) * t;
                b = c0[2] + (c1[2] - c0[2]) * t;
                a = c0[3] + (c1[3] - c0[3]) * t;
            }
            if (a <= 0.f) continue;
            uint8_t px[4];
            tiles.GetPixelU8(bx0 + x, by0 + y, px);
            PremulBlend(px, r, g, b, a);
            tiles.SetPixelU8(bx0 + x, by0 + y, px);
        }
    }
}

void StampDisk(TileCache& tiles, float cx, float cy, float radius,
               float r, float g, float b, float a) {
    int x0 = (int)std::floor(cx - radius - 1);
    int y0 = (int)std::floor(cy - radius - 1);
    int x1 = (int)std::ceil(cx + radius + 1);
    int y1 = (int)std::ceil(cy + radius + 1);
    float r2 = radius * radius;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float dx = (x + 0.5f) - cx;
            float dy = (y + 0.5f) - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            float cov = 1.f;
            if (radius > 0.75f) {
                float d = std::sqrt(d2);
                cov = std::clamp(radius - d + 0.5f, 0.f, 1.f);
            }
            if (cov <= 0.f) continue;
            uint8_t px[4];
            tiles.GetPixelU8(x, y, px);
            PremulBlend(px, r, g, b, a * cov);
            tiles.SetPixelU8(x, y, px);
        }
    }
}

void StrokeLoops(TileCache& tiles, const std::vector<std::vector<V2>>& loops,
                 float width, float r, float g, float b, float a,
                 float dashLen, float gapLen) {
    float rad = std::max(0.5f, width * 0.5f);
    float step = std::max(0.5f, rad * 0.45f);
    const bool dashed = dashLen > 0.5f && gapLen > 0.5f;
    const float period = dashLen + gapLen;
    for (const auto& loop : loops) {
        if (loop.size() < 2) continue;
        float dist = 0.f;
        for (size_t i = 1; i < loop.size(); ++i) {
            V2 a0 = loop[i - 1], a1 = loop[i];
            float dx = a1.x - a0.x, dy = a1.y - a0.y;
            float len = std::sqrt(dx * dx + dy * dy);
            int n = std::max(1, (int)std::ceil(len / step));
            for (int k = 0; k <= n; ++k) {
                float t = (float)k / (float)n;
                float segD = dist + t * len;
                if (dashed) {
                    float phase = std::fmod(segD, period);
                    if (phase < 0.f) phase += period;
                    if (phase > dashLen) continue;
                }
                StampDisk(tiles, a0.x + dx * t, a0.y + dy * t, rad, r, g, b, a);
            }
            dist += len;
        }
    }
}

void PaintShape(TileCache& tiles, const Shape& s, int bx0, int by0, int bw, int bh, bool coarse) {
    if (!s.visible) return;
    std::vector<std::vector<V2>> loops;
    FlattenShape(s, coarse ? 2.5f : 0.75f, loops, coarse);
    if (loops.empty()) return;

    // Ensure closed loops for fill have repeated first point
    for (auto& loop : loops) {
        if (loop.size() >= 3) {
            const V2& a = loop.front();
            const V2& b = loop.back();
            if (std::abs(a.x - b.x) > 1e-3f || std::abs(a.y - b.y) > 1e-3f)
                loop.push_back(a);
        }
    }

    if (s.style.fillEnabled && s.kind != ShapeKind::Line) {
        float gx0 = s.style.gradX0, gy0 = s.style.gradY0, gx1 = s.style.gradX1, gy1 = s.style.gradY1;
        bool useGrad = (s.style.fillPaint == FillPaint::LinearGrad);
        if (useGrad && s.style.gradUseShapeBounds) {
            float x0, y0, x1, y1;
            if (ShapeLocalBounds(s, x0, y0, x1, y1)) {
                gx0 = x0; gy0 = (y0 + y1) * 0.5f;
                gx1 = x1; gy1 = gy0;
            }
        }
        const float* c0 = useGrad ? s.style.gradRgba0 : s.style.fillRgba;
        const float* c1 = useGrad ? s.style.gradRgba1 : s.style.fillRgba;
        FillLoops(tiles, loops, bx0, by0, bw, bh, c0, c1, useGrad,
                  gx0, gy0, gx1, gy1, s.style.fillRule != 0);
    }
    if (s.style.strokeEnabled && s.style.strokeWidth > 0.f) {
        // Open path stroke shouldn't double-close: use original flatten without force-close
        std::vector<std::vector<V2>> strokeLoops;
        FlattenShape(s, coarse ? 2.5f : 0.75f, strokeLoops, coarse);
        StrokeLoops(tiles, strokeLoops, s.style.strokeWidth,
                    s.style.strokeRgba[0], s.style.strokeRgba[1], s.style.strokeRgba[2], s.style.strokeRgba[3],
                    s.style.dashLen, s.style.gapLen);
    }
}

} // namespace

bool RasterizeDocument(const Document& doc, TileCache& tiles, int docW, int docH, bool coarse) {
    if (docW <= 0 || docH <= 0) return false;
    if (tiles.GetWidth() != docW || tiles.GetHeight() != docH)
        tiles.Init(docW, docH, CanvasPixelFormat::RGBA8);

    int x0, y0, x1, y1;
    if (doc.HasDirty()) {
        x0 = doc.dirtyX0; y0 = doc.dirtyY0; x1 = doc.dirtyX1; y1 = doc.dirtyY1;
    } else if (doc.rasterGen == doc.generation && !tiles.IsEmpty()) {
        return false; // up to date
    } else {
        float fx0, fy0, fx1, fy1;
        if (!DocumentBounds(doc, fx0, fy0, fx1, fy1)) {
            // empty doc — clear nothing
            return false;
        }
        x0 = (int)std::floor(fx0); y0 = (int)std::floor(fy0);
        x1 = (int)std::ceil(fx1); y1 = (int)std::ceil(fy1);
    }

    // Pad for AA / stroke
    x0 = std::max(0, x0 - 2);
    y0 = std::max(0, y0 - 2);
    x1 = std::min(docW, x1 + 2);
    y1 = std::min(docH, y1 + 2);
    if (x1 <= x0 || y1 <= y0) return false;

    // For correctness when shapes overlap dirty region: if dirty is partial, still
    // clear+redraw only dirty AABB (shapes clipped by paint bounds). Full redraw of
    // all shapes into that AABB can miss geometry outside — so expand to union of
    // all shape bounds that intersect dirty, or full doc if many shapes.
    float ux0 = (float)x0, uy0 = (float)y0, ux1 = (float)x1, uy1 = (float)y1;
    bool expanded = false;
    for (const auto& s : doc.shapes) {
        if (!s.visible) continue;
        float a, b, c, d;
        if (!ShapeBounds(s, a, b, c, d, true)) continue;
        if (c < ux0 || d < uy0 || a > ux1 || b > uy1) continue;
        // shape intersects — include full shape bounds in paint region
        ux0 = std::min(ux0, a); uy0 = std::min(uy0, b);
        ux1 = std::max(ux1, c); uy1 = std::max(uy1, d);
        expanded = true;
    }
    if (expanded) {
        x0 = std::max(0, (int)std::floor(ux0) - 2);
        y0 = std::max(0, (int)std::floor(uy0) - 2);
        x1 = std::min(docW, (int)std::ceil(ux1) + 2);
        y1 = std::min(docH, (int)std::ceil(uy1) + 2);
    }

    // Cap max region to avoid huge work (tile-stream by chunks if needed)
    const int maxDim = 4096;
    if (x1 - x0 > maxDim || y1 - y0 > maxDim) {
        // Fall back to tiled chunks
        for (int cy = y0; cy < y1; cy += maxDim) {
            for (int cx = x0; cx < x1; cx += maxDim) {
                int cx1 = std::min(x1, cx + maxDim);
                int cy1 = std::min(y1, cy + maxDim);
                ClearRect(tiles, cx, cy, cx1, cy1);
                int bw = cx1 - cx, bh = cy1 - cy;
                for (const auto& s : doc.shapes)
                    PaintShape(tiles, s, cx, cy, bw, bh, coarse);
            }
        }
        return true;
    }

    ClearRect(tiles, x0, y0, x1, y1);
    int bw = x1 - x0, bh = y1 - y0;
    for (const auto& s : doc.shapes)
        PaintShape(tiles, s, x0, y0, bw, bh, coarse);
    return true;
}

bool RasterizeDocumentFull(const Document& doc, TileCache& tiles, int docW, int docH, bool coarse) {
    // Always wipe GPU-facing sparse store first — prevents residual strokes after cancel/delete.
    tiles.Clear();
    tiles.Init(docW, docH, CanvasPixelFormat::RGBA8);
    if (doc.shapes.empty()) {
        // Empty vector layer: blank tiles are correct
        return true;
    }
    Document tmp = doc;
    tmp.MarkAllDirty(docW, docH);
    tmp.rasterGen = 0; // force paint path
    return RasterizeDocument(tmp, tiles, docW, docH, coarse);
}

} // namespace vec
