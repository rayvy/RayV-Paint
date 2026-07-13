#include "ContentAwareFill.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <limits>
#include <cstring>

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

// =============================================================================
// PatchMatch + multi-scale pyramid + gaussian voting.
// Reference: Barnes et al., "PatchMatch: A Randomized Correspondence Algorithm
// for Structural Image Editing", SIGGRAPH 2009, plus the standard "inpainting
// via NNF voting" extension used by most content-aware-fill implementations.
//
// Design notes specific to RayVPaint's needs:
//  - Fully deterministic given the same (seed, level, iteration, pixel):
//    randomness is a hash of those four things, NOT a shared PRNG stream
//    advanced across threads/rows. That means re-baking a stroke from the
//    stored {params, seed} after undo/redo reproduces bit-identical output
//    regardless of thread count / scheduling.
//  - Validity checks ("is this candidate source patch entirely non-hole?")
//    use a summed-area table over the hole mask -> O(1) per check instead of
//    O(patchSize^2), which is what actually keeps this off the naive-O(N^2)
//    path the header warns about.
//  - Everything that is a pure gather (random search, voting, distance
//    refresh) is trivially row-parallel with std::thread. Propagation is a
//    serial scanline dependency chain by construction and is left serial;
//    it's cheap relative to the rest.
// =============================================================================

namespace detail {

inline int idx(int x, int y, int w) { return y * w + x; }

inline bool isHole(const CafMask& m, int x, int y) {
    return m.hole[(size_t)idx(x, y, m.w)] != 0;
}

// ---- deterministic per-pixel RNG -------------------------------------------

inline uint32_t hash32(uint32_t a) {
    a ^= a >> 16; a *= 0x7feb352du;
    a ^= a >> 15; a *= 0x846ca68bu;
    a ^= a >> 16;
    return a;
}

// phase distinguishes init (-1), NNF-upsample-repair (-2), vs. iteration index (>=0)
inline uint32_t pixelSeed(uint32_t baseSeed, int level, int phase, int x, int y) {
    uint32_t h = baseSeed ? baseSeed : 1u;
    h = hash32(h ^ (uint32_t)level * 0x9E3779B1u);
    h = hash32(h ^ (uint32_t)phase * 0x85EBCA6Bu);
    h = hash32(h ^ (uint32_t)x     * 0xC2B2AE35u);
    h = hash32(h ^ (uint32_t)y     * 0x27D4EB2Fu);
    return h ? h : 0x1u;
}

struct XorShift32 {
    uint32_t s;
    explicit XorShift32(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    // inclusive [lo, hi]
    int range(int lo, int hi) {
        if (hi <= lo) return lo;
        uint32_t span = (uint32_t)(hi - lo + 1);
        return lo + (int)(next() % span);
    }
};

// ---- summed-area table over the hole mask, for O(1) patch validity --------

struct HoleSAT {
    int w = 0, h = 0;
    std::vector<int64_t> sat; // (w+1)*(h+1) inclusive prefix sums

    void build(const CafMask& mask) {
        w = mask.w; h = mask.h;
        sat.assign((size_t)(w + 1) * (h + 1), 0);
        for (int y = 0; y < h; ++y) {
            int64_t rowSum = 0;
            const size_t rowIn = (size_t)y * w;
            const size_t rowOutPrev = (size_t)y * (w + 1);
            const size_t rowOut = (size_t)(y + 1) * (w + 1);
            for (int x = 0; x < w; ++x) {
                rowSum += mask.hole[rowIn + x] ? 1 : 0;
                sat[rowOut + x + 1] = sat[rowOutPrev + x + 1] + rowSum;
            }
        }
    }

