#include "GpuStagingUpload.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>

namespace {
int BytesPerDxgi(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8_UNORM:
        return 4; // R8 uses 1 but we allocate RGBA slots for simplicity when R8
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    default:
        return 4;
    }
}
} // namespace

bool GpuStagingUpload::Ensure(ID3D11Device* device, DXGI_FORMAT format,
                              int tileW, int tileH, int ringSize) {
    if (!device || tileW < 1 || tileH < 1 || ringSize < 1) return false;

    // Reuse if same shape/format
    if (IsReady() && m_Format == format && m_TileW == tileW && m_TileH == tileH &&
        (int)m_Ring.size() == ringSize)
        return true;

    Shutdown();
    m_Format = format;
    m_TileW = tileW;
    m_TileH = tileH;
    m_Bpp = BytesPerDxgi(format);
    // R8_UNORM true bpp is 1 — special case for mask uploads if we add later
    if (format == DXGI_FORMAT_R8_UNORM) m_Bpp = 1;

    m_Ring.resize((size_t)ringSize);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)tileW;
    td.Height = (UINT)tileH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    td.BindFlags = 0;

    for (int i = 0; i < ringSize; ++i) {
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_Ring[i].tex);
        if (FAILED(hr) || !m_Ring[i].tex) {
            Logger::Get().ErrorTag("gpu",
                "GpuStagingUpload: CreateTexture2D staging failed");
            Shutdown();
            return false;
        }
    }
    m_Next = 0;
    return true;
}

void GpuStagingUpload::Shutdown() {
    for (auto& s : m_Ring) {
        if (s.tex) { s.tex->Release(); s.tex = nullptr; }
    }
    m_Ring.clear();
    m_Next = 0;
    m_TileW = m_TileH = 0;
    m_Format = DXGI_FORMAT_UNKNOWN;
}

bool GpuStagingUpload::UploadRegion(ID3D11DeviceContext* ctx, ID3D11Texture2D* dest,
                                    int dstX, int dstY, int w, int h,
                                    const void* src, int srcPitchBytes) {
    if (!ctx || !dest || !src || !IsReady()) return false;
    if (w < 1 || h < 1 || w > m_TileW || h > m_TileH) return false;
    if (srcPitchBytes < w * m_Bpp) return false;

    Slot& slot = m_Ring[(size_t)m_Next];
    m_Next = (m_Next + 1) % (int)m_Ring.size();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    // DISCARD-like: STAGING WRITE often needs MAP_WRITE; avoid stalls by ring size
    HRESULT hr = ctx->Map(slot.tex, 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr)) {
        // Fallback: try again once after flush (rare)
        ctx->Flush();
        hr = ctx->Map(slot.tex, 0, D3D11_MAP_WRITE, 0, &mapped);
        if (FAILED(hr)) return false;
    }

    const uint8_t* srow = static_cast<const uint8_t*>(src);
    uint8_t* drow = static_cast<uint8_t*>(mapped.pData);
    const int rowBytes = w * m_Bpp;
    for (int y = 0; y < h; ++y) {
        std::memcpy(drow + (size_t)y * mapped.RowPitch,
                    srow + (size_t)y * srcPitchBytes,
                    (size_t)rowBytes);
    }
    ctx->Unmap(slot.tex, 0);

    D3D11_BOX box = {};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = (UINT)w;
    box.bottom = (UINT)h;
    box.back = 1;
    ctx->CopySubresourceRegion(dest, 0, (UINT)dstX, (UINT)dstY, 0,
                               slot.tex, 0, &box);
    return true;
}
