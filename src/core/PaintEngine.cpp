#include "PaintEngine.h"
#include "TileCache.h"
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Internal: single stamp at (px, py) into TileCache
// ---------------------------------------------------------------------------
static void StampAt(TileCache& cache, float px, float py,
                    const BrushSettings& brush,
                    const std::vector<uint8_t>& selectionMask) {
    int width  = cache.GetWidth();
    int height = cache.GetHeight();
    float r  = brush.radius;
    float h  = std::clamp(brush.hardness, 0.0f, 1.0f);
    float op = std::clamp(brush.opacity,  0.0f, 1.0f);

    int startX = std::max(0, (int)std::floor(px - r));
    int endX   = std::min(width  - 1, (int)std::ceil(px + r));
    int startY = std::max(0, (int)std::floor(py - r));
    int endY   = std::min(height - 1, (int)std::ceil(py + r));

    if (startX > endX || startY > endY) return;

    for (int y = startY; y <= endY; ++y) {
        for (int x = startX; x <= endX; ++x) {
            float dx   = x - px;
            float dy   = y - py;
            float dist = std::sqrt(dx*dx + dy*dy);
            if (dist >= r) continue;

            float intensity = 1.0f;
            if (h < 1.0f) {
                float core = r * h;
                if (dist > core)
                    intensity = 1.0f - (dist - core) / (r - core);
            }

            float selVal = 1.0f;
            if (!selectionMask.empty()) {
                selVal = selectionMask[(size_t)y * width + x] / 255.0f;
            }

            float stampAlpha = brush.color[3] * op * intensity * selVal;
            if (stampAlpha <= 0.0f) continue;

            float dest[4];
            cache.GetPixelF(x, y, dest);

            float out[4] = { dest[0], dest[1], dest[2], dest[3] };

            if (brush.erase) {
                float factor = 1.0f - stampAlpha;
                if (brush.writeR) out[0] *= factor;
                if (brush.writeG) out[1] *= factor;
                if (brush.writeB) out[2] *= factor;
                if (brush.writeA) out[3] *= factor;
            } else {
                float outA = stampAlpha + dest[3] * (1.0f - stampAlpha);
                if (outA > 0.0f) {
                    if (brush.writeR) out[0] = (brush.color[0]*stampAlpha + dest[0]*dest[3]*(1.0f-stampAlpha)) / outA;
                    if (brush.writeG) out[1] = (brush.color[1]*stampAlpha + dest[1]*dest[3]*(1.0f-stampAlpha)) / outA;
                    if (brush.writeB) out[2] = (brush.color[2]*stampAlpha + dest[2]*dest[3]*(1.0f-stampAlpha)) / outA;
                    if (brush.writeA) out[3] = outA;
                }
            }

            cache.SetPixelF(x, y, out);
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

    float spacing = std::max(1.0f, brush.radius * 2.0f * brush.spacing);
    float dirX    = dx / segLen, dirY = dy / segLen;
    float traveled = 0.0f;

    while (traveled <= segLen) {
        float needed = spacing - distanceAccumulator;
        if (traveled + needed <= segLen) {
            traveled += needed;
            float dabX = x0 + dirX * traveled;
            float dabY = y0 + dirY * traveled;
            DrawStamp(cache, dabX, dabY, brush, mirrorH, mirrorV, selectionMask);
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
