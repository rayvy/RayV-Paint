#include "TileCache.h"
#include <cstring>
#include <cassert>
#include <limits>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

inline uint32_t TileCache::Key(int tileX, int tileY) const {
    return (uint32_t)tileY * (uint32_t)m_TilesX + (uint32_t)tileX;
}

inline const TileCache::Tile* TileCache::FindTile(int tileX, int tileY) const {
    auto it = m_Tiles.find(Key(tileX, tileY));
    return (it != m_Tiles.end()) ? &it->second : nullptr;
}

inline TileCache::Tile* TileCache::FindTile(int tileX, int tileY) {
    auto it = m_Tiles.find(Key(tileX, tileY));
    return (it != m_Tiles.end()) ? &it->second : nullptr;
}

TileCache::Tile& TileCache::GetOrCreateTile(int tileX, int tileY) {
    uint32_t key = Key(tileX, tileY);
    auto it = m_Tiles.find(key);
    if (it != m_Tiles.end()) {
        it->second.lastAccess = ++m_AccessCounter;
        return it->second;
    }

    // LRU eviction before allocating
    if (m_Tiles.size() >= m_MaxTilesInRAM) {
        EvictLRU();
    }

    Tile& t = m_Tiles[key];
    t.data.assign((size_t)TILE_SIZE * TILE_SIZE * m_BytesPerPixel, 0);
    t.dirty      = false;
    t.lastAccess = ++m_AccessCounter;
    return t;
}

void TileCache::EvictLRU() {
    if (m_Tiles.empty()) return;
    // Find the tile with the smallest lastAccess that is not dirty
    // (prefer evicting clean tiles to avoid losing unsaved data)
    uint32_t victim = 0;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    bool     foundClean = false;

    for (auto& [k, t] : m_Tiles) {
        if (!t.dirty && t.lastAccess < oldest) {
            oldest     = t.lastAccess;
            victim     = k;
            foundClean = true;
        }
    }
    if (!foundClean) {
        // All dirty — fallback: evict oldest regardless (shouldn't normally happen)
        for (auto& [k, t] : m_Tiles) {
            if (t.lastAccess < oldest) {
                oldest = t.lastAccess;
                victim = k;
            }
        }
    }
    m_Tiles.erase(victim);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TileCache::Init(int width, int height, CanvasPixelFormat format) {
    m_Width         = width;
    m_Height        = height;
    m_Format        = format;
    m_BytesPerPixel = BytesPerPixel(format);
    m_TilesX        = (width  + TILE_SIZE - 1) / TILE_SIZE;
    m_TilesY        = (height + TILE_SIZE - 1) / TILE_SIZE;
    m_AccessCounter = 0;
    // Large documents need to stay resident; otherwise the cache will evict
    // freshly imported tiles and the image appears truncated.
    m_MaxTilesInRAM = std::max(m_MaxTilesInRAM, (size_t)m_TilesX * (size_t)m_TilesY);
    m_Tiles.clear();
    m_PendingGpuClears.clear();
}

void TileCache::Clear() {
    m_Tiles.clear();
    m_PendingGpuClears.clear();
    m_AccessCounter = 0;
}

void TileCache::Resize(int newWidth, int newHeight) {
    if (newWidth == m_Width && newHeight == m_Height) return;

    TileCache tmp;
    tmp.Init(newWidth, newHeight, m_Format);
    tmp.SetMaxTilesInRAM(m_MaxTilesInRAM);

    int copyW = std::min(m_Width, newWidth);
    int copyH = std::min(m_Height, newHeight);
    if (copyW > 0 && copyH > 0) {
        tmp.CopyFrom(*this, 0, 0, 0, 0, copyW, copyH);
    }

    *this = std::move(tmp);
}

bool TileCache::HasTile(int tileX, int tileY) const {
    return m_Tiles.count(Key(tileX, tileY)) > 0;
}

// ---- Pixel read/write ----

void TileCache::GetPixelF(int x, int y, float rgba[4]) const {
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }
    const Tile* t = FindTile(x / TILE_SIZE, y / TILE_SIZE);
    if (!t) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }
    int    lx  = x % TILE_SIZE;
    int    ly  = y % TILE_SIZE;
    size_t off = ((size_t)ly * TILE_SIZE + lx) * m_BytesPerPixel;

    if (m_Format == CanvasPixelFormat::RGBA8) {
        const uint8_t* p = t->data.data() + off;
        rgba[0] = p[0] / 255.0f;
        rgba[1] = p[1] / 255.0f;
        rgba[2] = p[2] / 255.0f;
        rgba[3] = p[3] / 255.0f;
    } else {
        const float* p = reinterpret_cast<const float*>(t->data.data() + off);
        rgba[0] = p[0]; rgba[1] = p[1]; rgba[2] = p[2]; rgba[3] = p[3];
    }
}

