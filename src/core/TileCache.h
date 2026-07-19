#pragma once
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <memory>
#include <functional>
#include <algorithm>

// ---- Pixel format for the canvas document ----
enum class CanvasPixelFormat {
    RGBA8,   // uint8_t x4,   4 bytes/pixel — color, albedo, masks (default)
    RGBA16F, // half    x4,   8 bytes/pixel — HDR / height mid precision
    RGBA32F, // float   x4,  16 bytes/pixel — full float height / HDR
};

inline int BytesPerPixel(CanvasPixelFormat fmt) {
    switch (fmt) {
        case CanvasPixelFormat::RGBA8:   return 4;
        case CanvasPixelFormat::RGBA16F: return 8;
        case CanvasPixelFormat::RGBA32F: return 16;
    }
    return 4;
}

// Tile side: 256. RGBA8=256KB, RGBA16F=512KB, RGBA32F=1MB per dense tile.
static constexpr int TILE_SIZE = 256;

// ---------------------------------------------------------------------------
// Shared tile pixel blob (Krita-like COW unit).
// Multiple TileCache slots and undo snapshots can share one TileData.
// Writes must go through EnsureWritable() so shared blobs are cloned first.
// ---------------------------------------------------------------------------
struct TileData {
    std::vector<uint8_t> pixels;

    explicit TileData(size_t byteCount)
        : pixels(byteCount, 0) {}

    TileData(const TileData& other) = default;
    TileData& operator=(const TileData& other) = default;

    size_t ByteSize() const { return pixels.size(); }
};

using TileDataPtr = std::shared_ptr<TileData>;

// Undo/history handle: nullptr data == tile absent (fully transparent).
struct TileSnapshot {
    TileDataPtr data; // shared; empty/null => no tile

    bool IsEmpty() const { return !data; }
    size_t ByteSize() const { return data ? data->ByteSize() : 0; }
};

// ---- TileCache ----
// Sparse store. Pixel buffers are ref-counted (COW on write).
class TileCache {
public:
    TileCache() = default;
    ~TileCache() = default;

    TileCache(const TileCache&) = delete;
    TileCache& operator=(const TileCache&) = delete;
    TileCache(TileCache&&) = default;
    TileCache& operator=(TileCache&&) = default;

    void Init(int width, int height, CanvasPixelFormat format);
    void Clear();
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

    // Approximate unique pixel RAM (dedupes shared TileData pointers).
    size_t EstimateUniquePixelBytes() const;

    bool HasTile(int tileX, int tileY) const;

    void GetPixelF(int x, int y, float rgba[4]) const;
    void SetPixelF(int x, int y, const float rgba[4]);

    void GetPixelU8(int x, int y, uint8_t rgba[4]) const;
    void SetPixelU8(int x, int y, const uint8_t rgba[4]);

    // Read-only pointer (no COW). nullptr if missing.
    const uint8_t* GetTileData(int tileX, int tileY) const;

    // Writable pointer: creates tile if needed, COW-clones if shared, marks dirty.
    uint8_t* LockTile(int tileX, int tileY);

    void Fill(const float rgba[4]);
    void FillRect(int x0, int y0, int x1, int y1, const float rgba[4]);

    void CopyFrom(const TileCache& src, int srcX, int srcY,
                  int dstX, int dstY, int w, int h);

    void ImportRGBA8(const uint8_t* data, int srcWidth, int srcHeight,
                     int dstX = 0, int dstY = 0);
    void ImportRGBA32F(const float* data, int srcWidth, int srcHeight,
                       int dstX = 0, int dstY = 0);

    void ExportRGBA8(uint8_t* outData, int outWidth, int outHeight) const;
    void ExportRGBA32F(float* outData, int outWidth, int outHeight) const;

    // Convert this cache to another pixel format (tile-wise; preserves sparse layout).
    // No-op if already `target`. Returns false on invalid state.
    bool ConvertFormat(CanvasPixelFormat target);

    // ---- Dirty / GPU clear tracking ----
    void MarkDirty(int tileX, int tileY);
    void MarkAllDirty();
    void ClearDirty(int tileX, int tileY);
    void ClearAllDirty();
    bool IsDirty(int tileX, int tileY) const;
    bool HasPendingGpuWork() const;

    void ForEachDirtyTile(
        std::function<void(int tx, int ty, const uint8_t* data, int pitch)> cb) const;

    void SetMaxTilesInRAM(size_t n) { m_MaxTilesInRAM = n; }

    // ---- Shared snapshots (cheap — shares TileData) ----
    TileSnapshot SnapshotTile(int tileX, int tileY) const;
    void RestoreTile(int tileX, int tileY, const TileSnapshot& snap);
    // After SnapshotTile for undo newState: clone live away from the shared history
    // blob so subsequent LockTile/writes can never mutate committed history.
    void DetachLiveFromHistory(int tileX, int tileY);
    // Expand resident cap to full document grid (never LRU-drop undo restores).
    void EnsureResidentCapacityForFullGrid();
    // Deep-copy a snapshot so undo history owns an immutable blob (never aliases live).
    static TileSnapshot DeepCopySnapshot(const TileSnapshot& snap);

private:
    struct Tile {
        TileDataPtr data;     // non-null while entry exists in m_Tiles
        bool        dirty = false;
        uint64_t    lastAccess = 0;
    };

    Tile&       GetOrCreateTile(int tileX, int tileY);
    const Tile* FindTile(int tileX, int tileY) const;
    Tile*       FindTile(int tileX, int tileY);
    uint32_t    Key(int tileX, int tileY) const;
    void        EvictLRU();
    void        QueueGpuClear(int tileX, int tileY);
    void        EnsureWritable(Tile& t);
    TileDataPtr MakeBlankData() const;

    int m_Width          = 0;
    int m_Height         = 0;
    int m_TilesX         = 0;
    int m_TilesY         = 0;
    int m_BytesPerPixel  = 4;
    CanvasPixelFormat m_Format = CanvasPixelFormat::RGBA8;
    size_t   m_MaxTilesInRAM  = 512;
    uint64_t m_AccessCounter  = 0;

    std::unordered_map<uint32_t, Tile> m_Tiles;
    std::unordered_set<uint32_t> m_PendingGpuClears;
};
