#include "GpuTileStore.h"
#include "Logger.h"
#include "MemoryStats.h"

#include <algorithm>

int GpuTileStore::BytesPerFormat(DXGI_FORMAT fmt) const {
    switch (fmt) {
    case DXGI_FORMAT_R8_UNORM:              return 1;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return 16;
    default:                               return 4;
    }
}

void GpuTileStore::ReleaseTile(TileGpu& t) {
    if (t.srv) { t.srv->Release(); t.srv = nullptr; }
    if (t.tex) { t.tex->Release(); t.tex = nullptr; }
    t.w = t.h = 0;
}

void GpuTileStore::Shutdown() {
    for (auto& kv : m_Surfaces) {
        for (auto& tk : kv.second.tiles)
            ReleaseTile(tk.second);
    }
    m_Surfaces.clear();
    m_NextId = 1;
}

GpuTileStore::SurfaceId GpuTileStore::CreateSurface(ID3D11Device* device, int docW, int docH,
                                                    DXGI_FORMAT format) {
    if (!device || docW < 1 || docH < 1) return kInvalidSurface;
    SurfaceId id = m_NextId++;
    if (id == kInvalidSurface) id = m_NextId++;
    Surface s;
    s.docW = docW;
    s.docH = docH;
    s.format = format;
    s.bpp = BytesPerFormat(format);
    m_Surfaces.emplace(id, std::move(s));
    return id;
}

void GpuTileStore::DestroySurface(SurfaceId id) {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end()) return;
    for (auto& tk : it->second.tiles)
        ReleaseTile(tk.second);
    m_Surfaces.erase(it);
}

bool GpuTileStore::HasSurface(SurfaceId id) const {
    return m_Surfaces.find(id) != m_Surfaces.end();
}

void GpuTileStore::ClearSurface(SurfaceId id) {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end()) return;
    for (auto& tk : it->second.tiles)
        ReleaseTile(tk.second);
    it->second.tiles.clear();
}

int GpuTileStore::DocWidth(SurfaceId id) const {
    auto it = m_Surfaces.find(id);
    return it != m_Surfaces.end() ? it->second.docW : 0;
}
int GpuTileStore::DocHeight(SurfaceId id) const {
    auto it = m_Surfaces.find(id);
    return it != m_Surfaces.end() ? it->second.docH : 0;
}
DXGI_FORMAT GpuTileStore::Format(SurfaceId id) const {
    auto it = m_Surfaces.find(id);
    return it != m_Surfaces.end() ? it->second.format : DXGI_FORMAT_UNKNOWN;
}

bool GpuTileStore::UploadTile(ID3D11Device* device, ID3D11DeviceContext* ctx, SurfaceId id,
                              int tx, int ty, const void* data, int srcPitchBytes,
                              int validW, int validH) {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end() || !device || !ctx || !data) return false;
    Surface& s = it->second;
    validW = std::clamp(validW, 1, kTileSize);
    validH = std::clamp(validH, 1, kTileSize);
    if (srcPitchBytes < validW * s.bpp) return false;

    TileKey key{ tx, ty };
    TileGpu& g = s.tiles[key];

    // (Re)create if missing or size/format mismatch
    if (!g.tex || g.w != validW || g.h != validH) {
        ReleaseTile(g);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)validW;
        td.Height = (UINT)validH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = s.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        // Init with first upload to avoid empty VRAM clear
        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = data;
        init.SysMemPitch = (UINT)srcPitchBytes;
        HRESULT hr = device->CreateTexture2D(&td, &init, &g.tex);
        if (FAILED(hr) || !g.tex) {
            s.tiles.erase(key);
            return false;
        }
        device->CreateShaderResourceView(g.tex, nullptr, &g.srv);
        g.w = validW;
        g.h = validH;
        return g.srv != nullptr;
    }

    // Update existing
    D3D11_BOX box = {};
    box.left = 0; box.top = 0; box.front = 0;
    box.right = (UINT)validW; box.bottom = (UINT)validH; box.back = 1;
    ctx->UpdateSubresource(g.tex, 0, &box, data, (UINT)srcPitchBytes, 0);
    return true;
}

void GpuTileStore::RemoveTile(SurfaceId id, int tx, int ty) {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end()) return;
    TileKey key{ tx, ty };
    auto jt = it->second.tiles.find(key);
    if (jt == it->second.tiles.end()) return;
    ReleaseTile(jt->second);
    it->second.tiles.erase(jt);
}

const GpuTileStore::TileGpu* GpuTileStore::FindTile(SurfaceId id, int tx, int ty) const {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end()) return nullptr;
    auto jt = it->second.tiles.find(TileKey{ tx, ty });
    if (jt == it->second.tiles.end()) return nullptr;
    return &jt->second;
}

void GpuTileStore::ForEachTile(SurfaceId id,
    const std::function<void(int tx, int ty, const TileGpu&)>& fn) const {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end() || !fn) return;
    for (const auto& kv : it->second.tiles)
        fn(kv.first.tx, kv.first.ty, kv.second);
}

size_t GpuTileStore::TileCount(SurfaceId id) const {
    auto it = m_Surfaces.find(id);
    return it != m_Surfaces.end() ? it->second.tiles.size() : 0;
}

size_t GpuTileStore::EstimateVramBytes(SurfaceId id) const {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end()) return 0;
    size_t bytes = 0;
    for (const auto& kv : it->second.tiles) {
        bytes += (size_t)kv.second.w * (size_t)kv.second.h * (size_t)it->second.bpp;
    }
    return bytes;
}
