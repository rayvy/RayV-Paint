#include "TileCache.h"
#include "HalfFloat.h"
#include <cstring>
#include <limits>
#include <unordered_set>
#include <unordered_map>

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

TileDataPtr TileCache::MakeBlankData() const {
    return std::make_shared<TileData>((size_t)TILE_SIZE * TILE_SIZE * m_BytesPerPixel);
}

void TileCache::EnsureWritable(Tile& t) {
    if (!t.data) {
        t.data = MakeBlankData();
        return;
    }
    // COW: if undo history / another cache still holds this blob, clone before write.
    if (t.data.use_count() > 1) {
        t.data = std::make_shared<TileData>(*t.data);
    }
}

TileCache::Tile& TileCache::GetOrCreateTile(int tileX, int tileY) {
    uint32_t key = Key(tileX, tileY);
    auto it = m_Tiles.find(key);
    if (it != m_Tiles.end()) {
        it->second.lastAccess = ++m_AccessCounter;
        return it->second;
    }

    if (m_Tiles.size() >= m_MaxTilesInRAM) {
        EvictLRU();
    }

    Tile& t = m_Tiles[key];
    t.data       = MakeBlankData();
    t.dirty      = false;
    t.lastAccess = ++m_AccessCounter;
    m_PendingGpuClears.erase(key);
    return t;
}

void TileCache::EvictLRU() {
    if (m_Tiles.empty()) return;
    uint32_t victim = 0;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    bool foundClean = false;

    for (auto& [k, t] : m_Tiles) {
        if (!t.dirty && t.lastAccess < oldest) {
            oldest     = t.lastAccess;
            victim     = k;
            foundClean = true;
        }
    }
    if (!foundClean) {
        for (auto& [k, t] : m_Tiles) {
            if (t.lastAccess < oldest) {
                oldest = t.lastAccess;
                victim = k;
            }
        }
    }
    // Shared TileData may live on in undo history after eviction.
    // GPU still has the slot — queue clear so compose does not draw a ghost.
    m_PendingGpuClears.insert(victim);
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
    m_MaxTilesInRAM = std::max(m_MaxTilesInRAM, (size_t)m_TilesX * (size_t)m_TilesY);
    m_Tiles.clear();
    m_PendingGpuClears.clear();
}

void TileCache::Clear() {
    // Queue GPU clears for every tile that existed — otherwise sparse GpuTileStore
    // keeps ghosts after vector full-raster / shape delete (CPU empty, VRAM still painted).
    for (const auto& [k, t] : m_Tiles)
        m_PendingGpuClears.insert(k);
    m_Tiles.clear();
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

size_t TileCache::EstimateUniquePixelBytes() const {
    std::unordered_set<const TileData*> seen;
    size_t bytes = 0;
    for (const auto& [k, t] : m_Tiles) {
        if (!t.data) continue;
        if (seen.insert(t.data.get()).second) {
            bytes += t.data->ByteSize();
        }
    }
    return bytes;
}

// ---- Pixel read/write ----

void TileCache::GetPixelF(int x, int y, float rgba[4]) const {
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }
    const Tile* t = FindTile(x / TILE_SIZE, y / TILE_SIZE);
    if (!t || !t->data) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }
    int    lx  = x % TILE_SIZE;
    int    ly  = y % TILE_SIZE;
    size_t off = ((size_t)ly * TILE_SIZE + lx) * m_BytesPerPixel;
    const uint8_t* base = t->data->pixels.data() + off;

    if (m_Format == CanvasPixelFormat::RGBA8) {
        rgba[0] = base[0] / 255.0f;
        rgba[1] = base[1] / 255.0f;
        rgba[2] = base[2] / 255.0f;
        rgba[3] = base[3] / 255.0f;
    } else if (m_Format == CanvasPixelFormat::RGBA16F) {
        HalfFloat::LoadRGBA16F(base, rgba);
    } else {
        const float* p = reinterpret_cast<const float*>(base);
        rgba[0] = p[0]; rgba[1] = p[1]; rgba[2] = p[2]; rgba[3] = p[3];
    }
}

