#include "ContentAwareFill.h"
#include <algorithm>
#include <cmath>

namespace caf {

bool HasHole(const CafMask& holeMask) {
    for (uint8_t v : holeMask.hole)
        if (v) return true;
    return false;
}

bool HoleBounds(const CafMask& holeMask, int& outX, int& outY, int& outW, int& outH) {
    if (holeMask.w < 1 || holeMask.h < 1 ||
        holeMask.hole.size() != (size_t)holeMask.w * holeMask.h)
        return false;
    int minX = holeMask.w, minY = holeMask.h, maxX = -1, maxY = -1;
    for (int y = 0; y < holeMask.h; ++y) {
        for (int x = 0; x < holeMask.w; ++x) {
            if (!holeMask.hole[(size_t)y * holeMask.w + x]) continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < minX) return false;
    outX = minX; outY = minY;
    outW = maxX - minX + 1;
    outH = maxY - minY + 1;
    return true;
}

// -----------------------------------------------------------------------------
// STUB IMPLEMENTATION
// Replace body with PatchMatch + pyramid + voting (see ContentAwareFill.h).
// Until then: fail clearly so UI can show a message (do not silently no-op).
// -----------------------------------------------------------------------------
CafResult ContentAwareFill(const CafImage& image,
                           const CafMask& holeMask,
                           const CafParams& params) {
    CafResult r;
    if (image.w < 1 || image.h < 1 ||
        image.rgba.size() != (size_t)image.w * image.h * 4) {
        r.error = "ContentAwareFill: invalid image buffer (expect W*H*4 float RGBA)";
        return r;
    }
    if (holeMask.w != image.w || holeMask.h != image.h ||
        holeMask.hole.size() != (size_t)image.w * image.h) {
        r.error = "ContentAwareFill: hole mask size mismatch";
        return r;
    }
    if (!HasHole(holeMask)) {
        // Nothing to fill — identity success
        r.ok = true;
        r.filled = image;
        return r;
    }
    // Need at least some known pixels to sample from
    bool anyKnown = false;
    for (uint8_t v : holeMask.hole) {
        if (!v) { anyKnown = true; break; }
    }
    if (!anyKnown) {
        r.error = "ContentAwareFill: entire image is hole — no source patches";
        return r;
    }

    if (params.progress)
        params.progress(0.f, "stub");

    // TODO(algorithm agent): PatchMatch multi-scale + voting.
    // Reference: Barnes et al. "PatchMatch: A Randomized Correspondence Algorithm
    // for Structural Image Editing", SIGGRAPH 2009.
    //
    // Suggested internal structures:
    //   struct NN { int16_t dx, dy; }; // offset from hole pixel to source
    //   std::vector<NN> nnf; // W*H, only hole pixels meaningful
    //   pyramid of CafImage + CafMask
    //
    // Do NOT write a naive full-image O(N^2) search for production.

    (void)params;
    r.error = "ContentAwareFill: algorithm not implemented yet "
              "(stub in utilities/ContentAwareFill.cpp). "
              "Implement PatchMatch + multi-scale + voting per header contract.";
    return r;
}

} // namespace caf
