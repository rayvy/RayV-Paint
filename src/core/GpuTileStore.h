#pragma once

#include <d3d11.h>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <functional>
#include <deque>

// ---------------------------------------------------------------------------
// GpuTileStore — consumer-agnostic sparse GPU tile cache with atlas packing.
//
// Surfaces hold document-sized logical grids of TILE_SIZE² tiles. Physical
// storage packs many tiles into shared atlas pages (kAtlasGrid² tiles per
// page) to cut CreateTexture2D/SRV thrash and VRAM fragmentation.
//
// Consumers identify surfaces by SurfaceId. Drawing uses TileGpu::srv plus
// UV helpers (AtlasU0/V0/U1/V1) — standalone and atlas tiles share the API.
// ---------------------------------------------------------------------------
class GpuTileStore {
public:
    static constexpr int kTileSize = 256;
    // 8×8 tiles → 2048² page (fits D3D11 well; good density for sparse paint).
    static constexpr int kAtlasGrid = 8;
    static constexpr int kAtlasPixels = kTileSize * kAtlasGrid; // 2048
    using SurfaceId = uint32_t;
    static constexpr SurfaceId kInvalidSurface = 0;

    struct TileKey {
        int tx = 0, ty = 0;
        bool operator==(const TileKey& o) const { return tx == o.tx && ty == o.ty; }
    };
    struct TileKeyHash {
        size_t operator()(const TileKey& k) const {
            return (size_t)k.tx * 73856093u ^ (size_t)k.ty * 19349663u;
        }
    };

    struct TileGpu {
        ID3D11Texture2D*          tex = nullptr; // owned only for standalone (atlasPage < 0)
        ID3D11ShaderResourceView* srv = nullptr; // atlas page SRV (not owned) or standalone
        int atlasPage = -1; // ≥0 ⇒ packed into page; -1 ⇒ standalone tex
        int slotX = 0, slotY = 0; // tile slot in atlas page
        int atlasPixelW = 0, atlasPixelH = 0; // page size for UV (0 if standalone)
        int w = 0; // valid pixel width  (≤ kTileSize)
        int h = 0;

        bool IsAtlas() const { return atlasPage >= 0; }

        // Texture-space UVs for the valid region (draw with these, not 0..1 when atlas).
        float U0() const {
            if (!IsAtlas() || atlasPixelW < 1) return 0.f;
            return (float)(slotX * kTileSize) / (float)atlasPixelW;
        }
        float V0() const {
            if (!IsAtlas() || atlasPixelH < 1) return 0.f;
            return (float)(slotY * kTileSize) / (float)atlasPixelH;
        }
        float U1() const {
            if (!IsAtlas() || atlasPixelW < 1) return 1.f;
            return (float)(slotX * kTileSize + w) / (float)atlasPixelW;
        }
        float V1() const {
            if (!IsAtlas() || atlasPixelH < 1) return 1.f;
            return (float)(slotY * kTileSize + h) / (float)atlasPixelH;
        }
    };

    GpuTileStore() = default;
    ~GpuTileStore() { Shutdown(); }
    GpuTileStore(const GpuTileStore&) = delete;
    GpuTileStore& operator=(const GpuTileStore&) = delete;

    void Shutdown();

    // Create empty surface. Returns id or kInvalidSurface.
    // useAtlas=true (default): pack tiles into 2048² pages. false: one tex per tile.
    SurfaceId CreateSurface(ID3D11Device* device, int docW, int docH, DXGI_FORMAT format,
                            bool useAtlas = true);
    void DestroySurface(SurfaceId id);
    bool HasSurface(SurfaceId id) const;
    void ClearSurface(SurfaceId id); // drop all GPU tiles (keep surface)

    int DocWidth(SurfaceId id) const;
    int DocHeight(SurfaceId id) const;
    DXGI_FORMAT Format(SurfaceId id) const;
    bool UsesAtlas(SurfaceId id) const;

    // Upload / replace one tile. data pitch = kTileSize * bpp (full tile row pitch).
    // w,h = valid region at top-left of the tile (edge tiles may be smaller).
    bool UploadTile(ID3D11Device* device, ID3D11DeviceContext* ctx, SurfaceId id,
                    int tx, int ty, const void* data, int srcPitchBytes,
                    int validW, int validH);

    void RemoveTile(SurfaceId id, int tx, int ty);
    const TileGpu* FindTile(SurfaceId id, int tx, int ty) const;

    // Iterate present tiles (read-only).
    void ForEachTile(SurfaceId id, const std::function<void(int tx, int ty, const TileGpu&)>& fn) const;

    size_t TileCount(SurfaceId id) const;
    size_t AtlasPageCount(SurfaceId id) const;
    size_t EstimateVramBytes(SurfaceId id) const;

private:
    struct AtlasPage {
        ID3D11Texture2D*          tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        // Free slot indices 0 .. kAtlasGrid*kAtlasGrid-1
        std::deque<int> freeSlots;
        int used = 0;
        size_t reservedVramBytes = 0; // for simulated VRAM release
    };

    struct Surface {
        int docW = 0, docH = 0;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        int bpp = 4;
        bool useAtlas = true;
        std::unordered_map<TileKey, TileGpu, TileKeyHash> tiles;
        std::vector<AtlasPage> pages;
    };

    void ReleaseTile(Surface& s, TileGpu& t);
    void ReleasePage(AtlasPage& p);
    int  AllocAtlasSlot(ID3D11Device* device, Surface& s, TileGpu& g);
    void FreeAtlasSlot(Surface& s, TileGpu& g);
    int  BytesPerFormat(DXGI_FORMAT fmt) const;

    std::unordered_map<SurfaceId, Surface> m_Surfaces;
    SurfaceId m_NextId = 1;
};