void TileCache::SetPixelF(int x, int y, const float rgba[4]) {
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return;
    Tile& t = GetOrCreateTile(x / TILE_SIZE, y / TILE_SIZE);
    EnsureWritable(t);
    int lx   = x % TILE_SIZE;
    int ly   = y % TILE_SIZE;
    size_t off = ((size_t)ly * TILE_SIZE + lx) * m_BytesPerPixel;
    uint8_t* base = t.data->pixels.data() + off;

    if (m_Format == CanvasPixelFormat::RGBA8) {
        base[0] = HalfFloat::FloatToU8(rgba[0]);
        base[1] = HalfFloat::FloatToU8(rgba[1]);
        base[2] = HalfFloat::FloatToU8(rgba[2]);
        base[3] = HalfFloat::FloatToU8(rgba[3]);
    } else if (m_Format == CanvasPixelFormat::RGBA16F) {
        HalfFloat::StoreRGBA16F(base, rgba);
    } else {
        float* p = reinterpret_cast<float*>(base);
        p[0] = rgba[0]; p[1] = rgba[1]; p[2] = rgba[2]; p[3] = rgba[3];
    }
    t.dirty = true;
}

void TileCache::GetPixelU8(int x, int y, uint8_t rgba[4]) const {
    float f[4];
    GetPixelF(x, y, f);
    rgba[0] = HalfFloat::FloatToU8(f[0]);
    rgba[1] = HalfFloat::FloatToU8(f[1]);
    rgba[2] = HalfFloat::FloatToU8(f[2]);
    rgba[3] = HalfFloat::FloatToU8(f[3]);
}

void TileCache::SetPixelU8(int x, int y, const uint8_t rgba[4]) {
    float f[4] = {
        rgba[0] / 255.0f, rgba[1] / 255.0f,
        rgba[2] / 255.0f, rgba[3] / 255.0f
    };
    SetPixelF(x, y, f);
}

const uint8_t* TileCache::GetTileData(int tileX, int tileY) const {
    const Tile* t = FindTile(tileX, tileY);
    return (t && t->data) ? t->data->pixels.data() : nullptr;
}

uint8_t* TileCache::LockTile(int tileX, int tileY) {
    Tile& t = GetOrCreateTile(tileX, tileY);
    EnsureWritable(t);
    t.dirty = true;
    m_PendingGpuClears.erase(Key(tileX, tileY));
    return t.data->pixels.data();
}

// ---- Bulk ops (via LockTile → automatic COW) ----

