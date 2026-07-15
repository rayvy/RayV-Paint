#pragma once

#include <d3d11.h>
#include <cstdint>
#include <vector>

// Ring of STAGING textures for CPU→GPU tile uploads (CopySubresourceRegion).
// Consumer-agnostic: any DEFAULT texture + pitch-aligned CPU source.
// Prefer this over UpdateSubresource on NVIDIA (fewer driver stalls).
class GpuStagingUpload {
public:
    static constexpr int kDefaultTile = 256;
    static constexpr int kDefaultRing  = 16;

    GpuStagingUpload() = default;
    ~GpuStagingUpload() { Shutdown(); }

    GpuStagingUpload(const GpuStagingUpload&) = delete;
    GpuStagingUpload& operator=(const GpuStagingUpload&) = delete;

    // Create/reuse ring for format. tileW/H = max upload footprint (usually TILE_SIZE).
    bool Ensure(ID3D11Device* device, DXGI_FORMAT format,
                int tileW = kDefaultTile, int tileH = kDefaultTile,
                int ringSize = kDefaultRing);

    void Shutdown();

    // Upload w×h region (w,h ≤ tile size) into dest at (dstX,dstY).
    // srcPitchBytes = row pitch of source (often tileW * bpp).
    bool UploadRegion(ID3D11DeviceContext* ctx, ID3D11Texture2D* dest,
                      int dstX, int dstY, int w, int h,
                      const void* src, int srcPitchBytes);

    bool IsReady() const { return !m_Ring.empty() && m_Ring[0].tex != nullptr; }
    DXGI_FORMAT Format() const { return m_Format; }

private:
    struct Slot {
        ID3D11Texture2D* tex = nullptr;
    };

    std::vector<Slot> m_Ring;
    int m_Next = 0;
    int m_TileW = 0;
    int m_TileH = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
    int m_Bpp = 4;
};