    // inclusive rect [x0,x1] x [y0,y1]; caller guarantees in-bounds
    int64_t rectHoleCount(int x0, int y0, int x1, int y1) const {
        return sat[(size_t)(y1 + 1) * (w + 1) + (x1 + 1)]
             - sat[(size_t)(y0)     * (w + 1) + (x1 + 1)]
             - sat[(size_t)(y1 + 1) * (w + 1) + (x0)]
             + sat[(size_t)(y0)     * (w + 1) + (x0)];
    }
};

// Source patch must be fully known (no hole) and in-bounds.
// When searchRadius >= 0, also require (sx,sy) within searchRadius of hole pixel (hx,hy)
// — Photoshop-like "Auto" sampling area, not whole-canvas search.
inline bool sourcePatchValid(const HoleSAT& sat, int w, int h, int sx, int sy, int r,
                             int hx = -1, int hy = -1, int searchRadius = -1) {
    if (sx - r < 0 || sy - r < 0 || sx + r >= w || sy + r >= h) return false;
    if (searchRadius >= 0 && hx >= 0 && hy >= 0) {
        int ddx = sx - hx, ddy = sy - hy;
        // Slightly prefer not sampling exactly on the hole pixel itself
        if (ddx == 0 && ddy == 0) return false;
        int64_t r2 = (int64_t)searchRadius * searchRadius;
        if ((int64_t)ddx * ddx + (int64_t)ddy * ddy > r2) return false;
    }
    return sat.rectHoleCount(sx - r, sy - r, sx + r, sy + r) == 0;
}

// Local "Auto" search radius around a hole pixel (per pyramid level).
// Scales with hole size but stays finite — never the full canvas diagonal.
inline int autoSearchRadius(int w, int h, int holeW, int holeH, int patchRadius) {
    const float holeDiag = std::sqrt((float)holeW * holeW + (float)holeH * holeH);
    // ~3× hole extent or 12× patch, capped so we don't roam the whole doc
    int byHole = (int)std::ceil(holeDiag * 2.5f) + patchRadius * 2;
    int byPatch = patchRadius * 16;
    int cap = std::max(w, h) / 3; // hard cap: never more than ~1/3 of longest side
    int r = std::max(byHole, byPatch);
    r = std::max(r, patchRadius * 6 + 16);
    if (cap > 0) r = std::min(r, cap);
    return std::max(1, r);
}

// ---- nearest-neighbour field -------------------------------------------

struct NNF {
    int w = 0, h = 0;
    std::vector<int32_t> dx, dy;
    std::vector<float> dist;