void TileCache::Fill(const float rgba[4]) {
    bool isTransparent = (rgba[0] == 0.0f && rgba[1] == 0.0f &&
                          rgba[2] == 0.0f && rgba[3] == 0.0f);
    if (isTransparent) {
        // Full clear: queue GPU clears for every existing tile, then drop them.
        for (const auto& [k, t] : m_Tiles) {
            m_PendingGpuClears.insert(k);
        }
        m_Tiles.clear();
        return;
    }
    for (int ty = 0; ty < m_TilesY; ++ty) {
        for (int tx = 0; tx < m_TilesX; ++tx) {
            uint8_t* data = LockTile(tx, ty);
            size_t pixels = (size_t)TILE_SIZE * TILE_SIZE;
            if (m_Format == CanvasPixelFormat::RGBA8) {
                uint8_t r = HalfFloat::FloatToU8(rgba[0]);
                uint8_t g = HalfFloat::FloatToU8(rgba[1]);
                uint8_t b = HalfFloat::FloatToU8(rgba[2]);
                uint8_t a = HalfFloat::FloatToU8(rgba[3]);
                for (size_t i = 0; i < pixels; ++i) {
                    data[i*4+0]=r; data[i*4+1]=g; data[i*4+2]=b; data[i*4+3]=a;
                }
            } else if (m_Format == CanvasPixelFormat::RGBA16F) {
                for (size_t i = 0; i < pixels; ++i)
                    HalfFloat::StoreRGBA16F(data + i * 8, rgba);
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
                    uint8_t r=HalfFloat::FloatToU8(rgba[0]);
                    uint8_t g=HalfFloat::FloatToU8(rgba[1]);
                    uint8_t b=HalfFloat::FloatToU8(rgba[2]);
                    uint8_t a=HalfFloat::FloatToU8(rgba[3]);
                    for (int lx = lx0; lx <= lx1; ++lx) {
                        size_t off = ((size_t)ly*TILE_SIZE+lx)*4;
                        raw[off]=r; raw[off+1]=g; raw[off+2]=b; raw[off+3]=a;
                    }
                } else if (m_Format == CanvasPixelFormat::RGBA16F) {
                    for (int lx = lx0; lx <= lx1; ++lx) {
                        size_t off = ((size_t)ly*TILE_SIZE+lx)*8;
                        HalfFloat::StoreRGBA16F(raw + off, rgba);
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
    if (w <= 0 || h <= 0) return;
    w = std::min(w, std::min(src.m_Width  - srcX, m_Width  - dstX));
    h = std::min(h, std::min(src.m_Height - srcY, m_Height - dstY));
    if (w <= 0 || h <= 0) return;

    // Fast path: same format + tile-aligned full tiles → share TileData (true COW share).
    if (m_Format == src.m_Format &&
        (srcX % TILE_SIZE) == 0 && (srcY % TILE_SIZE) == 0 &&
        (dstX % TILE_SIZE) == 0 && (dstY % TILE_SIZE) == 0 &&
        (w % TILE_SIZE) == 0 && (h % TILE_SIZE) == 0) {
        int tilesW = w / TILE_SIZE;
        int tilesH = h / TILE_SIZE;
        int stx0 = srcX / TILE_SIZE;
        int sty0 = srcY / TILE_SIZE;
        int dtx0 = dstX / TILE_SIZE;
        int dty0 = dstY / TILE_SIZE;
        for (int ty = 0; ty < tilesH; ++ty) {
            for (int tx = 0; tx < tilesW; ++tx) {
                const Tile* st = src.FindTile(stx0 + tx, sty0 + ty);
                int dtx = dtx0 + tx, dty = dty0 + ty;
                if (!st || !st->data) {
                    // Source empty → erase dest + GPU clear
                    RestoreTile(dtx, dty, TileSnapshot{});
                    continue;
                }
                uint32_t key = Key(dtx, dty);
                if (m_Tiles.size() >= m_MaxTilesInRAM && !m_Tiles.count(key)) {
                    EvictLRU();
                }
                Tile& dt = m_Tiles[key];
                dt.data = st->data; // share
                dt.dirty = true;
                dt.lastAccess = ++m_AccessCounter;
                m_PendingGpuClears.erase(key);
            }
        }
        return;
    }

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
            } else if (m_Format == CanvasPixelFormat::RGBA16F) {
                for (int lx = lx0; lx <= lx1; ++lx) {
                    int srcCol = (tx*TILE_SIZE + lx) - dstX;
                    const uint8_t* sp = data + ((size_t)row * srcWidth + srcCol) * 4;
                    float f[4] = { sp[0]/255.f, sp[1]/255.f, sp[2]/255.f, sp[3]/255.f };
                    HalfFloat::StoreRGBA16F(raw + ((size_t)ly * TILE_SIZE + lx) * 8, f);
                }
            } else {
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
            } else if (m_Format == CanvasPixelFormat::RGBA16F) {
                for (int lx = lx0; lx <= lx1; ++lx) {
                    int srcCol = (tx*TILE_SIZE + lx) - dstX;
                    const float* sp = data + ((size_t)row * srcWidth + srcCol) * 4;
                    HalfFloat::StoreRGBA16F(raw + ((size_t)ly * TILE_SIZE + lx) * 8, sp);
                }
            } else {
                for (int lx = lx0; lx <= lx1; ++lx) {
                    int srcCol = (tx*TILE_SIZE + lx) - dstX;
                    const float* sp = data + ((size_t)row * srcWidth + srcCol) * 4;
                    size_t off = ((size_t)ly * TILE_SIZE + lx) * 4;
                    raw[off+0]=HalfFloat::FloatToU8(sp[0]);
                    raw[off+1]=HalfFloat::FloatToU8(sp[1]);
                    raw[off+2]=HalfFloat::FloatToU8(sp[2]);
                    raw[off+3]=HalfFloat::FloatToU8(sp[3]);
                }
            }
        }
    }
}

void TileCache::ExportRGBA8(uint8_t* outData, int outWidth, int outHeight) const {
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
            if (!t || !t->data) continue;

            if (m_Format == CanvasPixelFormat::RGBA8) {
                const uint8_t* srcRow = t->data->pixels.data() + ((size_t)ly * TILE_SIZE) * 4;
                std::memcpy(dstRow + ((size_t)x0 * 4), srcRow, (size_t)(x1 - x0) * 4);
            } else if (m_Format == CanvasPixelFormat::RGBA16F) {
                const uint8_t* base = t->data->pixels.data();
                for (int cx = x0; cx < x1; ++cx) {
                    int lx = cx - x0;
                    float f[4];
                    HalfFloat::LoadRGBA16F(base + ((size_t)ly * TILE_SIZE + lx) * 8, f);
                    size_t di = (size_t)cx * 4;
                    dstRow[di + 0] = HalfFloat::FloatToU8(f[0]);
                    dstRow[di + 1] = HalfFloat::FloatToU8(f[1]);
                    dstRow[di + 2] = HalfFloat::FloatToU8(f[2]);
                    dstRow[di + 3] = HalfFloat::FloatToU8(f[3]);
                }
            } else {
                const float* fp = reinterpret_cast<const float*>(t->data->pixels.data());
                for (int cx = x0; cx < x1; ++cx) {
                    int lx = cx - x0;
                    size_t si = ((size_t)ly * TILE_SIZE + lx) * 4;
                    size_t di = ((size_t)cx) * 4;
                    dstRow[di + 0] = HalfFloat::FloatToU8(fp[si + 0]);
                    dstRow[di + 1] = HalfFloat::FloatToU8(fp[si + 1]);
                    dstRow[di + 2] = HalfFloat::FloatToU8(fp[si + 2]);
                    dstRow[di + 3] = HalfFloat::FloatToU8(fp[si + 3]);
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
            if (!t || !t->data) continue;

            if (m_Format == CanvasPixelFormat::RGBA32F) {
                const float* fp = reinterpret_cast<const float*>(t->data->pixels.data());
                std::memcpy(dstRow + ((size_t)x0 * 4), fp + ((size_t)ly * TILE_SIZE) * 4,
                            (size_t)(x1 - x0) * 4 * sizeof(float));
            } else if (m_Format == CanvasPixelFormat::RGBA16F) {
                const uint8_t* base = t->data->pixels.data();
                for (int cx = x0; cx < x1; ++cx) {
                    int lx = cx - x0;
                    HalfFloat::LoadRGBA16F(base + ((size_t)ly * TILE_SIZE + lx) * 8,
                                           dstRow + (size_t)cx * 4);
                }
            } else {
                for (int cx = x0; cx < x1; ++cx) {
                    int lx = cx - x0;
                    size_t si = ((size_t)ly * TILE_SIZE + lx) * 4;
                    size_t di = ((size_t)cx) * 4;
                    dstRow[di + 0] = t->data->pixels[si + 0] / 255.f;
                    dstRow[di + 1] = t->data->pixels[si + 1] / 255.f;
                    dstRow[di + 2] = t->data->pixels[si + 2] / 255.f;
                    dstRow[di + 3] = t->data->pixels[si + 3] / 255.f;
                }
            }
        }
    }
}

bool TileCache::ConvertFormat(CanvasPixelFormat target) {
    if (target == m_Format) return true;
    if (m_Width <= 0 || m_Height <= 0) {
        m_Format = target;
        m_BytesPerPixel = BytesPerPixel(target);
        return true;
    }

    // Tile-wise: read each existing tile as float, re-encode into new blob.
    // Preserves sparse layout — empty tiles stay empty.
    const int oldBpp = m_BytesPerPixel;
    const CanvasPixelFormat oldFmt = m_Format;
    std::unordered_map<uint32_t, TileDataPtr> converted;
    converted.reserve(m_Tiles.size());

    for (const auto& [key, tile] : m_Tiles) {
        if (!tile.data) continue;
        const uint8_t* src = tile.data->pixels.data();
        auto out = std::make_shared<TileData>((size_t)TILE_SIZE * TILE_SIZE * BytesPerPixel(target));
        uint8_t* dst = out->pixels.data();
        const size_t n = (size_t)TILE_SIZE * TILE_SIZE;
        for (size_t i = 0; i < n; ++i) {
            float rgba[4];
            if (oldFmt == CanvasPixelFormat::RGBA8) {
                const uint8_t* p = src + i * 4;
                rgba[0] = p[0] / 255.f; rgba[1] = p[1] / 255.f;
                rgba[2] = p[2] / 255.f; rgba[3] = p[3] / 255.f;
            } else if (oldFmt == CanvasPixelFormat::RGBA16F) {
                HalfFloat::LoadRGBA16F(src + i * 8, rgba);
            } else {
                const float* p = reinterpret_cast<const float*>(src + i * 16);
                rgba[0] = p[0]; rgba[1] = p[1]; rgba[2] = p[2]; rgba[3] = p[3];
            }

            if (target == CanvasPixelFormat::RGBA8) {
                uint8_t* p = dst + i * 4;
                p[0] = HalfFloat::FloatToU8(rgba[0]);
                p[1] = HalfFloat::FloatToU8(rgba[1]);
                p[2] = HalfFloat::FloatToU8(rgba[2]);
                p[3] = HalfFloat::FloatToU8(rgba[3]);
            } else if (target == CanvasPixelFormat::RGBA16F) {
                HalfFloat::StoreRGBA16F(dst + i * 8, rgba);
            } else {
                float* p = reinterpret_cast<float*>(dst + i * 16);
                p[0] = rgba[0]; p[1] = rgba[1]; p[2] = rgba[2]; p[3] = rgba[3];
            }
        }
        converted[key] = std::move(out);
        (void)oldBpp;
    }

    m_Format = target;
    m_BytesPerPixel = BytesPerPixel(target);
    for (auto& [key, tile] : m_Tiles) {
        auto it = converted.find(key);
        if (it != converted.end()) {
            tile.data = std::move(it->second);
            tile.dirty = true;
        }
    }
    return true;
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
        m_PendingGpuClears.erase(Key(tileX, tileY));
    } else {
        QueueGpuClear(tileX, tileY);
    }
}

