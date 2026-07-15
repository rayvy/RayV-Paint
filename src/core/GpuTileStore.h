#pragma once

#include <d3d11.h>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------
// GpuTileStore — consumer-agnostic sparse GPU tile cache.
//
// Surfaces hold document-sized logical grids of TILE_SIZE² textures. Only tiles
// that are uploaded exist on the GPU (no full-document layer texture).
//
// Consumers (2D canvas, future tools) identify surfaces by SurfaceId and upload
// / query tiles. Drawing is left to the consumer (Draw helper optional).
// ---------------------------------------------------------------------------
class GpuTileStore {
public:
    static constexpr int kTileSize = 256;
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
        ID3D11Texture2D*          tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0; // valid pixel width  (≤ kTileSize)
        int h = 0;
    };

    GpuTileStore() = default;
    ~GpuTileStore() { Shutdown(); }
    GpuTileStore(const GpuTileStore&) = delete;
    GpuTileStore& operator=(const GpuTileStore&) = delete;

    void Shutdown();

    // Create empty surface. Returns id or kInvalidSurface.
    SurfaceId CreateSurface(ID3D11Device* device, int docW, int docH, DXGI_FORMAT format);
    void DestroySurface(SurfaceId id);
    bool HasSurface(SurfaceId id) const;
    void ClearSurface(SurfaceId id); // drop all GPU tiles

    int DocWidth(SurfaceId id) const;
    int DocHeight(SurfaceId id) const;
    DXGI_FORMAT Format(SurfaceId id) const;

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
    size_t EstimateVramBytes(SurfaceId id) const;

private:
    struct Surface {
        int docW = 0, docH = 0;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        int bpp = 4;
        std::unordered_map<TileKey, TileGpu, TileKeyHash> tiles;
    };

    void ReleaseTile(TileGpu& t);
    int BytesPerFormat(DXGI_FORMAT fmt) const;

    std::unordered_map<SurfaceId, Surface> m_Surfaces;
    SurfaceId m_NextId = 1;
};
