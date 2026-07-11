#include "MaskTiles.h"
#include <algorithm>
#include <cstring>

void MaskTiles::Init(int width, int height, uint8_t defaultVal) {
    m_Width = std::max(0, width);
    m_Height = std::max(0, height);
    m_Default = defaultVal;
    m_TilesX = m_Width > 0 ? (m_Width + MASK_TILE_SIZE - 1) / MASK_TILE_SIZE : 0;
    m_TilesY = m_Height > 0 ? (m_Height + MASK_TILE_SIZE - 1) / MASK_TILE_SIZE : 0;
    m_Tiles.clear();
    ClearDirty();
}

void MaskTiles::Clear() {
    m_Tiles.clear();
    ClearDirty();
}

const std::vector<uint8_t>* MaskTiles::Find(int tx, int ty) const {
    if (tx < 0 || ty < 0 || tx >= m_TilesX || ty >= m_TilesY) return nullptr;
    auto it = m_Tiles.find(Key(tx, ty));
    if (it == m_Tiles.end() || !it->second) return nullptr;
    return it->second.get();
}

std::vector<uint8_t>& MaskTiles::EnsureWritable(int tx, int ty) {
    auto& slot = m_Tiles[Key(tx, ty)];
    if (!slot) {
        slot = std::make_shared<std::vector<uint8_t>>(
            (size_t)MASK_TILE_SIZE * MASK_TILE_SIZE, m_Default);
    } else if (slot.use_count() > 1) {
        // COW
        slot = std::make_shared<std::vector<uint8_t>>(*slot);
    }
    return *slot;
}

uint8_t MaskTiles::Get(int x, int y) const {
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return m_Default;
    int tx = x / MASK_TILE_SIZE, ty = y / MASK_TILE_SIZE;
    const auto* t = Find(tx, ty);
    if (!t) return m_Default;
    int lx = x - tx * MASK_TILE_SIZE;
    int ly = y - ty * MASK_TILE_SIZE;
    return (*t)[(size_t)ly * MASK_TILE_SIZE + lx];
}

void MaskTiles::Set(int x, int y, uint8_t v) {
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return;
    int tx = x / MASK_TILE_SIZE, ty = y / MASK_TILE_SIZE;
    // Skip allocate if writing default into absent tile
    if (v == m_Default && !Find(tx, ty)) {
        ExpandDirty(x, y, x, y);
        return;
    }
    auto& tile = EnsureWritable(tx, ty);
    int lx = x - tx * MASK_TILE_SIZE;
    int ly = y - ty * MASK_TILE_SIZE;
    tile[(size_t)ly * MASK_TILE_SIZE + lx] = v;
    ExpandDirty(x, y, x, y);
}

void MaskTiles::ExpandDirty(int x0, int y0, int x1, int y1) {
    if (m_Width <= 0) return;
    x0 = std::clamp(x0, 0, m_Width - 1);
    y0 = std::clamp(y0, 0, m_Height - 1);
    x1 = std::clamp(x1, 0, m_Width - 1);
    y1 = std::clamp(y1, 0, m_Height - 1);
    if (m_DirtyX1 < m_DirtyX0) {
        m_DirtyX0 = x0; m_DirtyY0 = y0; m_DirtyX1 = x1; m_DirtyY1 = y1;
    } else {
        m_DirtyX0 = std::min(m_DirtyX0, x0);
        m_DirtyY0 = std::min(m_DirtyY0, y0);
        m_DirtyX1 = std::max(m_DirtyX1, x1);
        m_DirtyY1 = std::max(m_DirtyY1, y1);
    }
}

void MaskTiles::ClearDirty() {
    m_DirtyX0 = 0; m_DirtyY0 = 0; m_DirtyX1 = -1; m_DirtyY1 = -1;
}

