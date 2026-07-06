#include "PaintEngine.h"
#include "TileCache.h"
#include <cmath>
#include <algorithm>
#include <cstring>

static inline void ReadPixelRaw(std::span<const uint8_t> p, CanvasPixelFormat fmt, std::span<float, 4> out) {
    if (fmt == CanvasPixelFormat::RGBA8) {
        out[0] = p[0] / 255.0f;
        out[1] = p[1] / 255.0f;
        out[2] = p[2] / 255.0f;
        out[3] = p[3] / 255.0f;
    } else {
        std::memcpy(out.data(), p.data(), 4 * sizeof(float));
    }
}

static inline void WritePixelRaw(std::span<uint8_t> p, CanvasPixelFormat fmt, std::span<const float, 4> in) {
    if (fmt == CanvasPixelFormat::RGBA8) {
        p[0] = (uint8_t)(std::clamp(in[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        p[1] = (uint8_t)(std::clamp(in[1], 0.0f, 1.0f) * 255.0f + 0.5f);
        p[2] = (uint8_t)(std::clamp(in[2], 0.0f, 1.0f) * 255.0f + 0.5f);
        p[3] = (uint8_t)(std::clamp(in[3], 0.0f, 1.0f) * 255.0f + 0.5f);
    } else {
        std::memcpy(p.data(), in.data(), 4 * sizeof(float));
    }
}

// ---------------------------------------------------------------------------
// Internal: single stamp at (px, py) into TileCache
// ---------------------------------------------------------------------------
static void StampAt(TileCache& cache, float px, float py,
                    const BrushSettings& brush,
                    const std::vector<uint8_t>& selectionMask) {
    int width  = cache.GetWidth();
    int height = cache.GetHeight();
    CanvasPixelFormat fmt = cache.GetFormat();
    int bytesPerPixel = cache.GetBytesPerPixel();
    float r  = brush.radius;
    float h  = std::clamp(brush.hardness, 0.0f, 1.0f);
    float op = std::clamp(brush.opacity,  0.0f, 1.0f);

    int startX = std::max(0, (int)std::floor(px - r));
    int endX   = std::min(width  - 1, (int)std::ceil(px + r));
    int startY = std::max(0, (int)std::floor(py - r));
    int endY   = std::min(height - 1, (int)std::ceil(py + r));

    if (startX > endX || startY > endY) return;

    int startTileX = startX / TILE_SIZE;
    int endTileX   = endX / TILE_SIZE;
    int startTileY = startY / TILE_SIZE;
    int endTileY   = endY / TILE_SIZE;

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
                    float dx = x - px;
                    float dy = y - py;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist >= r) continue;

                    float intensity = 1.0f;
                    if (h < 1.0f) {
                        float core = r * h;
                        if (dist > core) {
                            float denom = (r - core);
                            if (denom > 1e-6f) {
                                intensity = 1.0f - (dist - core) / denom;
                            }
                        }
                    }

                    float selVal = 1.0f;
                    if (!selectionMask.empty()) {
                        selVal = selectionMask[(size_t)y * width + x] / 255.0f;
                    }

                    float stampAlpha = brush.color[3] * op * intensity * selVal;
                    if (stampAlpha <= 0.0f) continue;

                    int lx = x - tileX0;
                    uint8_t* p = row + (size_t)lx * bytesPerPixel;

                    float dest[4];
                    ReadPixelRaw(std::span<const uint8_t>(p, (size_t)bytesPerPixel), fmt, std::span<float, 4>(dest, 4));
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
                            if (brush.writeR) out[0] = (brush.color[0] * stampAlpha + dest[0] * dest[3] * (1.0f - stampAlpha)) / outA;
                            if (brush.writeG) out[1] = (brush.color[1] * stampAlpha + dest[1] * dest[3] * (1.0f - stampAlpha)) / outA;
                            if (brush.writeB) out[2] = (brush.color[2] * stampAlpha + dest[2] * dest[3] * (1.0f - stampAlpha)) / outA;
                            if (brush.writeA) out[3] = outA;
                        }
                    }

                    WritePixelRaw(std::span<uint8_t>(p, (size_t)bytesPerPixel), fmt, std::span<const float, 4>(out, 4));
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