void TileCache::SetPixelF(int x, int y, const float rgba[4]) {
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return;
    Tile& t  = GetOrCreateTile(x / TILE_SIZE, y / TILE_SIZE);
    int lx   = x % TILE_SIZE;
    int ly   = y % TILE_SIZE;
    size_t off = ((size_t)ly * TILE_SIZE + lx) * m_BytesPerPixel;

    if (m_Format == CanvasPixelFormat::RGBA8) {
        uint8_t* p = t.data.data() + off;
        p[0] = (uint8_t)(std::clamp(rgba[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        p[1] = (uint8_t)(std::clamp(rgba[1], 0.0f, 1.0f) * 255.0f + 0.5f);
        p[2] = (uint8_t)(std::clamp(rgba[2], 0.0f, 1.0f) * 255.0f + 0.5f);
        p[3] = (uint8_t)(std::clamp(rgba[3], 0.0f, 1.0f) * 255.0f + 0.5f);
    } else {
        float* p = reinterpret_cast<float*>(t.data.data() + off);
        p[0] = rgba[0]; p[1] = rgba[1]; p[2] = rgba[2]; p[3] = rgba[3];
    }
    t.dirty = true;
}

void TileCache::GetPixelU8(int x, int y, uint8_t rgba[4]) const {
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return;
    }
    const Tile* t = FindTile(x / TILE_SIZE, y / TILE_SIZE);
    if (!t) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
    size_t off = ((size_t)(y % TILE_SIZE) * TILE_SIZE + (x % TILE_SIZE)) * 4;
    const uint8_t* p = t->data.data() + off;
    rgba[0] = p[0]; rgba[1] = p[1]; rgba[2] = p[2]; rgba[3] = p[3];
}

void TileCache::SetPixelU8(int x, int y, const uint8_t rgba[4]) {
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return;
    Tile& t    = GetOrCreateTile(x / TILE_SIZE, y / TILE_SIZE);
    size_t off = ((size_t)(y % TILE_SIZE) * TILE_SIZE + (x % TILE_SIZE)) * 4;
    uint8_t* p = t.data.data() + off;
    p[0] = rgba[0]; p[1] = rgba[1]; p[2] = rgba[2]; p[3] = rgba[3];
    t.dirty = true;
}

// ---- Direct tile access ----

const uint8_t* TileCache::GetTileData(int tileX, int tileY) const {
    const Tile* t = FindTile(tileX, tileY);
    return t ? t->data.data() : nullptr;
}

uint8_t* TileCache::LockTile(int tileX, int tileY) {
    Tile& t  = GetOrCreateTile(tileX, tileY);
    t.dirty  = true;
    return t.data.data();
}

// ---- Bulk ops ----

void TileCache::Fill(const float rgba[4]) {
    // Fill all existing tiles and any tile that would be needed (skip: only fills existing)
    // For a proper fill we need to create all tiles — only do so if canvas is small.
    // Practical usage: Fill is called on new empty layer → no tiles → nothing to do,
    // transparent (zeroed) is the correct initial state for RGBA unless a non-zero fill.
    bool isTransparent = (rgba[0] == 0.0f && rgba[1] == 0.0f &&
                          rgba[2] == 0.0f && rgba[3] == 0.0f);
    if (isTransparent) {
        Clear(); // zeroes == empty tile
        return;
    }
    // Create and fill all tiles
    for (int ty = 0; ty < m_TilesY; ++ty) {
        for (int tx = 0; tx < m_TilesX; ++tx) {
            uint8_t* data = LockTile(tx, ty);
            size_t pixels = (size_t)TILE_SIZE * TILE_SIZE;
            if (m_Format == CanvasPixelFormat::RGBA8) {
                uint8_t r = (uint8_t)(std::clamp(rgba[0],0.f,1.f)*255.f+.5f);
                uint8_t g = (uint8_t)(std::clamp(rgba[1],0.f,1.f)*255.f+.5f);
                uint8_t b = (uint8_t)(std::clamp(rgba[2],0.f,1.f)*255.f+.5f);
                uint8_t a = (uint8_t)(std::clamp(rgba[3],0.f,1.f)*255.f+.5f);
                for (size_t i = 0; i < pixels; ++i) {
                    data[i*4+0]=r; data[i*4+1]=g; data[i*4+2]=b; data[i*4+3]=a;
                }
            } else {
                float* fp = reinterpret_cast<float*>(data);
                for (size_t i = 0; i < pixels; ++i) {
                    fp[i*4+0]=rgba[0]; fp[i*4+1]=rgba[1];
                    fp[i*4+2]=rgba[2]; fp[i*4+3]=rgba[3];
                }
            }
        }
    }
}

void TileCache::FillRect(int x0, int y0, int x1, int y1, const float rgba[4]) {
    x0 = std::clamp(x0, 0, m_Width  - 1);
    y0 = std::clamp(y0, 0, m_Height - 1);
    x1 = std::clamp(x1, 0, m_Width  - 1);
    y1 = std::clamp(y1, 0, m_Height - 1);
    if (x0 > x1 || y0 > y1) return;

    int txMin = x0 / TILE_SIZE, txMax = x1 / TILE_SIZE;
    int tyMin = y0 / TILE_SIZE, tyMax = y1 / TILE_SIZE;

    for (int ty = tyMin; ty <= tyMax; ++ty) {
        for (int tx = txMin; tx <= txMax; ++tx) {
            uint8_t* raw = LockTile(tx, ty);
            int lx0 = std::max(x0, tx * TILE_SIZE)       - tx * TILE_SIZE;
            int lx1 = std::min(x1, (tx+1)*TILE_SIZE - 1) - tx * TILE_SIZE;
            int ly0 = std::max(y0, ty * TILE_SIZE)       - ty * TILE_SIZE;
            int ly1 = std::min(y1, (ty+1)*TILE_SIZE - 1) - ty * TILE_SIZE;
            for (int ly = ly0; ly <= ly1; ++ly) {
                if (m_Format == CanvasPixelFormat::RGBA8) {
                    uint8_t r=(uint8_t)(std::clamp(rgba[0],0.f,1.f)*255.f+.5f);
                    uint8_t g=(uint8_t)(std::clamp(rgba[1],0.f,1.f)*255.f+.5f);
                    uint8_t b=(uint8_t)(std::clamp(rgba[2],0.f,1.f)*255.f+.5f);
                    uint8_t a=(uint8_t)(std::clamp(rgba[3],0.f,1.f)*255.f+.5f);
                    for (int lx = lx0; lx <= lx1; ++lx) {
                        size_t off = ((size_t)ly*TILE_SIZE+lx)*4;
                        raw[off]=r; raw[off+1]=g; raw[off+2]=b; raw[off+3]=a;
                    }
                } else {
                    float* fp = reinterpret_cast<float*>(raw);
                    for (int lx = lx0; lx <= lx1; ++lx) {
                        size_t off = ((size_t)ly*TILE_SIZE+lx)*4;
                        fp[off]=rgba[0]; fp[off+1]=rgba[1];
                        fp[off+2]=rgba[2]; fp[off+3]=rgba[3];
                    }
                }
            }
        }
    }
}

void TileCache::CopyFrom(const TileCache& src, int srcX, int srcY,
                          int dstX, int dstY, int w, int h) {
    // Clamp to both canvases
    if (w <= 0 || h <= 0) return;
    w = std::min(w, std::min(src.m_Width  - srcX, m_Width  - dstX));
    h = std::min(h, std::min(src.m_Height - srcY, m_Height - dstY));
    if (w <= 0 || h <= 0) return;

    // Row-by-row copy via float interface (handles format mismatch)
    for (int row = 0; row < h; ++row) {
        int sy = srcY + row;
        int dy = dstY + row;
        for (int col = 0; col < w; ++col) {
            float rgba[4];
            src.GetPixelF(srcX + col, sy, rgba);
            SetPixelF(dstX + col, dy, rgba);
        }
    }
}

// ---- Import from raw buffers ----

void TileCache::ImportRGBA8(const uint8_t* data, int srcWidth, int srcHeight,
                             int dstX, int dstY) {
    int copyW = std::min(srcWidth,  m_Width  - dstX);
    int copyH = std::min(srcHeight, m_Height - dstY);
    if (copyW <= 0 || copyH <= 0) return;

    for (int row = 0; row < copyH; ++row) {
        int dy = dstY + row;
        int txMin = dstX / TILE_SIZE;
        int txMax = (dstX + copyW - 1) / TILE_SIZE;
        for (int tx = txMin; tx <= txMax; ++tx) {
            int lx0 = std::max(dstX, tx*TILE_SIZE)          - tx*TILE_SIZE;
            int lx1 = std::min(dstX+copyW-1,(tx+1)*TILE_SIZE-1) - tx*TILE_SIZE;
            int ly  = dy % TILE_SIZE;
            uint8_t* raw = LockTile(tx, dy / TILE_SIZE);

            if (m_Format == CanvasPixelFormat::RGBA8) {
                for (int lx = lx0; lx <= lx1; ++lx) {
                    int srcCol = (tx*TILE_SIZE + lx) - dstX;
                    const uint8_t* sp = data + ((size_t)row * srcWidth + srcCol) * 4;
                    uint8_t* dp = raw + ((size_t)ly * TILE_SIZE + lx) * 4;
                    dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=sp[3];
                }
            } else { // RGBA32F: convert uint8 → float
                float* fp = reinterpret_cast<float*>(raw);
                for (int lx = lx0; lx <= lx1; ++lx) {
                    int srcCol = (tx*TILE_SIZE + lx) - dstX;
                    const uint8_t* sp = data + ((size_t)row * srcWidth + srcCol) * 4;
                    size_t off = ((size_t)ly * TILE_SIZE + lx) * 4;
                    fp[off+0]=sp[0]/255.f; fp[off+1]=sp[1]/255.f;
                    fp[off+2]=sp[2]/255.f; fp[off+3]=sp[3]/255.f;
                }
            }
        }
    }
}

void TileCache::ImportRGBA32F(const float* data, int srcWidth, int srcHeight,
                               int dstX, int dstY) {
    int copyW = std::min(srcWidth,  m_Width  - dstX);
    int copyH = std::min(srcHeight, m_Height - dstY);
    if (copyW <= 0 || copyH <= 0) return;

    for (int row = 0; row < copyH; ++row) {
        int dy  = dstY + row;
        int txMin = dstX / TILE_SIZE;
        int txMax = (dstX + copyW - 1) / TILE_SIZE;
        for (int tx = txMin; tx <= txMax; ++tx) {
            int lx0 = std::max(dstX, tx*TILE_SIZE)              - tx*TILE_SIZE;
            int lx1 = std::min(dstX+copyW-1,(tx+1)*TILE_SIZE-1) - tx*TILE_SIZE;
            int ly  = dy % TILE_SIZE;
            uint8_t* raw = LockTile(tx, dy / TILE_SIZE);

            if (m_Format == CanvasPixelFormat::RGBA32F) {
                float* fp = reinterpret_cast<float*>(raw);
                for (int lx = lx0; lx <= lx1; ++lx) {
                    int srcCol = (tx*TILE_SIZE + lx) - dstX;
                    const float* sp = data + ((size_t)row * srcWidth + srcCol) * 4;
                    size_t off = ((size_t)ly * TILE_SIZE + lx) * 4;
                    fp[off+0]=sp[0]; fp[off+1]=sp[1]; fp[off+2]=sp[2]; fp[off+3]=sp[3];
                }
            } else { // RGBA8: clamp float → uint8
                for (int lx = lx0; lx <= lx1; ++lx) {
                    int srcCol = (tx*TILE_SIZE + lx) - dstX;
                    const float* sp = data + ((size_t)row * srcWidth + srcCol) * 4;
                    size_t off = ((size_t)ly * TILE_SIZE + lx) * 4;
                    raw[off+0]=(uint8_t)(std::clamp(sp[0],0.f,1.f)*255.f+.5f);
                    raw[off+1]=(uint8_t)(std::clamp(sp[1],0.f,1.f)*255.f+.5f);
                    raw[off+2]=(uint8_t)(std::clamp(sp[2],0.f,1.f)*255.f+.5f);
                    raw[off+3]=(uint8_t)(std::clamp(sp[3],0.f,1.f)*255.f+.5f);
                }
            }
        }
    }
}

// ---- Export ----

void TileCache::ExportRGBA8(uint8_t* outData, int outWidth, int outHeight) const {
    // Zero-fill output first (empty tiles → transparent)
    std::memset(outData, 0, (size_t)outWidth * outHeight * 4);

    int copyW = std::min(m_Width,  outWidth);
    int copyH = std::min(m_Height, outHeight);

    for (int y = 0; y < copyH; ++y) {
        int ty = y / TILE_SIZE;
        int ly = y % TILE_SIZE;
        uint8_t* dstRow = outData + ((size_t)y * outWidth) * 4;

        for (int tx = 0; tx < m_TilesX; ++tx) {
            int x0 = tx * TILE_SIZE;
            if (x0 >= copyW) break;
            int x1 = std::min(x0 + TILE_SIZE, copyW);
            const Tile* t = FindTile(tx, ty);
            if (!t) continue;

            if (m_Format == CanvasPixelFormat::RGBA8) {
                const uint8_t* srcRow = t->data.data() + ((size_t)ly * TILE_SIZE) * 4;
                std::memcpy(dstRow + ((size_t)x0 * 4), srcRow, (size_t)(x1 - x0) * 4);
            } else {
                const float* fp = reinterpret_cast<const float*>(t->data.data());
                for (int cx = x0; cx < x1; ++cx) {
                    int lx = cx - x0;
                    size_t si = ((size_t)ly * TILE_SIZE + lx) * 4;
                    size_t di = ((size_t)cx) * 4;
                    dstRow[di + 0] = (uint8_t)(std::clamp(fp[si + 0], 0.f, 1.f) * 255.f + .5f);
                    dstRow[di + 1] = (uint8_t)(std::clamp(fp[si + 1], 0.f, 1.f) * 255.f + .5f);
                    dstRow[di + 2] = (uint8_t)(std::clamp(fp[si + 2], 0.f, 1.f) * 255.f + .5f);
                    dstRow[di + 3] = (uint8_t)(std::clamp(fp[si + 3], 0.f, 1.f) * 255.f + .5f);
                }
            }
        }
    }
}

void TileCache::ExportRGBA32F(float* outData, int outWidth, int outHeight) const {
    std::memset(outData, 0, (size_t)outWidth * outHeight * 4 * sizeof(float));

    int copyW = std::min(m_Width,  outWidth);
    int copyH = std::min(m_Height, outHeight);

    for (int y = 0; y < copyH; ++y) {
        int ty = y / TILE_SIZE;
        int ly = y % TILE_SIZE;
        float* dstRow = outData + ((size_t)y * outWidth) * 4;

        for (int tx = 0; tx < m_TilesX; ++tx) {
            int x0 = tx * TILE_SIZE;
            if (x0 >= copyW) break;
            int x1 = std::min(x0 + TILE_SIZE, copyW);
            const Tile* t = FindTile(tx, ty);
            if (!t) continue;

            if (m_Format == CanvasPixelFormat::RGBA32F) {
                const float* fp = reinterpret_cast<const float*>(t->data.data());
                std::memcpy(dstRow + ((size_t)x0 * 4), fp + ((size_t)ly * TILE_SIZE) * 4, (size_t)(x1 - x0) * 4 * sizeof(float));
            } else {
                for (int cx = x0; cx < x1; ++cx) {
                    int lx = cx - x0;
                    size_t si = ((size_t)ly * TILE_SIZE + lx) * 4;
                    size_t di = ((size_t)cx) * 4;
                    dstRow[di + 0] = t->data[si + 0] / 255.f;
                    dstRow[di + 1] = t->data[si + 1] / 255.f;
                    dstRow[di + 2] = t->data[si + 2] / 255.f;
                    dstRow[di + 3] = t->data[si + 3] / 255.f;
                }
            }
        }
    }
}

// ---- Dirty tracking ----

void TileCache::QueueGpuClear(int tileX, int tileY) {
    if (tileX < 0 || tileY < 0 || tileX >= m_TilesX || tileY >= m_TilesY) return;
    m_PendingGpuClears.insert(Key(tileX, tileY));
}

void TileCache::MarkDirty(int tileX, int tileY) {
    Tile* t = FindTile(tileX, tileY);
    if (t) {
        t->dirty = true;
        // Restored/real tile supersedes a pending clear for this cell.
        m_PendingGpuClears.erase(Key(tileX, tileY));
    } else {
        // No CPU tile but GPU may still hold pixels (undo of first stroke).
        QueueGpuClear(tileX, tileY);
    }
}

void TileCache::MarkAllDirty() {
    for (auto& [k, t] : m_Tiles) t.dirty = true;
    m_PendingGpuClears.clear();
}

void TileCache::ClearDirty(int tileX, int tileY) {
    Tile* t = FindTile(tileX, tileY);
    if (t) t->dirty = false;
    m_PendingGpuClears.erase(Key(tileX, tileY));
}

void TileCache::ClearAllDirty() {
    for (auto& [k, t] : m_Tiles) t.dirty = false;
    m_PendingGpuClears.clear();
}

bool TileCache::IsDirty(int tileX, int tileY) const {
    if (m_PendingGpuClears.count(Key(tileX, tileY))) return true;
    const Tile* t = FindTile(tileX, tileY);
    return t && t->dirty;
}

bool TileCache::HasPendingGpuWork() const {
    if (!m_PendingGpuClears.empty()) return true;
    for (const auto& [k, t] : m_Tiles) {
        if (t.dirty) return true;
    }
    return false;
}

void TileCache::ForEachDirtyTile(
    std::function<void(int, int, const uint8_t*, int)> cb) const
{
    const int pitch = TILE_SIZE * m_BytesPerPixel;
    const size_t tileBytes = (size_t)pitch * TILE_SIZE;

    for (const auto& [key, t] : m_Tiles) {
        if (!t.dirty) continue;
        // Skip if also listed as clear (should not happen after MarkDirty).
        if (m_PendingGpuClears.count(key)) continue;
        int tx = (int)(key % (uint32_t)m_TilesX);
        int ty = (int)(key / (uint32_t)m_TilesX);
        cb(tx, ty, t.data.data(), pitch);
    }

    if (!m_PendingGpuClears.empty()) {
        // Shared zero slab for GPU clears (transparent tile).
        static thread_local std::vector<uint8_t> zeroTile;
        if (zeroTile.size() < tileBytes) {
            zeroTile.assign(tileBytes, 0);
        } else {
            std::fill(zeroTile.begin(), zeroTile.begin() + (std::ptrdiff_t)tileBytes, 0);
        }
        for (uint32_t key : m_PendingGpuClears) {
            // If a live tile exists for this key, dirty path above handles it.
            if (m_Tiles.count(key)) continue;
            int tx = (int)(key % (uint32_t)m_TilesX);
            int ty = (int)(key / (uint32_t)m_TilesX);
            cb(tx, ty, zeroTile.data(), pitch);
        }
    }
}

// ---- Undo/Redo snapshots ----

std::vector<uint8_t> TileCache::SnapshotTile(int tileX, int tileY) const {
    const Tile* t = FindTile(tileX, tileY);
    if (!t) return {}; // empty = tile doesn't exist (transparent)
    return t->data;    // copy
}

void TileCache::RestoreTile(int tileX, int tileY, const std::vector<uint8_t>& data) {
    if (tileX < 0 || tileY < 0 || tileX >= m_TilesX || tileY >= m_TilesY) return;

    if (data.empty()) {
        // Sparse erase + force GPU clear so undo of "first paint on empty"
        // does not leave stale texels on the layer texture.
        const uint32_t key = Key(tileX, tileY);
        m_Tiles.erase(key);
        m_PendingGpuClears.insert(key);
        return;
    }

    // Restoring real pixels cancels any pending clear for this cell.
    m_PendingGpuClears.erase(Key(tileX, tileY));
    Tile& t = GetOrCreateTile(tileX, tileY);
    t.data  = data;
    t.dirty = true;
}