    void init(int w_, int h_) {
        w = w_; h = h_;
        size_t n = (size_t)w * h;
        dx.assign(n, 0);
        dy.assign(n, 0);
        dist.assign(n, std::numeric_limits<float>::max());
    }
};

// ---- patch SSD, clamp-to-edge on target only (source is pre-validated) ----

inline float patchDistance(const CafImage& img, int tx, int ty, int sx, int sy,
                            int r, float cutoff) {
    const int w = img.w, h = img.h;
    float d = 0.f;
    for (int j = -r; j <= r; ++j) {
        int ty2 = ty + j; ty2 = ty2 < 0 ? 0 : (ty2 >= h ? h - 1 : ty2);
        int sy2 = sy + j; // guaranteed in-bounds by caller's validity check
        const size_t rowT = (size_t)ty2 * w;
        const size_t rowS = (size_t)sy2 * w;
        for (int i = -r; i <= r; ++i) {
            int tx2 = tx + i; tx2 = tx2 < 0 ? 0 : (tx2 >= w ? w - 1 : tx2);
            int sx2 = sx + i;
            const size_t ti = (rowT + tx2) * 4;
            const size_t si = (rowS + sx2) * 4;
            for (int c = 0; c < 4; ++c) {
                float diff = img.rgba[ti + c] - img.rgba[si + c];
                d += diff * diff;
            }
        }
        if (d > cutoff) return d; // early-out: caller only cares "worse than cutoff"
    }
    return d;
}

// ---- threading helper -------------------------------------------------

template <typename Fn>
void parallelForRows(int h, int numThreads, Fn&& fn) {
    numThreads = std::max(1, std::min(numThreads, h));
    if (numThreads <= 1) { fn(0, h); return; }
    std::vector<std::thread> pool;
    pool.reserve(numThreads);
    int chunk = (h + numThreads - 1) / numThreads;
    for (int t = 0; t < numThreads; ++t) {
        int y0 = t * chunk, y1 = std::min(h, y0 + chunk);
        if (y0 >= y1) break;
        pool.emplace_back(fn, y0, y1);
    }
    for (auto& th : pool) th.join();
}

// ---- pyramid level ------------------------------------------------------

struct Level {
    CafImage image; // known pixels: exact values. hole pixels: meaningless until filled.
    CafMask  mask;  // binary: 0 keep / 1 hole, downsampled from the finest level
};

CafMask makeBinaryMask(const CafMask& src) {
    CafMask m; m.w = src.w; m.h = src.h; m.hole.resize(src.hole.size());
    for (size_t i = 0; i < src.hole.size(); ++i) m.hole[i] = src.hole[i] ? 1 : 0;
    return m;
}

Level downsampleLevel(const Level& src) {
    const int sw = src.image.w, sh = src.image.h;
    const int dw = std::max(1, sw / 2), dh = std::max(1, sh / 2);
    Level dst;
    dst.image.w = dw; dst.image.h = dh; dst.image.rgba.assign((size_t)dw * dh * 4, 0.f);
    dst.mask.w = dw; dst.mask.h = dh; dst.mask.hole.assign((size_t)dw * dh, 0);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            const int sx0 = x * 2, sy0 = y * 2;
            float sum[4] = {0, 0, 0, 0};
            int cnt = 0; bool anyHole = false;
            for (int dyy = 0; dyy < 2; ++dyy) {
                for (int dxx = 0; dxx < 2; ++dxx) {
                    int sx = std::min(sx0 + dxx, sw - 1);
                    int sy = std::min(sy0 + dyy, sh - 1);
                    if (isHole(src.mask, sx, sy)) { anyHole = true; continue; }
                    // average only known pixels: hole pixels in the source
                    // buffer may hold caller-uninitialized garbage.
                    size_t sid = (size_t)idx(sx, sy, sw);
                    for (int c = 0; c < 4; ++c) sum[c] += src.image.rgba[sid * 4 + c];
                    ++cnt;
                }
            }
            size_t did = (size_t)idx(x, y, dw);
            if (cnt > 0) for (int c = 0; c < 4; ++c) dst.image.rgba[did * 4 + c] = sum[c] / (float)cnt;
            // "any" rule: never lose hole coverage on the way down.
            dst.mask.hole[did] = anyHole ? 1 : 0;
        }
    }
    return dst;
}

std::vector<Level> buildPyramid(const CafImage& image, const CafMask& binaryMask,
                                 int maxLevels, int patchRadius) {
    std::vector<Level> pyr;
    pyr.push_back(Level{ image, binaryMask });
    for (int lvl = 1; lvl < maxLevels; ++lvl) {
        const Level& prev = pyr.back();
        if (prev.image.w <= patchRadius * 2 + 2 || prev.image.h <= patchRadius * 2 + 2)
            break; // stop before degenerate tiny levels
        pyr.push_back(downsampleLevel(prev));
    }
    return pyr;
}