void TileCache::MarkAllDirty() {
    for (auto& [k, t] : m_Tiles) t.dirty = true;
    // Keep PendingGpuClears — absent tiles still need a zero/remove upload.
    // Callers that fully ClearSurface may clear pending themselves.
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
        if (!t.dirty || !t.data) continue;
        if (m_PendingGpuClears.count(key)) continue;
        int tx = (int)(key % (uint32_t)m_TilesX);
        int ty = (int)(key / (uint32_t)m_TilesX);
        cb(tx, ty, t.data->pixels.data(), pitch);
    }

    if (!m_PendingGpuClears.empty()) {
        static thread_local std::vector<uint8_t> zeroTile;
        if (zeroTile.size() < tileBytes) {
            zeroTile.assign(tileBytes, 0);
        } else {
            std::fill(zeroTile.begin(), zeroTile.begin() + (std::ptrdiff_t)tileBytes, 0);
        }
        for (uint32_t key : m_PendingGpuClears) {
            if (m_Tiles.count(key)) continue;
            int tx = (int)(key % (uint32_t)m_TilesX);
            int ty = (int)(key / (uint32_t)m_TilesX);
            cb(tx, ty, zeroTile.data(), pitch);
        }
    }
}

// ---- Shared snapshots ----

TileSnapshot TileCache::SnapshotTile(int tileX, int tileY) const {
    const Tile* t = FindTile(tileX, tileY);
    if (!t || !t->data) return {};
    // Share — no deep copy. First write after this will COW-clone.
    return TileSnapshot{ t->data };
}

