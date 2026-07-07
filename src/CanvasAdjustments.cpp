#include "Canvas.h"
#include "core/Logger.h"
#include "core/ThreadPool.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <mutex>
#include <future>
#include <thread>

// --- Helpers ---
static std::vector<float> ExportLayerF(const Layer& layer, int w, int h) {
    std::vector<float> out((size_t)w * h * 4, 0.f);
    if (layer.tileCache) layer.tileCache->ExportRGBA32F(out.data(), w, h);
    return out;
}

static void SetLayerPixelsF(Layer& layer, const std::vector<float>& pixels, int w, int h, CanvasPixelFormat fmt) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
    layer.tileCache->ImportRGBA32F(pixels.data(), w, h);
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    layer.filtersDirty = true;
}

static void EnsureLayerTileCache(Layer& layer, int w, int h, CanvasPixelFormat fmt) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
}

static float SelU82F(uint8_t v) {
    return v / 255.f;
}

static float GetSelWeight(const std::vector<uint8_t>& mask, int w, int x, int y, bool hasSel) {
    if (!hasSel || mask.empty()) return 1.f;
    return SelU82F(mask[(size_t)y * w + x]);
}

static bool LayerHasPixels(const Layer& layer) {
    return layer.tileCache && !layer.tileCache->IsEmpty();
}

static inline void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max({r,g,b}), mn = std::min({r,g,b});
    v = mx; float delta = mx - mn;
    s = (mx > 1e-6f) ? delta / mx : 0.f;
    if (delta < 1e-6f) { h = 0.f; return; }
    if      (mx == r) h = (g - b) / delta + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / delta + 2.f;
    else              h = (r - g) / delta + 4.f;
    h /= 6.f;
}

static inline void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b) {
    if (s < 1e-6f) { r = g = b = v; return; }
    float hh = h * 6.f; int i = (int)hh % 6; float f = hh - (int)hh;
    float p = v*(1.f-s), q = v*(1.f-s*f), t = v*(1.f-s*(1.f-f));
    switch(i) {
        case 0: r=v;g=t;b=p; break; case 1: r=q;g=v;b=p; break;
        case 2: r=p;g=v;b=t; break; case 3: r=p;g=q;b=v; break;
        case 4: r=t;g=p;b=v; break; default:r=v;g=p;b=q; break;
    }
}

static void BoxBlurH(std::vector<float>& px, int w, int h, int r) {
    std::vector<float> tmp(px.size());
    for (int y = 0; y < h; ++y) {
        float acc[4]={0,0,0,0}; int count=0;
        for (int kx=0; kx<=r; ++kx) { int cx=std::min(kx,w-1); for(int c=0;c<4;++c) acc[c]+=px[((size_t)y*w+cx)*4+c]; ++count; }
        for (int x = 0; x < w; ++x) {
            for(int c=0;c<4;++c) tmp[((size_t)y*w+x)*4+c]=acc[c]/(float)count;
            int add=x+r+1, rem=x-r;
            if (add<w) { for(int c=0;c<4;++c) acc[c]+=px[((size_t)y*w+add)*4+c]; ++count; }
            if (rem>=0) { for(int c=0;c<4;++c) acc[c]-=px[((size_t)y*w+rem)*4+c]; --count; }
        }
    }
    px=tmp;
}

static void BoxBlurV(std::vector<float>& px, int w, int h, int r) {
    std::vector<float> tmp(px.size());
    for (int x = 0; x < w; ++x) {
        float acc[4]={0,0,0,0}; int count=0;
        for (int ky=0; ky<=r; ++ky) { int cy=std::min(ky,h-1); for(int c=0;c<4;++c) acc[c]+=px[((size_t)cy*w+x)*4+c]; ++count; }
        for (int y = 0; y < h; ++y) {
            for(int c=0;c<4;++c) tmp[((size_t)y*w+x)*4+c]=acc[c]/(float)count;
            int add=y+r+1, rem=y-r;
            if (add<h) { for(int c=0;c<4;++c) acc[c]+=px[((size_t)add*w+x)*4+c]; ++count; }
            if (rem>=0) { for(int c=0;c<4;++c) acc[c]-=px[((size_t)rem*w+x)*4+c]; --count; }
        }
    }
    px=tmp;
}