// Bootstrap hole with LOCAL color near the hole — not whole-image mean.
// Global average (skin+hair+bg+highlight) was producing muddy gray fills.
// For each hole pixel: distance-weighted mean of known samples in expanding window.
void bootstrapFill(CafImage& working, const CafMask& mask, int baseRadius) {
    const int w = mask.w, h = mask.h;
    baseRadius = std::max(baseRadius, 8);
    const int maxR = std::max(w, h);

    // Last-resort global mean only if a pixel never finds local known samples
    double gSum[4] = {0, 0, 0, 0};
    size_t gCnt = 0;
    for (size_t i = 0; i < mask.hole.size(); ++i) {
        if (mask.hole[i]) continue;
        for (int c = 0; c < 4; ++c) gSum[c] += working.rgba[i * 4 + c];
        ++gCnt;
    }
    float gMean[4] = {0.5f, 0.5f, 0.5f, 1.f};
    if (gCnt > 0) for (int c = 0; c < 4; ++c) gMean[c] = (float)(gSum[c] / (double)gCnt);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!mask.hole[(size_t)idx(x, y, w)]) continue;

            float mean[4] = {gMean[0], gMean[1], gMean[2], gMean[3]};
            bool found = false;
            for (int R = baseRadius; R <= maxR && !found; R = (R < 48 ? R * 2 : R + 48)) {
                double s2[4] = {0, 0, 0, 0};
                double wsum = 0;
                int x0 = std::max(0, x - R), x1 = std::min(w - 1, x + R);
                int y0 = std::max(0, y - R), y1 = std::min(h - 1, y + R);
                for (int yy = y0; yy <= y1; ++yy) {
                    for (int xx = x0; xx <= x1; ++xx) {
                        if (mask.hole[(size_t)idx(xx, yy, w)]) continue;
                        float ddx = (float)(xx - x), ddy = (float)(yy - y);
                        float dist = std::sqrt(ddx * ddx + ddy * ddy);
                        float wgt = 1.f / (1.f + dist * 0.15f);
                        size_t sid = (size_t)idx(xx, yy, w);
                        for (int c = 0; c < 4; ++c) s2[c] += working.rgba[sid * 4 + c] * wgt;
                        wsum += wgt;
                    }
                }
                if (wsum > 1e-6) {
                    for (int c = 0; c < 4; ++c) mean[c] = (float)(s2[c] / wsum);
                    found = true;
                }
            }
            size_t did = (size_t)idx(x, y, w);
            for (int c = 0; c < 4; ++c) working.rgba[did * 4 + c] = mean[c];
        }
    }
}

void bilinearUpsampleHoleRegion(const CafImage& coarse, CafImage& fineWorking, const CafMask& fineMask) {
    const int fw = fineWorking.w, fh = fineWorking.h;
    const int cw = coarse.w, ch = coarse.h;
    for (int y = 0; y < fh; ++y) {
        for (int x = 0; x < fw; ++x) {
            if (!isHole(fineMask, x, y)) continue; // known pixel already exact
            float gx = (x + 0.5f) * cw / (float)fw - 0.5f;
            float gy = (y + 0.5f) * ch / (float)fh - 0.5f;
            int x0 = (int)std::floor(gx), y0 = (int)std::floor(gy);
            float fx = gx - x0, fy = gy - y0;
            int x1 = std::min(x0 + 1, cw - 1), y1 = std::min(y0 + 1, ch - 1);
            x0 = std::clamp(x0, 0, cw - 1); y0 = std::clamp(y0, 0, ch - 1);
            size_t id00 = (size_t)idx(x0, y0, cw), id10 = (size_t)idx(x1, y0, cw);
            size_t id01 = (size_t)idx(x0, y1, cw), id11 = (size_t)idx(x1, y1, cw);
            size_t did = (size_t)idx(x, y, fw);
            for (int c = 0; c < 4; ++c) {
                float top = coarse.rgba[id00 * 4 + c] * (1 - fx) + coarse.rgba[id10 * 4 + c] * fx;
                float bot = coarse.rgba[id01 * 4 + c] * (1 - fx) + coarse.rgba[id11 * 4 + c] * fx;
                fineWorking.rgba[did * 4 + c] = top * (1 - fy) + bot * fy;
            }
        }
    }
}

// ---- NNF init / upsample / propagate / random search / vote ---------------

