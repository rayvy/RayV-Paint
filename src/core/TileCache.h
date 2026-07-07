#pragma once
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <functional>
#include <algorithm>
#include <span>

// ---- Pixel format for the canvas document ----
enum class CanvasPixelFormat {
    RGBA8,   // uint8_t x4,  4 bytes/pixel — color, albedo, masks
    RGBA32F, // float   x4, 16 bytes/pixel — height maps, HDR, precision work
};

inline int BytesPerPixel(CanvasPixelFormat fmt) {
    return (fmt == CanvasPixelFormat::RGBA8) ? 4 : 16;
}

// Tile side: 256 pixels. One RGBA8 tile = 256KB, RGBA32F tile = 1MB.
static constexpr int TILE_SIZE = 256;

// ---- TileCache ----
// Sparse, LRU-backed pixel store. Replaces std::vector<float> pixels in Layer.
// Only allocated tiles are in RAM; empty regions cost nothing.
class TileCache {
public:
    TileCache() = default;
    ~TileCache() = default;

    TileCache(const TileCache&) = delete;
    TileCache& operator=(const TileCache&) = delete;
    TileCache(TileCache&&) = default;
    TileCache& operator=(TileCache&&) = default;

    // Initialise (or reinitialise) the cache. Clears all existing tiles.
    void Init(int width, int height, CanvasPixelFormat format);

    // Free all tile memory.
    void Clear();

    // Resize canvas dimensions, preserving content (top-left aligned).
    void Resize(int newWidth, int newHeight);

    // ---- Metadata ----
    int  GetWidth()         const { return m_Width; }
    int  GetHeight()        const { return m_Height; }
    int  GetTilesX()        const { return m_TilesX; }
    int  GetTilesY()        const { return m_TilesY; }
    CanvasPixelFormat GetFormat() const { return m_Format; }
    int  GetBytesPerPixel() const { return m_BytesPerPixel; }
    size_t GetTileCount()   const { return m_Tiles.size(); }
    bool IsEmpty()          const { return m_Tiles.empty(); }

    int GetAllocatedTileCount() const;
    size_t GetCpuRamBytes() const;
    int GetMaxTileCount() const;


    // ---- Tile existence ----
    bool HasTile(int tileX, int tileY) const;

    // ---- Float pixel interface (works for RGBA8 and RGBA32F) ----
    // Missing tile == fully transparent (0,0,0,0).
    void GetPixelF(int x, int y, float rgba[4]) const;
    void GetPixelF(int x, int y, std::span<float, 4> rgba) const;
    void SetPixelF(int x, int y, const float rgba[4]);
    void SetPixelF(int x, int y, std::span<const float, 4> rgba);

    // ---- Fast UINT8 interface (only valid when format == RGBA8) ----
    void GetPixelU8(int x, int y, uint8_t rgba[4]) const;
    void GetPixelU8(int x, int y, std::span<uint8_t, 4> rgba) const;
    void SetPixelU8(int x, int y, const uint8_t rgba[4]);
    void SetPixelU8(int x, int y, std::span<const uint8_t, 4> rgba);

    // ---- Direct tile data access (for GPU upload / PaintEngine hot path) ----
    // Returns nullptr if the tile doesn't exist (treat as zeroes).
    const uint8_t* GetTileData(int tileX, int tileY) const;

    // Create tile if needed, mark it dirty, return writable pointer.
    uint8_t* LockTile(int tileX, int tileY);

    // ---- Bulk operations ----
    void Fill(const float rgba[4]);
    void FillRect(int x0, int y0, int x1, int y1, const float rgba[4]);

    // Copy a region from another TileCache (formats must match).
    void CopyFrom(const TileCache& src, int srcX, int srcY,
                  int dstX, int dstY, int w, int h);

    // Import from raw flat RGBA8 buffer (always valid regardless of document format:
    // if document is RGBA32F the values are converted to float on import).
    void ImportRGBA8(const uint8_t* data, int srcWidth, int srcHeight,
                     int dstX = 0, int dstY = 0);

    // Import from raw flat RGBA32F float buffer.
    // For RGBA8 documents: values are clamped to [0,1] and quantised to uint8.
    void ImportRGBA32F(const float* data, int srcWidth, int srcHeight,
                       int dstX = 0, int dstY = 0);

    // Export entire canvas (or clipped) to a flat RGBA8 buffer (width*height*4 bytes).
    void ExportRGBA8(uint8_t* outData, int outWidth, int outHeight) const;

    // Export entire canvas to a flat RGBA32F buffer.
    void ExportRGBA32F(float* outData, int outWidth, int outHeight) const;

    // ---- Dirty tracking (incremental GPU upload) ----
    void MarkDirty(int tileX, int tileY);
    void MarkAllDirty();
    void ClearDirty(int tileX, int tileY);
    void ClearAllDirty();
    bool IsDirty(int tileX, int tileY) const;

    // Calls callback(tileX, tileY, rawData, pitchBytes) for each dirty tile.
    // pitchBytes = TILE_SIZE * m_BytesPerPixel.
    void ForEachDirtyTile(
        std::function<void(int tx, int ty, const uint8_t* data, int pitch)> cb) const;

    // ---- LRU limit ----
    void SetMaxTilesInRAM(size_t n) { m_MaxTilesInRAM = n; }

    // ---- Undo/Redo snapshots ----
    // Snapshot a single tile into a vector (empty = tile doesn't exist).
    std::vector<uint8_t> SnapshotTile(int tileX, int tileY) const;

    // Restore a tile from a snapshot. Pass empty vector to erase the tile.
    void RestoreTile(int tileX, int tileY, const std::vector<uint8_t>& data);

private:
    struct Tile {
        std::vector<uint8_t> data;   // TILE_SIZE*TILE_SIZE*m_BytesPerPixel bytes
        bool     dirty      = false;
        uint64_t lastAccess = 0;
    };

    Tile&       GetOrCreateTile(int tileX, int tileY);
    const Tile* FindTile(int tileX, int tileY) const;
    Tile*       FindTile(int tileX, int tileY);
    uint32_t    Key(int tileX, int tileY) const;
    void        EvictLRU();

    int m_Width          = 0;
    int m_Height         = 0;
    int m_TilesX         = 0;
    int m_TilesY         = 0;
    int m_BytesPerPixel  = 4;
    CanvasPixelFormat m_Format = CanvasPixelFormat::RGBA8;
    size_t   m_MaxTilesInRAM  = 512; // ~128 MB RGBA8 or ~512 MB RGBA32F
    uint64_t m_AccessCounter  = 0;

    std::unordered_map<uint32_t, Tile> m_Tiles;
};