// Monotone cubic spline LUT — called from UI curves editor
std::vector<float> Canvas_BuildSplineLUT(const std::vector<std::pair<float,float>>& pts) {
    std::vector<float> lut(256);
    if (pts.size() < 2) { for(int i=0;i<256;++i) lut[i]=(float)i/255.f; return lut; }
    int n=(int)pts.size();
    std::vector<float> d(n-1),m2(n,0.f);
    for (int i=0;i<n-1;++i) d[i]=(pts[i+1].second-pts[i].second)/(pts[i+1].first-pts[i].first+1e-9f);
    m2[0]=d[0];
    for (int i=1;i<n-1;++i) m2[i]=(d[i-1]+d[i])*0.5f;
    m2[n-1]=d[n-2];
    for (int i=0;i<n-1;++i) {
        if (fabsf(d[i])<1e-9f){m2[i]=m2[i+1]=0.f;continue;}
        float a=m2[i]/d[i], b=m2[i+1]/d[i];
        float ab2=a*a+b*b;
        if (ab2>9.f){float s2=3.f/sqrtf(ab2);m2[i]=s2*a*d[i];m2[i+1]=s2*b*d[i];}
    }
    for (int xi=0;xi<256;++xi) {
        float t=(float)xi/255.f;
        t=std::clamp(t,pts.front().first,pts.back().first);
        int seg=0; for(int i=0;i<n-2;++i) if(t>=pts[i+1].first) seg=i+1;
        float hh=(pts[seg+1].first-pts[seg].first);
        float u=(t-pts[seg].first)/(hh+1e-9f);
        float u2=u*u,u3=u2*u;
        float vv=(2*u3-3*u2+1)*pts[seg].second+(u3-2*u2+u)*hh*m2[seg]
                +(-2*u3+3*u2)*pts[seg+1].second+(u3-u2)*hh*m2[seg+1];
        lut[xi]=std::clamp(vv,0.f,1.f);
    }
    return lut;
}

// ============================================================
// Destructive Operations
// ============================================================

