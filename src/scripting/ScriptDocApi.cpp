#include "ScriptDocApi.h"
#include "ScriptMainThread.h"
#include "../core/ProjectManager.h"
#include "../core/Logger.h"
#include "../core/MemoryStats.h"
#include "../core/ImageManager.h"
#include "../core/PathUtil.h"
#include "../Canvas.h"
#include "../core/TileCache.h"
#include "../core/MaskTiles.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace script {

static ID3D11Device* g_Device = nullptr;

void SetDevice(ID3D11Device* device) { g_Device = device; }
ID3D11Device* Device() { return g_Device; }

static Canvas* C() {
    return ProjectManager::Get().ActiveCanvasPtr();
}

static constexpr size_t kMaxScriptPixels = (size_t)8192 * 8192;

static bool AllowPixelAlloc(int w, int h, int bytesPerPixel, const char* ctx) {
    if (w <= 0 || h <= 0) return false;
    const size_t pixels = (size_t)w * (size_t)h;
    if (pixels > kMaxScriptPixels) {
        Logger::Get().ErrorTag("script",
            std::string(ctx) + ": rect too large " + std::to_string(w) + "x" + std::to_string(h));
        return false;
    }
    const size_t est = pixels * (size_t)bytesPerPixel;
    if (MemoryStats::ExceedsRamBudget(est, 0.35)) {
        Logger::Get().ErrorTag("script",
            std::string(ctx) + ": estimated " + MemoryStats::FormatBytes(est) + " exceeds RAM budget");
        return false;
    }
    return true;
}

static Layer* LayerAt(Canvas& c, int index) {
    auto& layers = c.GetLayers();
    if (index < 0 || index >= (int)layers.size()) return nullptr;
    return &layers[(size_t)index];
}

static void EnsureTileCache(Layer& layer, Canvas& c) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(c.GetWidth(), c.GetHeight(),
            Canvas::FormatForBitDepth(c.GetDocumentBitDepth()));
    }
}

static bool RequireMain(const char* op) {
    if (IsMainThread()) return true;
    Logger::Get().ErrorTag("script",
        std::string(op) + " refused: must run on host main thread. "
        "From a worker: rayv.host.call_on_main(lambda: ...)");
    return false;
}

// ---------------------------------------------------------------------------
int Width() {
    Canvas* c = C();
    return c ? c->GetWidth() : 0;
}
int Height() {
    Canvas* c = C();
    return c ? c->GetHeight() : 0;
}

std::string BitDepth() {
    Canvas* c = C();
    if (!c) return "";
    switch (c->GetDocumentBitDepth()) {
    case Canvas::DocumentBitDepth::F32: return "F32";
    case Canvas::DocumentBitDepth::F16: return "F16";
    default: return "U8";
    }
}

std::string ProjectPath() {
    Canvas* c = C();
    return c ? c->GetCurrentProjectFilePath() : "";
}

bool IsModified() {
    Canvas* c = C();
    return c && c->IsDocumentModified();
}

bool Open(const std::string& path) {
    if (!RequireMain("doc.open")) return false;
    Canvas* c = C();
    if (!c || !g_Device || path.empty()) return false;
    bool ok = c->OpenDocument(g_Device, path);
    if (ok) {
        if (Project* p = ProjectManager::Get().ActiveProject())
            p->ApplyTextureSetsFromCanvas();
    }
    return ok;
}

bool SaveRayp(const std::string& path) {
    if (!RequireMain("doc.save_rayp")) return false;
    Canvas* c = C();
    if (!c || path.empty()) return false;
    if (Project* p = ProjectManager::Get().ActiveProject())
        p->InjectTextureSetsIntoCanvas();
    return c->SaveCanvasRayp(path);
}

bool SaveImage(const std::string& path) {
    if (!RequireMain("doc.save_image")) return false;
    Canvas* c = C();
    if (!c || path.empty()) return false;
    return c->SaveCanvasStandard(path);
}

bool NewBlank(int w, int h) {
    if (!RequireMain("doc.new_blank")) return false;
    Canvas* c = C();
    if (!c || !g_Device || w < 1 || h < 1) return false;
    w = std::min(w, 16384);
    h = std::min(h, 16384);
    c->ResizeCanvas(g_Device, w, h);
    return true;
}

// ---------------------------------------------------------------------------
int LayerCount() {
    Canvas* c = C();
    return c ? (int)c->GetLayers().size() : 0;
}

