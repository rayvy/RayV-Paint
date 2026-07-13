#pragma once
// =============================================================================
// ContentAwareFill — module contract for Photoshop-like Content-Aware Fill
//
// Implementation target (for algorithm agent):
//   PatchMatch (Barnes, Goldman et al. 2009) + multi-scale pyramid + voting
//
// Location: src/utilities/  (pure algorithm, no ImGui / no D3D / no Canvas)
// Linked from RayVPaint_Core via CMake SOURCES.
//
// OWNERSHIP / LAYERS
// ------------------
// • This module MUST NOT include Canvas.h, imgui, d3d11, or UI code.
// • Canvas::ApplyContentAwareFill() is the only integration point:
//     - builds CafImage + CafMask from active layer + selection
//     - calls ContentAwareFill()
//     - writes result back with undo
//
// COORDINATE / PIXEL CONVENTIONS (match RayV-Paint stack)
// -------------------------------------------------------
// • Origin: top-left, +X right, +Y down (same as TileCache / document space).
// • Color: straight (non-premultiplied) RGBA, float channels in [0, 1].
//   (HDR/float docs may exceed 1 on RGB; do not clamp RGB on F16/F32 paths
//    unless params.clampRgb is true. Default clamp for U8-friendly algorithms.)
// • Layout: row-major, pixel i = y * w + x, channels rgba[i*4 + {0,1,2,3}].
// • Hole mask: uint8, same w×h.  0 = known (preserve), >0 = hole (synthesize).
//   Soft edges (1..254) MAY be treated as partial hole; v1 may hard-threshold >0.
// • Known region of the output MUST equal the input (within float eps ~1e-4)
//   except where holeMask > 0.
//
// ALGORITHM (recommended)
// -----------------------
// 1) Build multi-scale pyramid of image + hole mask (downsample by 2 each level).
// 2) Coarse level: initialize NN field (random or zero) for hole pixels.
// 3) PatchMatch iterations per level: propagation + random search.
// 4) Upsample NN field to finer level; refine with more iterations.
// 5) Voting: for each hole pixel, aggregate overlapping patch contributions
//    (weighted by Gaussian / similarity) → final RGBA.
// 6) Optional: a few Poisson / mean-shift blend passes at hole boundary.
//
// THREADING
// ---------
// • ContentAwareFill may use multi-threading internally.
// • Must be safe to call from a background thread IF progress callback is
//   only used for logging / atomic progress floats (no ImGui from worker).
// • Cancel: check params.cancelFlag if non-null between iterations.
//
// PERFORMANCE TARGETS (soft)
// --------------------------
// • 2K selection hole, patch 7, 4 scales: aim < 5s on mid CPU (v1).
// • Peak extra RAM: O(W*H) for pyramid + NN field (int2 or float2 per pixel).
// =============================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <atomic>

namespace caf {

// Full document / layer buffer (straight RGBA32F).
struct CafImage {
    int w = 0;
    int h = 0;
    // size must be w * h * 4
    std::vector<float> rgba;
};

// Hole / fill mask aligned with CafImage.
struct CafMask {
    int w = 0;
    int h = 0;
    // size must be w * h; 0 = keep, >0 = fill
    std::vector<uint8_t> hole;
};

struct CafParams {
    // PatchMatch
    int patchSize = 7;            // odd, typically 5/7/9
    int multiScaleLevels = 4;     // pyramid depth (>=1)
    int searchIters = 5;          // PatchMatch iterations per level
    int randomSearchRadius = 0;   // 0 = auto (image diagonal / 2 at that scale)
    uint32_t seed = 1;

    // Voting / reconstruction
    float voteSigma = 1.0f;       // spatial weight inside patch
    bool  clampRgb = true;        // clamp RGB to [0,1] after voting (U8 docs)

    // Optional progress: progress01 in [0,1], stage short ASCII tag
    std::function<void(float progress01, const char* stage)> progress;

    // Optional cooperative cancel (set true from UI thread)
    std::atomic<bool>* cancelFlag = nullptr;
};

struct CafResult {
    bool ok = false;
    std::string error;   // empty if ok
    CafImage filled;     // same w/h as input image; hole filled, known preserved
};

// -----------------------------------------------------------------------------
// Primary API — implement this in ContentAwareFill.cpp
// -----------------------------------------------------------------------------
// Preconditions:
//   image.w/h > 0, image.rgba.size() == w*h*4
//   hole.w/h match image, hole.hole.size() == w*h
//   at least one hole pixel (else return copy of image, ok=true)
//   at least one non-hole pixel (else error: nothing to sample from)
//
// Postconditions (ok=true):
//   filled.w/h == image.w/h, filled.rgba.size() == w*h*4
//   for all pixels with hole==0: |filled-image| < 1e-4 (or exact)
//
// On failure: ok=false, error set, filled may be empty.
// -----------------------------------------------------------------------------
CafResult ContentAwareFill(const CafImage& image,
                           const CafMask& holeMask,
                           const CafParams& params = CafParams{});

// Optional helper: true if any hole pixel
bool HasHole(const CafMask& holeMask);

// Optional helper: AABB of hole; returns false if empty
bool HoleBounds(const CafMask& holeMask, int& outX, int& outY, int& outW, int& outH);

} // namespace caf
