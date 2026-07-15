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

void GpuTileStore::ReleasePage(AtlasPage& p) {
    if (p.srv) { p.srv->Release(); p.srv = nullptr; }
    if (p.tex) { p.tex->Release(); p.tex = nullptr; }
    p.freeSlots.clear();
    p.used = 0;
}

void GpuTileStore::FreeAtlasSlot(Surface& s, TileGpu& g) {
    if (g.atlasPage < 0 || g.atlasPage >= (int)s.pages.size()) {
        g.atlasPage = -1;
        g.srv = nullptr;
        return;
    }
    AtlasPage& page = s.pages[(size_t)g.atlasPage];
    const int slot = g.slotY * kAtlasGrid + g.slotX;
    page.freeSlots.push_back(slot);
    if (page.used > 0) --page.used;
    g.atlasPage = -1;
    g.srv = nullptr;
    g.tex = nullptr;
    g.atlasPixelW = g.atlasPixelH = 0;
}

void GpuTileStore::ReleaseTile(Surface& s, TileGpu& t) {
    if (t.IsAtlas()) {
        FreeAtlasSlot(s, t);
    } else {
        if (t.srv) { t.srv->Release(); t.srv = nullptr; }
        if (t.tex) { t.tex->Release(); t.tex = nullptr; }
    }
    t.w = t.h = 0;
    t.slotX = t.slotY = 0;
}

int GpuTileStore::AllocAtlasSlot(ID3D11Device* device, Surface& s, TileGpu& g) {
    // Prefer a page with free slots
    for (int pi = 0; pi < (int)s.pages.size(); ++pi) {
        AtlasPage& page = s.pages[(size_t)pi];
        if (page.freeSlots.empty() || !page.tex || !page.srv) continue;
        const int slot = page.freeSlots.front();
        page.freeSlots.pop_front();
        ++page.used;
        g.atlasPage = pi;
        g.slotX = slot % kAtlasGrid;
        g.slotY = slot / kAtlasGrid;
        g.tex = nullptr; // not owned
        g.srv = page.srv;
        g.atlasPixelW = kAtlasPixels;
        g.atlasPixelH = kAtlasPixels;
        return pi;
    }

    // New page
    AtlasPage page;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)kAtlasPixels;
    td.Height = (UINT)kAtlasPixels;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = s.format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &page.tex);
    if (FAILED(hr) || !page.tex) {
        Logger::Get().WarnTag("gpu", "GpuTileStore: atlas page CreateTexture2D failed");
        return -1;
    }
    device->CreateShaderResourceView(page.tex, nullptr, &page.srv);
    if (!page.srv) {
        page.tex->Release();
        page.tex = nullptr;
        return -1;
    }
    const int total = kAtlasGrid * kAtlasGrid;
    for (int i = 1; i < total; ++i) // slot 0 taken by this alloc
        page.freeSlots.push_back(i);
    page.used = 1;

    const int pi = (int)s.pages.size();
    s.pages.push_back(std::move(page));
    g.atlasPage = pi;
    g.slotX = 0;
    g.slotY = 0;
    g.tex = nullptr;
    g.srv = s.pages.back().srv;
    g.atlasPixelW = kAtlasPixels;
    g.atlasPixelH = kAtlasPixels;
    return pi;
}

void GpuTileStore::Shutdown() {
    for (auto& kv : m_Surfaces) {
        for (auto& tk : kv.second.tiles)
            ReleaseTile(kv.second, tk.second);
        for (auto& p : kv.second.pages)
            ReleasePage(p);
    }
    m_Surfaces.clear();
    m_NextId = 1;
}

GpuTileStore::SurfaceId GpuTileStore::CreateSurface(ID3D11Device* device, int docW, int docH,
                                                    DXGI_FORMAT format, bool useAtlas) {
    if (!device || docW < 1 || docH < 1) return kInvalidSurface;
    SurfaceId id = m_NextId++;
    if (id == kInvalidSurface) id = m_NextId++;
    Surface s;
    s.docW = docW;
    s.docH = docH;
    s.format = format;
    s.bpp = BytesPerFormat(format);
    s.useAtlas = useAtlas;
    m_Surfaces.emplace(id, std::move(s));
    return id;
}

void GpuTileStore::DestroySurface(SurfaceId id) {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end()) return;
    for (auto& tk : it->second.tiles)
        ReleaseTile(it->second, tk.second);
    for (auto& p : it->second.pages)
        ReleasePage(p);
    m_Surfaces.erase(it);
}

bool GpuTileStore::HasSurface(SurfaceId id) const {
    return m_Surfaces.find(id) != m_Surfaces.end();
}

void GpuTileStore::ClearSurface(SurfaceId id) {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end()) return;
    for (auto& tk : it->second.tiles)
        ReleaseTile(it->second, tk.second);
    it->second.tiles.clear();
    for (auto& p : it->second.pages)
        ReleasePage(p);
    it->second.pages.clear();
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
bool GpuTileStore::UsesAtlas(SurfaceId id) const {
    auto it = m_Surfaces.find(id);
    return it != m_Surfaces.end() && it->second.useAtlas;
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

    if (s.useAtlas) {
        if (!g.IsAtlas() || !g.srv) {
            // Drop any standalone leftover
            if (g.tex || g.srv) ReleaseTile(s, g);
            if (AllocAtlasSlot(device, s, g) < 0) {
                s.tiles.erase(key);
                return false;
            }
        }
        g.w = validW;
        g.h = validH;
        const UINT dstX = (UINT)(g.slotX * kTileSize);
        const UINT dstY = (UINT)(g.slotY * kTileSize);
        D3D11_BOX box = {};
        box.left = dstX; box.top = dstY; box.front = 0;
        box.right = dstX + (UINT)validW; box.bottom = dstY + (UINT)validH; box.back = 1;
        // Page tex is owned by AtlasPage; g.tex is null — look up page
        if (g.atlasPage < 0 || g.atlasPage >= (int)s.pages.size() || !s.pages[(size_t)g.atlasPage].tex) {
            s.tiles.erase(key);
            return false;
        }
        ctx->UpdateSubresource(s.pages[(size_t)g.atlasPage].tex, 0, &box, data, (UINT)srcPitchBytes, 0);
        return true;
    }

    // ---- Standalone path (legacy / opt-out) ----
    if (!g.tex || g.w != validW || g.h != validH || g.IsAtlas()) {
        ReleaseTile(s, g);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)validW;
        td.Height = (UINT)validH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = s.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

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
        g.atlasPage = -1;
        g.atlasPixelW = g.atlasPixelH = 0;
        return g.srv != nullptr;
    }

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
    ReleaseTile(it->second, jt->second);
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

size_t GpuTileStore::AtlasPageCount(SurfaceId id) const {
    auto it = m_Surfaces.find(id);
    return it != m_Surfaces.end() ? it->second.pages.size() : 0;
}

size_t GpuTileStore::EstimateVramBytes(SurfaceId id) const {
    auto it = m_Surfaces.find(id);
    if (it == m_Surfaces.end()) return 0;
    const Surface& s = it->second;
    size_t bytes = 0;
    if (s.useAtlas) {
        for (const auto& p : s.pages) {
            if (p.tex)
                bytes += (size_t)kAtlasPixels * (size_t)kAtlasPixels * (size_t)s.bpp;
        }
    } else {
        for (const auto& kv : s.tiles)
            bytes += (size_t)kv.second.w * (size_t)kv.second.h * (size_t)s.bpp;
    }
    return bytes;
}