int ActiveLayerIndex() {
    Canvas* c = C();
    return c ? c->GetActiveLayerIndex() : -1;
}

bool SetActiveLayer(int index) {
    if (!RequireMain("doc.set_active_layer")) return false;
    Canvas* c = C();
    if (!c || !LayerAt(*c, index)) return false;
    c->SetActiveLayerIndex(index);
    return true;
}

std::string LayerName(int index) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    return L ? L->name : "";
}

bool SetLayerName(int index, const std::string& name) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    if (!L) return false;
    L->name = name;
    c->SetDocumentModified(true);
    return true;
}

bool LayerVisible(int index) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    return L && L->visible;
}

bool SetLayerVisible(int index, bool visible) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    if (!L) return false;
    L->visible = visible;
    c->MarkCompositeDirty();
    c->SetDocumentModified(true);
    return true;
}

float LayerOpacity(int index) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    return L ? L->opacity : 0.f;
}

bool SetLayerOpacity(int index, float opacity) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    if (!L) return false;
    L->opacity = std::clamp(opacity, 0.f, 1.f);
    c->MarkCompositeDirty();
    c->SetDocumentModified(true);
    return true;
}

bool LayerIsGroup(int index) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    return L && L->isGroup;
}

bool LayerHasMask(int index) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    return L && L->hasMask;
}

bool LayerCanPaintContent(int index) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, index) : nullptr;
    return L && L->CanPaintContent();
}

int CreateLayer(const std::string& name) {
    if (!RequireMain("doc.create_layer")) return -1;
    Canvas* c = C();
    if (!c || !g_Device) return -1;
    c->CreateNewLayer(g_Device, name.empty() ? "Layer" : name);
    return c->GetActiveLayerIndex();
}

bool DeleteLayer(int index) {
    if (!RequireMain("doc.delete_layer")) return false;
    Canvas* c = C();
    if (!c || !LayerAt(*c, index)) return false;
    if ((int)c->GetLayers().size() <= 1) return false;
    c->DeleteLayer(index);
    return true;
}

// ---------------------------------------------------------------------------
void NotifyPixelsChanged(int layer) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!L) return;
    L->needsUpload = true;
    L->filtersDirty = true;
    L->stylesDirty = true;
    L->presentationDirty = true;
    L->thumbDirty = true;
    c->MarkCompositeDirty();
    c->SetDocumentModified(true);
}

std::vector<float> GetPixels(int layer, int x, int y, int w, int h) {
    std::vector<float> out;
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!L || !L->tileCache || w <= 0 || h <= 0) return out;
    if (!AllowPixelAlloc(w, h, 16, "GetPixels")) return out;

    const int cw = c->GetWidth(), ch = c->GetHeight();
    if (x < 0 || y < 0 || x + w > cw || y + h > ch) {
        Logger::Get().WarnTag("script", "GetPixels: rect out of bounds");
        return out;
    }

    out.resize((size_t)w * h * 4);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            float rgba[4];
            L->tileCache->GetPixelF(x + col, y + row, rgba);
            size_t i = ((size_t)row * w + col) * 4;
            out[i + 0] = rgba[0];
            out[i + 1] = rgba[1];
            out[i + 2] = rgba[2];
            out[i + 3] = rgba[3];
        }
    }
    return out;
}

bool SetPixels(int layer, int x, int y, int w, int h, const std::vector<float>& rgba) {
    if (!RequireMain("doc.set_pixels")) return false;
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!c || !L || w <= 0 || h <= 0) return false;
    if (!L->CanPaintContent() && !L->isGroup) {
        // Allow write on raster only
    }
    if (L->isGroup || L->IsFill()) {
        Logger::Get().WarnTag("script", "SetPixels: layer is not paint-raster");
        return false;
    }
    if ((size_t)w * h * 4 != rgba.size()) {
        Logger::Get().ErrorTag("script", "SetPixels: buffer size mismatch");
        return false;
    }
    const int cw = c->GetWidth(), ch = c->GetHeight();
    if (x < 0 || y < 0 || x + w > cw || y + h > ch) return false;

    EnsureTileCache(*L, *c);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            size_t i = ((size_t)row * w + col) * 4;
            float px[4] = { rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3] };
            L->tileCache->SetPixelF(x + col, y + row, px);
        }
    }
    NotifyPixelsChanged(layer);
    return true;
}

