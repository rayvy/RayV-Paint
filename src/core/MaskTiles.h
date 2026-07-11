#pragma once
// Sparse tiled R8 mask (default-white). Only non-default tiles allocate RAM.
// COW tile blobs for cheap undo snapshots of dirty regions.

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

static constexpr int MASK_TILE_SIZE = 256;

struct MaskTileSnapshot {
    int tileX = 0;
    int tileY = 0;
    // nullptr / empty shared => tile was default-filled (absent)
    std::shared_ptr<std::vector<uint8_t>> data;
};

class MaskTiles {
public:
    void Init(int width, int height, uint8_t defaultVal = 255);
    void Clear();

    int  Width()  const { return m_Width; }
    int  Height() const { return m_Height; }
    int  TilesX() const { return m_TilesX; }
    int  TilesY() const { return m_TilesY; }
    bool IsEmpty() const { return m_Tiles.empty(); }
    bool Valid() const { return m_Width > 0 && m_Height > 0; }
    uint8_t DefaultValue() const { return m_Default; }

    uint8_t Get(int x, int y) const;
    void    Set(int x, int y, uint8_t v);

    void ExpandDirty(int x0, int y0, int x1, int y1);
    void ClearDirty();
    bool HasDirty() const { return m_DirtyX1 >= m_DirtyX0; }
    void GetDirty(int& x0, int& y0, int& x1, int& y1) const {
        x0 = m_DirtyX0; y0 = m_DirtyY0; x1 = m_DirtyX1; y1 = m_DirtyY1;
    }

    std::vector<MaskTileSnapshot> SnapshotRect(int x0, int y0, int x1, int y1) const;
    std::vector<MaskTileSnapshot> SnapshotAll() const;
    void RestoreTiles(const std::vector<MaskTileSnapshot>& snaps);

    void Flatten(std::vector<uint8_t>& out) const;
    void ImportFlat(const std::vector<uint8_t>& flat, int w, int h);

    size_t EstimateBytes() const;
    size_t TileCount() const { return m_Tiles.size(); }

    void ForEachTile(const std::function<void(int tx, int ty, const uint8_t* data)>& cb) const;

private:
    uint32_t Key(int tx, int ty) const {
        return (uint32_t)tx | ((uint32_t)ty << 16);
    }
    std::vector<uint8_t>& EnsureWritable(int tx, int ty);
    const std::vector<uint8_t>* Find(int tx, int ty) const;

    int m_Width = 0, m_Height = 0;
    int m_TilesX = 0, m_TilesY = 0;
    uint8_t m_Default = 255;
    std::unordered_map<uint32_t, std::shared_ptr<std::vector<uint8_t>>> m_Tiles;

    int m_DirtyX0 = 0, m_DirtyY0 = 0, m_DirtyX1 = -1, m_DirtyY1 = -1;
};