void Canvas::InvertAlpha() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    if (!layer.tileCache) return;

    auto& tc = *layer.tileCache;
    int tilesX = tc.GetTilesX();
    int tilesY = tc.GetTilesY();
    int bpp = tc.GetBytesPerPixel();

    std::vector<std::pair<int, int>> activeTiles;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (tc.HasTile(tx, ty)) {
                activeTiles.push_back({tx, ty});
            }
        }
    }

    if (activeTiles.empty()) return;

    // Backup tiles for undo
    for (const auto& [tx, ty] : activeTiles) {
        BackupTile(tx, ty);
    }

    std::atomic<size_t> nextTileIndex{0};
    size_t totalTiles = activeTiles.size();
    int numWorkers = std::min((int)std::thread::hardware_concurrency(), (int)totalTiles);
    if (numWorkers < 1) numWorkers = 1;

    std::mutex tcMutex;

    std::vector<std::future<void>> futures;
    for (int w = 0; w < numWorkers; ++w) {
        futures.push_back(ThreadPool::Get().Enqueue([&]() {
            size_t idx;
            while ((idx = nextTileIndex.fetch_add(1)) < totalTiles) {
                int tx = activeTiles[idx].first;
                int ty = activeTiles[idx].second;

                uint8_t* tileData = nullptr;
                {
                    std::lock_guard<std::mutex> lock(tcMutex);
                    tileData = tc.LockTile(tx, ty);
                }

                if (!tileData) continue;

                for (int i = 0; i < TILE_SIZE * TILE_SIZE; ++i) {
                    int px = i % TILE_SIZE;
                    int py = i / TILE_SIZE;
                    int canvasX = tx * TILE_SIZE + px;
                    int canvasY = ty * TILE_SIZE + py;

                    float sel = 1.0f;
                    if (m_HasSelection && canvasX < m_Width && canvasY < m_Height) {
                        sel = m_SelectionMask[canvasY * m_Width + canvasX] / 255.f;
                    } else if (canvasX >= m_Width || canvasY >= m_Height) {
                        sel = 0.0f;
                    }

                    if (sel < 1e-4f) continue;

                    if (bpp == 4) { // RGBA8
                        float a = tileData[i * 4 + 3] / 255.f;
                        float na = a * (1.f - sel) + (1.f - a) * sel;
                        tileData[i * 4 + 3] = static_cast<uint8_t>(std::clamp(na * 255.f + 0.5f, 0.f, 255.f));
                    } else { // RGBA32F
                        float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                        float a = fp[3];
                        float na = a * (1.f - sel) + (1.f - a) * sel;
                        fp[3] = na;
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();

    for (const auto& [tx, ty] : activeTiles) {
        tc.MarkDirty(tx, ty);
    }

    if (!m_ActiveStrokeDeltas.empty()) {
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Invert Alpha", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }

    layer.needsUpload = true;
    m_CompositeDirty = true;
    SetDocumentModified(true);
    Logger::Get().Info("InvertAlpha (Tiled CPU)");
}

void Canvas::ApplyBlur(float radius) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    if (!layer.tileCache) return;

    auto& tc = *layer.tileCache;
    int R = static_cast<int>(std::ceil(radius));
    if (R < 1) return;

    int tilesX = tc.GetTilesX();
    int tilesY = tc.GetTilesY();
    int bpp = tc.GetBytesPerPixel();

    std::vector<std::pair<int, int>> activeTiles;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (tc.HasTile(tx, ty)) {
                activeTiles.push_back({tx, ty});
            }
        }
    }

    if (activeTiles.empty()) return;

    // Backup tiles for undo
    for (const auto& [tx, ty] : activeTiles) {
        BackupTile(tx, ty);
    }

    // Pre-allocate output tiles
    std::vector<std::vector<float>> outputTiles(activeTiles.size());

    std::atomic<size_t> nextTileIndex{0};
    size_t totalTiles = activeTiles.size();
    int numWorkers = std::min((int)std::thread::hardware_concurrency(), (int)totalTiles);
    if (numWorkers < 1) numWorkers = 1;

    std::mutex tcMutex;

    std::vector<std::future<void>> futures;
    for (int w = 0; w < numWorkers; ++w) {
        futures.push_back(ThreadPool::Get().Enqueue([&]() {
            size_t idx;
            while ((idx = nextTileIndex.fetch_add(1)) < totalTiles) {
                int tx = activeTiles[idx].first;
                int ty = activeTiles[idx].second;

                // Load neighborhood under mutex lock
                int paddedSize = TILE_SIZE + 2 * R;
                std::vector<float> padded(paddedSize * paddedSize * 4, 0.0f);
                {
                    std::lock_guard<std::mutex> lock(tcMutex);
                    for (int py = -R; py < TILE_SIZE + R; ++py) {
                        for (int px = -R; px < TILE_SIZE + R; ++px) {
                            int canvasX = tx * TILE_SIZE + px;
                            int canvasY = ty * TILE_SIZE + py;
                            float rgba[4] = {};
                            tc.GetPixelF(canvasX, canvasY, rgba);
                            int pidx = ((py + R) * paddedSize + (px + R)) * 4;
                            padded[pidx + 0] = rgba[0];
                            padded[pidx + 1] = rgba[1];
                            padded[pidx + 2] = rgba[2];
                            padded[pidx + 3] = rgba[3];
                        }
                    }
                }

                // 1. Horizontal blur pass on padded: (TILE_SIZE + 2*R) x (TILE_SIZE + 2*R) -> TILE_SIZE x (TILE_SIZE + 2*R)
                std::vector<float> tempH(TILE_SIZE * paddedSize * 4, 0.0f);
                
                // Build Gaussian weights
                std::vector<float> weights(2 * R + 1);
                float weightSum = 0.0f;
                for (int k = -R; k <= R; ++k) {
                    weights[k + R] = std::exp(-k * k / (2.0f * radius * radius));
                    weightSum += weights[k + R];
                }
                for (int i = 0; i < weights.size(); ++i) {
                    weights[i] /= weightSum;
                }

                for (int py = 0; py < paddedSize; ++py) {
                    for (int px = 0; px < TILE_SIZE; ++px) {
                        float sum[4] = {};
                        for (int k = -R; k <= R; ++k) {
                            int srcX = px + R + k;
                            int srcIdx = (py * paddedSize + srcX) * 4;
                            float w = weights[k + R];
                            sum[0] += padded[srcIdx + 0] * w;
                            sum[1] += padded[srcIdx + 1] * w;
                            sum[2] += padded[srcIdx + 2] * w;
                            sum[3] += padded[srcIdx + 3] * w;
                        }
                        int dstIdx = (py * TILE_SIZE + px) * 4;
                        tempH[dstIdx + 0] = sum[0];
                        tempH[dstIdx + 1] = sum[1];
                        tempH[dstIdx + 2] = sum[2];
                        tempH[dstIdx + 3] = sum[3];
                    }
                }

                // 2. Vertical blur pass: TILE_SIZE x (TILE_SIZE + 2*R) -> TILE_SIZE x TILE_SIZE
                std::vector<float> result(TILE_SIZE * TILE_SIZE * 4, 0.0f);
                for (int py = 0; py < TILE_SIZE; ++py) {
                    for (int px = 0; px < TILE_SIZE; ++px) {
                        float sum[4] = {};
                        for (int k = -R; k <= R; ++k) {
                            int srcY = py + R + k;
                            int srcIdx = (srcY * TILE_SIZE + px) * 4;
                            float w = weights[k + R];
                            sum[0] += tempH[srcIdx + 0] * w;
                            sum[1] += tempH[srcIdx + 1] * w;
                            sum[2] += tempH[srcIdx + 2] * w;
                            sum[3] += tempH[srcIdx + 3] * w;
                        }
                        int dstIdx = (py * TILE_SIZE + px) * 4;
                        result[dstIdx + 0] = sum[0];
                        result[dstIdx + 1] = sum[1];
                        result[dstIdx + 2] = sum[2];
                        result[dstIdx + 3] = sum[3];
                    }
                }

                // Apply selection blending and format write
                for (int py = 0; py < TILE_SIZE; ++py) {
                    for (int px = 0; px < TILE_SIZE; ++px) {
                        int canvasX = tx * TILE_SIZE + px;
                        int canvasY = ty * TILE_SIZE + py;
                        
                        float sel = 1.0f;
                        if (m_HasSelection && canvasX < m_Width && canvasY < m_Height) {
                            sel = m_SelectionMask[canvasY * m_Width + canvasX] / 255.f;
                        } else if (canvasX >= m_Width || canvasY >= m_Height) {
                            sel = 0.0f;
                        }

                        int dstIdx = (py * TILE_SIZE + px) * 4;
                        if (sel < 1.0f) {
                            int origIdx = ((py + R) * paddedSize + (px + R)) * 4;
                            result[dstIdx + 0] = padded[origIdx + 0] * (1.f - sel) + result[dstIdx + 0] * sel;
                            result[dstIdx + 1] = padded[origIdx + 1] * (1.f - sel) + result[dstIdx + 1] * sel;
                            result[dstIdx + 2] = padded[origIdx + 2] * (1.f - sel) + result[dstIdx + 2] * sel;
                            result[dstIdx + 3] = padded[origIdx + 3] * (1.f - sel) + result[dstIdx + 3] * sel;
                        }
                    }
                }

                outputTiles[idx] = std::move(result);
            }
        }));
    }
    for (auto& f : futures) f.wait();

    // Write back to active layer tile cache
    for (size_t idx = 0; idx < activeTiles.size(); ++idx) {
        int tx = activeTiles[idx].first;
        int ty = activeTiles[idx].second;
        const auto& blurred = outputTiles[idx];

        uint8_t* tileData = tc.LockTile(tx, ty);
        if (!tileData) continue;

        if (bpp == 4) { // RGBA8
            for (int i = 0; i < TILE_SIZE * TILE_SIZE; ++i) {
                tileData[i * 4 + 0] = static_cast<uint8_t>(std::clamp(blurred[i * 4 + 0] * 255.f + 0.5f, 0.f, 255.f));
                tileData[i * 4 + 1] = static_cast<uint8_t>(std::clamp(blurred[i * 4 + 1] * 255.f + 0.5f, 0.f, 255.f));
                tileData[i * 4 + 2] = static_cast<uint8_t>(std::clamp(blurred[i * 4 + 2] * 255.f + 0.5f, 0.f, 255.f));
                tileData[i * 4 + 3] = static_cast<uint8_t>(std::clamp(blurred[i * 4 + 3] * 255.f + 0.5f, 0.f, 255.f));
            }
        } else { // RGBA32F
            float* fp = reinterpret_cast<float*>(tileData);
            std::memcpy(fp, blurred.data(), TILE_SIZE * TILE_SIZE * 16);
        }
        tc.MarkDirty(tx, ty);
    }

    if (!m_ActiveStrokeDeltas.empty()) {
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Blur", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }

    layer.needsUpload = true;
    m_CompositeDirty = true;
    SetDocumentModified(true);
    Logger::Get().Info("ApplyBlur (Tiled CPU) r=" + std::to_string(R));
}

void Canvas::ApplyHSV(float dH, float dS, float dV) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    if (!layer.tileCache) return;

    auto& tc = *layer.tileCache;
    int tilesX = tc.GetTilesX();
    int tilesY = tc.GetTilesY();
    int bpp = tc.GetBytesPerPixel();

    std::vector<std::pair<int, int>> activeTiles;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (tc.HasTile(tx, ty)) {
                activeTiles.push_back({tx, ty});
            }
        }
    }

    if (activeTiles.empty()) return;

    // Backup tiles for undo
    for (const auto& [tx, ty] : activeTiles) {
        BackupTile(tx, ty);
    }

    std::atomic<size_t> nextTileIndex{0};
    size_t totalTiles = activeTiles.size();
    int numWorkers = std::min((int)std::thread::hardware_concurrency(), (int)totalTiles);
    if (numWorkers < 1) numWorkers = 1;

    std::mutex tcMutex;

    std::vector<std::future<void>> futures;
    for (int w = 0; w < numWorkers; ++w) {
        futures.push_back(ThreadPool::Get().Enqueue([&]() {
            size_t idx;
            while ((idx = nextTileIndex.fetch_add(1)) < totalTiles) {
                int tx = activeTiles[idx].first;
                int ty = activeTiles[idx].second;

                uint8_t* tileData = nullptr;
                {
                    std::lock_guard<std::mutex> lock(tcMutex);
                    tileData = tc.LockTile(tx, ty);
                }

                if (!tileData) continue;

                for (int i = 0; i < TILE_SIZE * TILE_SIZE; ++i) {
                    int px = i % TILE_SIZE;
                    int py = i / TILE_SIZE;
                    int canvasX = tx * TILE_SIZE + px;
                    int canvasY = ty * TILE_SIZE + py;

                    float sel = 1.0f;
                    if (m_HasSelection && canvasX < m_Width && canvasY < m_Height) {
                        sel = m_SelectionMask[canvasY * m_Width + canvasX] / 255.f;
                    } else if (canvasX >= m_Width || canvasY >= m_Height) {
                        sel = 0.0f;
                    }

                    if (sel < 1e-4f) continue;

                    float r, g, b, a;
                    if (bpp == 4) { // RGBA8
                        r = tileData[i * 4 + 0] / 255.f;
                        g = tileData[i * 4 + 1] / 255.f;
                        b = tileData[i * 4 + 2] / 255.f;
                        a = tileData[i * 4 + 3] / 255.f;
                    } else { // RGBA32F
                        float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                        r = fp[0]; g = fp[1]; b = fp[2]; a = fp[3];
                    }

                    float h, s, v;
                    RGBtoHSV(r, g, b, h, s, v);
                    h = std::fmod(h + dH + 1.f, 1.f);
                    s = std::clamp(s + dS, 0.f, 1.f);
                    v = std::clamp(v + dV, 0.f, 1.f);
                    float nr, ng, nb;
                    HSVtoRGB(h, s, v, nr, ng, nb);

                    nr = r * (1.f - sel) + nr * sel;
                    ng = g * (1.f - sel) + ng * sel;
                    nb = b * (1.f - sel) + nb * sel;

                    if (bpp == 4) {
                        tileData[i * 4 + 0] = static_cast<uint8_t>(std::clamp(nr * 255.f + 0.5f, 0.f, 255.f));
                        tileData[i * 4 + 1] = static_cast<uint8_t>(std::clamp(ng * 255.f + 0.5f, 0.f, 255.f));
                        tileData[i * 4 + 2] = static_cast<uint8_t>(std::clamp(nb * 255.f + 0.5f, 0.f, 255.f));
                    } else {
                        float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                        fp[0] = nr; fp[1] = ng; fp[2] = nb;
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();

    for (const auto& [tx, ty] : activeTiles) {
        tc.MarkDirty(tx, ty);
    }

    if (!m_ActiveStrokeDeltas.empty()) {
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("HSV Adjustment", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }

    layer.needsUpload = true;
    m_CompositeDirty = true;
    SetDocumentModified(true);
    Logger::Get().Info("ApplyHSV (Tiled CPU)");
}

void Canvas::ApplyCurves(const std::vector<float>& lut256) {
    if ((int)lut256.size() < 256 || m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    if (!layer.tileCache) return;

    auto& tc = *layer.tileCache;
    int tilesX = tc.GetTilesX();
    int tilesY = tc.GetTilesY();
    int bpp = tc.GetBytesPerPixel();

    std::vector<std::pair<int, int>> activeTiles;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (tc.HasTile(tx, ty)) {
                activeTiles.push_back({tx, ty});
            }
        }
    }

    if (activeTiles.empty()) return;

    // Backup tiles for undo
    for (const auto& [tx, ty] : activeTiles) {
        BackupTile(tx, ty);
    }

    std::atomic<size_t> nextTileIndex{0};
    size_t totalTiles = activeTiles.size();
    int numWorkers = std::min((int)std::thread::hardware_concurrency(), (int)totalTiles);
    if (numWorkers < 1) numWorkers = 1;

    std::mutex tcMutex;

    std::vector<std::future<void>> futures;
    for (int w = 0; w < numWorkers; ++w) {
        futures.push_back(ThreadPool::Get().Enqueue([&]() {
            size_t idx;
            while ((idx = nextTileIndex.fetch_add(1)) < totalTiles) {
                int tx = activeTiles[idx].first;
                int ty = activeTiles[idx].second;

                uint8_t* tileData = nullptr;
                {
                    std::lock_guard<std::mutex> lock(tcMutex);
                    tileData = tc.LockTile(tx, ty);
                }

                if (!tileData) continue;

                auto sample = [&](float val) -> float {
                    float fi = val * 255.f;
                    int ii = std::clamp((int)fi, 0, 254);
                    float t = fi - ii;
                    return lut256[ii] * (1.f - t) + lut256[ii + 1] * t;
                };

                for (int i = 0; i < TILE_SIZE * TILE_SIZE; ++i) {
                    int px = i % TILE_SIZE;
                    int py = i / TILE_SIZE;
                    int canvasX = tx * TILE_SIZE + px;
                    int canvasY = ty * TILE_SIZE + py;

                    float sel = 1.0f;
                    if (m_HasSelection && canvasX < m_Width && canvasY < m_Height) {
                        sel = m_SelectionMask[canvasY * m_Width + canvasX] / 255.f;
                    } else if (canvasX >= m_Width || canvasY >= m_Height) {
                        sel = 0.0f;
                    }

                    if (sel < 1e-4f) continue;

                    float r, g, b, a;
                    if (bpp == 4) { // RGBA8
                        r = tileData[i * 4 + 0] / 255.f;
                        g = tileData[i * 4 + 1] / 255.f;
                        b = tileData[i * 4 + 2] / 255.f;
                        a = tileData[i * 4 + 3] / 255.f;
                    } else { // RGBA32F
                        float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                        r = fp[0]; g = fp[1]; b = fp[2]; a = fp[3];
                    }

                    float nr = r * (1.f - sel) + sample(r) * sel;
                    float ng = g * (1.f - sel) + sample(g) * sel;
                    float nb = b * (1.f - sel) + sample(b) * sel;

                    if (bpp == 4) {
                        tileData[i * 4 + 0] = static_cast<uint8_t>(std::clamp(nr * 255.f + 0.5f, 0.f, 255.f));
                        tileData[i * 4 + 1] = static_cast<uint8_t>(std::clamp(ng * 255.f + 0.5f, 0.f, 255.f));
                        tileData[i * 4 + 2] = static_cast<uint8_t>(std::clamp(nb * 255.f + 0.5f, 0.f, 255.f));
                    } else {
                        float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                        fp[0] = nr; fp[1] = ng; fp[2] = nb;
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();

    for (const auto& [tx, ty] : activeTiles) {
        tc.MarkDirty(tx, ty);
    }

    if (!m_ActiveStrokeDeltas.empty()) {
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Curves", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }

    layer.needsUpload = true;
    m_CompositeDirty = true;
    SetDocumentModified(true);
    Logger::Get().Info("ApplyCurves (Tiled CPU)");
}

void Canvas::ApplyNoise(float strength, bool colorNoise) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    if (!layer.tileCache) return;

    auto& tc = *layer.tileCache;
    int tilesX = tc.GetTilesX();
    int tilesY = tc.GetTilesY();
    int bpp = tc.GetBytesPerPixel();

    std::vector<std::pair<int, int>> activeTiles;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (tc.HasTile(tx, ty)) {
                activeTiles.push_back({tx, ty});
            }
        }
    }

    if (activeTiles.empty()) return;

    // Backup tiles for undo
    for (const auto& [tx, ty] : activeTiles) {
        BackupTile(tx, ty);
    }

    std::atomic<size_t> nextTileIndex{0};
    size_t totalTiles = activeTiles.size();
    int numWorkers = std::min((int)std::thread::hardware_concurrency(), (int)totalTiles);
    if (numWorkers < 1) numWorkers = 1;

    std::mutex tcMutex;

    std::vector<std::future<void>> futures;
    for (int w = 0; w < numWorkers; ++w) {
        futures.push_back(ThreadPool::Get().Enqueue([&]() {
            size_t idx;
            while ((idx = nextTileIndex.fetch_add(1)) < totalTiles) {
                int tx = activeTiles[idx].first;
                int ty = activeTiles[idx].second;

                uint8_t* tileData = nullptr;
                {
                    std::lock_guard<std::mutex> lock(tcMutex);
                    tileData = tc.LockTile(tx, ty);
                }

                if (!tileData) continue;

                thread_local std::mt19937 rng(std::random_device{}());
                std::uniform_real_distribution<float> dist(-1.f, 1.f);

                for (int i = 0; i < TILE_SIZE * TILE_SIZE; ++i) {
                    int px = i % TILE_SIZE;
                    int py = i / TILE_SIZE;
                    int canvasX = tx * TILE_SIZE + px;
                    int canvasY = ty * TILE_SIZE + py;

                    float sel = 1.0f;
                    if (m_HasSelection && canvasX < m_Width && canvasY < m_Height) {
                        sel = m_SelectionMask[canvasY * m_Width + canvasX] / 255.f;
                    } else if (canvasX >= m_Width || canvasY >= m_Height) {
                        sel = 0.0f;
                    }

                    if (sel < 1e-4f) continue;

                    float r, g, b, a;
                    if (bpp == 4) { // RGBA8
                        r = tileData[i * 4 + 0] / 255.f;
                        g = tileData[i * 4 + 1] / 255.f;
                        b = tileData[i * 4 + 2] / 255.f;
                        a = tileData[i * 4 + 3] / 255.f;
                    } else { // RGBA32F
                        float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                        r = fp[0]; g = fp[1]; b = fp[2]; a = fp[3];
                    }

                    float nr, ng, nb;
                    if (colorNoise) {
                        nr = std::clamp(r + dist(rng) * strength * sel, 0.f, 1.f);
                        ng = std::clamp(g + dist(rng) * strength * sel, 0.f, 1.f);
                        nb = std::clamp(b + dist(rng) * strength * sel, 0.f, 1.f);
                    } else {
                        float n = dist(rng) * strength * sel;
                        nr = std::clamp(r + n, 0.f, 1.f);
                        ng = std::clamp(g + n, 0.f, 1.f);
                        nb = std::clamp(b + n, 0.f, 1.f);
                    }

                    if (bpp == 4) {
                        tileData[i * 4 + 0] = static_cast<uint8_t>(std::clamp(nr * 255.f + 0.5f, 0.f, 255.f));
                        tileData[i * 4 + 1] = static_cast<uint8_t>(std::clamp(ng * 255.f + 0.5f, 0.f, 255.f));
                        tileData[i * 4 + 2] = static_cast<uint8_t>(std::clamp(nb * 255.f + 0.5f, 0.f, 255.f));
                    } else {
                        float* fp = reinterpret_cast<float*>(tileData) + i * 4;
                        fp[0] = nr; fp[1] = ng; fp[2] = nb;
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.wait();

    for (const auto& [tx, ty] : activeTiles) {
        tc.MarkDirty(tx, ty);
    }

    if (!m_ActiveStrokeDeltas.empty()) {
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Noise", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }

    layer.needsUpload = true;
    m_CompositeDirty = true;
    SetDocumentModified(true);
    Logger::Get().Info("ApplyNoise (Tiled CPU)");
}

// ============================================================
// Non-destructive Filters
// ============================================================

void Canvas::RebuildFilteredPixels(Layer& layer) {
    if (!layer.filtersDirty) return;
    if (layer.filters.empty() || !LayerHasPixels(layer)) {
        layer.filteredCache.reset();
        layer.filtersDirty=false;
        return;
    }
    std::vector<float> tmp = ExportLayerF(layer, m_Width, m_Height);
    int w=m_Width,h=m_Height;
    for (auto& f : layer.filters) {
        if (!f.enabled) continue;
        switch (f.type) {
        case FilterType::Blur: { int rr=std::max(1,(int)f.p[0]); for(int p=0;p<3;++p){BoxBlurH(tmp,w,h,rr);BoxBlurV(tmp,w,h,rr);} } break;
        case FilterType::HSV: {
            for(int i=0;i<w*h;++i){
                size_t idx=(size_t)i*4; float hr,hs,hv;
                RGBtoHSV(tmp[idx],tmp[idx+1],tmp[idx+2],hr,hs,hv);
                hr=fmodf(hr+f.p[0]+1.f,1.f); hs=std::clamp(hs+f.p[1],0.f,1.f); hv=std::clamp(hv+f.p[2],0.f,1.f);
                float r2,g2,b2; HSVtoRGB(hr,hs,hv,r2,g2,b2); tmp[idx]=r2;tmp[idx+1]=g2;tmp[idx+2]=b2;
            }
        } break;
        case FilterType::Curves: {
            if ((int)f.lut.size()==256) {
                auto sam=[&](float v)->float{ float fi=v*255.f; int ii=std::clamp((int)fi,0,254); float t=fi-ii; return f.lut[ii]*(1.f-t)+f.lut[ii+1]*t; };
                for(int i=0;i<w*h;++i){ size_t idx=(size_t)i*4; for(int c=0;c<3;++c) tmp[idx+c]=sam(tmp[idx+c]); }
            }
        } break;
        case FilterType::AlphaInvert: for(int i=0;i<w*h;++i) tmp[(size_t)i*4+3]=1.f-tmp[(size_t)i*4+3]; break;
        case FilterType::Noise: {
            std::mt19937 rng2(1337); std::uniform_real_distribution<float> dist2(-1.f,1.f);
            bool col=(f.p[1]>0.5f);
            for(int i=0;i<w*h;++i){ size_t idx=(size_t)i*4;
                if(col){for(int c=0;c<3;++c) tmp[idx+c]=std::clamp(tmp[idx+c]+dist2(rng2)*f.p[0],0.f,1.f);}
                else { float n=dist2(rng2)*f.p[0]; for(int c=0;c<3;++c) tmp[idx+c]=std::clamp(tmp[idx+c]+n,0.f,1.f); }
            }
        } break;
        }
    }
    if (!layer.filteredCache) {
        layer.filteredCache = std::make_unique<TileCache>();
    }
    layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
    layer.filteredCache->ImportRGBA32F(tmp.data(), m_Width, m_Height);
    layer.filteredCache->MarkAllDirty();
    layer.filtersDirty=false;
}