void randomInitRange(NNF& nnf, const CafImage& working, const HoleSAT& sat,
                      const CafMask& mask, int r, int searchR, uint32_t seed, int level,
                      int y0, int y1) {
    const int w = mask.w, h = mask.h;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!isHole(mask, x, y)) continue;
            XorShift32 rng(pixelSeed(seed, level, -1, x, y));
            float bestDist = std::numeric_limits<float>::max();
            int bestSx = x, bestSy = y; bool found = false;
            // Sample only inside local Auto search disk around hole pixel
            int xLo = std::max(r, x - searchR), xHi = std::min(w - 1 - r, x + searchR);
            int yLo = std::max(r, y - searchR), yHi = std::min(h - 1 - r, y + searchR);
            if (xLo > xHi || yLo > yHi) {
                xLo = r; xHi = w - 1 - r; yLo = r; yHi = h - 1 - r;
            }
            for (int t = 0; t < 80; ++t) {
                int sx = rng.range(xLo, xHi), sy = rng.range(yLo, yHi);
                if (!sourcePatchValid(sat, w, h, sx, sy, r, x, y, searchR)) continue;
                float d = patchDistance(working, x, y, sx, sy, r, bestDist);
                if (d < bestDist) { bestDist = d; bestSx = sx; bestSy = sy; found = true; }
            }
            if (!found) {
                // Degenerate: scan local box only (then expand once if still empty)
                for (int expand = 0; expand < 2 && !found; ++expand) {
                    int er = searchR * (expand + 1);
                    int xx0 = std::max(r, x - er), xx1 = std::min(w - 1 - r, x + er);
                    int yy0 = std::max(r, y - er), yy1 = std::min(h - 1 - r, y + er);
                    for (int yy = yy0; yy <= yy1 && !found; ++yy)
                        for (int xx = xx0; xx <= xx1 && !found; ++xx)
                            if (sourcePatchValid(sat, w, h, xx, yy, r, x, y, er)) {
                                bestSx = xx; bestSy = yy; found = true;
                                bestDist = patchDistance(working, x, y, xx, yy, r,
                                    std::numeric_limits<float>::max());
                            }
                }
            }
            size_t id = (size_t)idx(x, y, w);
            nnf.dx[id] = bestSx - x; nnf.dy[id] = bestSy - y; nnf.dist[id] = bestDist;
        }
    }
}

void upsampleNNFRange(const NNF& coarseNNF, const CafMask& coarseMask,
                       NNF& fineNNF, const CafMask& fineMask, const HoleSAT& fineSat,
                       int r, int searchR, uint32_t seed, int level, int y0, int y1) {
    const int fw = fineMask.w, fh = fineMask.h, cw = coarseMask.w, ch = coarseMask.h;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < fw; ++x) {
            if (!isHole(fineMask, x, y)) continue;
            int cx = std::min(x / 2, cw - 1), cy = std::min(y / 2, ch - 1);
            int dx = 0, dy = 0; bool ok = false;
            if (isHole(coarseMask, cx, cy)) {
                size_t cid = (size_t)idx(cx, cy, cw);
                dx = coarseNNF.dx[cid] * 2; dy = coarseNNF.dy[cid] * 2;
                ok = sourcePatchValid(fineSat, fw, fh, x + dx, y + dy, r, x, y, searchR);
            }
            if (!ok) {
                XorShift32 rng(pixelSeed(seed, level, -2, x, y));
                bool found = false;
                int xLo = std::max(r, x - searchR), xHi = std::min(fw - 1 - r, x + searchR);
                int yLo = std::max(r, y - searchR), yHi = std::min(fh - 1 - r, y + searchR);
                if (xLo > xHi || yLo > yHi) {
                    xLo = r; xHi = fw - 1 - r; yLo = r; yHi = fh - 1 - r;
                }
                for (int t = 0; t < 64 && !found; ++t) {
                    int sx = rng.range(xLo, xHi), sy = rng.range(yLo, yHi);
                    if (sourcePatchValid(fineSat, fw, fh, sx, sy, r, x, y, searchR)) {
                        dx = sx - x; dy = sy - y; found = true;
                    }
                }
                if (!found) {
                    for (int yy = yLo; yy <= yHi && !found; ++yy)
                        for (int xx = xLo; xx <= xHi && !found; ++xx)
                            if (sourcePatchValid(fineSat, fw, fh, xx, yy, r, x, y, searchR)) {
                                dx = xx - x; dy = yy - y; found = true;
                            }
                }
            }
            size_t id = (size_t)idx(x, y, fw);
            fineNNF.dx[id] = dx; fineNNF.dy[id] = dy;
            fineNNF.dist[id] = std::numeric_limits<float>::max(); // refreshed by caller right after
        }
    }
}