std::vector<uint8_t> GetLayerRgba8(int layer) {
    std::vector<uint8_t> out;
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!L || !L->tileCache) return out;
    const int w = c->GetWidth(), h = c->GetHeight();
    if (!AllowPixelAlloc(w, h, 4, "GetLayerRgba8")) return out;
    out.resize((size_t)w * h * 4);
    L->tileCache->ExportRGBA8(out.data(), w, h);
    return out;
}

bool SetLayerRgba8(int layer, int srcW, int srcH, const std::vector<uint8_t>& rgba,
                   int dstX, int dstY) {
    if (!RequireMain("doc.set_layer_rgba8")) return false;
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!c || !L || srcW <= 0 || srcH <= 0) return false;
    if (L->isGroup || L->IsFill()) return false;
    if ((size_t)srcW * srcH * 4 != rgba.size()) return false;
    EnsureTileCache(*L, *c);
    L->tileCache->ImportRGBA8(rgba.data(), srcW, srcH, dstX, dstY);
    NotifyPixelsChanged(layer);
    return true;
}

bool GetPixel(int layer, int x, int y, float outRgba[4]) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!L || !L->tileCache || !outRgba) return false;
    if (x < 0 || y < 0 || x >= c->GetWidth() || y >= c->GetHeight()) return false;
    L->tileCache->GetPixelF(x, y, outRgba);
    return true;
}

bool SetPixel(int layer, int x, int y, float r, float g, float b, float a) {
    if (!RequireMain("doc.set_pixel")) return false;
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!c || !L || L->isGroup || L->IsFill()) return false;
    if (x < 0 || y < 0 || x >= c->GetWidth() || y >= c->GetHeight()) return false;
    EnsureTileCache(*L, *c);
    float px[4] = { r, g, b, a };
    L->tileCache->SetPixelF(x, y, px);
    NotifyPixelsChanged(layer);
    return true;
}

// ---------------------------------------------------------------------------
std::vector<uint8_t> GetMask(int layer) {
    std::vector<uint8_t> out;
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!L || !L->hasMask) return out;
    const int w = c->GetWidth(), h = c->GetHeight();
    if (!AllowPixelAlloc(w, h, 1, "GetMask")) return out;
    if (L->maskTiles && L->maskTiles->Valid()) {
        L->maskTiles->Flatten(out);
    } else if ((int)L->mask.size() == w * h) {
        out = L->mask;
    } else {
        out.assign((size_t)w * h, 255);
    }
    return out;
}

bool SetMask(int layer, const std::vector<uint8_t>& mask) {
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!c || !L || !g_Device) return false;
    const int w = c->GetWidth(), h = c->GetHeight();
    if ((int)mask.size() != w * h) return false;

    if (!L->hasMask) {
        c->CreateLayerMask(g_Device, layer);
        L = LayerAt(*c, layer);
        if (!L) return false;
    }
    L->mask = mask;
    if (!L->maskTiles) {
        L->maskTiles = std::make_unique<MaskTiles>();
    }
    L->maskTiles->ImportFlat(mask, w, h);
    L->maskNeedsUpload = true;
    L->maskDirtyX0 = 0; L->maskDirtyY0 = 0;
    L->maskDirtyX1 = w - 1; L->maskDirtyY1 = h - 1;
    c->UpdateLayerMaskTexture(g_Device, layer);
    c->MarkCompositeDirty();
    c->SetDocumentModified(true);
    return true;
}

bool CreateMask(int layer) {
    Canvas* c = C();
    if (!c || !g_Device || !LayerAt(*c, layer)) return false;
    if (LayerAt(*c, layer)->hasMask) return true;
    c->CreateLayerMask(g_Device, layer);
    return LayerAt(*c, layer)->hasMask;
}

// ---------------------------------------------------------------------------
bool HasSelection() {
    Canvas* c = C();
    return c && c->HasSelection();
}

std::vector<uint8_t> GetSelection() {
    std::vector<uint8_t> out;
    Canvas* c = C();
    if (!c || !c->HasSelection()) return out;
    const auto& m = c->GetSelectionMask();
    const int w = c->GetWidth(), h = c->GetHeight();
    if ((int)m.size() != w * h) return out;
    return m;
}

