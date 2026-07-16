#include "ScriptViewApi.h"
#include "ScriptDocApi.h"
#include <cmath>
#include <algorithm>

namespace script {
namespace {

ViewSnapshot g_View{};

// Match main.cpp mouse map: floor(pan + viewport*0.5)
inline void ScreenOrigin(const ViewSnapshot& v, float& ox, float& oy) {
    ox = std::floor(v.panX + v.viewportW * 0.5f);
    oy = std::floor(v.panY + v.viewportH * 0.5f);
}

} // namespace

void PublishViewSnapshot(const ViewSnapshot& s) {
    g_View = s;
    if (g_View.viewportW <= 0.f || g_View.viewportH <= 0.f ||
        g_View.docW <= 0 || g_View.docH <= 0 || g_View.zoom <= 1e-8f) {
        g_View.valid = false;
    }
}

void InvalidateViewSnapshot() {
    g_View.valid = false;
}

const ViewSnapshot& GetViewSnapshot() { return g_View; }

bool IsViewValid() { return g_View.valid; }

bool DocToScreen(float docX, float docY, float& outSx, float& outSy) {
    if (!g_View.valid) return false;

    float cx = docX;
    float cy = docY;
    // Mouse map applies flips after unrotate; invert first.
    if (g_View.flipH)
        cx = (float)g_View.docW - cx;
    if (g_View.flipV)
        cy = (float)g_View.docH - cy;

    const float centerX = g_View.docW * 0.5f;
    const float centerY = g_View.docH * 0.5f;
    const float dx = cx - centerX;
    const float dy = cy - centerY;
    const float cosA = std::cos(g_View.rotationRad);
    const float sinA = std::sin(g_View.rotationRad);
    // Inverse of: canvas = R * rel + center  (see main.cpp)
    const float relX = dx * cosA - dy * sinA;
    const float relY = dx * sinA + dy * cosA;
    const float rotatedX = relX + centerX;
    const float rotatedY = relY + centerY;

    float ox = 0.f, oy = 0.f;
    ScreenOrigin(g_View, ox, oy);
    const float localX = rotatedX * g_View.zoom + ox;
    const float localY = rotatedY * g_View.zoom + oy;
    outSx = g_View.imageMinX + localX;
    outSy = g_View.imageMinY + localY;
    return true;
}

bool ScreenToDoc(float sx, float sy, float& outDocX, float& outDocY) {
    if (!g_View.valid) return false;

    const float localX = sx - g_View.imageMinX;
    const float localY = sy - g_View.imageMinY;
    float ox = 0.f, oy = 0.f;
    ScreenOrigin(g_View, ox, oy);

    float rotatedX = (localX - ox) / g_View.zoom;
    float rotatedY = (localY - oy) / g_View.zoom;

    const float centerX = g_View.docW * 0.5f;
    const float centerY = g_View.docH * 0.5f;
    const float cosA = std::cos(g_View.rotationRad);
    const float sinA = std::sin(g_View.rotationRad);
    const float relX = rotatedX - centerX;
    const float relY = rotatedY - centerY;
    float canvasX = relX * cosA + relY * sinA + centerX;
    float canvasY = -relX * sinA + relY * cosA + centerY;

    if (g_View.flipH)
        canvasX = (float)g_View.docW - canvasX;
    if (g_View.flipV)
        canvasY = (float)g_View.docH - canvasY;

    outDocX = canvasX;
    outDocY = canvasY;
    return true;
}

bool ViewportScreenRect(float& outX, float& outY, float& outW, float& outH) {
    if (!g_View.valid) return false;
    outX = g_View.imageMinX;
    outY = g_View.imageMinY;
    outW = g_View.viewportW;
    outH = g_View.viewportH;
    return true;
}

bool SelectionScreenRect(float& outX, float& outY, float& outW, float& outH) {
    if (!g_View.valid) return false;
    int x = 0, y = 0, w = 0, h = 0;
    if (!SelectionBounds(x, y, w, h) || w <= 0 || h <= 0)
        return false;

    // Map four corners of document AABB → screen AABB (handles zoom/pan/rotation/flip).
    const float corners[4][2] = {
        {(float)x, (float)y},
        {(float)(x + w), (float)y},
        {(float)x, (float)(y + h)},
        {(float)(x + w), (float)(y + h)},
    };
    float minSx = 0.f, minSy = 0.f, maxSx = 0.f, maxSy = 0.f;
    for (int i = 0; i < 4; ++i) {
        float sx = 0.f, sy = 0.f;
        if (!DocToScreen(corners[i][0], corners[i][1], sx, sy))
            return false;
        if (i == 0) {
            minSx = maxSx = sx;
            minSy = maxSy = sy;
        } else {
            minSx = std::min(minSx, sx);
            minSy = std::min(minSy, sy);
            maxSx = std::max(maxSx, sx);
            maxSy = std::max(maxSy, sy);
        }
    }
    outX = minSx;
    outY = minSy;
    outW = std::max(0.f, maxSx - minSx);
    outH = std::max(0.f, maxSy - minSy);
    return outW > 0.5f && outH > 0.5f;
}

float ViewZoom() { return g_View.valid ? g_View.zoom : 1.f; }
float ViewPanX() { return g_View.valid ? g_View.panX : 0.f; }
float ViewPanY() { return g_View.valid ? g_View.panY : 0.f; }
float ViewRotationRad() { return g_View.valid ? g_View.rotationRad : 0.f; }

} // namespace script