// Serial: classic alternating scanline propagation (dependency chain).
void propagate(NNF& nnf, const CafImage& working, const HoleSAT& sat, const CafMask& mask,
                int r, int searchR, bool forward) {
    const int w = mask.w, h = mask.h;
    auto tryCandidate = [&](int x, int y, int cdx, int cdy) {
        int sx = x + cdx, sy = y + cdy;
        if (!sourcePatchValid(sat, w, h, sx, sy, r, x, y, searchR)) return;
        size_t id = (size_t)idx(x, y, w);
        float cutoff = nnf.dist[id];
        float d = patchDistance(working, x, y, sx, sy, r, cutoff);
        if (d < cutoff) { nnf.dx[id] = cdx; nnf.dy[id] = cdy; nnf.dist[id] = d; }
    };
    if (forward) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!isHole(mask, x, y)) continue;
                if (x > 0 && isHole(mask, x - 1, y)) {
                    size_t nid = (size_t)idx(x - 1, y, w);
                    tryCandidate(x, y, nnf.dx[nid], nnf.dy[nid]);
                }
                if (y > 0 && isHole(mask, x, y - 1)) {
                    size_t nid = (size_t)idx(x, y - 1, w);
                    tryCandidate(x, y, nnf.dx[nid], nnf.dy[nid]);
                }
            }
    } else {
        for (int y = h - 1; y >= 0; --y)
            for (int x = w - 1; x >= 0; --x) {
                if (!isHole(mask, x, y)) continue;
                if (x + 1 < w && isHole(mask, x + 1, y)) {
                    size_t nid = (size_t)idx(x + 1, y, w);
                    tryCandidate(x, y, nnf.dx[nid], nnf.dy[nid]);
                }
                if (y + 1 < h && isHole(mask, x, y + 1)) {
                    size_t nid = (size_t)idx(x, y + 1, w);
                    tryCandidate(x, y, nnf.dx[nid], nnf.dy[nid]);
                }
            }
    }
}

void randomSearchRange(NNF& nnf, const CafImage& working, const HoleSAT& sat, const CafMask& mask,
                        int r, int searchR, uint32_t seed, int level, int iter, int radius0,
                        int y0, int y1) {
    const int w = mask.w, h = mask.h;
    // Cap random-search window to Auto sampling radius (not half-canvas diagonal)
    const int radiusCap = std::max(1, searchR);
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!isHole(mask, x, y)) continue;
            size_t id = (size_t)idx(x, y, w);
            XorShift32 rng(pixelSeed(seed, level, iter, x, y));
            int curDx = nnf.dx[id], curDy = nnf.dy[id];
            int radius = std::min(radius0, radiusCap);
            while (radius >= 1) {
                int cdx = curDx + rng.range(-radius, radius);
                int cdy = curDy + rng.range(-radius, radius);
                int sx = x + cdx, sy = y + cdy;
                if (sourcePatchValid(sat, w, h, sx, sy, r, x, y, searchR)) {
                    float cutoff = nnf.dist[id];
                    float d = patchDistance(working, x, y, sx, sy, r, cutoff);
                    if (d < cutoff) {
                        nnf.dx[id] = cdx; nnf.dy[id] = cdy; nnf.dist[id] = d;
                        curDx = cdx; curDy = cdy;
                    }
                }
                radius /= 2;
            }
        }
    }
}