std::vector<MaskTileSnapshot> MaskTiles::SnapshotRect(int x0, int y0, int x1, int y1) const {
    std::vector<MaskTileSnapshot> out;
    if (m_Width <= 0 || x1 < x0 || y1 < y0) return out;
    x0 = std::clamp(x0, 0, m_Width - 1);
    y0 = std::clamp(y0, 0, m_Height - 1);
    x1 = std::clamp(x1, 0, m_Width - 1);
    y1 = std::clamp(y1, 0, m_Height - 1);
    int tx0 = x0 / MASK_TILE_SIZE, ty0 = y0 / MASK_TILE_SIZE;
    int tx1 = x1 / MASK_TILE_SIZE, ty1 = y1 / MASK_TILE_SIZE;
    out.reserve((size_t)(tx1 - tx0 + 1) * (ty1 - ty0 + 1));
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            MaskTileSnapshot s;
            s.tileX = tx; s.tileY = ty;
            auto it = m_Tiles.find(Key(tx, ty));
            if (it != m_Tiles.end())
                s.data = it->second; // share
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::vector<MaskTileSnapshot> MaskTiles::SnapshotAll() const {
    return SnapshotRect(0, 0, m_Width - 1, m_Height - 1);
}

void MaskTiles::RestoreTiles(const std::vector<MaskTileSnapshot>& snaps) {
    for (const auto& s : snaps) {
        if (s.tileX < 0 || s.tileY < 0 || s.tileX >= m_TilesX || s.tileY >= m_TilesY)
            continue;
        uint32_t k = Key(s.tileX, s.tileY);
        if (!s.data) {
            m_Tiles.erase(k);
        } else {
            m_Tiles[k] = s.data; // share restored blob
        }
        int x0 = s.tileX * MASK_TILE_SIZE;
        int y0 = s.tileY * MASK_TILE_SIZE;
        int x1 = std::min(m_Width - 1, x0 + MASK_TILE_SIZE - 1);
        int y1 = std::min(m_Height - 1, y0 + MASK_TILE_SIZE - 1);
        ExpandDirty(x0, y0, x1, y1);
    }
}

void MaskTiles::Flatten(std::vector<uint8_t>& out) const {
    out.assign((size_t)m_Width * (size_t)m_Height, m_Default);
    if (m_Width <= 0 || m_Height <= 0) return;
    for (const auto& kv : m_Tiles) {
        if (!kv.second) continue;
        int tx = (int)(kv.first & 0xFFFF);
        int ty = (int)(kv.first >> 16);
        const auto& tile = *kv.second;
        for (int ly = 0; ly < MASK_TILE_SIZE; ++ly) {
            int y = ty * MASK_TILE_SIZE + ly;
            if (y >= m_Height) break;
            for (int lx = 0; lx < MASK_TILE_SIZE; ++lx) {
                int x = tx * MASK_TILE_SIZE + lx;
                if (x >= m_Width) break;
                out[(size_t)y * m_Width + x] = tile[(size_t)ly * MASK_TILE_SIZE + lx];
            }
        }
    }
}

void MaskTiles::ImportFlat(const std::vector<uint8_t>& flat, int w, int h) {
    Init(w, h, m_Default);
    if ((int)flat.size() < w * h) return;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t v = flat[(size_t)y * w + x];
            if (v != m_Default)
                Set(x, y, v);
        }
    }
    // Full dirty for GPU
    if (w > 0 && h > 0)
        ExpandDirty(0, 0, w - 1, h - 1);
}

size_t MaskTiles::EstimateBytes() const {
    size_t n = 0;
    std::unordered_map<const void*, bool> seen;
    for (const auto& kv : m_Tiles) {
        if (!kv.second) continue;
        const void* p = kv.second.get();
        if (seen.count(p)) continue;
        seen[p] = true;
        n += kv.second->size();
    }
    return n;
}

void MaskTiles::ForEachTile(const std::function<void(int, int, const uint8_t*)>& cb) const {
    for (const auto& kv : m_Tiles) {
        if (!kv.second) continue;
        int tx = (int)(kv.first & 0xFFFF);
        int ty = (int)(kv.first >> 16);
        cb(tx, ty, kv.second->data());
    }
}