void TileCache::DetachLiveFromHistory(int tileX, int tileY) {
    Tile* t = FindTile(tileX, tileY);
    if (!t || !t->data) return;
    // If live still shares the history blob (use_count > 1), clone so history is immutable.
    if (t->data.use_count() > 1) {
        t->data = std::make_shared<TileData>(*t->data);
    }
}

TileSnapshot TileCache::DeepCopySnapshot(const TileSnapshot& snap) {
    if (!snap.data) return {};
    return TileSnapshot{ std::make_shared<TileData>(*snap.data) };
}

void TileCache::EnsureResidentCapacityForFullGrid() {
    const size_t need = (size_t)std::max(1, m_TilesX) * (size_t)std::max(1, m_TilesY);
    if (m_MaxTilesInRAM < need)
        m_MaxTilesInRAM = need;
}

void TileCache::RestoreTile(int tileX, int tileY, const TileSnapshot& snap) {
    if (tileX < 0 || tileY < 0 || tileX >= m_TilesX || tileY >= m_TilesY) return;
    const uint32_t key = Key(tileX, tileY);

    if (snap.IsEmpty()) {
        m_Tiles.erase(key);
        m_PendingGpuClears.insert(key);
        return;
    }

    m_PendingGpuClears.erase(key);
    // Undo/redo must never drop other restored tiles: grow cap to full grid first.
    EnsureResidentCapacityForFullGrid();
    if (m_Tiles.size() >= m_MaxTilesInRAM && !m_Tiles.count(key)) {
        EvictLRU();
    }
    Tile& t = m_Tiles[key];
    t.data       = snap.data; // share restored history blob (immutable while use_count>1)
    t.dirty      = true;
    t.lastAccess = ++m_AccessCounter;
}