// Gather-formulated voting: for target pixel p, every hole center c with
// |p-c| <= r on each axis contributes source pixel (p + nnf[c].offset),
// which is guaranteed non-hole because c's whole source patch was validated
// non-hole and p lies inside that patch. Pure read of never-mutated known
// pixels -> safe to write working[p] in place from multiple threads as long
// as each thread owns disjoint rows.
void voteRange(NNF& nnf, CafImage& working, const CafMask& mask, int r, float sigma, int y0, int y1) {
    const int w = mask.w, h = mask.h;
    const float twoSigma2 = std::max(1e-6f, 2.f * sigma * sigma);
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!isHole(mask, x, y)) continue;
            float acc[4] = {0, 0, 0, 0};
            float wsum = 0.f;
            int cy0 = std::max(0, y - r), cy1 = std::min(h - 1, y + r);
            int cx0 = std::max(0, x - r), cx1 = std::min(w - 1, x + r);
            for (int cy = cy0; cy <= cy1; ++cy) {
                for (int cx = cx0; cx <= cx1; ++cx) {
                    if (!isHole(mask, cx, cy)) continue;
                    size_t cid = (size_t)idx(cx, cy, w);
                    if (nnf.dist[cid] == std::numeric_limits<float>::max()) continue;
                    int sx = x + nnf.dx[cid], sy = y + nnf.dy[cid];
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    float ddx = (float)(x - cx), ddy = (float)(y - cy);
                    float wgt = std::exp(-(ddx * ddx + ddy * ddy) / twoSigma2);
                    size_t sid = (size_t)idx(sx, sy, w);
                    for (int c = 0; c < 4; ++c) acc[c] += wgt * working.rgba[sid * 4 + c];
                    wsum += wgt;
                }
            }
            if (wsum > 0.f) {
                size_t did = (size_t)idx(x, y, w);
                for (int c = 0; c < 4; ++c) working.rgba[did * 4 + c] = acc[c] / wsum;
            }
        }
    }
}

void refreshDistancesRange(NNF& nnf, const CafImage& working, const CafMask& mask, int r, int y0, int y1) {
    const int w = mask.w;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!isHole(mask, x, y)) continue;
            size_t id = (size_t)idx(x, y, w);
            int sx = x + nnf.dx[id], sy = y + nnf.dy[id];
            nnf.dist[id] = patchDistance(working, x, y, sx, sy, r, std::numeric_limits<float>::max());
        }
    }
}

} // namespace detail