bool SetSelection(const std::vector<uint8_t>& mask) {
    Canvas* c = C();
    if (!c || !g_Device) return false;
    const int w = c->GetWidth(), h = c->GetHeight();
    if ((int)mask.size() != w * h) return false;
    c->SetSelectionMask(mask);
    c->UpdateSelectionMaskTexture(g_Device);
    return true;
}

int ActiveLayerTileCount() {
    Canvas* c = C();
    return c ? (int)c->GetActiveLayerTileCount() : 0;
}

int TileSize() { return TILE_SIZE; }

static std::string ApplyTileName(const std::string& pattern, int tx, int ty) {
    std::string s = pattern;
    auto repl = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    repl(s, "{x}", std::to_string(tx));
    repl(s, "{y}", std::to_string(ty));
    repl(s, "{X}", std::to_string(tx));
    repl(s, "{Y}", std::to_string(ty));
    return s;
}

int ExportCompositeTiles(const std::string& outDir,
                         const std::string& namePattern,
                         int tilesX, int tilesY,
                         const std::string& mode,
                         std::string* outError) {
    auto fail = [&](const std::string& msg) -> int {
        Logger::Get().ErrorTag("script", "export_tiles refused: " + msg);
        if (outError) *outError = msg;
        return 0;
    };
    if (!RequireMain("doc.export_tiles")) return fail("not on main thread");

    Canvas* c = C();
    if (!c) return fail("no active document");
    const int w = c->GetWidth(), h = c->GetHeight();
    if (w < 1 || h < 1) return fail("invalid canvas size");

    std::string m = mode;
    for (char& ch : m) ch = (char)std::tolower((unsigned char)ch);
    if (m != "count" && m != "size")
        return fail("mode must be 'count' or 'size'");

    if (outDir.empty()) return fail("out_dir is empty");
    if (namePattern.empty()) return fail("name_pattern is empty");
    if (namePattern.find("{x}") == std::string::npos && namePattern.find("{X}") == std::string::npos)
        return fail("name_pattern must contain {x}");
    if (namePattern.find("{y}") == std::string::npos && namePattern.find("{Y}") == std::string::npos)
        return fail("name_pattern must contain {y}");

    int nX = 0, nY = 0;
    int cellW = 0, cellH = 0;
    if (m == "count") {
        if (tilesX < 1 || tilesX > 64 || tilesY < 1 || tilesY > 64)
            return fail("count mode: tiles X/Y must be in 1..64");
        nX = tilesX;
        nY = tilesY;
        cellW = w / nX;
        cellH = h / nY;
        if (cellW < 1 || cellH < 1)
            return fail("tile cell would be smaller than 1px");
    } else {
        // size: tilesX/Y are pixel sizes of each tile
        if (tilesX < 1 || tilesY < 1)
            return fail("size mode: tile width/height must be >= 1");
        if (tilesX > w || tilesY > h)
            return fail("size mode: tile size exceeds document");
        cellW = tilesX;
        cellH = tilesY;
        nX = (w + cellW - 1) / cellW;
        nY = (h + cellH - 1) / cellH;
        if (nX > 64 || nY > 64)
            return fail("size mode: would produce more than 64 tiles on an axis (max 64)");
    }

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::u8path(outDir);
    std::filesystem::create_directories(dir, ec);
    if (ec) return fail(std::string("cannot create out_dir: ") + ec.message());

    // Prefer U8 composite path via temporary float only if allowed
    if (!AllowPixelAlloc(w, h, 16, "export_tiles composite"))
        return fail("document too large for flat composite export");

    std::vector<float> full = c->GetCompositePixels();
    if ((int)full.size() != w * h * 4) {
        // fallback composed
        full = c->GetComposedPixels();
    }
    if ((int)full.size() != w * h * 4)
        return fail("failed to read composite pixels");

    int written = 0;
    for (int ty = 0; ty < nY; ++ty) {
        for (int tx = 0; tx < nX; ++tx) {
            int x0 = (m == "count") ? tx * cellW : tx * cellW;
            int y0 = (m == "count") ? ty * cellH : ty * cellH;
            int x1 = (m == "count")
                ? ((tx == nX - 1) ? w : (tx + 1) * cellW)
                : std::min(w, x0 + cellW);
            int y1 = (m == "count")
                ? ((ty == nY - 1) ? h : (ty + 1) * cellH)
                : std::min(h, y0 + cellH);
            int tw = x1 - x0, th = y1 - y0;
            if (tw < 1 || th < 1) continue;

            std::vector<float> tile((size_t)tw * th * 4);
            for (int row = 0; row < th; ++row) {
                for (int col = 0; col < tw; ++col) {
                    size_t si = ((size_t)(y0 + row) * w + (x0 + col)) * 4;
                    size_t di = ((size_t)row * tw + col) * 4;
                    tile[di + 0] = full[si + 0];
                    tile[di + 1] = full[si + 1];
                    tile[di + 2] = full[si + 2];
                    tile[di + 3] = full[si + 3];
                }
            }

            std::string fname = ApplyTileName(namePattern, tx, ty);
            // prevent path traversal
            if (fname.find("..") != std::string::npos || fname.find('/') != std::string::npos ||
                fname.find('\\') != std::string::npos)
                return fail("name_pattern resolved to unsafe path: " + fname);

            std::filesystem::path outPath = dir / std::filesystem::u8path(fname);
            std::string outUtf8 = PathUtil::WideToUtf8(outPath.wstring());
            if (!ImageManager::SaveImageToFile(outUtf8, tile, tw, th)) {
                return fail("failed to write " + outUtf8);
            }
            ++written;
        }
    }

    Logger::Get().InfoTag("script",
        "export_tiles: wrote " + std::to_string(written) + " file(s) to " + outDir +
        " mode=" + m + " grid=" + std::to_string(nX) + "x" + std::to_string(nY));
    if (outError) outError->clear();
    return written;
}