// -----------------------------------------------------------------------------
CafResult ContentAwareFill(const CafImage& image,
                           const CafMask& holeMask,
                           const CafParams& params) {
    using namespace detail;
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
        r.ok = true;
        r.filled = image;
        return r;
    }
    bool anyKnown = false;
    for (uint8_t v : holeMask.hole) { if (!v) { anyKnown = true; break; } }
    if (!anyKnown) {
        r.error = "ContentAwareFill: entire image is hole — no source patches";
        return r;
    }

    auto reportProgress = [&](float p, const char* stage) { if (params.progress) params.progress(p, stage); };
    auto cancelled = [&]() { return params.cancelFlag && params.cancelFlag->load(std::memory_order_relaxed); };

    const int patchRadius = std::max(1, params.patchSize / 2);
    const int numThreads = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t seed = params.seed ? params.seed : 1u;

    reportProgress(0.f, "pyramid");
    CafMask binMask = makeBinaryMask(holeMask);
    std::vector<Level> pyramid = buildPyramid(image, binMask,
                                               std::max(1, params.multiScaleLevels), patchRadius);
    const int numLevels = (int)pyramid.size();

    NNF nnf, nnfCoarser;
    CafImage workingCoarser;
    CafMask maskCoarser;
    bool haveCoarser = false;

    for (int lvl = numLevels - 1; lvl >= 0; --lvl) {
        if (cancelled()) { r.error = "ContentAwareFill: cancelled"; return r; }
        Level& L = pyramid[lvl];
        CafImage working = L.image; // copy: known pixels exact, hole pixels raw/garbage so far

        HoleSAT sat; sat.build(L.mask);

        // Hole AABB at this level → local Auto search radius (not full canvas)
        int hx = 0, hy = 0, hw = L.mask.w, hh = L.mask.h;
        if (!HoleBounds(L.mask, hx, hy, hw, hh)) {
            hx = 0; hy = 0; hw = L.mask.w; hh = L.mask.h;
        }
        const int levelSearchRadius = params.randomSearchRadius > 0
            ? std::max(1, params.randomSearchRadius >> lvl)
            : autoSearchRadius(L.mask.w, L.mask.h, hw, hh, patchRadius);

        if (!haveCoarser) {
            // Local bootstrap (known pixels near hole) — not global muddy mean
            bootstrapFill(working, L.mask, std::max(patchRadius * 4, 12));
            nnf.init(L.mask.w, L.mask.h);
            parallelForRows(L.mask.h, numThreads, [&](int y0, int y1) {
                randomInitRange(nnf, working, sat, L.mask, patchRadius, levelSearchRadius,
                                seed, lvl, y0, y1);
            });
        } else {
            bilinearUpsampleHoleRegion(workingCoarser, working, L.mask);
            nnf.init(L.mask.w, L.mask.h);
            parallelForRows(L.mask.h, numThreads, [&](int y0, int y1) {
                upsampleNNFRange(nnfCoarser, maskCoarser, nnf, L.mask, sat, patchRadius,
                                 levelSearchRadius, seed, lvl, y0, y1);
            });
            parallelForRows(L.mask.h, numThreads, [&](int y0, int y1) {
                refreshDistancesRange(nnf, working, L.mask, patchRadius, y0, y1);
            });
        }

        // Random-search step starts at min(searchR, something proportional) and halves
        const int randomSearchStart = std::max(1, levelSearchRadius);

        const int itersThisLevel = std::max(1, params.searchIters);
        for (int it = 0; it < itersThisLevel; ++it) {
            if (cancelled()) { r.error = "ContentAwareFill: cancelled"; return r; }

            propagate(nnf, working, sat, L.mask, patchRadius, levelSearchRadius, (it % 2) == 0);

            parallelForRows(L.mask.h, numThreads, [&](int y0, int y1) {
                randomSearchRange(nnf, working, sat, L.mask, patchRadius, levelSearchRadius,
                                   seed, lvl, it, randomSearchStart, y0, y1);
            });
            parallelForRows(L.mask.h, numThreads, [&](int y0, int y1) {
                voteRange(nnf, working, L.mask, patchRadius, params.voteSigma, y0, y1);
            });
            parallelForRows(L.mask.h, numThreads, [&](int y0, int y1) {
                refreshDistancesRange(nnf, working, L.mask, patchRadius, y0, y1);
            });

            float levelSpan = 1.f / (float)numLevels;
            float progInLevel = (float)(it + 1) / (float)itersThisLevel;
            reportProgress((float)(numLevels - 1 - lvl) * levelSpan + progInLevel * levelSpan, "patchmatch");
        }

        workingCoarser = std::move(working);
        nnfCoarser = std::move(nnf);
        maskCoarser = L.mask;
        haveCoarser = true;
    }

    reportProgress(0.95f, "compose");

    // Final composition: known pixels are an exact copy of the input; hole
    // pixels blend original->synthesized by the *original* (possibly soft)
    // mask value, so 1..254 edges feather instead of hard-cutting.
    r.filled = image;
    const CafImage& synth = workingCoarser; // finest-level (lvl 0) completed reconstruction
    for (size_t i = 0; i < holeMask.hole.size(); ++i) {
        uint8_t v = holeMask.hole[i];
        if (v == 0) continue;
        float a = std::min(1.f, v / 255.f);
        for (int c = 0; c < 4; ++c) {
            float orig = image.rgba[i * 4 + c];
            float fillv = synth.rgba[i * 4 + c];
            float blended = orig + (fillv - orig) * a;
            if (params.clampRgb && c < 3) blended = std::clamp(blended, 0.f, 1.f);
            r.filled.rgba[i * 4 + c] = blended;
        }
    }

    reportProgress(1.f, "done");
    r.ok = true;
    return r;
}

} // namespace caf