bool FillRect(int layer, int x, int y, int w, int h,
              float r, float g, float b, float a) {
    if (!RequireMain("doc.fill_rect")) return false;
    Canvas* c = C();
    Layer* L = c ? LayerAt(*c, layer) : nullptr;
    if (!c || !L) {
        Logger::Get().ErrorTag("script", "fill_rect refused: bad layer");
        return false;
    }
    if (L->isGroup || L->IsFill()) {
        Logger::Get().ErrorTag("script", "fill_rect refused: layer not raster");
        return false;
    }
    if (w < 1 || h < 1) {
        Logger::Get().ErrorTag("script", "fill_rect refused: non-positive size");
        return false;
    }
    if (x < 0 || y < 0 || x + w > c->GetWidth() || y + h > c->GetHeight()) {
        Logger::Get().ErrorTag("script", "fill_rect refused: out of bounds");
        return false;
    }
    EnsureTileCache(*L, *c);
    float rgba[4] = { r, g, b, a };
    // TileCache::FillRect uses inclusive x1,y1
    L->tileCache->FillRect(x, y, x + w - 1, y + h - 1, rgba);
    NotifyPixelsChanged(layer);
    return true;
}

bool BeginEdit(int layer) {
    if (!RequireMain("doc.begin_edit")) return false;
    Canvas* c = C();
    if (!c) return false;
    return c->BeginScriptPixelEdit(layer);
}

bool EndEdit(const std::string& actionName) {
    if (!RequireMain("doc.end_edit")) return false;
    Canvas* c = C();
    if (!c) return false;
    return c->EndScriptPixelEdit(actionName);
}

bool CancelEdit() {
    if (!RequireMain("doc.cancel_edit")) return false;
    Canvas* c = C();
    if (!c) return false;
    c->CancelScriptPixelEdit();
    return true;
}

bool IsEditActive() {
    Canvas* c = C();
    return c && c->IsScriptPixelEditActive();
}

bool SelectionBounds(int& x, int& y, int& w, int& h) {
    Canvas* c = C();
    if (!c) return false;
    return c->GetSelectionBoundsPublic(x, y, w, h);
}

bool GetPixelsSelectionOrFull(int layer, std::vector<float>& outRgba,
                              int& outX, int& outY, int& outW, int& outH) {
    Canvas* c = C();
    if (!c) return false;
    outX = outY = 0;
    outW = c->GetWidth();
    outH = c->GetHeight();
    if (c->HasSelection()) {
        int sx, sy, sw, sh;
        if (c->GetSelectionBoundsPublic(sx, sy, sw, sh) && sw > 0 && sh > 0) {
            outX = sx; outY = sy; outW = sw; outH = sh;
        }
    }
    outRgba = GetPixels(layer, outX, outY, outW, outH);
    return !outRgba.empty();
}

} // namespace script
