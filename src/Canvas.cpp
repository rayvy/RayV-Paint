#include "Canvas.h"
#include <d3d11_1.h>
#include "core/UndoRedoManager.h"
#include "layer/LayerStyles.h"
#include "core/TileCache.h"
#include "core/HalfFloat.h"
#include "core/Logger.h"
#include "core/JobManager.h"
#include "core/MemoryStats.h"
#include "core/ImageManager.h"
#include "core/IccProfiles.h"
#include "core/PathUtil.h"
#include "core/ClipboardHelper.h"
#include "assets/AssetStore.h"
#include "assets/AssetManager.h"
#include "utilities/ContentAwareFill.h"
#include "vector/PathMath.h"
#include "vector/VectorRasterizer.h"
#include "vector/SvgIo.h"
// Prefer PathUtil for all disk paths (UTF-8 / wide on Windows).
#include <opencv2/imgproc.hpp>
#include "core/ConfigManager.h"
#include "core/DdsCodec.h"
#include "modio/ModIniParser.h"
#include <chrono>
#include <sstream>
#include <d3dcompiler.h>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <stb_image.h>
#include <stb_image_write.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <new>

#ifdef _WIN32
#include <windows.h>
static std::wstring UTF8ToWString(const std::string& str) {
    return PathUtil::Utf8ToWide(str);
}
#endif

// Explicitly declare stbi_zlib_compress which is defined in ImageManager.cpp (via stb_image_write implementation)
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);
extern "C" char* stbi_zlib_decode_malloc(const char* buffer, int len, int* outlen);

// Defined later (near BackupTile); used by paint/transform commit paths.
static std::vector<TileDelta> SealActiveStrokeDeltas(
    std::unordered_map<int, TileDelta>& active, TileCache* cache);

using json = nlohmann::json;

// ============================================================
// Pixel-buffer compatibility helpers
// These bridge TileCache layers with code expecting flat float RGBA.
// Use ExportLayerF + SetLayerPixelsF for non-interactive paths
// (filters, compositing, save/load). NOT for paint hot paths.
// ============================================================
static std::vector<float> ExportLayerF(const Layer& layer, int w, int h) {
    std::vector<float> out((size_t)w * h * 4, 0.f);
    if (layer.tileCache) layer.tileCache->ExportRGBA32F(out.data(), w, h);
    return out;
}
static void SetLayerPixelsF(Layer& layer, const std::vector<float>& pixels, int w, int h,
                             CanvasPixelFormat fmt = CanvasPixelFormat::RGBA8,
                             bool markFiltersDirty = true) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
    layer.tileCache->ImportRGBA32F(pixels.data(), w, h);
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    if (markFiltersDirty)
        layer.filtersDirty = true;
}
static bool LayerHasPixels(const Layer& layer) {
    if (layer.IsFill()) return true;
    return layer.tileCache && !layer.tileCache->IsEmpty();
}
static void EnsureLayerTileCache(Layer& layer, int w, int h, CanvasPixelFormat fmt) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
}
// Selection mask helpers: convert float[0,1] <-> uint8
static uint8_t SelF2U8(float v) {
    return (uint8_t)(std::clamp(v, 0.f, 1.f) * 255.f + .5f);
}
static float SelU82F(uint8_t v) {
    return v / 255.f;
}
static float GetSelWeight(const std::vector<uint8_t>& mask, int w, int h, int x, int y, bool hasSel) {
    // Active selection = hasSel + full-document mask. Empty / wrong size → full canvas (no clip).
    if (!hasSel || mask.empty() || w <= 0 || h <= 0) return 1.f;
    if (mask.size() != (size_t)w * (size_t)h) return 1.f;
    if (x < 0 || y < 0 || x >= w || y >= h) return 0.f;
    return SelU82F(mask[(size_t)y * (size_t)w + (size_t)x]);
}
// Overload kept for call sites that only have width (document square indexing).
static float GetSelWeight(const std::vector<uint8_t>& mask, int w, int x, int y, bool hasSel) {
    // height unknown — require exact size match via width when square not assumed:
    if (!hasSel || mask.empty() || w <= 0) return 1.f;
    if ((mask.size() % (size_t)w) != 0) return 1.f;
    const int h = (int)(mask.size() / (size_t)w);
    return GetSelWeight(mask, w, h, x, y, hasSel);
}
// Full-document float composites explode on large textures (16K RGBA32F ≈ 4 GiB).
static constexpr int kMaxFlatCompositePixels = 8192 * 8192;

static bool CanAllocateFlatComposite(int w, int h, const char* context) {
    const size_t pixels = (size_t)std::max(0, w) * (size_t)std::max(0, h);
    if (pixels > (size_t)kMaxFlatCompositePixels) {
        Logger::Get().ErrorTag("mem",
            std::string(context) + ": refusing full float composite for " +
            std::to_string(w) + "x" + std::to_string(h) +
            " (" + MemoryStats::FormatBytes(pixels * 16) +
            "). Use tiled export / crop, or raise threshold later.");
        return false;
    }
    const size_t est = MemoryStats::EstimateImageBytes(w, h, 16);
    // Soft budget (EMERGENCY_SAFE_PLAN): warn but proceed — hard refuse only absurd ceiling.
    if (MemoryStats::ExceedsRamBudget(est, 0.40)) {
        MemoryStats::LogSoftBudget(std::string(context) + " flat_float_composite", est, 0.40);
    }
    if (MemoryStats::ExceedsHardRamCeiling(est)) {
        Logger::Get().ErrorTag("mem",
            std::string(context) + ": hard ceiling — estimated " +
            MemoryStats::FormatBytes(est) + " (>95% RAM or >64GiB). Refusing.");
        return false;
    }
    return true;
}

static bool ComposeVisibleLayersRGBA8(const std::vector<Layer>& layers, int w, int h,
                                      std::vector<uint8_t>& out);

// Legacy float composite used by clipboard / internal helpers.
// Prefer ComposeVisibleLayersRGBA8 for export (filters + blend modes + lower peak RAM).
static std::vector<float> ComposeVisibleLayers(const std::vector<Layer>& layers, int w, int h) {
    std::vector<uint8_t> rgba8;
    if (!ComposeVisibleLayersRGBA8(layers, w, h, rgba8) || rgba8.empty()) {
        // Fallback empty
        if (!CanAllocateFlatComposite(w, h, "ComposeVisibleLayers")) return {};
        return std::vector<float>((size_t)w * h * 4, 0.f);
    }
    std::vector<float> composite((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
        composite[i] = rgba8[i] / 255.f;
    }
    return composite;
}

// Match Canvas.hlsl PSLayerBlend RGB blend modes (source s, destination d).
static void ApplyBlendModeRGB(BlendMode mode,
                              float sr, float sg, float sb,
                              float dr, float dg, float db,
                              float& or_, float& og, float& ob) {
    or_ = sr; og = sg; ob = sb;
    switch (mode) {
    case BlendMode::Multiply:
        or_ = sr * dr; og = sg * dg; ob = sb * db;
        break;
    case BlendMode::Screen:
        or_ = 1.f - (1.f - sr) * (1.f - dr);
        og = 1.f - (1.f - sg) * (1.f - dg);
        ob = 1.f - (1.f - sb) * (1.f - db);
        break;
    case BlendMode::Overlay:
        or_ = (dr < 0.5f) ? 2.f * sr * dr : 1.f - 2.f * (1.f - sr) * (1.f - dr);
        og = (dg < 0.5f) ? 2.f * sg * dg : 1.f - 2.f * (1.f - sg) * (1.f - dg);
        ob = (db < 0.5f) ? 2.f * sb * db : 1.f - 2.f * (1.f - sb) * (1.f - db);
        break;
    case BlendMode::Add:
        or_ = std::min(sr + dr, 1.f);
        og = std::min(sg + dg, 1.f);
        ob = std::min(sb + db, 1.f);
        break;
    case BlendMode::Subtract:
        or_ = std::max(dr - sr, 0.f);
        og = std::max(dg - sg, 0.f);
        ob = std::max(db - sb, 0.f);
        break;
    case BlendMode::Darken:
        or_ = std::min(sr, dr); og = std::min(sg, dg); ob = std::min(sb, db);
        break;
    case BlendMode::Lighten:
        or_ = std::max(sr, dr); og = std::max(sg, dg); ob = std::max(sb, db);
        break;
    case BlendMode::HardLight:
        or_ = (sr < 0.5f) ? 2.f * sr * dr : 1.f - 2.f * (1.f - sr) * (1.f - dr);
        og = (sg < 0.5f) ? 2.f * sg * dg : 1.f - 2.f * (1.f - sg) * (1.f - dg);
        ob = (sb < 0.5f) ? 2.f * sb * db : 1.f - 2.f * (1.f - sb) * (1.f - db);
        break;
    case BlendMode::SoftLight:
        or_ = (sr < 0.5f) ? dr - (1.f - 2.f * sr) * dr * (1.f - dr)
                          : dr + (2.f * sr - 1.f) * (std::sqrt(std::max(dr, 0.f)) - dr);
        og = (sg < 0.5f) ? dg - (1.f - 2.f * sg) * dg * (1.f - dg)
                          : dg + (2.f * sg - 1.f) * (std::sqrt(std::max(dg, 0.f)) - dg);
        ob = (sb < 0.5f) ? db - (1.f - 2.f * sb) * db * (1.f - db)
                          : db + (2.f * sb - 1.f) * (std::sqrt(std::max(db, 0.f)) - db);
        break;
    case BlendMode::Normal:
    default:
        break;
    }
    or_ = std::clamp(or_, 0.f, 1.f);
    og = std::clamp(og, 0.f, 1.f);
    ob = std::clamp(ob, 0.f, 1.f);
}

// Matches D3D layer blend: SRC_ALPHA / INV_SRC_ALPHA after optional RGB blend mode.
// alphaRewrite: if false, sa is RGB morph strength only — destination alpha is preserved.
static inline void BlendLayerPixelU8(uint8_t* dest, float sr, float sg, float sb, float sa, BlendMode mode,
                                     bool alphaRewrite = true) {
    if (sa <= 0.f) return;
    float dr = dest[0] / 255.f;
    float dg = dest[1] / 255.f;
    float db = dest[2] / 255.f;
    float da = dest[3] / 255.f;

    float br = sr, bg = sg, bb = sb;
    if (mode != BlendMode::Normal) {
        ApplyBlendModeRGB(mode, sr, sg, sb, dr, dg, db, br, bg, bb);
    }

    const float inv = 1.f - sa;
    dest[0] = (uint8_t)(std::clamp(br * sa + dr * inv, 0.f, 1.f) * 255.f + 0.5f);
    dest[1] = (uint8_t)(std::clamp(bg * sa + dg * inv, 0.f, 1.f) * 255.f + 0.5f);
    dest[2] = (uint8_t)(std::clamp(bb * sa + db * inv, 0.f, 1.f) * 255.f + 0.5f);
    if (alphaRewrite)
        dest[3] = (uint8_t)(std::clamp(sa + da * inv, 0.f, 1.f) * 255.f + 0.5f);
    // else keep dest[3]
}

// Float composite blend — no 0..1 clamp on RGB (HDR / height values preserved).
static inline void BlendLayerPixelF(float* dest, float sr, float sg, float sb, float sa, BlendMode mode,
                                    bool alphaRewrite = true) {
    if (sa <= 0.f) return;
    float dr = dest[0], dg = dest[1], db = dest[2], da = dest[3];
    float br = sr, bg = sg, bb = sb;
    if (mode != BlendMode::Normal) {
        ApplyBlendModeRGB(mode, sr, sg, sb, dr, dg, db, br, bg, bb);
    }
    const float inv = 1.f - sa;
    dest[0] = br * sa + dr * inv;
    dest[1] = bg * sa + dg * inv;
    dest[2] = bb * sa + db * inv;
    if (alphaRewrite)
        dest[3] = sa + da * inv;
}

static bool LayerEffectivelyVisible(const std::vector<Layer>& layers, const Layer& layer) {
    if (!layer.visible) return false;
    // Children of groups are drawn only inside group isolation (not as top-level).
    if (layer.parentGroupId >= 0 && layer.parentGroupId < (int)layers.size())
        return false;
    if (layer.isGroup) {
        // Group with baked presentation (FX) exports as a single unit.
        if (layer.presentationCache && !layer.presentationCache->IsEmpty())
            return true;
        // Group without FX: expand children in ComposeVisibleLayersRGBA8 instead
        return false;
    }
    return LayerHasPixels(layer) || layer.IsFill();
}

// True if any ancestor group is hidden.
static bool AncestorGroupHidden(const std::vector<Layer>& layers, int parentId) {
    int p = parentId;
    while (p >= 0 && p < (int)layers.size()) {
        if (!layers[p].visible) return true;
        p = layers[p].parentGroupId;
    }
    return false;
}

static const TileCache* LayerExportCache(const Layer& layer) {
    // Prefer baked presentation (styles / group flatten) when available
    if (layer.presentationCache && !layer.presentationCache->IsEmpty())
        return layer.presentationCache.get();
    if (!layer.filters.empty() && layer.filteredCache && !layer.filteredCache->IsEmpty()) {
        return layer.filteredCache.get();
    }
    return layer.tileCache.get();
}

static bool LayerNeedsPresentationBake(const Layer& layer) {
    return layer.HasEnabledStyles() ||
           (layer.IsFill() && (LayerFilterListHasEnabled(layer.filters) || layer.HasEnabledStyles()));
}

// Tile-stream composite with non-destructive filters + blend modes (matches viewport).
// Peak: ~w*h*4 (1 GiB @ 16K). Caller should rebuild filtered caches first.
static bool ComposeVisibleLayersRGBA8(const std::vector<Layer>& layers, int w, int h,
                                      std::vector<uint8_t>& out) {
    const size_t bytes = (size_t)w * (size_t)h * 4ull;
    if (bytes == 0) {
        out.clear();
        return true;
    }
    // Soft budget: try allocation even when over soft %. Hard ceiling still refuses.
    if (MemoryStats::ExceedsRamBudget(bytes, 0.50)) {
        MemoryStats::LogSoftBudget("ComposeVisibleLayersRGBA8", bytes, 0.50);
    }
    if (MemoryStats::ExceedsHardRamCeiling(bytes)) {
        Logger::Get().ErrorTag("mem",
            "ComposeVisibleLayersRGBA8: hard ceiling " + MemoryStats::FormatBytes(bytes));
        return false;
    }

    Logger::Get().InfoTag("io",
        "Export composite RGBA8 " + std::to_string(w) + "x" + std::to_string(h) +
        " est=" + MemoryStats::FormatBytes(bytes) + " (filters+blend modes)");
    MemoryStats::LogSnapshot("export_rgba8_alloc");

    try {
        out.assign(bytes, 0);
    } catch (const std::bad_alloc&) {
        Logger::Get().ErrorTag("mem",
            "ComposeVisibleLayersRGBA8: bad_alloc for " + MemoryStats::FormatBytes(bytes) +
            " — clean refuse (no crash)");
        out.clear();
        return false;
    }

    // Build export list: top-level only. Groups must already have presentationCache (export prep).
    // IMPORTANT: export composes ALL visible layers (viewport map/role filter is display-only).
    // Multi-map packing / per-role export is handled by TextureSetIO + QuickExportAllMaps.
    std::vector<const Layer*> vis;
    vis.reserve(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        const Layer& layer = layers[i];
        if (!layer.visible || layer.parentGroupId >= 0) continue;
        if (layer.isGroup) {
            if (layer.presentationCache && !layer.presentationCache->IsEmpty())
                vis.push_back(&layer);
            continue;
        }
        if (LayerHasPixels(layer) || layer.IsFill())
            vis.push_back(&layer);
    }
    if (vis.empty()) return true;

    // Fast path: one Normal layer — full RGBA copy (keeps RGB even when A=0).
    if (vis.size() == 1 &&
        vis[0]->blendMode == BlendMode::Normal &&
        vis[0]->opacity >= 0.999f &&
        vis[0]->filters.empty() &&
        !(vis[0]->hasMask && vis[0]->mask.size() == (size_t)w * h) &&
        vis[0]->tileCache) {
        vis[0]->tileCache->ExportRGBA8(out.data(), w, h);
        MemoryStats::LogSnapshot("export_rgba8_single_layer");
        return true;
    }

    const int tilesX = (w + TILE_SIZE - 1) / TILE_SIZE;
    const int tilesY = (h + TILE_SIZE - 1) / TILE_SIZE;
    const int progressStep = std::max(1, tilesY / 20);

    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const int x0 = tx * TILE_SIZE;
            const int y0 = ty * TILE_SIZE;
            const int x1 = std::min(x0 + TILE_SIZE, w);
            const int y1 = std::min(y0 + TILE_SIZE, h);
            const int tw = x1 - x0;
            const int th = y1 - y0;

            bool firstLayer = true;
            for (const Layer* layer : vis) {
                // Baked presentation already includes fillOpacity + styles + mask
                // Groups always export via presentationCache when present
                const bool baked =
                    (layer->HasEnabledStyles() || layer->isGroup) &&
                    layer->presentationCache && !layer->presentationCache->IsEmpty();
                const TileCache* cache = LayerExportCache(*layer);

                const bool useMask = !baked && layer->hasMask && layer->mask.size() == (size_t)w * h;
                // Opacity: content fill only when not baked; styles path draws at 1
                const float opacity = baked ? 1.f : layer->opacity;
                const BlendMode mode = layer->blendMode;

                float fillSolid[4] = {0,0,0,0};
                const bool solidFill = layer->IsFill() && !cache;
                // Textured fill without presentation cache: sample asset per pixel
                std::shared_ptr<const assets::TexturePayload> fillPay;
                if (solidFill) {
                    layer->fill.ResolveRgba(fillSolid);
                    if (layer->fill.useTexture && !layer->fill.textureAssetKey.empty())
                        fillPay = assets::AssetStore::Get().GetPayload(layer->fill.textureAssetKey);
                }

                if (!cache && !solidFill) continue;

                const uint8_t* tile = (!solidFill && cache && cache->GetFormat() == CanvasPixelFormat::RGBA8)
                    ? cache->GetTileData(tx, ty) : nullptr;

                for (int ly = 0; ly < th; ++ly) {
                    const int y = y0 + ly;
                    for (int lx = 0; lx < tw; ++lx) {
                        const int x = x0 + lx;
                        float sr, sg, sb, saPix;
                        if (solidFill) {
                            sr = fillSolid[0]; sg = fillSolid[1]; sb = fillSolid[2]; saPix = fillSolid[3];
                            if (fillPay && fillPay->w > 0 && fillPay->h > 0) {
                                float u = (float)x / (float)std::max(1, w) * layer->fill.texScale[0]
                                        + layer->fill.texOffset[0];
                                float v = (float)y / (float)std::max(1, h) * layer->fill.texScale[1]
                                        + layer->fill.texOffset[1];
                                u = u - std::floor(u);
                                v = v - std::floor(v);
                                int sx = std::min(fillPay->w - 1, (int)(u * fillPay->w));
                                int sy = std::min(fillPay->h - 1, (int)(v * fillPay->h));
                                if (sx < 0) sx = 0;
                                if (sy < 0) sy = 0;
                                const uint8_t* p = fillPay->rgba.data()
                                    + ((size_t)sy * fillPay->w + sx) * 4;
                                sr *= p[0] / 255.f; sg *= p[1] / 255.f;
                                sb *= p[2] / 255.f; saPix *= p[3] / 255.f;
                            }
                        } else if (tile) {
                            const uint8_t* sp = tile + ((size_t)ly * TILE_SIZE + lx) * 4;
                            sr = sp[0] / 255.f; sg = sp[1] / 255.f; sb = sp[2] / 255.f;
                            saPix = sp[3] / 255.f;
                        } else {
                            float rgba[4];
                            cache->GetPixelF(x, y, rgba);
                            sr = rgba[0]; sg = rgba[1]; sb = rgba[2];
                            saPix = rgba[3];
                        }
                        float sa = saPix * opacity;
                        if (useMask) sa *= layer->mask[(size_t)y * w + x] / 255.f;
                        uint8_t* dp = out.data() + ((size_t)y * w + x) * 4;

                        if (firstLayer) {
                            float aWrite = saPix * opacity;
                            if (useMask) aWrite *= layer->mask[(size_t)y * w + x] / 255.f;
                            dp[0] = (uint8_t)(std::clamp(sr, 0.f, 1.f) * 255.f + 0.5f);
                            dp[1] = (uint8_t)(std::clamp(sg, 0.f, 1.f) * 255.f + 0.5f);
                            dp[2] = (uint8_t)(std::clamp(sb, 0.f, 1.f) * 255.f + 0.5f);
                            dp[3] = (uint8_t)(std::clamp(aWrite, 0.f, 1.f) * 255.f + 0.5f);
                        } else {
                            if (sa <= 0.f) continue;
                            BlendLayerPixelU8(dp, sr, sg, sb, sa, mode, layer->alphaRewrite);
                        }
                    }
                }
                firstLayer = false;
            }
        }
        if (ty == 0 || ((ty + 1) % progressStep) == 0 || ty + 1 == tilesY) {
            int pct = (int)(((ty + 1) * 100.0) / tilesY);
            Logger::Get().InfoTag("io",
                "Export composite " + std::to_string(pct) + "% (" +
                std::to_string(ty + 1) + "/" + std::to_string(tilesY) + " tile-rows)");
        }
    }

    MemoryStats::LogSnapshot("export_rgba8_done");
    return true;
}

// Interactive style/filter bake proxy. Full-res only for small docs.
// Was threshold 4096 → 4K did FULL float bake (multi×256MiB) → lag/OOM/crash on FX.
static void ComputeCompositePreviewSize(int canvasW, int canvasH, int& outW, int& outH) {
    constexpr int kProxyThreshold = 2048; // >2K → proxy
    constexpr int kProxyMaxDim = 1536;    // viewport bake budget

    int maxDim = std::max(canvasW, canvasH);
    if (maxDim <= kProxyThreshold) {
        outW = std::max(1, canvasW);
        outH = std::max(1, canvasH);
        return;
    }

    float scale = (float)kProxyMaxDim / (float)maxDim;
    outW = std::max(1, (int)std::round(canvasW * scale));
    outH = std::max(1, (int)std::round(canvasH * scale));
}

// Get the directory containing the running executable
static std::wstring GetExecutableDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : path.substr(0, pos);
}

// Helper to compile shaders from file at runtime (with caching support)
static HRESULT CompileShaderFromFile(const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut) {
    // Build cache file path: e.g. "shaders/VSMain.cso"
    std::wstring cachePath = GetExecutableDir() + L"\\shaders\\" + std::wstring(szEntryPoint, szEntryPoint + strlen(szEntryPoint)) + L".cso";
    
    // Check if Canvas.hlsl and cache file exist
    bool hlslExists = std::filesystem::exists(szFileName);
    bool cacheExists = std::filesystem::exists(cachePath);
    
    bool useCache = false;
    if (cacheExists) {
        if (hlslExists) {
            // Compare timestamps
            auto hlslTime = std::filesystem::last_write_time(szFileName);
            auto cacheTime = std::filesystem::last_write_time(cachePath);
            if (cacheTime >= hlslTime) {
                useCache = true;
            }
        } else {
            // HLSL doesn't exist but cache does
            useCache = true;
        }
    }
    
    if (useCache) {
        // Read compiled shader bytecode directly from disk
        std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            
            HRESULT hr = D3DCreateBlob(static_cast<SIZE_T>(size), ppBlobOut);
            if (SUCCEEDED(hr)) {
                if (file.read(reinterpret_cast<char*>((*ppBlobOut)->GetBufferPointer()), size)) {
                    Logger::Get().Debug("Loaded cached shader bytecode: " + std::string(szEntryPoint));
                    return S_OK;
                }
                (*ppBlobOut)->Release();
                *ppBlobOut = nullptr;
            }
        }
    }

    // Otherwise, compile it
    Logger::Get().Info("Compiling shader: " + std::string(szEntryPoint));
    DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    dwShaderFlags |= D3DCOMPILE_DEBUG;
    dwShaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* pErrorBlob = nullptr;
    HRESULT hr = D3DCompileFromFile(szFileName, nullptr, nullptr, szEntryPoint, szShaderModel,
        dwShaderFlags, 0, ppBlobOut, &pErrorBlob);
    if (FAILED(hr)) {
        if (pErrorBlob) {
            OutputDebugStringA((char*)pErrorBlob->GetBufferPointer());
            std::cerr << "Shader compile error in " << szEntryPoint << ": " << (char*)pErrorBlob->GetBufferPointer() << std::endl;
            pErrorBlob->Release();
        } else {
            std::wcerr << L"Failed to open/read shader file: " << szFileName << std::endl;
        }
        return hr;
    }
    if (pErrorBlob) pErrorBlob->Release();

    // Cache the compiled bytecode to disk
    if (SUCCEEDED(hr) && *ppBlobOut) {
        // Ensure directory exists
        std::filesystem::create_directories(GetExecutableDir() + L"\\shaders");
        std::ofstream file(cachePath, std::ios::binary);
        if (file.is_open()) {
            file.write(reinterpret_cast<const char*>((*ppBlobOut)->GetBufferPointer()), (*ppBlobOut)->GetBufferSize());
            Logger::Get().Debug("Cached compiled shader bytecode to disk: " + std::string(szEntryPoint));
        }
    }

    return S_OK;
}

Canvas::Canvas()
    : m_Width(0)
    , m_Height(0)
    , m_Zoom(1.0f)
    , m_Pan(0.0f, 0.0f) {
    ResetView();
}

Canvas::~Canvas() {
    Shutdown();
}

void Canvas::ResetView() {
    m_Zoom = 1.0f;
    m_Pan.x = -m_Width * 0.5f * m_Zoom;
    m_Pan.y = -m_Height * 0.5f * m_Zoom;
    m_RotationAngle = 0.0f;
    m_ViewportFlipH = false;
    m_ViewportFlipV = false;
}

bool Canvas::Initialize(ID3D11Device* device) {
    HRESULT hr;

    // Build absolute path to shaders folder inside output directory
    std::wstring shaderPath = GetExecutableDir() + L"\\shaders\\Canvas.hlsl";

    // 1. Compile and Create Vertex Shader
    ID3DBlob* vsBlob = nullptr;
    hr = CompileShaderFromFile(shaderPath.c_str(), "VSMain", "vs_4_0", &vsBlob);
    if (FAILED(hr)) {
        std::cerr << "Failed compiling Canvas VS" << std::endl;
        return false;
    }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_VertexShader);
    if (FAILED(hr)) {
        vsBlob->Release();
        return false;
    }

    // Compile and Create Layer Composition Vertex Shader
    ID3DBlob* vsLayerBlob = nullptr;
    hr = CompileShaderFromFile(shaderPath.c_str(), "VSLayerMain", "vs_4_0", &vsLayerBlob);
    if (FAILED(hr)) {
        std::cerr << "Failed compiling VSLayerMain" << std::endl;
        vsBlob->Release();
        return false;
    }

    hr = device->CreateVertexShader(vsLayerBlob->GetBufferPointer(), vsLayerBlob->GetBufferSize(), nullptr, &m_LayerVertexShader);
    vsLayerBlob->Release();
    if (FAILED(hr)) {
        vsBlob->Release();
        return false;
    }

    // Define the input layout for the shader
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    UINT numElements = sizeof(layout) / sizeof(layout[0]);

    hr = device->CreateInputLayout(layout, numElements, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_InputLayout);
    vsBlob->Release();
    if (FAILED(hr)) {
        return false;
    }

    // 2. Compile and Create Pixel Shader (Presentation)
    ID3DBlob* psBlob = nullptr;
    hr = CompileShaderFromFile(shaderPath.c_str(), "PSMain", "ps_4_0", &psBlob);
    if (FAILED(hr)) {
        std::cerr << "Failed compiling Canvas PS" << std::endl;
        return false;
    }

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_PixelShader);
    psBlob->Release();
    if (FAILED(hr)) {
        return false;
    }

    // 3. Compile and Create Pixel Shader (Layer Blend)
    ID3DBlob* layerBlob = nullptr;
    hr = CompileShaderFromFile(shaderPath.c_str(), "PSLayerBlend", "ps_4_0", &layerBlob);
    if (FAILED(hr)) {
        std::cerr << "Failed compiling PSLayerBlend" << std::endl;
        return false;
    }

    hr = device->CreatePixelShader(layerBlob->GetBufferPointer(), layerBlob->GetBufferSize(), nullptr, &m_LayerBlendPixelShader);
    layerBlob->Release();
    if (FAILED(hr)) {
        return false;
    }

    // 3.5 Compile and Create Pixel Shader (Selection Outline)
    ID3DBlob* outlineBlob = nullptr;
    hr = CompileShaderFromFile(shaderPath.c_str(), "PSSelectionOutline", "ps_4_0", &outlineBlob);
    if (FAILED(hr)) {
        std::cerr << "Failed compiling PSSelectionOutline" << std::endl;
        return false;
    }

    hr = device->CreatePixelShader(outlineBlob->GetBufferPointer(), outlineBlob->GetBufferSize(), nullptr, &m_SelectionOutlinePixelShader);
    outlineBlob->Release();
    if (FAILED(hr)) {
        return false;
    }

    // 4. Create Vertex Buffer (Unit Quad)
    Vertex vertices[] = {
        { DirectX::XMFLOAT2(0.0f, 0.0f), DirectX::XMFLOAT2(0.0f, 0.0f) }, // Top-Left
        { DirectX::XMFLOAT2(1.0f, 0.0f), DirectX::XMFLOAT2(1.0f, 0.0f) }, // Top-Right
        { DirectX::XMFLOAT2(1.0f, 1.0f), DirectX::XMFLOAT2(1.0f, 1.0f) }, // Bottom-Right
        { DirectX::XMFLOAT2(0.0f, 1.0f), DirectX::XMFLOAT2(0.0f, 1.0f) }, // Bottom-Left
    };

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(vertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA InitData = {};
    InitData.pSysMem = vertices;

    hr = device->CreateBuffer(&bd, &InitData, &m_VertexBuffer);
    if (FAILED(hr)) {
        return false;
    }

    // Dynamic quad VB for sparse GPU-tile draws (pos/uv updated per tile)
    {
        D3D11_BUFFER_DESC tbd = {};
        tbd.Usage = D3D11_USAGE_DYNAMIC;
        tbd.ByteWidth = sizeof(Vertex) * 4;
        tbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        tbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&tbd, nullptr, &m_TileQuadVB))) {
            Logger::Get().WarnTag("gpu", "TileQuadVB create failed — tiled GPU draw disabled");
        }
    }

    // 5. Create Index Buffer
    unsigned short indices[] = {
        0, 1, 2,
        0, 2, 3
    };

    bd.ByteWidth = sizeof(indices);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    InitData.pSysMem = indices;

    hr = device->CreateBuffer(&bd, &InitData, &m_IndexBuffer);
    if (FAILED(hr)) {
        return false;
    }

    // 6. Create Constant Buffers
    bd.ByteWidth = sizeof(CanvasBuffer);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&bd, nullptr, &m_ConstantBuffer);
    if (FAILED(hr)) {
        return false;
    }

    bd.ByteWidth = sizeof(LayerBuffer);
    hr = device->CreateBuffer(&bd, nullptr, &m_LayerConstantBuffer);
    if (FAILED(hr)) {
        return false;
    }

    // 7. Create Sampler State (Point filtering for crisp pixels)
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device->CreateSamplerState(&sampDesc, &m_SamplerState);
    if (FAILED(hr)) {
        return false;
    }

    // 8. Create Blend State for Layer Composition (Pre-multiplied / Standard Alpha)
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = device->CreateBlendState(&blendDesc, &m_LayerBlendState);
    if (FAILED(hr)) {
        return false;
    }

    // Alpha Rewrite OFF: RGB uses SRC_ALPHA as morph strength; destination A is never written.
    D3D11_BLEND_DESC blendPreserveA = blendDesc;
    blendPreserveA.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blendPreserveA.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    hr = device->CreateBlendState(&blendPreserveA, &m_LayerBlendStateAlphaPreserve);
    if (FAILED(hr)) {
        return false;
    }

    // First (bottom) layer: replace composite with full RGBA (RGB kept even when A=0).
    D3D11_BLEND_DESC blendReplace = {};
    blendReplace.RenderTarget[0].BlendEnable = TRUE;
    blendReplace.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendReplace.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    blendReplace.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendReplace.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendReplace.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendReplace.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendReplace.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&blendReplace, &m_LayerBlendStateReplace);
    if (FAILED(hr)) {
        return false;
    }
    // Fill write-mask blend states are created lazily in GetFillWriteBlend

    // Create rasterizer state with CullMode = D3D11_CULL_NONE
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    hr = device->CreateRasterizerState(&rd, &m_RasterizerState);
    if (FAILED(hr)) {
        return false;
    }
    // Scissor-enabled for dirty-rect proxy compose
    rd.ScissorEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, &m_RasterizerStateScissor);
    if (FAILED(hr)) {
        return false;
    }

    // Staging ring for tile uploads (created for default RGBA8; re-ensured on format change)
    m_TileStaging.Ensure(device, DXGI_FORMAT_R8G8B8A8_UNORM, TILE_SIZE, TILE_SIZE, 16);

    // Composite targets and default layer are created lazily on first use.

    return true;
}

void Canvas::Shutdown() {
    ReleaseCompositeResources();
    UpdateScheduler::Get().CancelAll();

    if (m_VertexBuffer) { m_VertexBuffer->Release(); m_VertexBuffer = nullptr; }
    if (m_TileQuadVB) { m_TileQuadVB->Release(); m_TileQuadVB = nullptr; }
    m_GpuTiles.Shutdown();
    m_GpuBlur.Shutdown();
    m_AsyncFilters.CancelAll();
    if (m_IndexBuffer) { m_IndexBuffer->Release(); m_IndexBuffer = nullptr; }
    if (m_ConstantBuffer) { m_ConstantBuffer->Release(); m_ConstantBuffer = nullptr; }
    if (m_LayerConstantBuffer) { m_LayerConstantBuffer->Release(); m_LayerConstantBuffer = nullptr; }

    if (m_VertexShader) { m_VertexShader->Release(); m_VertexShader = nullptr; }
    if (m_LayerVertexShader) { m_LayerVertexShader->Release(); m_LayerVertexShader = nullptr; }
    if (m_PixelShader) { m_PixelShader->Release(); m_PixelShader = nullptr; }
    if (m_LayerBlendPixelShader) { m_LayerBlendPixelShader->Release(); m_LayerBlendPixelShader = nullptr; }
    if (m_SelectionOutlinePixelShader) { m_SelectionOutlinePixelShader->Release(); m_SelectionOutlinePixelShader = nullptr; }
    if (m_InputLayout) { m_InputLayout->Release(); m_InputLayout = nullptr; }
    if (m_SamplerState) { m_SamplerState->Release(); m_SamplerState = nullptr; }
    if (m_LayerBlendState) { m_LayerBlendState->Release(); m_LayerBlendState = nullptr; }
    if (m_LayerBlendStateAlphaPreserve) { m_LayerBlendStateAlphaPreserve->Release(); m_LayerBlendStateAlphaPreserve = nullptr; }
    if (m_LayerBlendStateReplace) { m_LayerBlendStateReplace->Release(); m_LayerBlendStateReplace = nullptr; }
    for (int i = 0; i < 16; ++i) {
        if (m_FillWriteMaskBlend[i]) { m_FillWriteMaskBlend[i]->Release(); m_FillWriteMaskBlend[i] = nullptr; }
        if (m_FillWriteMaskReplace[i]) { m_FillWriteMaskReplace[i]->Release(); m_FillWriteMaskReplace[i] = nullptr; }
    }
    if (m_RasterizerState) { m_RasterizerState->Release(); m_RasterizerState = nullptr; }
    if (m_RasterizerStateScissor) { m_RasterizerStateScissor->Release(); m_RasterizerStateScissor = nullptr; }
    m_TileStaging.Shutdown();

    if (m_SelectionMaskTexture) { m_SelectionMaskTexture->Release(); m_SelectionMaskTexture = nullptr; }
    if (m_SelectionMaskSRV) { m_SelectionMaskSRV->Release(); m_SelectionMaskSRV = nullptr; }

    for (auto& layer : m_Layers) {
        ReleaseFillPatternGpu(layer);
        if (layer.texture) layer.texture->Release();
        if (layer.srv) layer.srv->Release();
        if (layer.maskTexture) layer.maskTexture->Release();
        if (layer.maskSRV) layer.maskSRV->Release();
        if (layer.thumbSRV) { layer.thumbSRV->Release(); layer.thumbSRV = nullptr; }
        if (layer.thumbTex) { layer.thumbTex->Release(); layer.thumbTex = nullptr; }
    }
    m_Layers.clear();
}

void Canvas::InvalidateCompositeRect(int x0, int y0, int x1, int y1) {
    m_CompositeDirty = true;
    if (m_CompositeDirtyFull) return;
    if (m_Width <= 0 || m_Height <= 0) {
        m_CompositeDirtyFull = true;
        return;
    }
    // Clamp + order
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(m_Width - 1, x1); y1 = std::min(m_Height - 1, y1);
    if (x1 < x0 || y1 < y0) return;

    if (!m_CompositeDirtyValid) {
        m_CompositeDirtyX0 = x0; m_CompositeDirtyY0 = y0;
        m_CompositeDirtyX1 = x1; m_CompositeDirtyY1 = y1;
        m_CompositeDirtyValid = true;
    } else {
        m_CompositeDirtyX0 = std::min(m_CompositeDirtyX0, x0);
        m_CompositeDirtyY0 = std::min(m_CompositeDirtyY0, y0);
        m_CompositeDirtyX1 = std::max(m_CompositeDirtyX1, x1);
        m_CompositeDirtyY1 = std::max(m_CompositeDirtyY1, y1);
    }
    // Huge rect → full recompose is cheaper than scissor thrash
    const int64_t area = (int64_t)(m_CompositeDirtyX1 - m_CompositeDirtyX0 + 1) *
                         (int64_t)(m_CompositeDirtyY1 - m_CompositeDirtyY0 + 1);
    const int64_t full = (int64_t)m_Width * (int64_t)m_Height;
    if (full > 0 && area * 100 > full * 70) {
        m_CompositeDirtyFull = true;
        m_CompositeDirtyValid = false;
    }
}

void Canvas::UploadLayerTile(ID3D11DeviceContext* context, ID3D11Texture2D* dest,
                             int tx, int ty, const uint8_t* data, int bytesPerPixel,
                             int docW, int docH) {
    if (!context || !data || bytesPerPixel < 1) return;
    const int x0 = tx * TILE_SIZE;
    const int y0 = ty * TILE_SIZE;
    const int w = std::min(TILE_SIZE, docW - x0);
    const int h = std::min(TILE_SIZE, docH - y0);
    if (w < 1 || h < 1) return;

    const int pitch = TILE_SIZE * bytesPerPixel;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);

    // Prefer sparse GPU tile store when dest is null (tiled surface path uses UploadLayerTileToLayer)
    if (dest) {
        if (device) {
            m_TileStaging.Ensure(device, GetLayerDxgiFormat(), TILE_SIZE, TILE_SIZE, 16);
        }
        if (m_TileStaging.IsReady() &&
            m_TileStaging.UploadRegion(context, dest, x0, y0, w, h, data, pitch)) {
            if (device) device->Release();
            return;
        }
        D3D11_BOX box = {};
        box.left = (UINT)x0; box.top = (UINT)y0; box.front = 0;
        box.right = (UINT)(x0 + w); box.bottom = (UINT)(y0 + h); box.back = 1;
        context->UpdateSubresource(dest, 0, &box, data, (UINT)pitch, 0);
    }
    if (device) device->Release();
}

// Returns false if GPU upload failed (caller must keep tile dirty for retry).
bool Canvas::UploadLayerContentTile(ID3D11Device* device, ID3D11DeviceContext* context,
                                    Layer& layer, int tx, int ty, const uint8_t* data, int bpp) {
    if (!context || !data || bpp < 1) return false;
    const int x0 = tx * TILE_SIZE;
    const int y0 = ty * TILE_SIZE;
    const int w = std::min(TILE_SIZE, m_Width - x0);
    const int h = std::min(TILE_SIZE, m_Height - y0);
    if (w < 1 || h < 1) return true;
    const int pitch = TILE_SIZE * bpp;
    if (layer.gpuSurfaceId && device) {
        return m_GpuTiles.UploadTile(device, context, layer.gpuSurfaceId, tx, ty, data, pitch, w, h);
    }
    if (layer.texture) {
        UploadLayerTile(context, layer.texture, tx, ty, data, bpp, m_Width, m_Height);
        return true;
    }
    // No GPU backing yet — not a hard failure; recreate path will upload later.
    return true;
}

void Canvas::EnsureGpuBlur(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (m_GpuBlurTried || !device || !ctx) return;
    m_GpuBlurTried = true;
    std::wstring path = GetExecutableDir() + L"\\shaders\\LayerFxBlur.hlsl";
    gpu_fx::InitGpuBlur(m_GpuBlur, device, ctx, path.c_str());
}

void Canvas::DrawTiledLayer(ID3D11DeviceContext* context, Layer& layer,
                            float opacityMul, bool useMask, bool isFirst,
                            ID3D11RenderTargetView* targetRtv) {
    if (!context || !layer.gpuSurfaceId || !m_TileQuadVB || m_Width < 1 || m_Height < 1) return;
    if (m_GpuTiles.TileCount(layer.gpuSurfaceId) == 0) return;

    ID3D11RenderTargetView* rtv = targetRtv ? targetRtv : m_CompositeRTV;
    if (!rtv) return;

    ID3D11BlendState* blend = isFirst ? m_LayerBlendStateReplace
        : ((!layer.alphaRewrite && m_LayerBlendStateAlphaPreserve)
            ? m_LayerBlendStateAlphaPreserve : m_LayerBlendState);
    context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    context->OMSetRenderTargets(1, &rtv, nullptr);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, &m_TileQuadVB, &stride, &offset);
    context->IASetIndexBuffer(m_IndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_LayerVertexShader, nullptr, 0);
    context->PSSetShader(m_LayerBlendPixelShader, nullptr, 0);

    const float invW = 1.f / (float)m_Width;
    const float invH = 1.f / (float)m_Height;

    m_GpuTiles.ForEachTile(layer.gpuSurfaceId, [&](int tx, int ty, const GpuTileStore::TileGpu& tg) {
        if (!tg.srv) return;
        // Document-space positions (where the tile sits on the canvas).
        const float u0 = (float)(tx * TILE_SIZE) * invW;
        const float v0 = (float)(ty * TILE_SIZE) * invH;
        const float u1 = (float)(tx * TILE_SIZE + tg.w) * invW;
        const float v1 = (float)(ty * TILE_SIZE + tg.h) * invH;
        // Texture UVs: atlas sub-rect or full 0..1 for standalone tiles.
        const float tu0 = tg.U0(), tv0 = tg.V0(), tu1 = tg.U1(), tv1 = tg.V1();

        D3D11_MAPPED_SUBRESOURCE vm = {};
        if (FAILED(context->Map(m_TileQuadVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &vm))) return;
        Vertex* v = (Vertex*)vm.pData;
        v[0] = { DirectX::XMFLOAT2(u0, v0), DirectX::XMFLOAT2(tu0, tv0) };
        v[1] = { DirectX::XMFLOAT2(u1, v0), DirectX::XMFLOAT2(tu1, tv0) };
        v[2] = { DirectX::XMFLOAT2(u1, v1), DirectX::XMFLOAT2(tu1, tv1) };
        v[3] = { DirectX::XMFLOAT2(u0, v1), DirectX::XMFLOAT2(tu0, tv1) };
        context->Unmap(m_TileQuadVB, 0);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            LayerBuffer* lb = (LayerBuffer*)mapped.pData;
            float hasMaskVal = (useMask && layer.hasMask && layer.maskSRV) ? 1.f : 0.f;
            lb->layerParams = DirectX::XMFLOAT4(opacityMul, hasMaskVal, 0.f, 0.f);
            lb->transformParams = DirectX::XMFLOAT4(1.f, 1.f, 0.f, 0.f);
            float flags = (layer.alphaRewrite ? 1.f : 0.f) + (isFirst ? 2.f : 0.f);
            lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)layer.blendMode, flags);
            lb->floatRect = DirectX::XMFLOAT4(0.f, 0.f, 0.f, 0.f);
            lb->fillColor = DirectX::XMFLOAT4(1.f, 1.f, 1.f, 1.f);
            context->Unmap(m_LayerConstantBuffer, 0);
        }
        context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
        context->PSSetShaderResources(0, 1, const_cast<ID3D11ShaderResourceView**>(&tg.srv));
        if (useMask && layer.maskSRV)
            context->PSSetShaderResources(1, 1, &layer.maskSRV);
        else {
            ID3D11ShaderResourceView* n = nullptr;
            context->PSSetShaderResources(1, 1, &n);
        }
        if (layer.blendMode != BlendMode::Normal && m_CompositeHistoryTexture && m_CompositeHistorySRV) {
            context->CopyResource(m_CompositeHistoryTexture, m_CompositeTexture);
            context->PSSetShaderResources(2, 1, &m_CompositeHistorySRV);
        } else {
            ID3D11ShaderResourceView* n = nullptr;
            context->PSSetShaderResources(2, 1, &n);
        }
        context->DrawIndexed(6, 0, 0);
    });

    // Restore full-screen VB for subsequent draws
    context->IASetVertexBuffers(0, 1, &m_VertexBuffer, &stride, &offset);
}

void Canvas::ScheduleDeferredPresentation(int layerIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
    UpdateScheduler::Key key{ UpdateScheduler::Kind::Presentation, layerIdx, 0 };
    if (UpdateScheduler::Get().IsPending(key)) return;
    UpdateScheduler::Get().Submit(
        key,
        UpdateScheduler::Priority::High,
        /*work=*/{},
        [this, layerIdx]() {
            if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
            Layer& layer = m_Layers[layerIdx];
            if (!LayerStylesPreviewActive(layer) && !layer.HasEnabledStyles())
                return;
            RebuildLayerPresentation(layer, /*fullQuality=*/false);
            layer.gpuDisplayKind = 0xFF;
            layer.needsUpload = true;
            if (layer.presentationCache)
                layer.presentationCache->MarkAllDirty();
            MarkCompositeDirty();
            layer.thumbDirty = true;
        });
}

void Canvas::ScheduleThumbJob(int layerIdx, int size) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size() || size < 1) return;
    if (m_IsStrokeActive || m_IsMovingPixels) return;
    Layer& layer = m_Layers[layerIdx];
    if (!layer.thumbDirty || layer.isGroup || !layer.tileCache) return;

    UpdateScheduler::Key key{ UpdateScheduler::Kind::Thumb, layerIdx, size };
    if (UpdateScheduler::Get().IsPending(key)) return;

    // Sample only size×size on the main thread (never full-document export — 8K would
    // allocate 256 MiB per thumb). Apply just uploads the small GPU texture.
    const TileCache* cache = LayerExportCache(layer);
    if (!cache) return;
    const int docW = m_Width, docH = m_Height;
    auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)size * size * 4, 0);
    for (int y = 0; y < size; ++y) {
        const int sy = (docH > 0) ? (int)((int64_t)y * docH / size) : 0;
        for (int x = 0; x < size; ++x) {
            const int sx = (docW > 0) ? (int)((int64_t)x * docW / size) : 0;
            float f[4] = {};
            cache->GetPixelF(sx, sy, f);
            size_t di = ((size_t)y * size + x) * 4;
            (*rgba)[di + 0] = HalfFloat::FloatToU8(f[0]);
            (*rgba)[di + 1] = HalfFloat::FloatToU8(f[1]);
            (*rgba)[di + 2] = HalfFloat::FloatToU8(f[2]);
            (*rgba)[di + 3] = 255;
        }
    }

    UpdateScheduler::Get().Submit(
        key,
        UpdateScheduler::Priority::Low,
        /*work=*/{},
        [this, layerIdx, size, rgba]() {
            if (layerIdx < 0 || layerIdx >= (int)m_Layers.size() || !rgba || rgba->empty())
                return;
            Layer& layer = m_Layers[layerIdx];
            if (!layer.thumbDirty && layer.thumbSRV && layer.thumbSize == size) return;

            ID3D11Device* device = nullptr;
            if (layer.thumbTex)
                layer.thumbTex->GetDevice(&device);
            else if (layer.texture)
                layer.texture->GetDevice(&device);
            if (!device) return;

            if (layer.thumbSRV) { layer.thumbSRV->Release(); layer.thumbSRV = nullptr; }
            if (layer.thumbTex) { layer.thumbTex->Release(); layer.thumbTex = nullptr; }

            D3D11_TEXTURE2D_DESC td = {};
            td.Width = (UINT)size;
            td.Height = (UINT)size;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init = {};
            init.pSysMem = rgba->data();
            init.SysMemPitch = (UINT)(size * 4);
            if (SUCCEEDED(device->CreateTexture2D(&td, &init, &layer.thumbTex)) && layer.thumbTex) {
                device->CreateShaderResourceView(layer.thumbTex, nullptr, &layer.thumbSRV);
                layer.thumbSize = size;
                layer.thumbDirty = false;
            }
            device->Release();
        });
}

void Canvas::SubmitAsyncFilterBake(int layerIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[layerIdx];
    if (!layer.tileCache || !LayerFilterListHasEnabled(layer.filters)) return;

    async_fx::FilterJobInput job;
    job.layerIdx = layerIdx;
    job.docW = m_Width;
    job.docH = m_Height;
    job.format = m_CanvasFormat;
    job.filters = layer.filters;
    job.halo = layer_fx::MaxFilterSupportRadius(layer.filters);

    // Seeds: stroke deltas or all dirty / all present tiles
    std::unordered_set<uint64_t> seedSet;
    auto addSeed = [&](int tx, int ty) {
        seedSet.insert(((uint64_t)(uint32_t)tx) | ((uint64_t)(uint32_t)ty << 32));
    };
    if (!m_ActiveStrokeDeltas.empty() && layerIdx == m_ActiveLayerIdx) {
        for (const auto& p : m_ActiveStrokeDeltas)
            addSeed(p.second.tileX, p.second.tileY);
    } else {
        for (int ty = 0; ty < layer.tileCache->GetTilesY(); ++ty)
            for (int tx = 0; tx < layer.tileCache->GetTilesX(); ++tx)
                if (layer.tileCache->HasTile(tx, ty) &&
                    (layer.filtersDirty || layer.tileCache->IsDirty(tx, ty)))
                    addSeed(tx, ty);
    }
    if (seedSet.empty()) {
        for (int ty = 0; ty < layer.tileCache->GetTilesY(); ++ty)
            for (int tx = 0; tx < layer.tileCache->GetTilesX(); ++tx)
                if (layer.tileCache->HasTile(tx, ty))
                    addSeed(tx, ty);
    }
    for (uint64_t k : seedSet)
        job.seeds.emplace_back((int)(k & 0xffffffffu), (int)(k >> 32));

    // Neighbor tiles for halo
    const int expand = (job.halo + TILE_SIZE - 1) / TILE_SIZE;
    std::unordered_set<uint64_t> need;
    for (auto [sx, sy] : job.seeds) {
        for (int dy = -expand; dy <= expand; ++dy)
            for (int dx = -expand; dx <= expand; ++dx) {
                int tx = sx + dx, ty = sy + dy;
                if (tx < 0 || ty < 0 || tx >= layer.tileCache->GetTilesX() ||
                    ty >= layer.tileCache->GetTilesY())
                    continue;
                if (!layer.tileCache->HasTile(tx, ty)) continue;
                need.insert(((uint64_t)(uint32_t)tx) | ((uint64_t)(uint32_t)ty << 32));
            }
    }

    const int bpp = layer.tileCache->GetBytesPerPixel();
    job.contentTiles.reserve(need.size());
    for (uint64_t k : need) {
        int tx = (int)(k & 0xffffffffu), ty = (int)(k >> 32);
        const uint8_t* raw = layer.tileCache->GetTileData(tx, ty);
        if (!raw) continue;
        async_fx::FilterJobInput::SnapTile snap;
        snap.tx = tx; snap.ty = ty;
        snap.validW = std::min(TILE_SIZE, m_Width - tx * TILE_SIZE);
        snap.validH = std::min(TILE_SIZE, m_Height - ty * TILE_SIZE);
        snap.bytes.assign(raw, raw + (size_t)TILE_SIZE * TILE_SIZE * bpp);
        job.contentTiles.push_back(std::move(snap));
    }

    try {
        m_AsyncFilters.Submit(std::move(job));
    } catch (...) {
        // Pool not ready — sync fallback
        RefreshFilteredCache(layer, /*onlyDirtyTiles=*/true);
    }
}

void Canvas::PollAsyncFilterResults() {
    auto results = m_AsyncFilters.Poll();
    for (auto& r : results) {
        if (r.layerIdx < 0 || r.layerIdx >= (int)m_Layers.size()) continue;
        Layer& layer = m_Layers[r.layerIdx];
        if (!layer.filteredCache) {
            layer.filteredCache = std::make_unique<TileCache>();
            layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
        }
        for (auto& t : r.tiles) {
            if (t.rgba.empty() || t.w < 1 || t.h < 1) continue;
            layer.filteredCache->ImportRGBA32F(t.rgba.data(), t.w, t.h, t.x0, t.y0);
        }
        layer.filtersDirty = false;
        layer.presentationDirty = true;
        layer.needsUpload = true;
        MarkCompositeDirty();
    }
}

void Canvas::CreateCompositeResources(ID3D11Device* device) {
    ReleaseCompositeResources();

    ComputeCompositePreviewSize(m_Width, m_Height, m_CompositeWidth, m_CompositeHeight);
    Logger::Get().InfoTag("gpu",
        "CreateCompositeResources proxy=" + std::to_string(m_CompositeWidth) + "x" +
        std::to_string(m_CompositeHeight) + " (canvas " + std::to_string(m_Width) + "x" +
        std::to_string(m_Height) + ")");

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_CompositeWidth;
    desc.Height = m_CompositeHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = GetLayerDxgiFormat();
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_CompositeTexture);
    if (SUCCEEDED(hr)) {
        device->CreateRenderTargetView(m_CompositeTexture, nullptr, &m_CompositeRTV);
        device->CreateShaderResourceView(m_CompositeTexture, nullptr, &m_CompositeSRV);
    } else {
        std::ostringstream oss;
        oss << "CreateCompositeResources CreateTexture2D failed hr=0x" << std::hex << (unsigned)hr
            << " size=" << std::dec << m_CompositeWidth << "x" << m_CompositeHeight;
        Logger::Get().ErrorTag("gpu", oss.str());
    }

    // Ping/history texture: blend modes sample previous composite while writing new one.
    // Reading+writing the same resource is illegal in D3D11 and caused black Overlay strokes.
    if (m_CompositeHistoryTexture) { m_CompositeHistoryTexture->Release(); m_CompositeHistoryTexture = nullptr; }
    if (m_CompositeHistorySRV) { m_CompositeHistorySRV->Release(); m_CompositeHistorySRV = nullptr; }
    D3D11_TEXTURE2D_DESC histDesc = desc;
    histDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = device->CreateTexture2D(&histDesc, nullptr, &m_CompositeHistoryTexture);
    if (SUCCEEDED(hr)) {
        device->CreateShaderResourceView(m_CompositeHistoryTexture, nullptr, &m_CompositeHistorySRV);
    } else {
        Logger::Get().ErrorTag("gpu", "CreateCompositeResources: history texture failed");
    }

    // Do NOT allocate a full-document selection mask here (16K = 256 MiB).
    // Selection tools allocate lazily when first used.
    m_SelectionMask.clear();
    m_HasSelection = false;
    m_SelectionMaskNeedsUpload = false;
    m_CompositeDirty = true;
    m_CompositeDirtyFull = true;
    m_CompositeDirtyValid = false;
    if (device)
        m_TileStaging.Ensure(device, GetLayerDxgiFormat(), TILE_SIZE, TILE_SIZE, 16);
    CreateGroupCompositeResources(device);
    MemoryStats::LogSnapshot("after_CreateCompositeResources");
}

void Canvas::ReleaseCompositeResources() {
    if (m_CompositeTexture) { m_CompositeTexture->Release(); m_CompositeTexture = nullptr; }
    if (m_CompositeRTV) { m_CompositeRTV->Release(); m_CompositeRTV = nullptr; }
    if (m_CompositeSRV) { m_CompositeSRV->Release(); m_CompositeSRV = nullptr; }
    if (m_CompositeHistoryTexture) { m_CompositeHistoryTexture->Release(); m_CompositeHistoryTexture = nullptr; }
    if (m_CompositeHistorySRV) { m_CompositeHistorySRV->Release(); m_CompositeHistorySRV = nullptr; }
    m_CompositeWidth = 0;
    m_CompositeHeight = 0;
    if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
    if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }
    if (m_FloatingMaskTexture) { m_FloatingMaskTexture->Release(); m_FloatingMaskTexture = nullptr; }
    if (m_FloatingMaskSRV) { m_FloatingMaskSRV->Release(); m_FloatingMaskSRV = nullptr; }
    ClearViewMapUnderlay();
    ReleaseChannelPreviewResources();
}

void Canvas::CreateLayerMask(ID3D11Device* device, int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];

    const size_t maskBytes = MemoryStats::EstimateImageBytes(m_Width, m_Height, 1);
    if (maskBytes > 64ull * 1024ull * 1024ull) {
        Logger::Get().WarnTag("mem",
            "CreateLayerMask: full mask is " + MemoryStats::FormatBytes(maskBytes) +
            " — large-document masks are still flat; consider tiled masks later.");
    }
    // Soft limit: do not refuse mask creation on budget alone (EMERGENCY_SAFE_PLAN).
    if (MemoryStats::ExceedsRamBudget(maskBytes, 0.25)) {
        MemoryStats::LogSoftBudget("CreateLayerMask", maskBytes, 0.25);
    }
    if (MemoryStats::ExceedsHardRamCeiling(maskBytes)) {
        Logger::Get().ErrorTag("mem",
            "CreateLayerMask hard ceiling " + MemoryStats::FormatBytes(maskBytes) + " — refuse");
        return;
    }

    bool oldHas = layer.hasMask;
    std::vector<MaskTileSnapshot> oldTiles;
    if (oldHas && layer.maskTiles && layer.maskTiles->Valid())
        oldTiles = layer.maskTiles->SnapshotAll();
    // Photoshop default: white mask = fully reveal (sparse tiles = empty = default 255)
    try {
        layer.maskTiles = std::make_unique<MaskTiles>();
        layer.maskTiles->Init(m_Width, m_Height, 255);
        layer.mask.assign((size_t)m_Width * m_Height, 255);
    } catch (const std::bad_alloc&) {
        Logger::Get().ErrorTag("mem",
            "CreateLayerMask bad_alloc for " + MemoryStats::FormatBytes(maskBytes) +
            " — clean refuse (no crash)");
        layer.maskTiles.reset();
        layer.mask.clear();
        layer.hasMask = false;
        return;
    }
    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    layer.maskDirtyX0 = 0; layer.maskDirtyY0 = 0;
    layer.maskDirtyX1 = m_Width - 1; layer.maskDirtyY1 = m_Height - 1;
    m_PaintTarget = PaintTarget::LayerMask;
    if (m_ActiveLayerIdx != index) m_ActiveLayerIdx = index;

    if (device) {
        UpdateLayerMaskTexture(device, index);
    }
    // White mask = empty tile set (cheap undo)
    m_UndoRedoManager.PushCommand(std::make_shared<LayerMaskCommand>(
        "Add Mask", index, oldHas, std::move(oldTiles),
        true, std::vector<MaskTileSnapshot>{}, m_Width, m_Height));
    m_IsDocumentModified = true;
    NotifyLayerVisualsChanged(index, /*contentPixelsChanged=*/false, /*markFiltersDirty=*/false);
    Logger::Get().InfoTag("io", "Created layer mask on '" + layer.name + "' (paint target = mask)");
}

void Canvas::CreateLayerMaskFromSelection(ID3D11Device* device, int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];

    bool oldHas = layer.hasMask;
    std::vector<MaskTileSnapshot> oldTiles;
    if (oldHas && layer.maskTiles && layer.maskTiles->Valid())
        oldTiles = layer.maskTiles->SnapshotAll();

    layer.mask.assign((size_t)m_Width * m_Height, 0);
    if (m_HasSelection && m_SelectionMask.size() == (size_t)m_Width * m_Height) {
        std::copy(m_SelectionMask.begin(), m_SelectionMask.end(), layer.mask.begin());
    } else {
        layer.mask.assign((size_t)m_Width * m_Height, 255);
    }
    layer.maskTiles = std::make_unique<MaskTiles>();
    layer.maskTiles->ImportFlat(layer.mask, m_Width, m_Height);
    auto newTiles = layer.maskTiles->SnapshotAll();

    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    layer.maskDirtyX0 = 0; layer.maskDirtyY0 = 0;
    layer.maskDirtyX1 = m_Width - 1; layer.maskDirtyY1 = m_Height - 1;
    m_PaintTarget = PaintTarget::LayerMask;
    if (m_ActiveLayerIdx != index) m_ActiveLayerIdx = index;

    if (device) {
        UpdateLayerMaskTexture(device, index);
    }
    m_UndoRedoManager.PushCommand(std::make_shared<LayerMaskCommand>(
        "Add Mask from Selection", index, oldHas, std::move(oldTiles),
        true, std::move(newTiles), m_Width, m_Height));
    m_IsDocumentModified = true;
    NotifyLayerVisualsChanged(index, /*contentPixelsChanged=*/false, /*markFiltersDirty=*/false);

    Logger::Get().Info("Created layer mask from selection for layer: " + layer.name);
}

// Note: CreateLayerMask / CreateLayerMaskFromSelection work on groups too (group mask).

void Canvas::DeleteLayerMask(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    if (!layer.hasMask) return;
    std::vector<MaskTileSnapshot> oldTiles;
    if (layer.maskTiles && layer.maskTiles->Valid())
        oldTiles = layer.maskTiles->SnapshotAll();
    else if (!layer.mask.empty()) {
        // migrate flat → tiles for compact snap
        MaskTiles tmp;
        tmp.Init(m_Width, m_Height, 255);
        tmp.ImportFlat(layer.mask, m_Width, m_Height);
        oldTiles = tmp.SnapshotAll();
    }
    if (layer.maskTexture) { layer.maskTexture->Release(); layer.maskTexture = nullptr; }
    if (layer.maskSRV) { layer.maskSRV->Release(); layer.maskSRV = nullptr; }
    layer.mask.clear();
    layer.maskTiles.reset();
    layer.hasMask = false;
    layer.maskNeedsUpload = false;
    if (m_PaintTarget == PaintTarget::LayerMask && m_ActiveLayerIdx == index) {
        m_PaintTarget = PaintTarget::LayerContent;
    }
    m_CompositeDirty = true;
    m_UndoRedoManager.PushCommand(std::make_shared<LayerMaskCommand>(
        "Delete Mask", index, true, std::move(oldTiles),
        false, std::vector<MaskTileSnapshot>{}, m_Width, m_Height));
    m_IsDocumentModified = true;
    Logger::Get().Info("Deleted layer mask for layer: " + layer.name);
}

bool Canvas::ActiveLayerHasMask() const {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return false;
    return m_Layers[m_ActiveLayerIdx].hasMask && !m_Layers[m_ActiveLayerIdx].mask.empty();
}

void Canvas::SetPaintTarget(PaintTarget t) {
    if (t == PaintTarget::LayerMask) {
        if (!ActiveLayerHasMask()) {
            Logger::Get().WarnTag("io", "SetPaintTarget(Mask): active layer has no mask");
            return;
        }
    }
    m_PaintTarget = t;
    Logger::Get().InfoTag("io",
        t == PaintTarget::LayerMask ? "Paint target = LayerMask" : "Paint target = LayerContent");
}

void Canvas::EnsureActiveLayerMaskAllocated() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    const size_t need = (size_t)m_Width * (size_t)m_Height;
    if (!layer.hasMask) {
        layer.hasMask = true;
        layer.maskTiles = std::make_unique<MaskTiles>();
        layer.maskTiles->Init(m_Width, m_Height, 255); // sparse white
        layer.mask.assign(need, 255);
    } else {
        if (!layer.maskTiles) {
            layer.maskTiles = std::make_unique<MaskTiles>();
            layer.maskTiles->Init(m_Width, m_Height, 255);
            if (layer.mask.size() == need)
                layer.maskTiles->ImportFlat(layer.mask, m_Width, m_Height);
        } else if (!layer.maskTiles->Valid() ||
                   layer.maskTiles->Width() != m_Width || layer.maskTiles->Height() != m_Height) {
            layer.maskTiles->Init(m_Width, m_Height, 255);
            if (layer.mask.size() == need)
                layer.maskTiles->ImportFlat(layer.mask, m_Width, m_Height);
        }
        if (layer.mask.size() != need)
            layer.maskTiles->Flatten(layer.mask);
    }
}

void Canvas::PaintMaskStamp(float cx, float cy, const BrushSettings& brush) {
    EnsureActiveLayerMaskAllocated();
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.hasMask || !layer.maskTiles) return;

    const float radius = std::max(1.f, brush.radius);
    const float hardness = std::clamp(brush.hardness, 0.f, 1.f);
    const float opacity = std::clamp(brush.opacity, 0.f, 1.f);
    float paintVal = 1.f;
    if (brush.erase) {
        paintVal = 0.f;
    } else {
        paintVal = std::clamp((brush.color[0] + brush.color[1] + brush.color[2]) / 3.f, 0.f, 1.f);
    }

    const int rCeil = (int)std::ceil(radius);
    const int x0 = std::max(0, (int)std::floor(cx) - rCeil);
    const int y0 = std::max(0, (int)std::floor(cy) - rCeil);
    const int x1 = std::min(m_Width - 1, (int)std::ceil(cx) + rCeil);
    const int y1 = std::min(m_Height - 1, (int)std::ceil(cy) + rCeil);
    if (x0 > x1 || y0 > y1) return;

    const float softStart = radius * hardness;
    const float r2 = radius * radius;

    // Selection is a temp mask: paint/erase layer mask only inside selection.
    const bool clipSel = m_HasSelection &&
        m_SelectionMask.size() == (size_t)m_Width * (size_t)m_Height;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float dist2 = dx * dx + dy * dy;
            if (dist2 > r2) continue;
            float dist = std::sqrt(dist2);
            float w = 1.f;
            if (dist > softStart && radius > softStart) {
                w = 1.f - (dist - softStart) / (radius - softStart);
                w = w * w * (3.f - 2.f * w);
            }
            w *= opacity;
            if (clipSel) {
                const float sel = SelU82F(m_SelectionMask[(size_t)y * (size_t)m_Width + (size_t)x]);
                if (sel <= 0.f) continue;
                w *= sel;
            }
            if (w <= 0.f) continue;
            float cur = layer.maskTiles->Get(x, y) / 255.f;
            float out = cur * (1.f - w) + paintVal * w;
            uint8_t v = (uint8_t)(std::clamp(out, 0.f, 1.f) * 255.f + 0.5f);
            layer.maskTiles->Set(x, y, v);
            // Keep flat cache in sync for pack/styles (cheap per-pixel)
            if (layer.mask.size() == (size_t)m_Width * (size_t)m_Height)
                layer.mask[(size_t)y * m_Width + x] = v;
        }
    }

    layer.maskTiles->GetDirty(layer.maskDirtyX0, layer.maskDirtyY0,
                              layer.maskDirtyX1, layer.maskDirtyY1);
    layer.maskNeedsUpload = true;
    m_CompositeDirty = true;
}

void Canvas::ApplyLayerMask(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    if (!layer.hasMask) return;
    
    int oldActive = m_ActiveLayerIdx;
    m_ActiveLayerIdx = index;
    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;
    for (int ty = 0; ty < numTilesY; ++ty) {
        for (int tx = 0; tx < numTilesX; ++tx) {
            BackupTile(tx, ty);
        }
    }
    
    if (layer.mask.size() != (size_t)m_Width * m_Height) {
        Logger::Get().Error("ApplyLayerMask: Mask size mismatch! Reallocating mask.");
        layer.mask.resize((size_t)m_Width * m_Height, 255);
    }

    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    // Tile walk — avoid GetPixelF/SetPixelF per document pixel
    const int ntx = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    const int nty = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    for (int ty = 0; ty < nty; ++ty) {
        for (int tx = 0; tx < ntx; ++tx) {
            if (!layer.tileCache->HasTile(tx, ty)) continue;
            uint8_t* raw = layer.tileCache->LockTile(tx, ty);
            if (!raw) continue;
            const int x0 = tx * TILE_SIZE, y0 = ty * TILE_SIZE;
            const int x1 = std::min(x0 + TILE_SIZE, m_Width);
            const int y1 = std::min(y0 + TILE_SIZE, m_Height);
            const auto fmt = layer.tileCache->GetFormat();
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const int lx = x - x0, ly = y - y0;
                    float aMul = SelU82F(layer.mask[(size_t)y * m_Width + x]);
                    if (fmt == CanvasPixelFormat::RGBA8) {
                        size_t off = ((size_t)ly * TILE_SIZE + lx) * 4;
                        raw[off + 3] = (uint8_t)std::clamp((int)std::lround(raw[off + 3] * aMul), 0, 255);
                    } else if (fmt == CanvasPixelFormat::RGBA16F) {
                        float px[4];
                        HalfFloat::LoadRGBA16F(raw + ((size_t)ly * TILE_SIZE + lx) * 8, px);
                        px[3] *= aMul;
                        HalfFloat::StoreRGBA16F(raw + ((size_t)ly * TILE_SIZE + lx) * 8, px);
                    } else {
                        float* fp = reinterpret_cast<float*>(raw);
                        size_t off = ((size_t)ly * TILE_SIZE + lx) * 4;
                        fp[off + 3] *= aMul;
                    }
                }
            }
        }
    }
    layer.tileCache->MarkAllDirty();

    layer.needsUpload = true;
    layer.filtersDirty = true;
    DeleteLayerMask(index);
    
    if (!m_ActiveStrokeDeltas.empty()) {
        auto deltas = SealActiveStrokeDeltas(m_ActiveStrokeDeltas, layer.tileCache.get());
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Apply Mask", index, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }
    m_ActiveLayerIdx = oldActive;
    
    Logger::Get().Info("Applied layer mask to layer alpha: " + layer.name);
}

void Canvas::UpdateLayerMaskTexture(ID3D11Device* device, int index) {
    if (!device || index < 0 || index >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    if (!layer.hasMask) return;

    const size_t need = (size_t)m_Width * (size_t)m_Height;
    if (need == 0) return;
    if (layer.mask.size() != need) {
        // Keep CPU buffer in sync (resize / load edge cases).
        layer.mask.assign(need, 255);
        layer.maskDirtyX0 = 0;
        layer.maskDirtyY0 = 0;
        layer.maskDirtyX1 = m_Width - 1;
        layer.maskDirtyY1 = m_Height - 1;
    }

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);

    auto releaseMaskGpu = [&]() {
        // Unbind before Release — ImGui / previous compose may still reference slot t1.
        if (ctx) {
            ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
            ctx->PSSetShaderResources(0, 2, nulls);
        }
        if (layer.maskSRV) { layer.maskSRV->Release(); layer.maskSRV = nullptr; }
        if (layer.maskTexture) { layer.maskTexture->Release(); layer.maskTexture = nullptr; }
    };

    bool needCreate = (layer.maskTexture == nullptr);
    if (layer.maskTexture) {
        D3D11_TEXTURE2D_DESC existing = {};
        layer.maskTexture->GetDesc(&existing);
        if (existing.Width != (UINT)m_Width || existing.Height != (UINT)m_Height ||
            existing.Format != DXGI_FORMAT_R8_UNORM) {
            releaseMaskGpu();
            needCreate = true;
        }
    }

    if (needCreate) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_Width;
        desc.Height = m_Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = layer.mask.data();
        initData.SysMemPitch = (UINT)m_Width * sizeof(uint8_t);

        HRESULT hr = device->CreateTexture2D(&desc, &initData, &layer.maskTexture);
        if (SUCCEEDED(hr) && layer.maskTexture) {
            device->CreateShaderResourceView(layer.maskTexture, nullptr, &layer.maskSRV);
        } else {
            Logger::Get().ErrorTag("gpu",
                "UpdateLayerMaskTexture: CreateTexture2D failed size=" +
                std::to_string(m_Width) + "x" + std::to_string(m_Height));
        }
    } else if (ctx && !layer.mask.empty()) {
        // In-place upload: keep maskSRV pointer stable (ImGui ImageButton holds it).
        // Old code destroyed+recreated every dab → use-after-free crash on long strokes.
        // Unbind first: texture may still be bound from last compose / ImGui draw.
        {
            ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
            ctx->PSSetShaderResources(0, 2, nulls);
        }
        const bool hasDirty = layer.maskDirtyX1 >= layer.maskDirtyX0 &&
                              layer.maskDirtyY1 >= layer.maskDirtyY0;
        if (hasDirty) {
            const int bx0 = std::clamp(layer.maskDirtyX0, 0, m_Width - 1);
            const int by0 = std::clamp(layer.maskDirtyY0, 0, m_Height - 1);
            const int bx1 = std::clamp(layer.maskDirtyX1, 0, m_Width - 1);
            const int by1 = std::clamp(layer.maskDirtyY1, 0, m_Height - 1);
            D3D11_BOX box = {};
            box.left   = (UINT)bx0;
            box.top    = (UINT)by0;
            box.front  = 0;
            box.right  = (UINT)(bx1 + 1);
            box.bottom = (UINT)(by1 + 1);
            box.back   = 1;
            const uint8_t* src = layer.mask.data() + (size_t)by0 * (size_t)m_Width + (size_t)bx0;
            ctx->UpdateSubresource(layer.maskTexture, 0, &box, src,
                                   (UINT)m_Width * sizeof(uint8_t), 0);
        } else {
            ctx->UpdateSubresource(layer.maskTexture, 0, nullptr, layer.mask.data(),
                                   (UINT)m_Width * sizeof(uint8_t), 0);
        }
    }

    layer.maskDirtyX0 = 0;
    layer.maskDirtyY0 = 0;
    layer.maskDirtyX1 = -1;
    layer.maskDirtyY1 = -1;
    layer.maskNeedsUpload = false;
    m_CompositeDirty = true;
    if (ctx) ctx->Release();
}

DXGI_FORMAT Canvas::GetLayerDxgiFormat() const {
    switch (m_CanvasFormat) {
        case CanvasPixelFormat::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case CanvasPixelFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case CanvasPixelFormat::RGBA8:
        default:                         return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

CanvasPixelFormat Canvas::FormatForBitDepth(DocumentBitDepth d) {
    switch (d) {
        case DocumentBitDepth::F32: return CanvasPixelFormat::RGBA32F;
        case DocumentBitDepth::F16: return CanvasPixelFormat::RGBA16F;
        case DocumentBitDepth::U8:
        default:                    return CanvasPixelFormat::RGBA8;
    }
}

void Canvas::RecreateLayerTexture(ID3D11Device* device, Layer& layer) {
    if (!device) return;
    if (layer.texture) { layer.texture->Release(); layer.texture = nullptr; }
    if (layer.srv)     { layer.srv->Release();     layer.srv     = nullptr; }

    // Large docs without layer styles: sparse GPU tiles (VRAM-friendly).
    // Styles require full presentation bake → classic full texture.
    if (UseTiledGpuForLayer(layer)) {
        if (layer.gpuSurfaceId)
            m_GpuTiles.DestroySurface(layer.gpuSurfaceId);
        // Atlas packs many 256² tiles into 2048² pages (fewer textures / less VRAM churn).
        layer.gpuSurfaceId = m_GpuTiles.CreateSurface(device, m_Width, m_Height, GetLayerDxgiFormat(),
                                                       /*useAtlas=*/true);
        layer.gpuDisplayKind = 0xFF; // force re-upload
        if (layer.tileCache) {
            layer.tileCache->MarkAllDirty();
            layer.needsUpload = true;
        } else {
            layer.needsUpload = false;
        }
        Logger::Get().InfoTag("gpu",
            "RecreateLayerTexture tiled surface id=" + std::to_string(layer.gpuSurfaceId) +
            " " + std::to_string(m_Width) + "x" + std::to_string(m_Height));
        return;
    }

    if (layer.gpuSurfaceId) {
        m_GpuTiles.DestroySurface(layer.gpuSurfaceId);
        layer.gpuSurfaceId = 0;
    }
    layer.gpuDisplayKind = 0xFF;

    constexpr UINT kMaxTexDim = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if ((UINT)m_Width > kMaxTexDim || (UINT)m_Height > kMaxTexDim) {
        Logger::Get().ErrorTag("gpu",
            "RecreateLayerTexture: canvas " + std::to_string(m_Width) + "x" +
            std::to_string(m_Height) + " exceeds D3D11 max texture dim " +
            std::to_string(kMaxTexDim));
        return;
    }

    const size_t estGpu = MemoryStats::EstimateImageBytes(m_Width, m_Height, BytesPerPixel(m_CanvasFormat));
    Logger::Get().InfoTag("gpu",
        "RecreateLayerTexture " + std::to_string(m_Width) + "x" + std::to_string(m_Height) +
        " estVRAM~" + MemoryStats::FormatBytes(estGpu));

    if (!MemoryStats::TryReserveVram(estGpu, "RecreateLayerTexture.full")) {
        // Soft VRAM budget: keep CPU tiles. Clear needsUpload so compose does not
        // re-call Create every frame (was thrashing + log flood under --vram-budget-mb).
        layer.needsUpload = false;
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = m_Width;
    desc.Height           = m_Height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = GetLayerDxgiFormat();
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &layer.texture);
    if (FAILED(hr)) {
        MemoryStats::ReleaseVram(estGpu);
        std::ostringstream oss;
        oss << "RecreateLayerTexture: CreateTexture2D failed hr=0x" << std::hex << (unsigned)hr
            << " size=" << std::dec << m_Width << "x" << m_Height
            << " format=" << (unsigned)desc.Format
            << " estVRAM~" << MemoryStats::FormatBytes(estGpu);
        Logger::Get().ErrorTag("gpu", oss.str());
        MemoryStats::LogSnapshot("after_CreateTexture2D_fail");
        return;
    }
    device->CreateShaderResourceView(layer.texture, nullptr, &layer.srv);
    Logger::Get().InfoTag("gpu", "RecreateLayerTexture OK");

    if (layer.tileCache) {
        layer.tileCache->MarkAllDirty();
        layer.needsUpload = true;
    }
    layer.needsUpload = false;
    MemoryStats::LogSnapshot("after_RecreateLayerTexture");
}

LayerStackCommand::Snap Canvas::CaptureLayerSnap(const Layer& L, int index, int docW, int docH,
                                                 CanvasPixelFormat fmt) {
    LayerStackCommand::Snap s;
    s.index = index;
    s.name = L.name;
    s.type = (uint8_t)L.type;
    s.isGroup = L.isGroup;
    s.visible = L.visible;
    s.opacity = L.opacity;
    s.blendMode = L.blendMode;
    s.alphaRewrite = L.alphaRewrite;
    s.parentGroupId = L.parentGroupId;
    s.groupExpanded = L.groupExpanded;
    s.fill = L.fill;
    // Self-contained undo: embed fill texture pixels if only AssetStore-backed.
    // DeleteLayer may Release the asset key; undo must still restore pixels.
    if (s.fill.useTexture && s.fill.textureRgba.empty() && !s.fill.textureAssetKey.empty()) {
        if (auto pay = assets::AssetStore::Get().GetPayload(s.fill.textureAssetKey)) {
            if (!pay->rgba.empty() && pay->w > 0 && pay->h > 0) {
                s.fill.textureRgba = pay->rgba;
                s.fill.textureW = pay->w;
                s.fill.textureH = pay->h;
            }
        }
    }
    s.filters = L.filters;
    s.styles = L.styles;
    // Outline textures also live as embedded rgba on style — already copied via styles.
    s.smartPath = L.smartSourcePath;
    s.smartBytes = L.smartSourceBytes;
    s.smartScale = L.smartScale;
    if (L.vectorDoc)
        s.vectorJson = vec::DocumentToJson(*L.vectorDoc);
    s.workSpace = L.workSpace;
    s.hasMask = L.hasMask;
    s.maskW = docW; s.maskH = docH;
    if (L.hasMask) {
        if (L.maskTiles && L.maskTiles->Valid()) {
            s.maskTiles = L.maskTiles->SnapshotAll();
            L.maskTiles->Flatten(s.maskFlat);
        } else {
            s.maskFlat = L.mask;
        }
    }
    if (L.tileCache && !L.isGroup) {
        int txN = L.tileCache->GetTilesX();
        int tyN = L.tileCache->GetTilesY();
        for (int ty = 0; ty < tyN; ++ty) {
            for (int tx = 0; tx < txN; ++tx) {
                if (!L.tileCache->HasTile(tx, ty)) continue;
                TileDelta d;
                d.layerIdx = index;
                d.tileX = tx; d.tileY = ty;
                d.newState = L.tileCache->SnapshotTile(tx, ty);
                s.tiles.push_back(std::move(d));
            }
        }
    }
    if (L.nativeMapCache && L.nativeMapW > 0 && L.nativeMapH > 0) {
        s.hasNative = true;
        s.nativeW = L.nativeMapW;
        s.nativeH = L.nativeMapH;
        s.nativeKind = L.nativeMapKind;
        int txN = L.nativeMapCache->GetTilesX();
        int tyN = L.nativeMapCache->GetTilesY();
        for (int ty = 0; ty < tyN; ++ty) {
            for (int tx = 0; tx < txN; ++tx) {
                if (!L.nativeMapCache->HasTile(tx, ty)) continue;
                TileDelta d;
                d.tileX = tx; d.tileY = ty;
                d.newState = L.nativeMapCache->SnapshotTile(tx, ty);
                s.nativeTiles.push_back(std::move(d));
            }
        }
    }
    (void)fmt;
    return s;
}

LayerPropsCommand::Props Canvas::CaptureLayerProps(const Layer& L) {
    LayerPropsCommand::Props p;
    p.name = L.name;
    p.visible = L.visible;
    p.opacity = L.opacity;
    p.blendMode = L.blendMode;
    p.alphaRewrite = L.alphaRewrite;
    p.filters = L.filters;
    p.styles = L.styles;
    p.isFill = L.IsFill();
    if (p.isFill) {
        p.fill = L.fill;
        if (p.fill.useTexture && p.fill.textureRgba.empty() && !p.fill.textureAssetKey.empty()) {
            if (auto pay = assets::AssetStore::Get().GetPayload(p.fill.textureAssetKey)) {
                if (!pay->rgba.empty() && pay->w > 0 && pay->h > 0) {
                    p.fill.textureRgba = pay->rgba;
                    p.fill.textureW = pay->w;
                    p.fill.textureH = pay->h;
                }
            }
        }
    }
    return p;
}

void Canvas::CommitLayerPropsEdit(int layerIdx, const LayerPropsCommand::Props& before,
                                  const char* actionName) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
    LayerPropsCommand::Props after = CaptureLayerProps(m_Layers[layerIdx]);
    // Cheap equality: name/opacity/blend/flags + filter/style counts & enabled + key params
    auto same = [&]() -> bool {
        if (before.name != after.name || before.visible != after.visible ||
            before.opacity != after.opacity || before.blendMode != after.blendMode ||
            before.alphaRewrite != after.alphaRewrite ||
            before.filters.size() != after.filters.size() ||
            before.styles.size() != after.styles.size())
            return false;
        if (before.isFill != after.isFill) return false;
        if (before.isFill) {
            if (before.fill.texturePath != after.fill.texturePath ||
                before.fill.textureAssetKey != after.fill.textureAssetKey ||
                before.fill.useTexture != after.fill.useTexture)
                return false;
            for (int i = 0; i < 4; ++i)
                if (before.fill.color[i] != after.fill.color[i]) return false;
        }
        for (size_t i = 0; i < before.filters.size(); ++i) {
            const auto& a = before.filters[i];
            const auto& b = after.filters[i];
            if (a.type != b.type || a.enabled != b.enabled || a.curvesChannels != b.curvesChannels)
                return false;
            for (int k = 0; k < 4; ++k)
                if (a.p[k] != b.p[k]) return false;
            if (a.lut != b.lut || a.lutR != b.lutR || a.lutG != b.lutG ||
                a.lutB != b.lutB || a.lutA != b.lutA)
                return false;
        }
        for (size_t i = 0; i < before.styles.size(); ++i) {
            const auto& a = before.styles[i];
            const auto& b = after.styles[i];
            if (a.type != b.type || a.enabled != b.enabled || a.opacity != b.opacity ||
                a.distance != b.distance || a.angleDeg != b.angleDeg ||
                a.offsetX != b.offsetX || a.offsetY != b.offsetY ||
                a.spread != b.spread || a.size != b.size ||
                a.outlineSize != b.outlineSize || a.outlinePos != b.outlinePos ||
                a.outlineFill != b.outlineFill || a.outlineGradientMap != b.outlineGradientMap)
                return false;
            for (int k = 0; k < 4; ++k) {
                if (a.shadowColor[k] != b.shadowColor[k]) return false;
                if (a.outlineColor[k] != b.outlineColor[k]) return false;
            }
            if (a.outlineTexturePath != b.outlineTexturePath) return false;
            if (a.outlineTextureAssetKey != b.outlineTextureAssetKey) return false;
            if (a.outlineGradient.size() != b.outlineGradient.size()) return false;
        }
        return true;
    };
    if (same()) return;
    // Opacity/visibility/blend → composite only. Filters/styles/fill asset → rebind tiles.
    bool rebindGpu = before.filters.size() != after.filters.size()
                  || before.styles.size() != after.styles.size()
                  || before.isFill != after.isFill;
    if (!rebindGpu && before.isFill) {
        if (before.fill.texturePath != after.fill.texturePath ||
            before.fill.textureAssetKey != after.fill.textureAssetKey ||
            before.fill.useTexture != after.fill.useTexture)
            rebindGpu = true;
        for (int i = 0; i < 4 && !rebindGpu; ++i)
            if (before.fill.color[i] != after.fill.color[i]) rebindGpu = true;
    }
    if (!rebindGpu) {
        for (size_t i = 0; i < before.filters.size() && !rebindGpu; ++i) {
            const auto& a = before.filters[i];
            const auto& b = after.filters[i];
            if (a.type != b.type || a.enabled != b.enabled || a.curvesChannels != b.curvesChannels)
                rebindGpu = true;
            for (int k = 0; k < 4 && !rebindGpu; ++k)
                if (a.p[k] != b.p[k]) rebindGpu = true;
            if (!rebindGpu && (a.lut != b.lut || a.lutR != b.lutR || a.lutG != b.lutG ||
                               a.lutB != b.lutB || a.lutA != b.lutA))
                rebindGpu = true;
        }
        for (size_t i = 0; i < before.styles.size() && !rebindGpu; ++i) {
            const auto& a = before.styles[i];
            const auto& b = after.styles[i];
            if (a.type != b.type || a.enabled != b.enabled || a.opacity != b.opacity ||
                a.distance != b.distance || a.angleDeg != b.angleDeg ||
                a.offsetX != b.offsetX || a.offsetY != b.offsetY ||
                a.spread != b.spread || a.size != b.size ||
                a.outlineSize != b.outlineSize || a.outlinePos != b.outlinePos ||
                a.outlineFill != b.outlineFill || a.outlineGradientMap != b.outlineGradientMap)
                rebindGpu = true;
            for (int k = 0; k < 4 && !rebindGpu; ++k) {
                if (a.shadowColor[k] != b.shadowColor[k]) rebindGpu = true;
                if (a.outlineColor[k] != b.outlineColor[k]) rebindGpu = true;
            }
            if (!rebindGpu && (a.outlineTexturePath != b.outlineTexturePath ||
                               a.outlineTextureAssetKey != b.outlineTextureAssetKey ||
                               a.outlineGradient.size() != b.outlineGradient.size()))
                rebindGpu = true;
        }
    }
    m_UndoRedoManager.PushCommand(std::make_shared<LayerPropsCommand>(
        actionName ? actionName : "Layer Edit", layerIdx, before, std::move(after)));
    m_IsDocumentModified = true;
    NotifyLayerVisualsChanged(layerIdx, /*contentPixelsChanged=*/rebindGpu,
                              /*markFiltersDirty=*/rebindGpu);
}

void Canvas::CreateNewLayer(ID3D11Device* device, const std::string& name) {
    Layer newLayer;
    newLayer.name    = name;
    newLayer.visible = true;
    newLayer.opacity = 1.0f;

    // Initialise TileCache for this layer (no tiles allocated yet — truly lazy)
    newLayer.tileCache = std::make_unique<TileCache>();
    newLayer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);

    if (device && m_Width > 0 && m_Height > 0) {
        if (!m_CompositeTexture) {
            CreateCompositeResources(device);
        }
        RecreateLayerTexture(device, newLayer);
    }

    int insertAt = (int)m_Layers.size();
    m_Layers.push_back(std::move(newLayer));
    m_ActiveLayerIdx = insertAt;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    auto snap = CaptureLayerSnap(m_Layers[insertAt], insertAt, m_Width, m_Height, m_CanvasFormat);
    m_UndoRedoManager.PushCommand(std::make_shared<LayerStackCommand>(
        "New Layer", LayerStackCommand::Kind::Insert, std::move(snap)));
    Logger::Get().Info("Created new layer: " + name);
}

bool Canvas::ImportImageAsMapLayer(ID3D11Device* device, const std::string& filepath,
                                   texset::MapKind mapKind, const std::string& layerName) {
    if (!device || filepath.empty()) return false;
    if (m_Width <= 0 || m_Height <= 0) {
        // No document yet — fall back to full open as base (sets size)
        if (mapKind == texset::MapKind::Diffuse)
            return LoadImageToLayer(device, filepath);
        Logger::Get().ErrorTag("texset", "ImportImageAsMapLayer: document size unknown; load Diffuse first");
        return false;
    }

    std::string ext;
    size_t dot = filepath.find_last_of('.');
    if (dot != std::string::npos) {
        ext = filepath.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
    }

    int imgW = 0, imgH = 0;
    std::unique_ptr<TileCache> loaded = std::make_unique<TileCache>();
    if (ext == "dds") {
        DdsFormat fmt = DdsFormat::RGBA8_UNORM;
        if (!DdsHelper::LoadDDSToTileCache(filepath, *loaded, imgW, imgH, fmt)) {
            Logger::Get().ErrorTag("texset", "ImportImageAsMapLayer DDS failed: " + filepath);
            return false;
        }
    } else {
        std::vector<uint8_t> rgba;
        if (!ImageManager::LoadImageFromFile(filepath, rgba, imgW, imgH)) {
            Logger::Get().ErrorTag("texset", "ImportImageAsMapLayer image failed: " + filepath);
            return false;
        }
        loaded->Init(imgW, imgH, CanvasPixelFormat::RGBA8);
        loaded->ImportRGBA8(rgba.data(), imgW, imgH);
    }

    Layer layer;
    if (!layerName.empty())
        layer.name = layerName;
    else {
        size_t slash = filepath.find_last_of("/\\");
        layer.name = (slash == std::string::npos) ? filepath : filepath.substr(slash + 1);
    }
    layer.visible = true;
    layer.opacity = 1.f;
    layer.alphaRewrite = true;
    layer.type = Layer::Type::Raster;
    // Bind to one map — still a normal layer in the list
    layer.workSpace = texset::LayerWorkSpace{};
    layer.workSpace.mapMask = 0;
    layer.workSpace.roleMask = 0;
    layer.workSpace.channelWriteMask = 0xF;
    layer.workSpace.SetMap(mapKind, true);

    // Native map storage when resolution ≠ document (export prefers this)
    if (imgW != m_Width || imgH != m_Height) {
        layer.nativeMapCache = std::make_unique<TileCache>();
        if (loaded->GetFormat() == m_CanvasFormat) {
            layer.nativeMapCache = std::move(loaded);
            loaded.reset();
        } else {
            layer.nativeMapCache->Init(imgW, imgH, m_CanvasFormat);
            for (int y = 0; y < imgH; ++y)
                for (int x = 0; x < imgW; ++x) {
                    float px[4]; loaded->GetPixelF(x, y, px);
                    layer.nativeMapCache->SetPixelF(x, y, px);
                }
        }
        layer.nativeMapW = imgW;
        layer.nativeMapH = imgH;
        layer.nativeMapKind = mapKind;
    }

    // Document UV space for viewport / paint
    layer.tileCache = std::make_unique<TileCache>();
    layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
    if (imgW == m_Width && imgH == m_Height && loaded) {
        if (loaded->GetFormat() == m_CanvasFormat)
            layer.tileCache = std::move(loaded);
        else {
            for (int y = 0; y < m_Height; ++y)
                for (int x = 0; x < m_Width; ++x) {
                    float px[4]; loaded->GetPixelF(x, y, px);
                    layer.tileCache->SetPixelF(x, y, px);
                }
        }
    } else if (layer.nativeMapCache) {
        for (int y = 0; y < m_Height; ++y) {
            int sy = std::min(layer.nativeMapH - 1, (int)((y + 0.5f) / m_Height * layer.nativeMapH));
            for (int x = 0; x < m_Width; ++x) {
                int sx = std::min(layer.nativeMapW - 1, (int)((x + 0.5f) / m_Width * layer.nativeMapW));
                float px[4];
                layer.nativeMapCache->GetPixelF(sx, sy, px);
                layer.tileCache->SetPixelF(x, y, px);
            }
        }
    }
    layer.tileCache->MarkAllDirty();
    RecreateLayerTexture(device, layer);

    m_Layers.push_back(std::move(layer));
    m_ActiveLayerIdx = (int)m_Layers.size() - 1;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().InfoTag("texset",
        "Map layer '" + m_Layers.back().name + "' → " + texset::MapKindName(mapKind) +
        " (" + std::to_string(imgW) + "x" + std::to_string(imgH) +
        (m_Layers.back().nativeMapCache ? " native kept" : "") + ")");
    return true;
}

void Canvas::DeleteLayer(int index) {
    if (index < 0 || index >= m_Layers.size()) return;

    Logger::Get().Info("Deleted layer: " + m_Layers[index].name);

    // Capture before destroy for undo
    auto snap = CaptureLayerSnap(m_Layers[index], index, m_Width, m_Height, m_CanvasFormat);
    m_UndoRedoManager.PushCommand(std::make_shared<LayerStackCommand>(
        "Delete Layer", LayerStackCommand::Kind::Remove, std::move(snap)));

    if (!m_Layers[index].fill.textureAssetKey.empty()) {
        assets::AssetStore::Get().Release(m_Layers[index].fill.textureAssetKey);
        m_Layers[index].fill.textureAssetKey.clear();
    }
    if (!m_Layers[index].smartAssetKey.empty()) {
        assets::AssetStore::Get().Release(m_Layers[index].smartAssetKey);
        m_Layers[index].smartAssetKey.clear();
    }
    for (auto& st : m_Layers[index].styles) {
        if (!st.outlineTextureAssetKey.empty()) {
            assets::AssetStore::Get().Release(st.outlineTextureAssetKey);
            st.outlineTextureAssetKey.clear();
        }
    }
    ReleaseFillPatternGpu(m_Layers[index]);
    if (m_Layers[index].texture) {
        DeferReleaseTex(m_Layers[index].texture);
        m_Layers[index].texture = nullptr;
    }
    if (m_Layers[index].srv) {
        DeferReleaseSRV(m_Layers[index].srv);
        m_Layers[index].srv = nullptr;
    }
    if (m_Layers[index].gpuSurfaceId) {
        m_GpuTiles.DestroySurface(m_Layers[index].gpuSurfaceId);
        m_Layers[index].gpuSurfaceId = 0;
    }
    if (m_Layers[index].maskTexture) {
        DeferReleaseTex(m_Layers[index].maskTexture);
        m_Layers[index].maskTexture = nullptr;
    }
    if (m_Layers[index].maskSRV) {
        DeferReleaseSRV(m_Layers[index].maskSRV);
        m_Layers[index].maskSRV = nullptr;
    }
    if (m_Layers[index].thumbSRV) {
        DeferReleaseSRV(m_Layers[index].thumbSRV);
        m_Layers[index].thumbSRV = nullptr;
    }
    if (m_Layers[index].thumbTex) {
        DeferReleaseTex(m_Layers[index].thumbTex);
        m_Layers[index].thumbTex = nullptr;
    }

    for (auto& l : m_Layers) {
        if (l.parentGroupId == index) {
            l.parentGroupId = -1;
        } else if (l.parentGroupId > index) {
            l.parentGroupId--;
        }
    }

    m_Layers.erase(m_Layers.begin() + index);

    if (m_Layers.empty()) {
        m_ActiveLayerIdx = -1;
    } else {
        m_ActiveLayerIdx = std::clamp(m_ActiveLayerIdx, 0, static_cast<int>(m_Layers.size()) - 1);
    }

    m_CompositeDirty = true;
    m_IsDocumentModified = true;
}

int Canvas::MergeLayerDown(ID3D11Device* device, int upperIdx) {
    if (upperIdx <= 0 || upperIdx >= (int)m_Layers.size()) return -1;
    const int lowerIdx = upperIdx - 1;
    Layer& upper = m_Layers[upperIdx];
    Layer& lower = m_Layers[lowerIdx];
    if (upper.isGroup || lower.isGroup) {
        Logger::Get().Warn("MergeLayerDown: cannot merge groups");
        return -1;
    }

    // Flatten non-destructive FX into pixels before merge (filters + styles + fill).
    auto bakeForMerge = [&](Layer& L) {
        if (L.IsFill()) {
            // Materialize fill into tiles for merge
            std::vector<float> solid;
            layer_fx::FillSolidBuffer(solid, m_Width, m_Height, L.fill);
            if (L.HasEnabledStyles() || !L.filters.empty()) {
                layer_fx::PresentationParams pp;
                pp.fillOpacity = L.opacity;
                pp.bakeFillOpacity = true;
                pp.hasMask = L.hasMask && !L.mask.empty();
                pp.mask = pp.hasMask ? L.mask.data() : nullptr;
                pp.maskBytes = L.mask.size();
                solid = layer_fx::BuildPresentation(solid, m_Width, m_Height, L.filters, L.styles, pp);
            } else {
                for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i) {
                    solid[i * 4 + 3] *= L.opacity;
                    if (L.hasMask && L.mask.size() == (size_t)m_Width * m_Height)
                        solid[i * 4 + 3] *= L.mask[i] / 255.f;
                }
            }
            if (!L.tileCache) {
                L.tileCache = std::make_unique<TileCache>();
                L.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
            }
            L.tileCache->ImportRGBA32F(solid.data(), m_Width, m_Height);
            L.tileCache->MarkAllDirty();
            L.type = Layer::Type::Raster;
            L.filters.clear();
            L.styles.clear();
            L.filteredCache.reset();
            L.presentationCache.reset();
            L.opacity = 1.f;
            return;
        }
        if (!L.filters.empty()) {
            L.filtersDirty = true;
            RebuildFilteredPixels(L);
        }
        if (L.HasEnabledStyles()) {
            L.presentationDirty = true;
            RebuildLayerPresentation(L, /*fullQuality=*/true);
            // Copy presentation into tileCache as merge source
            if (L.presentationCache && !L.presentationCache->IsEmpty()) {
                std::vector<float> px((size_t)m_Width * m_Height * 4);
                L.presentationCache->ExportRGBA32F(px.data(), m_Width, m_Height);
                EnsureLayerTileCache(L, m_Width, m_Height, m_CanvasFormat);
                L.tileCache->ImportRGBA32F(px.data(), m_Width, m_Height);
                L.tileCache->MarkAllDirty();
                L.styles.clear();
                L.presentationCache.reset();
                L.opacity = 1.f;
                L.hasMask = false;
                L.mask.clear();
            }
        } else if (!L.filters.empty() && L.filteredCache) {
            std::vector<float> px((size_t)m_Width * m_Height * 4);
            L.filteredCache->ExportRGBA32F(px.data(), m_Width, m_Height);
            EnsureLayerTileCache(L, m_Width, m_Height, m_CanvasFormat);
            L.tileCache->ImportRGBA32F(px.data(), m_Width, m_Height);
            L.tileCache->MarkAllDirty();
            L.filters.clear();
            L.filteredCache.reset();
        }
    };
    bakeForMerge(upper);
    bakeForMerge(lower);

    if (!upper.tileCache || !lower.tileCache) {
        Logger::Get().Warn("MergeLayerDown: missing tile cache after bake");
        return -1;
    }

    const TileCache* upperCache = upper.tileCache.get();
    const TileCache* lowerCache = lower.tileCache.get();
    if (!upperCache || !lowerCache) return -1;

    EnsureLayerTileCache(lower, m_Width, m_Height, m_CanvasFormat);
    // Undo: full-tile backup of the surviving (lower) layer before pixels change.
    int prevActive = m_ActiveLayerIdx;
    m_ActiveLayerIdx = lowerIdx;
    BackupAllActiveLayerTiles();

    // After style bake, opacity/mask already applied → merge at full strength
    const float op = std::clamp(upper.opacity, 0.f, 1.f);
    const BlendMode mode = upper.blendMode;
    const bool useMask = upper.hasMask && upper.mask.size() == (size_t)m_Width * m_Height;

    // Tile-wise merge to avoid full-doc float when sparse
    const int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    const int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const bool upperHas = upperCache->HasTile(tx, ty);
            const bool lowerHas = lowerCache->HasTile(tx, ty);
            if (!upperHas && !lowerHas) continue;

            const int x0 = tx * TILE_SIZE;
            const int y0 = ty * TILE_SIZE;
            const int x1 = std::min(x0 + TILE_SIZE, m_Width);
            const int y1 = std::min(y0 + TILE_SIZE, m_Height);

            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    float L[4] = {}, U[4] = {};
                    lowerCache->GetPixelF(x, y, L);
                    upperCache->GetPixelF(x, y, U);
                    float sa = U[3] * op;
                    if (useMask) sa *= upper.mask[(size_t)y * m_Width + x] / 255.f;
                    if (sa <= 1e-8f) {
                        // upper contributes nothing; keep L
                        lower.tileCache->SetPixelF(x, y, L);
                        continue;
                    }
                    float br, bg, bb;
                    ApplyBlendModeRGB(mode, U[0], U[1], U[2], L[0], L[1], L[2], br, bg, bb);
                    // sa = morph strength when !alphaRewrite; always RGB over
                    const float inv = 1.f - sa;
                    float out[4];
                    out[0] = br * sa + L[0] * inv;
                    out[1] = bg * sa + L[1] * inv;
                    out[2] = bb * sa + L[2] * inv;
                    out[3] = upper.alphaRewrite ? (sa + L[3] * inv) : L[3];
                    lower.tileCache->SetPixelF(x, y, out);
                }
            }
        }
    }

    lower.filters.clear();
    lower.styles.clear();
    lower.filteredCache.reset();
    lower.presentationCache.reset();
    lower.filtersDirty = false;
    lower.stylesDirty = false;
    lower.presentationDirty = false;
    lower.needsUpload = true;
    lower.thumbDirty = true;
    lower.tileCache->MarkAllDirty();

    // Commit lower mutation for undo, then delete upper
    m_ActiveLayerIdx = lowerIdx;
    CommitActiveLayerMutation("Merge Layer Down");

    std::string mergedName = lower.name;
    if (mergedName.find(" (merged)") == std::string::npos)
        lower.name = mergedName; // keep lower name
    Logger::Get().Info("Merged '" + upper.name + "' down into '" + lower.name +
        "' blend=" + std::to_string((int)mode));

    DeleteLayer(upperIdx);
    m_ActiveLayerIdx = lowerIdx;
    if (device) {
        RecreateLayerTexture(device, m_Layers[lowerIdx]);
        m_Layers[lowerIdx].needsUpload = true;
    }
    m_CompositeDirty = true;
    m_ChannelPreviewDirty = true;
    (void)prevActive;
    return lowerIdx;
}

int Canvas::MergeLayers(ID3D11Device* device, const std::vector<int>& indices) {
    if (indices.empty()) return -1;
    std::vector<int> sorted = indices;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.size() == 1)
        return MergeLayerDown(device, sorted[0]);

    // Require contiguous stack (bottom..top).
    for (size_t k = 1; k < sorted.size(); ++k) {
        if (sorted[k] != sorted[k - 1] + 1) {
            Logger::Get().Warn("MergeLayers: selection must be a contiguous stack; merging top down only");
            return MergeLayerDown(device, sorted.back());
        }
    }
    const int bottom = sorted.front();
    const int count = (int)sorted.size();
    for (int n = 0; n < count - 1; ++n) {
        // Always merge the layer immediately above bottom into bottom.
        int r = MergeLayerDown(device, bottom + 1);
        if (r < 0) break;
    }
    m_ActiveLayerIdx = std::clamp(bottom, 0, (int)m_Layers.size() - 1);
    return m_ActiveLayerIdx;
}

int Canvas::DuplicateLayer(ID3D11Device* device, int index) {
    if (index < 0 || index >= (int)m_Layers.size()) return -1;
    const Layer& src = m_Layers[index];

    Layer dup;
    dup.name = src.name + " copy";
    dup.visible = src.visible;
    dup.opacity = src.opacity;
    dup.blendMode = src.blendMode;
    dup.alphaRewrite = src.alphaRewrite;
    dup.isGroup = src.isGroup;
    dup.type = src.type;
    dup.parentGroupId = src.parentGroupId;
    dup.groupExpanded = src.groupExpanded;
    dup.smartSourceBytes = src.smartSourceBytes;
    dup.smartSourcePath = src.smartSourcePath;
    dup.smartAssetKey = src.smartAssetKey;
    dup.smartScale = src.smartScale;
    dup.fill = src.fill;
    // Shared fill texture: bump AssetStore ref (copy does not Acquire).
    if (!dup.fill.textureAssetKey.empty())
        assets::AssetStore::Get().AddRef(dup.fill.textureAssetKey);
    if (!dup.smartAssetKey.empty())
        assets::AssetStore::Get().AddRef(dup.smartAssetKey);
    dup.workSpace = src.workSpace; // map participation
    dup.filters = src.filters;
    dup.filtersDirty = true;
    dup.styles = src.styles;
    for (auto& st : dup.styles) {
        if (!st.outlineTextureAssetKey.empty())
            assets::AssetStore::Get().AddRef(st.outlineTextureAssetKey);
    }
    dup.stylesDirty = true;
    dup.presentationDirty = true;

    if (dup.IsFill()) {
        if (src.hasMask) {
            dup.hasMask = true;
            dup.mask = src.mask;
            if (src.maskTiles && src.maskTiles->Valid()) {
                dup.maskTiles = std::make_unique<MaskTiles>();
                dup.maskTiles->Init(m_Width, m_Height, 255);
                dup.maskTiles->RestoreTiles(src.maskTiles->SnapshotAll());
            }
            dup.maskNeedsUpload = true;
        }
        if (device && m_Width > 0 && m_Height > 0) {
            if (!m_CompositeTexture) CreateCompositeResources(device);
            EnsureFillLayerGpu(device, dup);
        }
    } else if (!dup.isGroup) {
        dup.tileCache = std::make_unique<TileCache>();
        dup.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
        if (src.tileCache && !src.tileCache->IsEmpty()) {
            auto pixels = ExportLayerF(src, m_Width, m_Height);
            SetLayerPixelsF(dup, pixels, m_Width, m_Height, m_CanvasFormat);
        }
        if (src.nativeMapCache && src.nativeMapW > 0) {
            dup.nativeMapCache = std::make_unique<TileCache>();
            dup.nativeMapCache->Init(src.nativeMapW, src.nativeMapH, m_CanvasFormat);
            // COW-share tiles via snapshot restore
            int txN = src.nativeMapCache->GetTilesX();
            int tyN = src.nativeMapCache->GetTilesY();
            for (int ty = 0; ty < tyN; ++ty)
                for (int tx = 0; tx < txN; ++tx)
                    if (src.nativeMapCache->HasTile(tx, ty))
                        dup.nativeMapCache->RestoreTile(tx, ty, src.nativeMapCache->SnapshotTile(tx, ty));
            dup.nativeMapW = src.nativeMapW;
            dup.nativeMapH = src.nativeMapH;
            dup.nativeMapKind = src.nativeMapKind;
        }
        if (src.hasMask) {
            dup.hasMask = true;
            dup.mask = src.mask;
            if (src.maskTiles && src.maskTiles->Valid()) {
                dup.maskTiles = std::make_unique<MaskTiles>();
                dup.maskTiles->Init(m_Width, m_Height, 255);
                dup.maskTiles->RestoreTiles(src.maskTiles->SnapshotAll());
            }
            dup.maskNeedsUpload = true;
        }
        if (device && m_Width > 0 && m_Height > 0) {
            if (!m_CompositeTexture) CreateCompositeResources(device);
            RecreateLayerTexture(device, dup);
        }
    }

    int insertAt = index + 1;
    m_Layers.insert(m_Layers.begin() + insertAt, std::move(dup));

    // Remap parentGroupId for all layers (including the new one)
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        if (m_Layers[i].parentGroupId >= insertAt)
            m_Layers[i].parentGroupId++;
    }

    if (device && m_Layers[insertAt].hasMask)
        UpdateLayerMaskTexture(device, insertAt);

    m_ActiveLayerIdx = insertAt;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    {
        auto snap = CaptureLayerSnap(m_Layers[insertAt], insertAt, m_Width, m_Height, m_CanvasFormat);
        m_UndoRedoManager.PushCommand(std::make_shared<LayerStackCommand>(
            "Duplicate Layer", LayerStackCommand::Kind::Insert, std::move(snap)));
    }
    Logger::Get().Info("Duplicated layer -> " + m_Layers[insertAt].name);
    return insertAt;
}

void Canvas::DuplicateLayers(ID3D11Device* device, const std::vector<int>& indices) {
    if (indices.empty()) return;
    std::vector<int> sorted = indices;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    // Duplicate from high to low so earlier indices stay valid until we process them...
    // Actually each insert shifts indices after insertAt. Process low→high and track offset.
    int offset = 0;
    int lastNew = -1;
    for (int idx : sorted) {
        int src = idx + offset;
        int neu = DuplicateLayer(device, src);
        if (neu >= 0) {
            lastNew = neu;
            offset++; // one extra layer inserted
        }
    }
    if (lastNew >= 0) m_ActiveLayerIdx = lastNew;
}

void Canvas::DeleteLayers(const std::vector<int>& indices) {
    if (indices.empty()) return;
    std::vector<int> sorted = indices;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (int idx : sorted) {
        if ((int)m_Layers.size() <= 1) break; // keep at least one
        DeleteLayer(idx);
    }
}

void Canvas::SetActiveLayerIndex(int idx) {
    if (idx >= 0 && idx < m_Layers.size()) {
        m_ActiveLayerIdx = idx;
    }
}

void Canvas::ToggleLayerIsolation(int layerIdx) {
    if (layerIdx < 0 || layerIdx >= static_cast<int>(m_Layers.size())) return;

    if (m_IsIsolatedMode && m_IsolatedLayerIdx == layerIdx) {
        // Turn off isolation: restore visibility states
        for (size_t i = 0; i < m_Layers.size(); ++i) {
            if (i < m_PreIsolationVisibility.size()) {
                m_Layers[i].visible = m_PreIsolationVisibility[i];
            } else {
                m_Layers[i].visible = true;
            }
        }
        m_IsIsolatedMode = false;
        m_IsolatedLayerIdx = -1;
        m_PreIsolationVisibility.clear();
    } else {
        // If already in isolated mode, first restore visibility before new isolation
        if (m_IsIsolatedMode) {
            for (size_t i = 0; i < m_Layers.size(); ++i) {
                if (i < m_PreIsolationVisibility.size()) {
                    m_Layers[i].visible = m_PreIsolationVisibility[i];
                }
            }
        }

        // Turn on isolation for layerIdx
        m_PreIsolationVisibility.resize(m_Layers.size());
        for (size_t i = 0; i < m_Layers.size(); ++i) {
            m_PreIsolationVisibility[i] = m_Layers[i].visible;
            m_Layers[i].visible = (static_cast<int>(i) == layerIdx);
        }
        m_IsIsolatedMode = true;
        m_IsolatedLayerIdx = layerIdx;
    }

    m_CompositeDirty = true;
}

// Snapshot newState + deep-copy both ends into an immutable undo delta list.
static std::vector<TileDelta> SealActiveStrokeDeltas(
    std::unordered_map<int, TileDelta>& active,
    TileCache* cache) {
    std::vector<TileDelta> deltas;
    deltas.reserve(active.size());
    for (auto& pair : active) {
        auto& delta = pair.second;
        delta.newState = cache ? cache->SnapshotTile(delta.tileX, delta.tileY) : TileSnapshot{};
        if (delta.oldState.data && delta.newState.data &&
            delta.oldState.data.get() == delta.newState.data.get())
            continue;
        if (!delta.oldState.data && !delta.newState.data) continue;
        delta.oldState = TileCache::DeepCopySnapshot(delta.oldState);
        delta.newState = TileCache::DeepCopySnapshot(delta.newState);
        deltas.push_back(std::move(delta));
    }
    return deltas;
}

void Canvas::BackupTile(int tileX, int tileY) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    int numTilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    if (numTilesX < 1) numTilesX = 1;
    int key = tileY * numTilesX + tileX;

    if (m_ActiveStrokeDeltas.count(key)) return; // already backed up

    auto& layer = m_Layers[m_ActiveLayerIdx];
    TileDelta delta;
    delta.layerIdx  = m_ActiveLayerIdx;
    delta.tileX     = tileX;
    delta.tileY     = tileY;
    // Shared snapshot (empty if tile doesn't exist). Write will COW-clone later.
    delta.oldState = layer.tileCache ? layer.tileCache->SnapshotTile(tileX, tileY) : TileSnapshot{};

    m_ActiveStrokeDeltas[key] = std::move(delta);
}

void Canvas::BackupAllActiveLayerTiles() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    m_ActiveStrokeDeltas.clear();
    const int ntx = (m_Width  + TILE_SIZE - 1) / TILE_SIZE;
    const int nty = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    for (int ty = 0; ty < nty; ++ty)
        for (int tx = 0; tx < ntx; ++tx)
            BackupTile(tx, ty);
}

void Canvas::BackupTilesInRect(int x0, int y0, int x1, int y1) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    if (m_Width <= 0 || m_Height <= 0) return;
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(m_Width - 1, x1); y1 = std::min(m_Height - 1, y1);
    if (x1 < x0 || y1 < y0) return;
    const int ntx = (m_Width  + TILE_SIZE - 1) / TILE_SIZE;
    const int nty = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    int tx0 = x0 / TILE_SIZE, ty0 = y0 / TILE_SIZE;
    int tx1 = x1 / TILE_SIZE, ty1 = y1 / TILE_SIZE;
    for (int ty = ty0; ty <= ty1 && ty < nty; ++ty)
        for (int tx = tx0; tx <= tx1 && tx < ntx; ++tx)
            BackupTile(tx, ty);
}

void Canvas::CommitActiveLayerMutation(const std::string& actionName) {
    CommitTransformation(actionName);
    InvalidateWandSourceCache();
    m_QuickSelectEdgeValid = false;
}

void Canvas::RestoreActiveLayerMutation() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) {
        m_ActiveStrokeDeltas.clear();
        return;
    }
    auto& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.tileCache) {
        for (const auto& pair : m_ActiveStrokeDeltas) {
            const auto& d = pair.second;
            layer.tileCache->RestoreTile(d.tileX, d.tileY, d.oldState);
            layer.tileCache->MarkDirty(d.tileX, d.tileY);
        }
    }
    m_ActiveStrokeDeltas.clear();
    // Match stroke-end: filters sparse + styles + parent groups (cancel must not leave bake)
    NotifyLayerVisualsChanged(m_ActiveLayerIdx, /*contentPixelsChanged=*/true,
                              /*markFiltersDirty=*/true);
}

extern float g_PenPressure;

void Canvas::PaintOnActiveLayer(float currRawX, float currRawY, StrokePhase phase, const BrushSettings& brush) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;

    auto& layer = m_Layers[m_ActiveLayerIdx];

    // Fill / Group: content paint blocked; mask paint still allowed.
    if (m_PaintTarget == PaintTarget::LayerContent && !layer.CanPaintContent()) {
        if (phase == StrokePhase::Begin) {
            Logger::Get().Info(layer.IsFill()
                ? "Paint blocked: Fill Layer content is not paintable — paint the mask instead."
                : "Paint blocked: layer content is not paintable.");
        }
        return;
    }

    BrushSettings activeBrush = brush;

    // --- Channel isolation ---
    // R/G/B toggles select which color channels tools write.
    // Channel A is a *view / solo-edit* flag, NOT a transparency lock:
    //   • A ON alone  → paint only alpha (grayscale coverage)
    //   • A OFF       → stamp = opacity×hardness×tip (color.a forced 1); tools still
    //                   establish / erase coverage when Alpha Rewrite is ON
    // Alpha Rewrite ON  → A is real coverage (brush builds it, eraser punches holes)
    // Alpha Rewrite OFF → A is RGB morph strength (brush keeps A; eraser fades A)
    // Global channel view toggles AND layer workSpace physical write mask
    activeBrush.writeR = m_ChannelR && layer.workSpace.WritesChan(texset::Chan::R);
    activeBrush.writeG = m_ChannelG && layer.workSpace.WritesChan(texset::Chan::G);
    activeBrush.writeB = m_ChannelB && layer.workSpace.WritesChan(texset::Chan::B);

    const bool onlyAlphaChannel = m_ChannelA && !m_ChannelR && !m_ChannelG && !m_ChannelB;
    const bool channelAOff = !m_ChannelA;

    if (onlyAlphaChannel) {
        activeBrush.writeR = activeBrush.writeG = activeBrush.writeB = false;
        activeBrush.writeA = layer.workSpace.WritesChan(texset::Chan::A);
        activeBrush.rgbMorphOnly = false;
    } else {
        // All RGB channels off + A off → still paint RGB (A is not a write lock),
        // but still respect layer write mask.
        if (!activeBrush.writeR && !activeBrush.writeG && !activeBrush.writeB) {
            activeBrush.writeR = layer.workSpace.WritesChan(texset::Chan::R);
            activeBrush.writeG = layer.workSpace.WritesChan(texset::Chan::G);
            activeBrush.writeB = layer.workSpace.WritesChan(texset::Chan::B);
            if (!activeBrush.writeR && !activeBrush.writeG && !activeBrush.writeB) {
                activeBrush.writeR = activeBrush.writeG = activeBrush.writeB = true;
            }
        }

        if (layer.alphaRewrite) {
            // Coverage layer: always write A so brush/eraser work without needing
            // Channels→Alpha ON. A-off only changes stamp source (opacity×hardness).
            activeBrush.writeA = layer.workSpace.WritesChan(texset::Chan::A);
            if (!activeBrush.writeA && (activeBrush.writeR || activeBrush.writeG || activeBrush.writeB))
                activeBrush.writeA = true; // need coverage for RGB paint on rewrite layers
            activeBrush.rgbMorphOnly = channelAOff;
            if (channelAOff)
                activeBrush.color[3] = 1.0f;
        } else {
            // Strength / decal layer: A multiplies RGB over underlay; never invent A
            // on brush — only fade it with eraser so the overlay lifts.
            activeBrush.rgbMorphOnly = true;
            activeBrush.color[3] = 1.0f;
            activeBrush.writeA = activeBrush.erase;
        }
    }

    if (brush.pressureRadius) {
        activeBrush.radius = brush.radius * g_PenPressure;
        if (activeBrush.radius < 1.0f) activeBrush.radius = 1.0f;
    }
    if (brush.pressureHardness) {
        activeBrush.hardness = brush.hardness * g_PenPressure;
    }
    if (brush.pressureOpacity) {
        activeBrush.opacity = brush.opacity * g_PenPressure;
    }
    // Pressure → rotation: fold into rotationDeg before paint engine (Phase A).
    // pressure 0 → -90°, 0.5 → base angle, 1 → +90° from base.
    if (brush.pressureRotation) {
        activeBrush.rotationDeg = brush.rotationDeg + (g_PenPressure - 0.5f) * 180.0f;
        // Keep in a reasonable range for tip UV (wrap not required for cos/sin).
    }
    // Expand effective backup radius when scatter is active
    const float paintRadius = activeBrush.radius * (1.0f + std::clamp(activeBrush.scatter, 0.f, 1.f));

    // ---- Mask paint path (Photoshop-like) ----
    if (m_PaintTarget == PaintTarget::LayerMask) {
        if (!layer.hasMask) {
            // Lazy create white mask if UI switched to mask without CreateLayerMask.
            EnsureActiveLayerMaskAllocated();
        }
        if (phase == StrokePhase::Begin) {
            m_IsStrokeActive = true;
            m_StrokeDistanceAccumulator = 0.0f;
            m_LastDabX = currRawX;
            m_LastDabY = currRawY;
            m_PrevStabilizedX = currRawX;
            m_PrevStabilizedY = currRawY;
            // COW tile snap of full mask grid (absent tiles = white default, tiny overhead)
            m_MaskStrokeBackupTiles.clear();
            m_MaskStrokeBackupValid = layer.hasMask && layer.maskTiles && layer.maskTiles->Valid();
            if (m_MaskStrokeBackupValid)
                m_MaskStrokeBackupTiles = layer.maskTiles->SnapshotAll();
            if (!activeBrush.erase) {
                activeBrush.color[0] = activeBrush.color[1] = activeBrush.color[2] = 1.f;
            }
            PaintMaskStamp(currRawX, currRawY, activeBrush);
            if (m_MirrorHorizontal) PaintMaskStamp((float)m_Width - currRawX, currRawY, activeBrush);
            if (m_MirrorVertical) PaintMaskStamp(currRawX, (float)m_Height - currRawY, activeBrush);
            if (m_MirrorHorizontal && m_MirrorVertical)
                PaintMaskStamp((float)m_Width - currRawX, (float)m_Height - currRawY, activeBrush);
        } else if (phase == StrokePhase::Update && m_IsStrokeActive) {
            float weight = 1.0f / static_cast<float>(std::max(1, activeBrush.stabilization));
            float sx = m_PrevStabilizedX + weight * (currRawX - m_PrevStabilizedX);
            float sy = m_PrevStabilizedY + weight * (currRawY - m_PrevStabilizedY);
            if (!activeBrush.erase) {
                activeBrush.color[0] = activeBrush.color[1] = activeBrush.color[2] = 1.f;
            }
            // Match PaintEngine::DrawStrokeSegment — full segment walk so SHIFT+click
            // long lines complete (old 64-stamp cap left only a short dash).
            float x0 = m_LastDabX, y0 = m_LastDabY;
            float x1 = sx, y1 = sy;
            float dx = x1 - x0, dy = y1 - y0;
            float segLen = std::sqrt(dx * dx + dy * dy);
            const float spacingMul = (activeBrush.tip) ? activeBrush.tip->spacingMul : 1.0f;
            float step = std::max(1.f, activeBrush.radius * 2.0f *
                std::max(0.05f, activeBrush.spacing) * spacingMul);
            if (segLen > 1e-4f) {
                float dirX = dx / segLen, dirY = dy / segLen;
                float traveled = 0.f;
                constexpr int kMaxMaskStampsPerUpdate = 4096;
                int stamps = 0;
                while (traveled <= segLen && stamps < kMaxMaskStampsPerUpdate) {
                    float needed = step - m_StrokeDistanceAccumulator;
                    if (traveled + needed <= segLen) {
                        traveled += needed;
                        float px = x0 + dirX * traveled;
                        float py = y0 + dirY * traveled;
                        PaintMaskStamp(px, py, activeBrush);
                        if (m_MirrorHorizontal) PaintMaskStamp((float)m_Width - px, py, activeBrush);
                        if (m_MirrorVertical) PaintMaskStamp(px, (float)m_Height - py, activeBrush);
                        if (m_MirrorHorizontal && m_MirrorVertical)
                            PaintMaskStamp((float)m_Width - px, (float)m_Height - py, activeBrush);
                        m_LastDabX = px; m_LastDabY = py;
                        m_StrokeDistanceAccumulator = 0.f;
                        ++stamps;
                    } else {
                        float ex = x1 - m_LastDabX, ey = y1 - m_LastDabY;
                        m_StrokeDistanceAccumulator = std::sqrt(ex * ex + ey * ey);
                        break;
                    }
                }
                if (stamps >= kMaxMaskStampsPerUpdate) {
                    // Extreme path: final stamp at endpoint so SHIFT lines still close.
                    PaintMaskStamp(x1, y1, activeBrush);
                    if (m_MirrorHorizontal) PaintMaskStamp((float)m_Width - x1, y1, activeBrush);
                    if (m_MirrorVertical) PaintMaskStamp(x1, (float)m_Height - y1, activeBrush);
                    if (m_MirrorHorizontal && m_MirrorVertical)
                        PaintMaskStamp((float)m_Width - x1, (float)m_Height - y1, activeBrush);
                    m_LastDabX = x1; m_LastDabY = y1;
                    m_StrokeDistanceAccumulator = 0.f;
                }
            }
            m_PrevStabilizedX = sx; m_PrevStabilizedY = sy;
        } else if (phase == StrokePhase::End) {
            m_IsStrokeActive = false;
            m_IsDocumentModified = true;
            // Mask paint undo: only tiles that actually changed (COW pointers)
            if (m_MaskStrokeBackupValid && m_ActiveLayerIdx >= 0 &&
                m_ActiveLayerIdx < (int)m_Layers.size() &&
                m_Layers[m_ActiveLayerIdx].hasMask &&
                m_Layers[m_ActiveLayerIdx].maskTiles) {
                auto& L = m_Layers[m_ActiveLayerIdx];
                auto newAll = L.maskTiles->SnapshotAll();
                std::unordered_map<uint32_t, MaskTileSnapshot> oldMap, newMap;
                for (auto& t : m_MaskStrokeBackupTiles)
                    oldMap[(uint32_t)t.tileX | ((uint32_t)t.tileY << 16)] = t;
                for (auto& t : newAll)
                    newMap[(uint32_t)t.tileX | ((uint32_t)t.tileY << 16)] = t;
                std::vector<MaskTileSnapshot> oldCh, newCh;
                // Union of keys
                std::unordered_set<uint32_t> keys;
                for (auto& kv : oldMap) keys.insert(kv.first);
                for (auto& kv : newMap) keys.insert(kv.first);
                for (uint32_t k : keys) {
                    auto o = oldMap.find(k);
                    auto n = newMap.find(k);
                    const MaskTileSnapshot* op = o != oldMap.end() ? &o->second : nullptr;
                    const MaskTileSnapshot* np = n != newMap.end() ? &n->second : nullptr;
                    auto od = op ? op->data.get() : nullptr;
                    auto nd = np ? np->data.get() : nullptr;
                    if (od == nd) continue;
                    MaskTileSnapshot oSnap, nSnap;
                    oSnap.tileX = nSnap.tileX = (int)(k & 0xFFFF);
                    oSnap.tileY = nSnap.tileY = (int)(k >> 16);
                    if (op) oSnap = *op;
                    if (np) nSnap = *np;
                    oldCh.push_back(std::move(oSnap));
                    newCh.push_back(std::move(nSnap));
                }
                if (!oldCh.empty()) {
                    m_UndoRedoManager.PushCommand(std::make_shared<LayerMaskPaintCommand>(
                        brush.erase ? "Mask Erase" : "Mask Paint",
                        m_ActiveLayerIdx,
                        std::move(oldCh), std::move(newCh)));
                }
            }
            m_MaskStrokeBackupValid = false;
            m_MaskStrokeBackupTiles.clear();
            // Group FX / isolation must see mask change without waiting for a content stroke.
            NotifyLayerVisualsChanged(m_ActiveLayerIdx, /*contentPixelsChanged=*/false,
                                      /*markFiltersDirty=*/false);
        }
        return;
    }

    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;

    auto backupSymmetricTiles = [&](float cx, float cy, float radius) {
        float minX = cx - radius;
        float maxX = cx + radius;
        float minY = cy - radius;
        float maxY = cy + radius;
        
        int minTileX = std::max(0, static_cast<int>(minX) / 256);
        int maxTileX = std::max(0, static_cast<int>(maxX) / 256);
        int minTileY = std::max(0, static_cast<int>(minY) / 256);
        int maxTileY = std::max(0, static_cast<int>(maxY) / 256);

        for (int ty = minTileY; ty <= std::min(maxTileY, numTilesY - 1); ++ty) {
            for (int tx = minTileX; tx <= std::min(maxTileX, numTilesX - 1); ++tx) {
                BackupTile(tx, ty);
            }
        }
    };

    if (phase == StrokePhase::Begin) {
        m_IsStrokeActive = true;
        m_StrokeDistanceAccumulator = 0.0f;
        m_LastDabX = currRawX;
        m_LastDabY = currRawY;
        m_PrevStabilizedX = currRawX;
        m_PrevStabilizedY = currRawY;
        m_ActiveStrokeDeltas.clear();

        // Backup tiles covered by the first stamp (and its symmetries); expand for scatter
        backupSymmetricTiles(currRawX, currRawY, paintRadius);
        if (m_MirrorHorizontal) {
            backupSymmetricTiles(static_cast<float>(m_Width) - currRawX, currRawY, paintRadius);
        }
        if (m_MirrorVertical) {
            backupSymmetricTiles(currRawX, static_cast<float>(m_Height) - currRawY, paintRadius);
        }
        if (m_MirrorHorizontal && m_MirrorVertical) {
            backupSymmetricTiles(static_cast<float>(m_Width) - currRawX, static_cast<float>(m_Height) - currRawY, paintRadius);
        }

        // Lazy-allocate TileCache on first paint
        if (!m_Layers[m_ActiveLayerIdx].tileCache) {
            m_Layers[m_ActiveLayerIdx].tileCache = std::make_unique<TileCache>();
            m_Layers[m_ActiveLayerIdx].tileCache->Init(m_Width, m_Height, m_CanvasFormat);
        }

        // Place the very first stamp immediately
        // CRITICAL: never pass a temporary copy of the full selection mask (8K = 64 MiB/dab).
        // Selection acts as a temp paint mask: empty / size-mismatch → no clip.
        static const std::vector<uint8_t> kEmptySel;
        const size_t selNeed = (size_t)m_Width * (size_t)m_Height;
        const std::vector<uint8_t>& selMask =
            (m_HasSelection && m_SelectionMask.size() == selNeed) ? m_SelectionMask : kEmptySel;
        PaintEngine::DrawStamp(*m_Layers[m_ActiveLayerIdx].tileCache,
                               currRawX, currRawY, activeBrush,
                               m_MirrorHorizontal, m_MirrorVertical,
                               selMask);
        m_Layers[m_ActiveLayerIdx].needsUpload = true;
        // Mid-stroke: raw content only. FX refilter deferred to StrokePhase::End (see ComposeLayers).
        {
            const int r = (int)std::ceil(paintRadius) + 2;
            const int ix = (int)currRawX, iy = (int)currRawY;
            InvalidateCompositeRect(ix - r, iy - r, ix + r, iy + r);
        }
    }
    else if (phase == StrokePhase::Update && m_IsStrokeActive) {
        // Apply stabilization
        float weight = 1.0f / static_cast<float>(std::max(1, activeBrush.stabilization));
        float stabilizedX = m_PrevStabilizedX + weight * (currRawX - m_PrevStabilizedX);
        float stabilizedY = m_PrevStabilizedY + weight * (currRawY - m_PrevStabilizedY);

        // Backup tiles covered by the stroke segment (and its symmetries)
        auto backupSegment = [&](float x0, float y0, float x1, float y1) {
            float minX = std::min(x0, x1) - paintRadius;
            float maxX = std::max(x0, x1) + paintRadius;
            float minY = std::min(y0, y1) - paintRadius;
            float maxY = std::max(y0, y1) + paintRadius;

            int minTileX = std::max(0, static_cast<int>(minX) / 256);
            int maxTileX = std::max(0, static_cast<int>(maxX) / 256);
            int minTileY = std::max(0, static_cast<int>(minY) / 256);
            int maxTileY = std::max(0, static_cast<int>(maxY) / 256);

            for (int ty = minTileY; ty <= std::min(maxTileY, numTilesY - 1); ++ty) {
                for (int tx = minTileX; tx <= std::min(maxTileX, numTilesX - 1); ++tx) {
                    BackupTile(tx, ty);
                }
            }
        };

        backupSegment(m_PrevStabilizedX, m_PrevStabilizedY, stabilizedX, stabilizedY);
        if (m_MirrorHorizontal) {
            backupSegment(static_cast<float>(m_Width) - m_PrevStabilizedX, m_PrevStabilizedY,
                          static_cast<float>(m_Width) - stabilizedX, stabilizedY);
        }
        if (m_MirrorVertical) {
            backupSegment(m_PrevStabilizedX, static_cast<float>(m_Height) - m_PrevStabilizedY,
                          stabilizedX, static_cast<float>(m_Height) - stabilizedY);
        }
        if (m_MirrorHorizontal && m_MirrorVertical) {
            backupSegment(static_cast<float>(m_Width) - m_PrevStabilizedX, static_cast<float>(m_Height) - m_PrevStabilizedY,
                          static_cast<float>(m_Width) - stabilizedX, static_cast<float>(m_Height) - stabilizedY);
        }

        // Draw stroke segment (no selection-mask copy); selection = temp paint mask
        static const std::vector<uint8_t> kEmptySelUpd;
        const size_t selNeedUpd = (size_t)m_Width * (size_t)m_Height;
        const std::vector<uint8_t>& selMask =
            (m_HasSelection && m_SelectionMask.size() == selNeedUpd) ? m_SelectionMask : kEmptySelUpd;
        PaintEngine::DrawStrokeSegment(*m_Layers[m_ActiveLayerIdx].tileCache,
                                       m_PrevStabilizedX, m_PrevStabilizedY,
                                       stabilizedX, stabilizedY,
                                       activeBrush, m_StrokeDistanceAccumulator,
                                       m_LastDabX, m_LastDabY,
                                       m_MirrorHorizontal, m_MirrorVertical,
                                       selMask);

        {
            const int r = (int)std::ceil(paintRadius) + 2;
            const int x0 = (int)std::floor(std::min(m_PrevStabilizedX, stabilizedX)) - r;
            const int y0 = (int)std::floor(std::min(m_PrevStabilizedY, stabilizedY)) - r;
            const int x1 = (int)std::ceil(std::max(m_PrevStabilizedX, stabilizedX)) + r;
            const int y1 = (int)std::ceil(std::max(m_PrevStabilizedY, stabilizedY)) + r;
            InvalidateCompositeRect(x0, y0, x1, y1);
        }
        m_PrevStabilizedX = stabilizedX;
        m_PrevStabilizedY = stabilizedY;
        m_Layers[m_ActiveLayerIdx].needsUpload = true;
    }
    else if (phase == StrokePhase::End) {
        m_IsStrokeActive = false;
        // FX bake may touch a halo around stroke tiles — full proxy is safest/cheapest here.
        MarkCompositeDirty();
        m_ChannelPreviewDirty = true;
        if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
            auto& al = m_Layers[m_ActiveLayerIdx];
            al.thumbDirty = true;
            // Sync filter bake on stroke end (correct seams; async was causing ghost tiles).
            if (m_EffectsPreviewEnabled && LayerFilterListHasEnabled(al.filters) && al.tileCache) {
                for (const auto& pair : m_ActiveStrokeDeltas) {
                    al.tileCache->MarkDirty(pair.second.tileX, pair.second.tileY);
                }
                RefreshFilteredCache(al, /*onlyDirtyTiles=*/true);
                al.gpuDisplayKind = 0xFF; // force GPU re-upload of filtered result
            }
            // Styles bake after stroke (sync — deferred apply raced with undo/history).
            if (LayerStylesPreviewActive(al)) {
                al.presentationDirty = true;
                al.stylesDirty = true;
                RebuildLayerPresentation(al, /*fullQuality=*/false);
                al.gpuDisplayKind = 0xFF;
            }
            if (al.HasEnabledStyles()) {
                al.presentationDirty = true;
                al.stylesDirty = true;
            }
            // Parent groups with FX need re-flatten
            int p = al.parentGroupId;
            while (p >= 0 && p < (int)m_Layers.size()) {
                if (m_Layers[p].isGroup &&
                    (m_Layers[p].HasEnabledStyles() || LayerFilterListHasEnabled(m_Layers[p].filters))) {
                    m_Layers[p].presentationDirty = true;
                    m_Layers[p].stylesDirty = true;
                }
                p = m_Layers[p].parentGroupId;
            }
        }
        if (!m_ActiveStrokeDeltas.empty()) {
            auto& layer = m_Layers[m_ActiveLayerIdx];
            std::vector<TileDelta> deltas;
            deltas.reserve(m_ActiveStrokeDeltas.size());

            deltas = SealActiveStrokeDeltas(m_ActiveStrokeDeltas, layer.tileCache.get());

            auto cmd = std::make_shared<PaintStrokeCommand>(
                brush.erase ? "Eraser Stroke" : "Brush Stroke",
                m_ActiveLayerIdx,
                std::move(deltas)
            );
            m_UndoRedoManager.PushCommand(cmd);
            m_ActiveStrokeDeltas.clear();
            m_IsDocumentModified = true;
        }
    }
}

void Canvas::ResizeCanvas(ID3D11Device* device, int width, int height) {
    int oldW = m_Width;
    int oldH = m_Height;
    
    m_Width = std::max(1, std::min(width, 16384));
    m_Height = std::max(1, std::min(height, 16384));

    if (m_Width == oldW && m_Height == oldH) return;

    Logger::Get().Info("Resizing canvas from " + std::to_string(oldW) + "x" + std::to_string(oldH) +
                       " to " + std::to_string(m_Width) + "x" + std::to_string(m_Height));

    // Resize selection mask only if one was allocated (lazy for large docs).
    {
        std::vector<uint8_t> oldSel = std::move(m_SelectionMask);
        m_HasSelection = false;
        if (!oldSel.empty()) {
            m_SelectionMask.assign((size_t)m_Width * m_Height, 0);
            int copyW = std::min(oldW, m_Width);
            int copyH = std::min(oldH, m_Height);
            for (int y = 0; y < copyH; ++y) {
                for (int x = 0; x < copyW; ++x) {
                    uint8_t v = oldSel[(size_t)y * oldW + x];
                    m_SelectionMask[(size_t)y * m_Width + x] = v;
                    if (v > 0) m_HasSelection = true;
                }
            }
        } else {
            m_SelectionMask.clear();
        }
    }

    // Recreate composition texture when a device is available
    if (device) {
        CreateCompositeResources(device);
    }

    // Resize each layer's TileCache and GPU texture
    for (size_t i = 0; i < m_Layers.size(); ++i) {
        auto& layer = m_Layers[i];
        if (layer.isGroup) continue;

        if (layer.tileCache) {
            layer.tileCache->Resize(m_Width, m_Height);
            layer.tileCache->MarkAllDirty();
        }

        if (device) {
            RecreateLayerTexture(device, layer);
        }

        // Resize mask
        if (layer.hasMask && !layer.mask.empty()) {
            std::vector<uint8_t> oldMask = std::move(layer.mask);
            layer.mask.assign((size_t)m_Width * m_Height, 255);
            int copyW = std::min(oldW, m_Width);
            int copyH = std::min(oldH, m_Height);
            for (int y = 0; y < copyH; ++y) {
                for (int x = 0; x < copyW; ++x) {
                    layer.mask[(size_t)y * m_Width + x] = oldMask[(size_t)y * oldW + x];
                }
            }
            if (device) {
                UpdateLayerMaskTexture(device, (int)i);
            }
        }
    }

    m_CompositeDirty = true;
}


void Canvas::Update(float viewportWidth, float viewportHeight, bool isMouseOverCanvas, 
                    float mouseX, float mouseY, bool isDragging, float dragDx, float dragDy, float wheelDelta) {
    if (isDragging) {
        m_Pan.x += dragDx;
        m_Pan.y += dragDy;
    }

    if (isMouseOverCanvas && wheelDelta != 0.0f) {
        float zoomFactor = (wheelDelta > 0.0f) ? 1.15f : 0.85f;
        float oldZoom = m_Zoom;
        m_Zoom = std::clamp(m_Zoom * zoomFactor, 0.05f, 64.0f);

        float originX = std::floor(m_Pan.x + viewportWidth * 0.5f);
        float originY = std::floor(m_Pan.y + viewportHeight * 0.5f);

        float mouseInCanvasX = (mouseX - originX) / oldZoom;
        float mouseInCanvasY = (mouseY - originY) / oldZoom;

        m_Pan.x = mouseX - mouseInCanvasX * m_Zoom - viewportWidth * 0.5f;
        m_Pan.y = mouseY - mouseInCanvasY * m_Zoom - viewportHeight * 0.5f;
    }
}

void Canvas::ComposeLayers(ID3D11DeviceContext* context) {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (device)
        EnsureGpuBlur(device, context);
    // Phase C: drain finished scheduler jobs (budgeted apply).
    if (UpdateScheduler::Get().PendingCount() > 0)
        UpdateScheduler::Get().Poll(m_IsStrokeActive ? 1.5 : 6.0);
    PollAsyncFilterResults();
    // Moving floating content: dirty its AABB (not full proxy every frame).
    if (m_IsMovingPixels && m_FloatingBBoxW > 0 && m_FloatingBBoxH > 0) {
        const float sx = std::max(std::fabs(m_FloatingScaleX), 1.f);
        const float sy = std::max(std::fabs(m_FloatingScaleY), 1.f);
        const int pad = (int)std::ceil(std::max(m_FloatingBBoxW * sx, m_FloatingBBoxH * sy) * 0.15f) + 8;
        int x0 = m_FloatingBBoxX + m_FloatingOffsetX - pad;
        int y0 = m_FloatingBBoxY + m_FloatingOffsetY - pad;
        int x1 = m_FloatingBBoxX + m_FloatingBBoxW + m_FloatingOffsetX + pad;
        int y1 = m_FloatingBBoxY + m_FloatingBBoxH + m_FloatingOffsetY + pad;
        // also cover original cut region
        InvalidateCompositeRect(m_FloatingBBoxX - 2, m_FloatingBBoxY - 2,
            m_FloatingBBoxX + m_FloatingBBoxW + 2, m_FloatingBBoxY + m_FloatingBBoxH + 2);
        InvalidateCompositeRect(x0, y0, x1, y1);
    }
    bool needsCompositeRebuild = m_CompositeDirty || m_IsMovingPixels;

    if (device) {
        // Recreate composite after geometry undo/crop when RT was released.
        if (!m_CompositeRTV || !m_CompositeSRV) {
            CreateCompositeResources(device);
            needsCompositeRebuild = true;
        }
        for (size_t i = 0; i < m_Layers.size(); ++i) {
            auto& layer = m_Layers[i];
            if (layer.isGroup) continue;

            // Fill layer: solid 1×1, GPU pattern texture, or FX presentation bake
            if (layer.IsFill()) {
                bool fillDirty = layer.needsUpload || layer.stylesDirty || layer.presentationDirty ||
                                 layer.filtersDirty || !layer.texture || !layer.srv;
                // Texture without FX: also dirty if pattern missing after async load
                if (!fillDirty && layer.fill.HasTexture() && !LayerFxPreviewActive(layer) &&
                    !layer.fillPatternSRV)
                    fillDirty = true;
                // Invisible fill must not force a full recomposite every frame.
                if (fillDirty) {
                    if (layer.visible) {
                        EnsureFillLayerGpu(device, layer);
                        needsCompositeRebuild = true;
                        layer.thumbDirty = true;
                        m_ChannelPreviewDirty = true;
                    } else {
                        // Defer GPU; clear sticky flags (texture no longer forces full bake).
                        if (!LayerFxPreviewActive(layer)) {
                            layer.needsUpload = false;
                            layer.presentationDirty = false;
                            layer.stylesDirty = false;
                        }
                    }
                }
                // FX-only full-buffer upload (styles/filters). Texture alone uses GPU pattern.
                if (layer.visible && LayerFxPreviewActive(layer)) {
                    if (m_EffectsPreviewEnabled && LayerFilterListHasEnabled(layer.filters) &&
                        (layer.filtersDirty || !layer.filteredCache || layer.filteredCache->IsEmpty()))
                        RefreshFilteredCache(layer, /*onlyDirtyTiles=*/false);
                    if (LayerStylesPreviewActive(layer) && !m_IsStrokeActive &&
                        (layer.presentationDirty || layer.stylesDirty))
                        RebuildLayerPresentation(layer);
                    TileCache* src = nullptr;
                    if (LayerStylesPreviewActive(layer) && layer.presentationCache &&
                        !layer.presentationCache->IsEmpty())
                        src = layer.presentationCache.get();
                    else if (m_EffectsPreviewEnabled && layer.filteredCache &&
                             !layer.filteredCache->IsEmpty())
                        src = layer.filteredCache.get();
                    if (src) {
                        if (!layer.texture || [&]() {
                                D3D11_TEXTURE2D_DESC d{}; layer.texture->GetDesc(&d);
                                return d.Width != (UINT)m_Width || d.Height != (UINT)m_Height;
                            }()) {
                            RecreateLayerTexture(device, layer);
                        }
                        const int fbpp = src->GetBytesPerPixel();
                        src->ForEachDirtyTile([&](int tx, int ty, const uint8_t* data, int) {
                            UploadLayerTile(context, layer.texture, tx, ty, data, fbpp, m_Width, m_Height);
                        });
                        src->ClearAllDirty();
                        layer.needsUpload = false;
                        needsCompositeRebuild = true;
                        MarkCompositeDirty();
                    }
                }
                if (layer.visible && layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV)) {
                    UpdateLayerMaskTexture(device, static_cast<int>(i));
                    needsCompositeRebuild = true;
                }
                continue;
            }

            if (!layer.texture && !layer.gpuSurfaceId) {
                // Retry GPU create only on explicit dirty / pending work / styles —
                // NOT merely "has CPU tiles" (that spun after soft VRAM refuse every frame).
                if (layer.needsUpload || layer.HasEnabledStyles() ||
                    (layer.tileCache && layer.tileCache->HasPendingGpuWork())) {
                    RecreateLayerTexture(device, layer);
                }
                if (!layer.texture && !layer.gpuSurfaceId) continue;
            }

            // Filters: NEVER refilter mid-stroke on the active paint layer.
            // Stroke end bakes FX (sync RefreshFilteredCache — reliable seams).
            const bool isActivePaintLayer =
                m_IsStrokeActive && (int)i == m_ActiveLayerIdx;
            bool filtersWereDirty = false;
            // Do NOT thrash compose every frame for sticky stylesDirty mid-stroke
            // (styles bake is deferred until pen-up — avoids lag when Outline/Shadow exist).
            bool stylesWereDirty = !m_IsStrokeActive && LayerStylesPreviewActive(layer) &&
                (layer.stylesDirty || layer.presentationDirty);

            if (m_EffectsPreviewEnabled && LayerFilterListHasEnabled(layer.filters) &&
                !isActivePaintLayer) {
                if (layer.filtersDirty || !layer.filteredCache || layer.filteredCache->IsEmpty()) {
                    RefreshFilteredCache(layer, /*onlyDirtyTiles=*/false);
                    filtersWereDirty = true;
                } else if (layer.tileCache && layer.tileCache->HasPendingGpuWork()) {
                    RefreshFilteredCache(layer, /*onlyDirtyTiles=*/true);
                    filtersWereDirty = true;
                }
            } else if (!LayerFilterListHasEnabled(layer.filters) && layer.filteredCache) {
                // Filter disabled/removed: drop filtered cache so GPU falls back to raw immediately
                layer.filteredCache.reset();
                layer.filtersDirty = false;
                if (layer.tileCache) layer.tileCache->MarkAllDirty();
                layer.gpuDisplayKind = 0xFF;
                filtersWereDirty = true;
            }
            if (LayerStylesPreviewActive(layer) && !m_IsStrokeActive)
                RebuildLayerPresentation(layer);
            // Styles removed: drop presentation so we don't keep drawing baked styles
            if (!layer.HasEnabledStyles() && layer.presentationCache) {
                layer.presentationCache.reset();
                layer.stylesDirty = false;
                layer.presentationDirty = false;
                if (layer.tileCache) layer.tileCache->MarkAllDirty();
                layer.gpuDisplayKind = 0xFF;
                stylesWereDirty = true;
            }

            // Pick source: presentation (styles, idle) > filtered (idle FX) > raw
            // Active stroke always uses raw tileCache for lightning-fast dabs.
            TileCache* src = nullptr;
            uint8_t wantKind = 0; // 0=raw 1=filtered 2=presentation
            bool layerNeedsUpload = layer.needsUpload;
            const bool usePres = !isActivePaintLayer && LayerStylesPreviewActive(layer) &&
                !m_IsStrokeActive &&
                layer.presentationCache && !layer.presentationCache->IsEmpty();
            if (usePres) {
                src = layer.presentationCache.get();
                wantKind = 2;
            } else if (!isActivePaintLayer && m_EffectsPreviewEnabled &&
                       LayerFilterListHasEnabled(layer.filters) &&
                       layer.filteredCache && !layer.filteredCache->IsEmpty() && !layer.filtersDirty) {
                src = layer.filteredCache.get();
                wantKind = 1;
            }
            if (!src) {
                src = layer.tileCache.get();
                wantKind = 0;
            }
            if (!src) continue;

            // Ensure correct GPU backing: styles → classic full texture; else tiled if large.
            if (layer.HasEnabledStyles() && layer.gpuSurfaceId) {
                // Switch to classic for styles
                RecreateLayerTexture(device, layer);
            } else if (!layer.HasEnabledStyles() && UseTiledGpuForLayer(layer) &&
                       !layer.gpuSurfaceId && !layer.IsFill()) {
                RecreateLayerTexture(device, layer);
            } else if (!layer.texture && !layer.gpuSurfaceId && !layer.IsFill()) {
                RecreateLayerTexture(device, layer);
            }

            // Display source changed (raw↔filtered↔presentation): full re-upload
            if (layer.gpuDisplayKind != wantKind) {
                if (layer.gpuSurfaceId)
                    m_GpuTiles.ClearSurface(layer.gpuSurfaceId);
                src->MarkAllDirty();
                layer.gpuDisplayKind = wantKind;
                layerNeedsUpload = true;
            }

            // needsUpload alone must not no-op: ensure CPU dirt / GPU clear queue exists
            if (layerNeedsUpload && !src->HasPendingGpuWork()) {
                if (!src->IsEmpty())
                    src->MarkAllDirty();
                else if (layer.gpuSurfaceId)
                    m_GpuTiles.ClearSurface(layer.gpuSurfaceId);
            }

            bool layerHadUploads = false;
            size_t dirtyUploaded = 0;
            const bool hadPending = src->HasPendingGpuWork() || layerNeedsUpload;
            const int bpp = src->GetBytesPerPixel();

            // Phase C LOD stroke: budget mid-stroke uploads (nearest pen first).
            // Idle path flushes everything (same as Phase B).
            const bool isActiveStrokeLayer =
                m_IsStrokeActive && (int)i == m_ActiveLayerIdx;

            if (!isActiveStrokeLayer) {
                // ClearDirty after ForEach (not during) — pending-clear set is iterated.
                std::vector<std::pair<int,int>> uploaded;
                uploaded.reserve(64);
                bool anyUploadFail = false;
                src->ForEachDirtyTile([&](int tx, int ty, const uint8_t* data, int /*pitch*/) {
                    layerHadUploads = true;
                    ++dirtyUploaded;
                    if (UploadLayerContentTile(device, context, layer, tx, ty, data, bpp)) {
                        uploaded.emplace_back(tx, ty);
                        InvalidateCompositeRect(tx * TILE_SIZE, ty * TILE_SIZE,
                            std::min(m_Width - 1, tx * TILE_SIZE + TILE_SIZE - 1),
                            std::min(m_Height - 1, ty * TILE_SIZE + TILE_SIZE - 1));
                    } else {
                        anyUploadFail = true;
                    }
                });
                for (const auto& p : uploaded)
                    src->ClearDirty(p.first, p.second);
                // Never wipe all dirty after partial fail — leaves permanent GPU tile holes.
                layer.needsUpload = anyUploadFail || src->HasPendingGpuWork();
                if (layer.needsUpload)
                    m_CompositeDirty = true;
            } else {
                struct Key { int tx, ty; float d2; };
                std::vector<Key> keys;
                keys.reserve(32);
                src->ForEachDirtyTile([&](int tx, int ty, const uint8_t*, int) {
                    float cx = (float)(tx * TILE_SIZE + TILE_SIZE / 2) - m_PrevStabilizedX;
                    float cy = (float)(ty * TILE_SIZE + TILE_SIZE / 2) - m_PrevStabilizedY;
                    keys.push_back({ tx, ty, cx * cx + cy * cy });
                });
                std::sort(keys.begin(), keys.end(),
                    [](const Key& a, const Key& b) { return a.d2 < b.d2; });
                const int n = std::min((int)keys.size(), kLodStrokeUploadBudget);
                for (int di = 0; di < n; ++di) {
                    const int tx = keys[(size_t)di].tx, ty = keys[(size_t)di].ty;
                    const uint8_t* data = src->GetTileData(tx, ty);
                    static thread_local std::vector<uint8_t> zeroTile;
                    if (!data) {
                        const size_t need = (size_t)TILE_SIZE * TILE_SIZE * (size_t)std::max(1, bpp);
                        if (zeroTile.size() < need) zeroTile.assign(need, 0);
                        data = zeroTile.data();
                    }
                    if (UploadLayerContentTile(device, context, layer, tx, ty, data, bpp)) {
                        src->ClearDirty(tx, ty);
                        InvalidateCompositeRect(tx * TILE_SIZE, ty * TILE_SIZE,
                            std::min(m_Width - 1, tx * TILE_SIZE + TILE_SIZE - 1),
                            std::min(m_Height - 1, ty * TILE_SIZE + TILE_SIZE - 1));
                        layerHadUploads = true;
                        ++dirtyUploaded;
                    }
                }
                const bool lodDeferred = src->HasPendingGpuWork();
                layer.needsUpload = lodDeferred;
                if (lodDeferred)
                    m_CompositeDirty = true;
            }

            if (layerHadUploads || filtersWereDirty || stylesWereDirty || layerNeedsUpload ||
                hadPending || layer.needsUpload) {
                needsCompositeRebuild = true;
                if (!m_IsStrokeActive) {
                    layer.thumbDirty = true;
                    m_ChannelPreviewDirty = true;
                }
            }

            if (layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV)) {
                UpdateLayerMaskTexture(device, static_cast<int>(i));
                needsCompositeRebuild = true;
            }
        }

        // Selection mask: create GPU texture if needed + upload (ants + future GPU tools).
        // Previous path only uploaded when texture already existed → Select All / undo
        // after dormancy left m_HasSelection true but no SRV (invisible ants).
        if (m_HasSelection && (m_SelectionMaskNeedsUpload || !m_SelectionMaskSRV)) {
            UpdateSelectionMaskTexture(device);
        } else if (!m_HasSelection && m_SelectionMaskNeedsUpload && m_SelectionMaskTexture) {
            UpdateSelectionMaskTexture(device); // clear GPU mask on deselect
        }

        device->Release();
    }

    if (!m_CompositeRTV || !m_CompositeSRV) return;
    if (!needsCompositeRebuild) return;

    // Save previous targets & viewport
    ID3D11RenderTargetView* prevRTV = nullptr;
    ID3D11DepthStencilView* prevDSV = nullptr;
    context->OMGetRenderTargets(1, &prevRTV, &prevDSV);

    UINT numViewports = 1;
    D3D11_VIEWPORT prevViewport = {};
    context->RSGetViewports(&numViewports, &prevViewport);

    // Render into the proxy-sized composite target. The final viewport draw
    // stretches this texture to screen size, avoiding a 16K full-frame pass.
    D3D11_VIEWPORT compViewport = {};
    compViewport.Width = static_cast<float>(std::max(1, m_CompositeWidth));
    compViewport.Height = static_cast<float>(std::max(1, m_CompositeHeight));
    compViewport.MinDepth = 0.0f;
    compViewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &compViewport);

    context->OMSetRenderTargets(1, &m_CompositeRTV, nullptr);

    // --- Dirty-rect compose: clear + scissor only the changed proxy region ---
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool useScissor = false;
    D3D11_RECT scissorRect = {};
    if (!m_CompositeDirtyFull && m_CompositeDirtyValid && m_Width > 0 && m_Height > 0 &&
        m_CompositeWidth > 0 && m_CompositeHeight > 0) {
        const float sx = (float)m_CompositeWidth / (float)m_Width;
        const float sy = (float)m_CompositeHeight / (float)m_Height;
        int px0 = (int)std::floor(m_CompositeDirtyX0 * sx) - 2;
        int py0 = (int)std::floor(m_CompositeDirtyY0 * sy) - 2;
        int px1 = (int)std::ceil((m_CompositeDirtyX1 + 1) * sx) + 2;
        int py1 = (int)std::ceil((m_CompositeDirtyY1 + 1) * sy) + 2;
        px0 = std::max(0, px0); py0 = std::max(0, py0);
        px1 = std::min(m_CompositeWidth, px1); py1 = std::min(m_CompositeHeight, py1);
        const int area = std::max(0, px1 - px0) * std::max(0, py1 - py0);
        const int fullA = m_CompositeWidth * m_CompositeHeight;
        if (px1 > px0 && py1 > py0 && fullA > 0 && area * 100 <= fullA * 85) {
            useScissor = true;
            scissorRect.left = px0; scissorRect.top = py0;
            scissorRect.right = px1; scissorRect.bottom = py1;
        }
    }

    if (useScissor) {
        // ClearView respects rect (D3D11.1); ClearRenderTargetView does not.
        ID3D11DeviceContext1* ctx1 = nullptr;
        if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1)) && ctx1) {
            ctx1->ClearView(m_CompositeRTV, clearColor, &scissorRect, 1);
            ctx1->Release();
        } else {
            // No 11.1: full clear + still scissor draws (safe overdraw of layers)
            context->ClearRenderTargetView(m_CompositeRTV, clearColor);
        }
        context->RSSetScissorRects(1, &scissorRect);
        if (m_RasterizerStateScissor)
            context->RSSetState(m_RasterizerStateScissor);
    } else {
        context->ClearRenderTargetView(m_CompositeRTV, clearColor);
        if (m_RasterizerState)
            context->RSSetState(m_RasterizerState);
    }

    // Bind resources
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, &m_VertexBuffer, &stride, &offset);
    context->IASetIndexBuffer(m_IndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    context->IASetInputLayout(m_InputLayout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context->VSSetShader(m_LayerVertexShader, nullptr, 0);
    context->PSSetShader(m_LayerBlendPixelShader, nullptr, 0);
    context->PSSetSamplers(0, 1, &m_SamplerState);
    context->PSSetConstantBuffers(0, 1, &m_ConstantBuffer);

    // Draw visible layers bottom-to-top.
    // Top-level only: children of groups are drawn into group RT first (isolation).
    // First layer: REPLACE full RGBA. Later: SRC_ALPHA.
    // layer.opacity = content/fill opacity; styles bake their own opacity into presentation.
    bool firstVisible = true;
    auto drawLayerSrv = [&](Layer& layer, ID3D11ShaderResourceView* srv, bool useMask, float opacityMul,
                            bool fillTexMode = false) {
        if (!srv) return false;
        const bool isFirst = firstVisible && m_LayerBlendStateReplace;
        ID3D11BlendState* blend = nullptr;
        // Fill: per-map channel write mask (unchecked = leave underlay; never black-fill)
        uint8_t fillWrite = 0xF;
        float fillTint[4] = {1.f, 1.f, 1.f, 1.f};
        if (layer.IsFill()) {
            if (!layer.fill.ResolveForMap(m_ActiveSetMaps, m_ViewMapKind, fillTint, &fillWrite))
                return false;
            ID3D11Device* dev = nullptr;
            context->GetDevice(&dev);
            blend = GetFillWriteBlend(dev, fillWrite, isFirst);
            if (dev) dev->Release();
        } else if (isFirst) {
            blend = m_LayerBlendStateReplace;
        } else if (!layer.alphaRewrite && m_LayerBlendStateAlphaPreserve) {
            blend = m_LayerBlendStateAlphaPreserve;
        } else {
            blend = m_LayerBlendState;
        }
        context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            LayerBuffer* lb = (LayerBuffer*)mapped.pData;
            float hasMaskVal = (useMask && layer.hasMask && layer.maskSRV) ? 1.0f : 0.0f;
            lb->layerParams = DirectX::XMFLOAT4(opacityMul, hasMaskVal, 0.0f, 0.0f);
            if (fillTexMode) {
                // w=2 → PSLayerBlend fill-texture path (wrap sample + tint)
                lb->transformParams = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 2.0f);
                lb->floatRect = DirectX::XMFLOAT4(
                    layer.fill.texScale[0], layer.fill.texScale[1],
                    layer.fill.texOffset[0], layer.fill.texOffset[1]);
                lb->fillColor = DirectX::XMFLOAT4(fillTint[0], fillTint[1], fillTint[2], fillTint[3]);
            } else {
                lb->transformParams = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);
                lb->floatRect = DirectX::XMFLOAT4(0.f, 0.f, 0.f, 0.f);
                lb->fillColor = DirectX::XMFLOAT4(1.f, 1.f, 1.f, 1.f);
            }
            float flags = (layer.alphaRewrite ? 1.f : 0.f) + (isFirst ? 2.f : 0.f);
            lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)layer.blendMode, flags);
            context->Unmap(m_LayerConstantBuffer, 0);
        }
        context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
        context->PSSetShaderResources(0, 1, &srv);
        if (useMask && layer.hasMask && layer.maskSRV)
            context->PSSetShaderResources(1, 1, &layer.maskSRV);
        else {
            ID3D11ShaderResourceView* nullSRV = nullptr;
            context->PSSetShaderResources(1, 1, &nullSRV);
        }
        if (layer.blendMode != BlendMode::Normal && m_CompositeHistoryTexture && m_CompositeHistorySRV) {
            context->CopyResource(m_CompositeHistoryTexture, m_CompositeTexture);
            context->PSSetShaderResources(2, 1, &m_CompositeHistorySRV);
        } else {
            ID3D11ShaderResourceView* nullHist = nullptr;
            context->PSSetShaderResources(2, 1, &nullHist);
        }
        context->OMSetRenderTargets(1, &m_CompositeRTV, nullptr);
        context->DrawIndexed(6, 0, 0);
        ID3D11ShaderResourceView* nullSRV2 = nullptr;
        context->PSSetShaderResources(2, 1, &nullSRV2);
        firstVisible = false;
        return true;
    };

    // Ensure group composite resources for groups with children
    ID3D11Device* groupDev = nullptr;
    context->GetDevice(&groupDev);
    if (groupDev && !m_GroupCompositeRTV && m_CompositeWidth > 0)
        CreateGroupCompositeResources(groupDev);

    // View filter: layers that opt into active map (+ underlay for imported maps)
    const texset::MapKind viewMap = m_ViewMapKind;
    const bool roleIso = m_ViewRoleIsolate;
    const texset::ChannelRole soloRole = m_ViewSoloRole;

    // Maps are layers — no hidden underlay. Filter by workSpace below.

    for (size_t i = 0; i < m_Layers.size(); ++i) {
        Layer& layer = m_Layers[i];
        // Only top-level entries; grouped children rendered inside parent group
        if (layer.parentGroupId >= 0) continue;
        if (!layer.visible) continue;
        if (!layer.isGroup &&
            !layer.ParticipatesInView(viewMap, roleIso, soloRole))
            continue;

        if (layer.isGroup) {
            const bool groupHasFx = LayerFxPreviewActive(layer);

            // Live Group FX: CPU flatten children + group styles/filters → group texture
            if (groupHasFx && groupDev) {
                if (layer.presentationDirty || layer.stylesDirty || layer.filtersDirty ||
                    !layer.presentationCache || layer.presentationCache->IsEmpty()) {
                    RebuildGroupPresentation((int)i, /*fullQuality=*/false);
                }
                // Still dirty after rebuild (e.g. was skipped) → drop stale bake so we never
                // show pre-membership flatten. Mid-stroke keep last bake (stroke-end rebuilds).
                if (!m_IsStrokeActive &&
                    (layer.presentationDirty || layer.stylesDirty || layer.filtersDirty) &&
                    layer.presentationCache) {
                    layer.presentationCache.reset();
                    layer.needsUpload = true;
                }
                if (layer.presentationCache && !layer.presentationCache->IsEmpty()) {
                    TileCache* src = layer.presentationCache.get();
                    const int pw = src->GetWidth();
                    const int ph = src->GetHeight();
                    // Proxy group bake may be smaller than document — match texture to cache.
                    bool sizeOk = false;
                    if (layer.texture) {
                        D3D11_TEXTURE2D_DESC d{};
                        layer.texture->GetDesc(&d);
                        sizeOk = (int)d.Width == pw && (int)d.Height == ph;
                    }
                    if (!layer.texture || !sizeOk) {
                        if (layer.texture) { DeferReleaseTex(layer.texture); layer.texture = nullptr; }
                        if (layer.srv) { DeferReleaseSRV(layer.srv); layer.srv = nullptr; }
                        if (pw > 0 && ph > 0 &&
                            MemoryStats::TryReserveVram(
                                MemoryStats::EstimateImageBytes(pw, ph, BytesPerPixel(m_CanvasFormat)),
                                "GroupPresentation.tex")) {
                            D3D11_TEXTURE2D_DESC desc = {};
                            desc.Width = (UINT)pw;
                            desc.Height = (UINT)ph;
                            desc.MipLevels = 1;
                            desc.ArraySize = 1;
                            desc.Format = GetLayerDxgiFormat();
                            desc.SampleDesc.Count = 1;
                            desc.Usage = D3D11_USAGE_DEFAULT;
                            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                            if (SUCCEEDED(groupDev->CreateTexture2D(&desc, nullptr, &layer.texture)) &&
                                layer.texture) {
                                groupDev->CreateShaderResourceView(layer.texture, nullptr, &layer.srv);
                                src->MarkAllDirty();
                            } else {
                                MemoryStats::ReleaseVram(
                                    MemoryStats::EstimateImageBytes(pw, ph, BytesPerPixel(m_CanvasFormat)));
                            }
                        }
                    }
                    if (layer.texture) {
                        const int gbpp = src->GetBytesPerPixel();
                        src->ForEachDirtyTile([&](int tx, int ty, const uint8_t* data, int) {
                            UploadLayerTile(context, layer.texture, tx, ty, data, gbpp, pw, ph);
                        });
                        src->ClearAllDirty();
                        layer.needsUpload = false;
                    }
                }
                // Upload group mask if needed (groups skip the raster upload loop)
                if (layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV) && groupDev)
                    UpdateLayerMaskTexture(groupDev, (int)i);

                if (layer.srv) {
                    // Presentation already bakes group fill + styles → draw at opacity 1
                    // Group mask (if any) multiplies coverage when not baked into presentation.
                    // When presentation was rebuilt with mask, double-mask is mild darkening at edges —
                    // prefer GPU mask only if bake didn't include mask (empty presentation mask path).
                    const bool useGroupMask = layer.hasMask && layer.maskSRV;
                    const bool gFirst = firstVisible && m_LayerBlendStateReplace;
                    ID3D11BlendState* gBlend = gFirst ? m_LayerBlendStateReplace : m_LayerBlendState;
                    context->OMSetBlendState(gBlend, nullptr, 0xFFFFFFFF);
                    context->OMSetRenderTargets(1, &m_CompositeRTV, nullptr);
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        LayerBuffer* lb = (LayerBuffer*)mapped.pData;
                        // Mask already in presentation bake when RebuildGroupPresentation ran with mask —
                        // pass hasMask=0 to avoid double application.
                        lb->layerParams = DirectX::XMFLOAT4(1.f, 0.f, 0.f, 0.f);
                        lb->transformParams = DirectX::XMFLOAT4(1.f, 1.f, 0.f, 0.f);
                        float flags = 1.f + (gFirst ? 2.f : 0.f);
                        lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)layer.blendMode, flags);
                        lb->floatRect = DirectX::XMFLOAT4(0.f, 0.f, 0.f, 0.f);
                        lb->fillColor = DirectX::XMFLOAT4(1.f, 1.f, 1.f, 1.f);
                        context->Unmap(m_LayerConstantBuffer, 0);
                    }
                    context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
                    context->PSSetShaderResources(0, 1, &layer.srv);
                    {
                        ID3D11ShaderResourceView* n = nullptr;
                        context->PSSetShaderResources(1, 1, &n);
                        context->PSSetShaderResources(2, 1, &n);
                    }
                    if (layer.blendMode != BlendMode::Normal && m_CompositeHistoryTexture && m_CompositeHistorySRV) {
                        context->CopyResource(m_CompositeHistoryTexture, m_CompositeTexture);
                        context->PSSetShaderResources(2, 1, &m_CompositeHistorySRV);
                    }
                    context->DrawIndexed(6, 0, 0);
                    firstVisible = false;
                    (void)useGroupMask;
                }
                continue;
            }

            // No group FX: GPU isolate children into group RT, then blend with group opacity + group mask
            if (layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV) && groupDev)
                UpdateLayerMaskTexture(groupDev, (int)i);
            if (!m_GroupCompositeRTV || !m_GroupCompositeSRV) continue;
            float clearG[4] = {0,0,0,0};
            context->ClearRenderTargetView(m_GroupCompositeRTV, clearG);

            bool groupHadContent = false;
            bool firstInGroup = true;
            for (size_t ci = 0; ci < m_Layers.size(); ++ci) {
                Layer& child = m_Layers[ci];
                // Direct children only for GPU path (nested groups with no FX on parent)
                if ((int)child.parentGroupId != (int)i || !child.visible || child.isGroup) continue;
                if (!child.ParticipatesInView(viewMap, roleIso, soloRole)) continue;

                const bool childBaked = LayerStylesPreviewActive(child) && !m_IsStrokeActive &&
                    child.presentationCache && !child.presentationCache->IsEmpty();
                float childOp = childBaked ? 1.f : child.opacity;
                bool childMask = !childBaked && child.hasMask && child.maskSRV;

                // Large docs: children use sparse GpuTileStore (no classic layer.srv).
                // Old path skipped them entirely → empty/stale group until a stroke forced rebind.
                if (child.gpuSurfaceId && !childBaked && !child.HasEnabledStyles()) {
                    DrawTiledLayer(context, child, childOp, childMask, firstInGroup, m_GroupCompositeRTV);
                    // Restore full-quad VS buffers after tile quads
                    UINT stride = sizeof(Vertex);
                    UINT offset = 0;
                    context->IASetVertexBuffers(0, 1, &m_VertexBuffer, &stride, &offset);
                    context->IASetIndexBuffer(m_IndexBuffer, DXGI_FORMAT_R16_UINT, 0);
                    context->VSSetShader(m_LayerVertexShader, nullptr, 0);
                    context->PSSetShader(m_LayerBlendPixelShader, nullptr, 0);
                    firstInGroup = false;
                    groupHadContent = true;
                    continue;
                }

                // GPU fill pattern into group RT (do not use drawLayerSrv — it mutates main firstVisible)
                if (child.IsFill() && child.fillPatternSRV && !LayerFxPreviewActive(child) &&
                    child.fill.HasTexture()) {
                    context->OMSetRenderTargets(1, &m_GroupCompositeRTV, nullptr);
                    float fillTint[4] = {1.f, 1.f, 1.f, 1.f};
                    uint8_t fillWrite = 0xF;
                    if (!child.fill.ResolveForMap(m_ActiveSetMaps, m_ViewMapKind, fillTint, &fillWrite))
                        continue;
                    ID3D11Device* fdev = nullptr;
                    context->GetDevice(&fdev);
                    ID3D11BlendState* fblend = GetFillWriteBlend(fdev, fillWrite, firstInGroup);
                    if (fdev) fdev->Release();
                    context->OMSetBlendState(fblend, nullptr, 0xFFFFFFFF);
                    D3D11_MAPPED_SUBRESOURCE fmapped;
                    if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &fmapped))) {
                        LayerBuffer* lb = (LayerBuffer*)fmapped.pData;
                        float hasMaskVal = childMask ? 1.f : 0.f;
                        lb->layerParams = DirectX::XMFLOAT4(childOp, hasMaskVal, 0.f, 0.f);
                        lb->transformParams = DirectX::XMFLOAT4(1.f, 1.f, 0.f, 2.f); // fill-texture mode
                        lb->floatRect = DirectX::XMFLOAT4(
                            child.fill.texScale[0], child.fill.texScale[1],
                            child.fill.texOffset[0], child.fill.texOffset[1]);
                        lb->fillColor = DirectX::XMFLOAT4(fillTint[0], fillTint[1], fillTint[2], fillTint[3]);
                        float flags = (child.alphaRewrite ? 1.f : 0.f) + (firstInGroup ? 2.f : 0.f);
                        lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)child.blendMode, flags);
                        context->Unmap(m_LayerConstantBuffer, 0);
                    }
                    context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
                    context->PSSetShaderResources(0, 1, &child.fillPatternSRV);
                    if (childMask)
                        context->PSSetShaderResources(1, 1, &child.maskSRV);
                    else {
                        ID3D11ShaderResourceView* n = nullptr;
                        context->PSSetShaderResources(1, 1, &n);
                    }
                    {
                        ID3D11ShaderResourceView* n = nullptr;
                        context->PSSetShaderResources(2, 1, &n);
                    }
                    context->DrawIndexed(6, 0, 0);
                    firstInGroup = false;
                    groupHadContent = true;
                    continue;
                }

                if (!child.srv) continue;

                context->OMSetRenderTargets(1, &m_GroupCompositeRTV, nullptr);
                ID3D11BlendState* blend = firstInGroup && m_LayerBlendStateReplace
                    ? m_LayerBlendStateReplace
                    : ((!child.alphaRewrite && m_LayerBlendStateAlphaPreserve)
                        ? m_LayerBlendStateAlphaPreserve : m_LayerBlendState);
                context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    LayerBuffer* lb = (LayerBuffer*)mapped.pData;
                    float hasMaskVal = childMask ? 1.f : 0.f;
                    lb->layerParams = DirectX::XMFLOAT4(childOp, hasMaskVal, 0.f, 0.f);
                    lb->transformParams = DirectX::XMFLOAT4(1.f, 1.f, 0.f, 0.f);
                    float flags = (child.alphaRewrite ? 1.f : 0.f) + (firstInGroup ? 2.f : 0.f);
                    lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)child.blendMode, flags);
                    lb->floatRect = DirectX::XMFLOAT4(0.f, 0.f, 0.f, 0.f);
                    lb->fillColor = DirectX::XMFLOAT4(1.f, 1.f, 1.f, 1.f);
                    context->Unmap(m_LayerConstantBuffer, 0);
                }
                context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
                context->PSSetShaderResources(0, 1, &child.srv);
                if (childMask)
                    context->PSSetShaderResources(1, 1, &child.maskSRV);
                else {
                    ID3D11ShaderResourceView* n = nullptr;
                    context->PSSetShaderResources(1, 1, &n);
                }
                ID3D11ShaderResourceView* nullHist = nullptr;
                context->PSSetShaderResources(2, 1, &nullHist);
                context->DrawIndexed(6, 0, 0);
                firstInGroup = false;
                groupHadContent = true;
            }

            if (!groupHadContent)
                continue;

            context->OMSetRenderTargets(1, &m_CompositeRTV, nullptr);
            const bool gFirst = firstVisible && m_LayerBlendStateReplace;
            const bool gMask = layer.hasMask && layer.maskSRV;
            ID3D11BlendState* gBlend = gFirst ? m_LayerBlendStateReplace : m_LayerBlendState;
            context->OMSetBlendState(gBlend, nullptr, 0xFFFFFFFF);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                LayerBuffer* lb = (LayerBuffer*)mapped.pData;
                lb->layerParams = DirectX::XMFLOAT4(layer.opacity, gMask ? 1.f : 0.f, 0.f, 0.f);
                lb->transformParams = DirectX::XMFLOAT4(1.f, 1.f, 0.f, 0.f);
                float flags = 1.f + (gFirst ? 2.f : 0.f);
                lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)layer.blendMode, flags);
                lb->floatRect = DirectX::XMFLOAT4(0.f, 0.f, 0.f, 0.f);
                lb->fillColor = DirectX::XMFLOAT4(1.f, 1.f, 1.f, 1.f);
                context->Unmap(m_LayerConstantBuffer, 0);
            }
            context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
            context->PSSetShaderResources(0, 1, &m_GroupCompositeSRV);
            if (gMask)
                context->PSSetShaderResources(1, 1, &layer.maskSRV);
            else {
                ID3D11ShaderResourceView* n = nullptr;
                context->PSSetShaderResources(1, 1, &n);
            }
            {
                ID3D11ShaderResourceView* n = nullptr;
                context->PSSetShaderResources(2, 1, &n);
            }
            if (layer.blendMode != BlendMode::Normal && m_CompositeHistoryTexture && m_CompositeHistorySRV) {
                context->CopyResource(m_CompositeHistoryTexture, m_CompositeTexture);
                context->PSSetShaderResources(2, 1, &m_CompositeHistorySRV);
            }
            context->DrawIndexed(6, 0, 0);
            firstVisible = false;
            continue;
        }

        if (layer.srv || layer.gpuSurfaceId ||
            (layer.IsFill() && layer.fillPatternSRV && !LayerFxPreviewActive(layer))) {
            if (layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV)) {
                ID3D11Device* device = nullptr;
                context->GetDevice(&device);
                if (device) {
                    UpdateLayerMaskTexture(device, static_cast<int>(i));
                    device->Release();
                }
            }

            // GPU fill pattern: no full-doc texture — sample source-res pattern in shader
            if (layer.IsFill() && layer.fillPatternSRV && !LayerFxPreviewActive(layer) &&
                layer.fill.HasTexture()) {
                const bool useMask = layer.hasMask && layer.maskSRV;
                drawLayerSrv(layer, layer.fillPatternSRV, useMask, layer.opacity, /*fillTexMode=*/true);
            } else {
            // Styles baked → opacity=1, mask already in presentation
            // During stroke / FX preview OFF: live content path with fill opacity
            const bool baked = LayerStylesPreviewActive(layer) && !m_IsStrokeActive &&
                layer.presentationCache && !layer.presentationCache->IsEmpty();
            const float op = baked ? 1.f : layer.opacity;
            const bool useMask = !baked && layer.hasMask && layer.maskSRV;
            const bool isFirst = firstVisible && m_LayerBlendStateReplace;
            // Tiled GPU path only for raw/filtered (no styles). Styles use classic srv.
            if (layer.gpuSurfaceId && !baked && !layer.HasEnabledStyles()) {
                DrawTiledLayer(context, layer, op, useMask, isFirst);
                firstVisible = false;
            } else if (layer.srv) {
                drawLayerSrv(layer, layer.srv, useMask, op);
            }
            }

            if (m_IsMovingPixels && i == m_StartActiveLayerIdx && m_FloatingSRV) {
                float uOff = (float)m_FloatingOffsetX / (float)m_Width;
                float vOff = (float)m_FloatingOffsetY / (float)m_Height;
                // Cached center — never re-scan full selection mask per compose frame
                float centerX = m_FloatingCenterX / (float)m_Width;
                float centerY = m_FloatingCenterY / (float)m_Height;
                float rectU = (float)m_FloatingBBoxX / (float)m_Width;
                float rectV = (float)m_FloatingBBoxY / (float)m_Height;
                float rectSU = (float)std::max(1, m_FloatingBufW) / (float)m_Width;
                float rectSV = (float)std::max(1, m_FloatingBufH) / (float)m_Height;

                ID3D11BlendState* fBlend = layer.alphaRewrite
                    ? m_LayerBlendState
                    : (m_LayerBlendStateAlphaPreserve ? m_LayerBlendStateAlphaPreserve : m_LayerBlendState);
                context->OMSetBlendState(fBlend, nullptr, 0xFFFFFFFF);
                D3D11_MAPPED_SUBRESOURCE fMapped;
                if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &fMapped))) {
                    LayerBuffer* lb = (LayerBuffer*)fMapped.pData;
                    float hasFMaskVal = m_FloatingMaskSRV ? 1.0f : 0.0f;
                    lb->layerParams = DirectX::XMFLOAT4(layer.opacity, hasFMaskVal, uOff, vOff);
                    lb->transformParams = DirectX::XMFLOAT4(m_FloatingScaleX, m_FloatingScaleY, m_FloatingRotation, 1.0f); // isFloating = 1.0f
                    lb->centerParams = DirectX::XMFLOAT4(centerX, centerY, 0.0f, layer.alphaRewrite ? 1.0f : 0.0f);
                    lb->floatRect = DirectX::XMFLOAT4(rectU, rectV, rectSU, rectSV);
                    lb->fillColor = DirectX::XMFLOAT4(1.f, 1.f, 1.f, 1.f);
                    context->Unmap(m_LayerConstantBuffer, 0);
                }
                context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
                context->PSSetShaderResources(0, 1, &m_FloatingSRV);
                if (m_FloatingMaskSRV) {
                    context->PSSetShaderResources(1, 1, &m_FloatingMaskSRV);
                } else {
                    ID3D11ShaderResourceView* nullSRV = nullptr;
                    context->PSSetShaderResources(1, 1, &nullSRV);
                }
                context->DrawIndexed(6, 0, 0);
            }
        }
    }

    if (groupDev) groupDev->Release();

    // Restore previous target
    context->OMSetRenderTargets(1, &prevRTV, prevDSV);
    if (prevRTV) prevRTV->Release();
    if (prevDSV) prevDSV->Release();

    context->RSSetViewports(1, &prevViewport);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_CompositeDirty = false;
    m_CompositeDirtyFull = false;
    m_CompositeDirtyValid = false;
}

void Canvas::Render(ID3D11DeviceContext* context, float viewportWidth, float viewportHeight) {
    // 1. Update constant buffer first so ComposeLayers has access to up-to-date visMode
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = context->Map(m_ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        CanvasBuffer* cb = (CanvasBuffer*)mappedResource.pData;
        cb->viewportSizeAndZoom = DirectX::XMFLOAT4(viewportWidth, viewportHeight, m_Zoom, m_RotationAngle);
        cb->offsetAndCanvasSize = DirectX::XMFLOAT4(m_Pan.x, m_Pan.y, (float)m_Width, (float)m_Height);
        cb->channelMasks = DirectX::XMFLOAT4(m_ChannelR ? 1.0f : 0.0f, m_ChannelG ? 1.0f : 0.0f, m_ChannelB ? 1.0f : 0.0f, m_ChannelA ? 1.0f : 0.0f);
        cb->viewportFlags = DirectX::XMFLOAT4(m_ViewportFlipH ? 1.0f : 0.0f, m_ViewportFlipV ? 1.0f : 0.0f, 0.0f, 0.0f);
        context->Unmap(m_ConstantBuffer, 0);
    }

    // 2. Compose layers
    ComposeLayers(context);

    // 3. Draw composite texture onto viewport
    context->VSSetShader(m_VertexShader, nullptr, 0);
    context->PSSetShader(m_PixelShader, nullptr, 0);
    context->IASetInputLayout(m_InputLayout);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, &m_VertexBuffer, &stride, &offset);
    context->IASetIndexBuffer(m_IndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context->VSSetConstantBuffers(0, 1, &m_ConstantBuffer);
    context->PSSetConstantBuffers(0, 1, &m_ConstantBuffer);
    context->PSSetShaderResources(0, 1, &m_CompositeSRV);
    context->PSSetSamplers(0, 1, &m_SamplerState);

    context->RSSetState(m_RasterizerState);
    context->DrawIndexed(6, 0, 0);
    context->RSSetState(nullptr);

    // 3.5 Draw selection outline overlay if active (marching ants)
    if (m_HasSelection && m_SelectionOutlinePixelShader) {
        // Guarantee GPU mask exists even if ComposeLayers early-out skipped upload.
        if (!m_SelectionMaskSRV || m_SelectionMaskNeedsUpload) {
            ID3D11Device* dev = nullptr;
            context->GetDevice(&dev);
            if (dev) {
                UpdateSelectionMaskTexture(dev);
                dev->Release();
            }
        }
        if (m_SelectionMaskSRV) {
            m_SelectionOutlineTime += 0.016f; // approx 60 FPS step

            // Re-upload constant buffer with u_ViewportFlags.z set to m_SelectionOutlineTime
            D3D11_MAPPED_SUBRESOURCE mappedResource2;
            if (SUCCEEDED(context->Map(m_ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource2))) {
                CanvasBuffer* cb = (CanvasBuffer*)mappedResource2.pData;
                cb->viewportSizeAndZoom = DirectX::XMFLOAT4(viewportWidth, viewportHeight, m_Zoom, m_RotationAngle);
                cb->offsetAndCanvasSize = DirectX::XMFLOAT4(m_Pan.x, m_Pan.y, (float)m_Width, (float)m_Height);
                cb->channelMasks = DirectX::XMFLOAT4(m_ChannelR ? 1.0f : 0.0f, m_ChannelG ? 1.0f : 0.0f, m_ChannelB ? 1.0f : 0.0f, m_ChannelA ? 1.0f : 0.0f);
                cb->viewportFlags = DirectX::XMFLOAT4(m_ViewportFlipH ? 1.0f : 0.0f, m_ViewportFlipV ? 1.0f : 0.0f, m_SelectionOutlineTime, 0.0f);
                context->Unmap(m_ConstantBuffer, 0);
            }

            // Opaque overwrite — ants must not depend on leftover blend from compose.
            context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            context->PSSetShader(m_SelectionOutlinePixelShader, nullptr, 0);
            context->PSSetShaderResources(1, 1, &m_SelectionMaskSRV);
            context->PSSetSamplers(0, 1, &m_SamplerState);

            context->RSSetState(m_RasterizerState);
            context->DrawIndexed(6, 0, 0);
            context->RSSetState(nullptr);

            ID3D11ShaderResourceView* nullSRV1 = nullptr;
            context->PSSetShaderResources(1, 1, &nullSRV1);
        }
    }

    // Clean slot
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
}

static bool ExtractICCFromPNG(const std::string& pngPath, std::vector<uint8_t>& outIccData, std::string& outProfileName) {
#ifdef _WIN32
    std::ifstream file(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(pngPath)), std::ios::binary);
#else
    std::ifstream file(pngPath, std::ios::binary);
#endif
    if (!file.is_open()) return false;

    // Check PNG signature
    uint8_t sig[8];
    if (!file.read(reinterpret_cast<char*>(sig), 8)) return false;
    if (sig[0] != 0x89 || sig[1] != 0x50 || sig[2] != 0x4E || sig[3] != 0x47) return false;

    while (true) {
        uint8_t lenBytes[4];
        if (!file.read(reinterpret_cast<char*>(lenBytes), 4)) break;
        uint32_t len = (lenBytes[0] << 24) | (lenBytes[1] << 16) | (lenBytes[2] << 8) | lenBytes[3];

        char type[4];
        if (!file.read(type, 4)) break;

        if (std::memcmp(type, "iCCP", 4) == 0) {
            std::vector<uint8_t> chunkData(len);
            if (!file.read(reinterpret_cast<char*>(chunkData.data()), len)) break;

            size_t nameLen = 0;
            while (nameLen < len && chunkData[nameLen] != 0) {
                nameLen++;
            }
            if (nameLen >= len || nameLen > 79) return false;

            outProfileName = std::string(reinterpret_cast<char*>(chunkData.data()), nameLen);

            if (nameLen + 2 >= len) return false;
            uint8_t compMethod = chunkData[nameLen + 1];
            if (compMethod != 0) return false;

            size_t compSize = len - (nameLen + 2);
            const uint8_t* compPtr = chunkData.data() + nameLen + 2;

            int decompSize = 0;
            char* decomp = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(compPtr), static_cast<int>(compSize), &decompSize);
            if (decomp && decompSize > 0) {
                outIccData.assign(decomp, decomp + decompSize);
                free(decomp);
                return true;
            }
            return false;
        } else {
            file.seekg(len + 4, std::ios::cur);
        }
    }
    return false;
}

bool Canvas::ExtractAndSetICCProfile(const std::string& pngPath) {
    std::vector<uint8_t> iccData;
    std::string profileName;
    if (ExtractICCFromPNG(pngPath, iccData, profileName)) {
        std::string iccPath = pngPath;
        size_t dotPos = iccPath.find_last_of('.');
        if (dotPos != std::string::npos) {
            iccPath = iccPath.substr(0, dotPos) + ".icc";
        } else {
            iccPath += ".icc";
        }
#ifdef _WIN32
        std::ofstream outFile(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(iccPath)), std::ios::binary);
#else
        std::ofstream outFile(iccPath, std::ios::binary);
#endif
        if (outFile.is_open()) {
            outFile.write(reinterpret_cast<const char*>(iccData.data()), iccData.size());
            outFile.close();
            m_ExportPngColorSpace = iccPath;
            Logger::Get().Info("Extracted embedded ICC profile '" + profileName + "' to: " + iccPath);
            return true;
        }
    }

    // Fallback: check for next-to-image .icc or .icm files
    std::string base = pngPath;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    if (std::filesystem::exists(base + ".icc")) {
        m_ExportPngColorSpace = base + ".icc";
        Logger::Get().Info("Found external ICC profile next to image: " + m_ExportPngColorSpace);
        return true;
    } else if (std::filesystem::exists(base + ".icm")) {
        m_ExportPngColorSpace = base + ".icm";
        Logger::Get().Info("Found external ICM profile next to image: " + m_ExportPngColorSpace);
        return true;
    }

    m_ExportPngColorSpace = "sRGB";
    return false;
}

bool Canvas::OpenDocument(ID3D11Device* device, const std::string& filepath, LoadProgressFn progress) {
    std::string ext;
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    if (progress) progress(0.f, "start");
    if (ext == "rayp") {
        Logger::Get().InfoTag("io", "OpenDocument: .rayp project → LoadCanvasRayp: " + filepath);
        bool ok = LoadCanvasRayp(filepath, device, progress);
        if (progress) progress(ok ? 1.f : 0.f, ok ? "done" : "error");
        return ok;
    }
    if (ext == "svg") {
        Logger::Get().InfoTag("io", "OpenDocument: SVG → smart object: " + filepath);
        bool ok = ImportSvgAsSmartObject(device, filepath);
        if (progress) progress(ok ? 1.f : 0.f, ok ? "done" : "error");
        return ok;
    }
    Logger::Get().InfoTag("io", "OpenDocument: image import → LoadImageToLayer: " + filepath);
    bool ok = LoadImageToLayer(device, filepath, progress);
    if (progress) progress(ok ? 1.f : 0.f, ok ? "done" : "error");
    return ok;
}

bool Canvas::LoadImageToLayer(ID3D11Device* device, const std::string& filepath, LoadProgressFn progress) {
    ScopedTimer loadTimer("LoadImageToLayer " + filepath);
    MemoryStats::LogSnapshot("before_LoadImageToLayer");
    if (progress) progress(0.05f, "open");

    std::string ext;
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    // Never decode projects with STB — route explicitly.
    if (ext == "rayp") {
        Logger::Get().ErrorTag("io",
            "LoadImageToLayer refused .rayp project. Use Open Project / OpenDocument: " + filepath);
        return false;
    }

    int imgWidth = 0, imgHeight = 0;
    std::vector<uint8_t> loadedU8;
    DdsFormat loadedDdsFormat = DdsFormat::RGBA8_UNORM;
    std::unique_ptr<TileCache> loadedTileCache;

    if (ext == "dds") {
        if (progress) progress(0.1f, "decode_dds");
        loadedTileCache = std::make_unique<TileCache>();
        if (!DdsHelper::LoadDDSToTileCache(filepath, *loadedTileCache, imgWidth, imgHeight, loadedDdsFormat)) {
            Logger::Get().ErrorTag("io", "LoadDDSToTileCache failed: " + filepath);
            MemoryStats::LogSnapshot("after_LoadDDS_fail");
            return false;
        }
        if (progress) progress(0.7f, "decoded");
        MemoryStats::LogSnapshot("after_LoadDDSToTileCache");
        if (loadedTileCache) {
            const size_t tiles = loadedTileCache->GetTileCount();
            const size_t bytes = MemoryStats::EstimateTileBytes(tiles, loadedTileCache->GetBytesPerPixel());
            Logger::Get().InfoTag("io",
                "TileCache loaded " + std::to_string(imgWidth) + "x" + std::to_string(imgHeight) +
                " tiles=" + std::to_string(tiles) + " est=" + MemoryStats::FormatBytes(bytes));
        }

        if (loadedDdsFormat == DdsFormat::R8_UNORM || loadedDdsFormat == DdsFormat::R16_FLOAT || loadedDdsFormat == DdsFormat::R32_FLOAT) {
            m_ChannelR = true; m_ChannelG = false; m_ChannelB = false; m_ChannelA = false;
            Logger::Get().Info("Single-channel DDS detected. Auto-configured R-only channels.");
        } else if (loadedDdsFormat == DdsFormat::R8G8_UNORM) {
            m_ChannelR = true; m_ChannelG = true; m_ChannelB = false; m_ChannelA = false;
            Logger::Get().Info("R8G8 DDS detected. Auto-configured RG channels.");
        }
    } else {
        // Non-DDS path still uses a flat decode buffer (stb). Guard huge images.
        // We only know size after decode for most formats; soft-warn via post check.
        if (!ImageManager::LoadImageFromFile(filepath, loadedU8, imgWidth, imgHeight)) return false;
        const size_t flat = MemoryStats::EstimateImageBytes(imgWidth, imgHeight, 4);
        if (flat > 512ull * 1024ull * 1024ull) {
            Logger::Get().WarnTag("mem",
                "Non-DDS load used flat buffer " + MemoryStats::FormatBytes(flat) +
                ". Prefer DDS streaming path for large textures.");
        }
    }

    // Preflight: decoded RGBA8 + GPU layer texture estimate.
    {
        const size_t cpuEst = MemoryStats::EstimateImageBytes(imgWidth, imgHeight, 4);
        const size_t gpuEst = cpuEst; // full layer texture
        const size_t totalEst = cpuEst + gpuEst;
        Logger::Get().InfoTag("mem",
            "Preflight open est CPU=" + MemoryStats::FormatBytes(cpuEst) +
            " GPU=" + MemoryStats::FormatBytes(gpuEst) +
            " total~" + MemoryStats::FormatBytes(totalEst));
        if (MemoryStats::ExceedsRamBudget(totalEst, 0.55)) {
            Logger::Get().WarnTag("mem",
                "Estimated open cost exceeds 55% of system RAM — proceeding may OOM.");
        }
    }

    std::string lowerPath = filepath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
    if (lowerPath.find("normal") != std::string::npos ||
        lowerPath.find("nrm")    != std::string::npos ||
        lowerPath.find("bc5")    != std::string::npos) {
        m_ChannelR = true; m_ChannelG = true; m_ChannelB = false; m_ChannelA = false;
        Logger::Get().Info("Normal map detected. Auto-configured RG channels.");
    }

    // --- Detect if this is the first real image load ---
    bool isFirst = m_Layers.empty() ||
        (m_Layers.size() == 1 &&
         m_Layers[0].name == "Background" &&
         (!m_Layers[0].tileCache || m_Layers[0].tileCache->IsEmpty()));

    // CRITICAL: import must reset undo. Stale "New Layer" / paint cmds from a blank
    // project survive m_Layers.clear() and a later Ctrl+Z removes the imported layer
    // (session "out of bounds" → permanent tile/layer corruption on redo).
    ClearUndoHistory();

    if (isFirst) {
        m_Width  = imgWidth;
        m_Height = imgHeight;
        // Promote document depth only for explicit float/HDR sources.
        // BC7/RGBA8/depth-view/R8G8 stay U8 (default, fast).
        m_CanvasFormat = CanvasPixelFormat::RGBA8;
        m_DocumentBitDepth = DocumentBitDepth::U8;
        if (loadedTileCache) {
            const auto lf = loadedTileCache->GetFormat();
            if (lf == CanvasPixelFormat::RGBA32F) {
                m_CanvasFormat = CanvasPixelFormat::RGBA32F;
                m_DocumentBitDepth = DocumentBitDepth::F32;
                Logger::Get().Info("Canvas format: RGBA32F / DocumentBitDepth F32 (float source)");
            } else if (lf == CanvasPixelFormat::RGBA16F) {
                m_CanvasFormat = CanvasPixelFormat::RGBA16F;
                m_DocumentBitDepth = DocumentBitDepth::F16;
                Logger::Get().Info("Canvas format: RGBA16F / DocumentBitDepth F16 (half/HDR source)");
            } else {
                Logger::Get().Info("Canvas format: RGBA8 / DocumentBitDepth U8");
            }
        } else {
            Logger::Get().Info("Canvas format: RGBA8 / DocumentBitDepth U8");
        }

        CreateCompositeResources(device);
        m_Layers.clear();
        // First real image into a blank tab: stay Simple unless caller already
        // switched to Advanced (wizard / multi-map import sets type BEFORE load).
        // Previously default was Advanced → Quick Export ran batch packing with no stroke.
        if (m_ProjectType != ProjectType::Advanced &&
            m_ProjectType != ProjectType::AdvancedModMode) {
            m_ProjectType = ProjectType::Simple;
        }
        // If type is still the default Simple — good. If Advanced was set first, keep it.
        m_CurrentProjectFilePath = filepath;
        m_ExportPath         = filepath;
        if (m_ProjectType == ProjectType::Simple) {
            m_ExportContainer = (ext == "dds")
                ? ExportContainer::DDS
                : ExportContainer::PNG;
        }

        if (ext == "dds") {
            // Round-trip: export format / container / mips follow source via DirectXTex Analyze
            DdsCodec::SourceInfo srcInfo;
            if (DdsCodec::AnalyzeFile(filepath, srcInfo)) {
                m_ExportFormat = srcInfo.uiLabel.empty() ? srcInfo.formatLabel : srcInfo.uiLabel;
                m_ExportContainer = ExportContainer::DDS;
                // Do NOT auto-enable mip generation from source mip count.
                // Rebuilding a full BC7 mip chain on CPU is minutes for 2K/4K (regression vs texconv).
                // Keep existing UI preference; default stays false unless user enables Mipmaps.
                Logger::Get().Info("Auto-configured DDS export: " + m_ExportFormat +
                    " (source mips=" + std::to_string(srcInfo.mipCount) +
                    ", export mips=" + (m_ExportGenerateMipMaps ? "on" : "off") + ")");
            } else {
                // Fallback from legacy enum
                if (loadedDdsFormat == DdsFormat::RGBA32_FLOAT) m_ExportFormat = "RGBA32_FLOAT";
                else if (loadedDdsFormat == DdsFormat::RGBA16_UNORM) m_ExportFormat = "RGBA16_UNORM";
                else if (loadedDdsFormat == DdsFormat::RGBA16_FLOAT) m_ExportFormat = "RGBA16_FLOAT";
                else if (loadedDdsFormat == DdsFormat::R8_UNORM)     m_ExportFormat = "R8 (Linear, Unsigned, L8)";
                else if (loadedDdsFormat == DdsFormat::R8G8_UNORM)   m_ExportFormat = "R8G8 (Linear, Unsigned)";
                else if (loadedDdsFormat == DdsFormat::R16_FLOAT)    m_ExportFormat = "R16_FLOAT";
                else if (loadedDdsFormat == DdsFormat::R32_FLOAT)    m_ExportFormat = "R32 (Linear, Float)";
                else m_ExportFormat = "BC7 (sRGB, DX 11+)";
                Logger::Get().Info("Auto-configured DDS export format (legacy): " + m_ExportFormat);
            }
        } else if (ext == "png") {
            ExtractAndSetICCProfile(filepath);
        }
    }

    // --- Build Layer with TileCache ---
    Layer imported;
    imported.name    = filepath.substr(filepath.find_last_of("\\/") + 1);
    imported.visible = true;
    imported.opacity = 1.0f;
    // Base document open → Alpha Rewrite ON; extra imports (decals) → OFF (A = RGB morph only).
    imported.alphaRewrite = isFirst;
    if (loadedTileCache) {
        if (loadedTileCache->GetWidth() == m_Width && loadedTileCache->GetHeight() == m_Height) {
            imported.tileCache = std::move(loadedTileCache);
        } else {
            imported.tileCache = std::make_unique<TileCache>();
            imported.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
            imported.tileCache->CopyFrom(*loadedTileCache, 0, 0, 0, 0, imgWidth, imgHeight);
        }
    } else {
        imported.tileCache = std::make_unique<TileCache>();
        imported.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
        if (!loadedU8.empty()) {
            imported.tileCache->ImportRGBA8(loadedU8.data(), imgWidth, imgHeight);
        }
    }
    imported.tileCache->MarkAllDirty();

    if (progress) progress(0.85f, "gpu_upload");
    RecreateLayerTexture(device, imported);
    // Large docs use sparse GpuTileStore (gpuSurfaceId) — classic layer.texture stays null.
    if (!imported.texture && !imported.gpuSurfaceId) {
        Logger::Get().ErrorTag("gpu",
            "LoadImageToLayer: no GPU surface after RecreateLayerTexture. "
            "CPU TileCache may still hold pixels, but viewport will be blank.");
        // Still keep CPU data so headless tests can validate tiles.
    }
    m_Layers.push_back(std::move(imported));
    m_ActiveLayerIdx = (int)m_Layers.size() - 1;
    m_PaintTarget = PaintTarget::LayerContent;
    m_CompositeDirty = true;

    ResetView();
    if (progress) progress(0.95f, "finalize");
    MemoryStats::LogSnapshot("after_LoadImageToLayer_success");
    Logger::Get().Info("Successfully imported layer from: " + filepath);
    return true;
}

bool Canvas::SaveCanvas(const std::string& filepath, DdsFormat ddsFormat) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save.");
        return false;
    }

    if (!CanAllocateFlatComposite(m_Width, m_Height, "SaveCanvas")) {
        return false;
    }

    std::vector<float> composite = ComposeVisibleLayers(m_Layers, m_Width, m_Height);
    if (composite.empty()) {
        Logger::Get().Error("SaveCanvas: composite is empty.");
        return false;
    }

    DdsImage dds;
    dds.width = m_Width;
    dds.height = m_Height;
    dds.format = ddsFormat;
    dds.pixels = std::move(composite);

    return DdsHelper::SaveDDS(filepath, dds);
}

bool Canvas::SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save.");
        return false;
    }

    ScopedTimer saveTimer("SaveCanvasStandard " + filepath);
    MemoryStats::LogSnapshot("before_SaveCanvasStandard");

    // Rebuild non-destructive FX caches so export matches the viewport.
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        auto& layer = m_Layers[i];
        if (layer.isGroup) {
            // Always flatten groups for export (isolation + opacity + optional FX)
            layer.presentationDirty = true;
            RebuildGroupPresentation(i, /*fullQuality=*/true);
            Logger::Get().InfoTag("io", "Export: rebuilt group composite '" + layer.name + "'");
            continue;
        }
        if (!layer.filters.empty()) {
            layer.filtersDirty = true;
            RebuildFilteredPixels(layer);
            Logger::Get().InfoTag("io",
                "Export: rebuilt filters on layer '" + layer.name +
                "' (count=" + std::to_string(layer.filters.size()) +
                ", blend=" + std::to_string((int)layer.blendMode) + ")");
        }
        if (layer.HasEnabledStyles()) {
            layer.presentationDirty = true;
            RebuildLayerPresentation(layer, /*fullQuality=*/true);
        }
    }

    std::vector<uint8_t> rgba8;
    if (!ComposeVisibleLayersRGBA8(m_Layers, m_Width, m_Height, rgba8)) {
        Logger::Get().Error("SaveCanvasStandard: RGBA8 composite failed.");
        return false;
    }

    const bool ok = ImageManager::SaveRGBA8ToFile(
        filepath, rgba8.data(), m_Width, m_Height, m_Width * 4, iccProfilePath);
    rgba8.clear();
    rgba8.shrink_to_fit();
    MemoryStats::LogSnapshot("after_SaveCanvasStandard");
    return ok;
}

bool Canvas::SaveCanvasCompressed(const std::string& filepath, const std::string& formatStr, bool generateMips, const std::string& mipFilter, const std::string& speed) {
    DXGI_FORMAT dxgi = DXGI_FORMAT_BC7_UNORM_SRGB;
    DdsCodec::UiLabelToDxgi(formatStr, dxgi);

    DdsCodec::SaveOptions opt;
    opt.format = dxgi;
    opt.generateMips = generateMips;
    opt.mipFilter = mipFilter;
    opt.compressionSpeed = speed;

    // Prefer float path for HDR document / float targets
    const bool wantFloat =
        DdsCodec::IsFloatFormat(dxgi) ||
        m_DocumentBitDepth == DocumentBitDepth::F16 ||
        m_DocumentBitDepth == DocumentBitDepth::F32;

    if (wantFloat && (DdsCodec::IsFloatFormat(dxgi) ||
                      dxgi == DXGI_FORMAT_BC6H_UF16 || dxgi == DXGI_FORMAT_BC6H_SF16)) {
        auto pixels = GetCompositePixels(); // float RGBA
        if (pixels.empty()) {
            Logger::Get().Error("SaveCanvasCompressed: empty composite");
            return false;
        }
        return DdsCodec::SaveRgba32F(filepath, pixels.data(), m_Width, m_Height, opt);
    }

    std::vector<uint8_t> rgba8;
    if (!ComposeVisibleLayersRGBA8(m_Layers, m_Width, m_Height, rgba8) || rgba8.empty()) {
        Logger::Get().Error("SaveCanvasCompressed: RGBA8 composite failed");
        return false;
    }
    return DdsCodec::SaveRgba8(filepath, rgba8.data(), m_Width, m_Height, opt);
}

void Canvas::SetExportContainer(ExportContainer c) {
    if (m_ExportContainer == c) {
        SyncExportPathExtension();
        return;
    }
    m_ExportContainer = c;
    SyncExportPathExtension();
    m_IsDocumentModified = true;
    Logger::Get().Info(std::string("Export container → ") +
        (c == ExportContainer::DDS ? "DDS" : "PNG"));
}

std::string Canvas::SyncExportPathExtension() {
    const char* wantExt = (m_ExportContainer == ExportContainer::DDS) ? ".dds" : ".png";
    std::string path = m_ExportPath;
    if (path.empty()) {
        path = (m_ExportContainer == ExportContainer::DDS) ? "export.dds" : "export.png";
        m_ExportPath = path;
        return m_ExportPath;
    }
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    std::string base = path;
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        base = path.substr(0, dot);
    m_ExportPath = base + wantExt;
    return m_ExportPath;
}

bool Canvas::ExportWithProjectSettings(std::string* outPathUsed) {
    SyncExportPathExtension();
    std::string path = m_ExportPath;
    if (path.empty()) {
        path = (m_ExportContainer == ExportContainer::DDS) ? "export.dds" : "export.png";
        m_ExportPath = path;
    }
    if (outPathUsed) *outPathUsed = path;

    bool ok = false;
    if (m_ExportContainer == ExportContainer::DDS) {
        ok = SaveCanvasCompressed(path, m_ExportFormat, m_ExportGenerateMipMaps,
                                  m_ExportMipFilter, m_ExportCompressionSpeed);
        if (ok) Logger::Get().Info("Export DDS: " + path + " [" + m_ExportFormat + "]");
        else Logger::Get().Error("Export DDS failed: " + path);
    } else {
        ok = SaveCanvasStandard(path, GetExportIccPreset());
        if (ok) Logger::Get().Info("Export PNG: " + path + " [ICC " +
                                   IccPresetName(m_ExportIccPreset) + "]");
        else Logger::Get().Error("Export PNG failed: " + path);
    }
    return ok;
}

bool Canvas::ExportWithProjectSettingsAsync(std::function<void(bool, const std::string&)> onDone) {
    static std::atomic<bool> s_exportBusy{false};
    if (s_exportBusy.exchange(true)) {
        Logger::Get().Warn("Export already in progress");
        return false;
    }

    SyncExportPathExtension();
    std::string path = m_ExportPath;
    if (path.empty()) {
        path = (m_ExportContainer == ExportContainer::DDS) ? "export.dds" : "export.png";
        m_ExportPath = path;
    }

    auto& jobs = core::JobManager::Get();
    const uint64_t jobId = jobs.Begin("Export", /*locksDocument=*/true, /*cancellable=*/false);
    jobs.SetProgress(jobId, 0.05f, "Compositing…");

    ScopedTimer exportTimer("ExportWithProjectSettingsAsync " + path);

    // --- Main thread: rebuild FX + snapshot composite (document stays locked via job) ---
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        auto& layer = m_Layers[i];
        if (layer.isGroup) {
            layer.presentationDirty = true;
            RebuildGroupPresentation(i, /*fullQuality=*/true);
            continue;
        }
        if (!layer.filters.empty()) {
            layer.filtersDirty = true;
            RebuildFilteredPixels(layer);
        }
        if (layer.HasEnabledStyles()) {
            layer.presentationDirty = true;
            RebuildLayerPresentation(layer, /*fullQuality=*/true);
        }
    }

    const bool asDds = (m_ExportContainer == ExportContainer::DDS);
    const int w = m_Width, h = m_Height;
    std::string format = m_ExportFormat;
    bool genMips = m_ExportGenerateMipMaps;
    std::string mipFilter = m_ExportMipFilter;
    std::string speed = m_ExportCompressionSpeed;
    IccPreset iccPreset = GetExportIccPreset();

    std::vector<uint8_t> rgba8;
    std::vector<float> rgbaF;
    bool wantFloat = false;
    DXGI_FORMAT dxgi = DXGI_FORMAT_BC7_UNORM_SRGB;

    auto failComposite = [&]() {
        jobs.Complete(jobId, false, "Composite failed");
        s_exportBusy = false;
        if (onDone) onDone(false, path);
        return false;
    };

    if (asDds) {
        DdsCodec::UiLabelToDxgi(format, dxgi);
        wantFloat =
            DdsCodec::IsFloatFormat(dxgi) ||
            m_DocumentBitDepth == DocumentBitDepth::F16 ||
            m_DocumentBitDepth == DocumentBitDepth::F32;
        if (wantFloat && (DdsCodec::IsFloatFormat(dxgi) ||
                          dxgi == DXGI_FORMAT_BC6H_UF16 || dxgi == DXGI_FORMAT_BC6H_SF16)) {
            rgbaF = GetCompositePixels();
            if (rgbaF.empty()) return failComposite();
        } else {
            wantFloat = false;
            if (!ComposeVisibleLayersRGBA8(m_Layers, w, h, rgba8) || rgba8.empty())
                return failComposite();
        }
    } else {
        if (!ComposeVisibleLayersRGBA8(m_Layers, w, h, rgba8) || rgba8.empty())
            return failComposite();
    }

    // ICC bytes must be copied for the worker (static presets are OK, but copy is safer).
    std::vector<uint8_t> iccBytes;
    std::string iccName;
    if (!asDds && iccPreset != IccPreset::None) {
        iccBytes = GetIccPresetBytes(iccPreset);
        iccName = IccPresetName(iccPreset);
    }

    jobs.SetProgress(jobId, 0.35f, "Writing…");

    std::thread([path, asDds, w, h, format, genMips, mipFilter, speed, wantFloat, dxgi,
                 rgba8 = std::move(rgba8), rgbaF = std::move(rgbaF),
                 iccBytes = std::move(iccBytes), iccName = std::move(iccName),
                 jobId, onDone]() mutable {
        bool ok = false;
        try {
            core::JobManager::Get().SetProgress(jobId, 0.45f,
                asDds ? "Encoding DDS…" : "Writing PNG…");
            if (asDds) {
                DdsCodec::SaveOptions opt;
                opt.format = dxgi;
                opt.generateMips = genMips;
                opt.mipFilter = mipFilter;
                opt.compressionSpeed = speed;
                if (wantFloat)
                    ok = DdsCodec::SaveRgba32F(path, rgbaF.data(), w, h, opt);
                else
                    ok = DdsCodec::SaveRgba8(path, rgba8.data(), w, h, opt);
            } else if (iccBytes.empty()) {
                ok = ImageManager::SaveRGBA8ToFile(
                    path, rgba8.data(), w, h, w * 4, std::string());
            } else {
                ok = ImageManager::SaveRGBA8ToFile(
                    path, rgba8.data(), w, h, w * 4,
                    iccBytes.data(), iccBytes.size(), iccName.c_str());
            }
        } catch (...) {
            ok = false;
        }

        if (ok)
            Logger::Get().Info(std::string("Async export OK: ") + path);
        else
            Logger::Get().Error(std::string("Async export failed: ") + path);

        core::JobManager::Get().Complete(jobId, ok,
            ok ? ("Exported " + path) : ("Export failed: " + path));
        s_exportBusy = false;
        if (onDone) onDone(ok, path);
    }).detach();

    return true;
}

std::vector<float> Canvas::GetCompositePixels() const {
    if (m_Layers.empty()) {
        return std::vector<float>((size_t)m_Width * m_Height * 4, 0.0f);
    }
    return ComposeVisibleLayers(m_Layers, m_Width, m_Height);
}

bool Canvas::ComposePackedMapRGBA8(texset::MapKind kind,
                                   const std::vector<texset::MapSlot>& maps,
                                   const TileCache* /*importedBase unused — maps are layers now*/,
                                   std::vector<uint8_t>& outRgba,
                                   int& outW, int& outH) const {
    const texset::MapSlot* slot = texset::FindMap(maps, kind);
    outW = (slot && slot->width > 0) ? slot->width : m_Width;
    outH = (slot && slot->height > 0) ? slot->height : m_Height;
    if (outW <= 0 || outH <= 0) return false;

    // IMPORTANT: only layers bound to this map (workSpace). Never dump all maps into Diffuse.
    outRgba.assign((size_t)outW * (size_t)outH * 4u, 0);
    const bool isNormal = (kind == texset::MapKind::NormalMap);
    for (size_t i = 0; i < outRgba.size(); i += 4) {
        if (isNormal) {
            outRgba[i + 0] = 128; outRgba[i + 1] = 128; outRgba[i + 2] = 255; outRgba[i + 3] = 255;
        } else {
            outRgba[i + 0] = 0; outRgba[i + 1] = 0; outRgba[i + 2] = 0; outRgba[i + 3] = 255;
        }
    }

    // Blend layers that write this map only. writeMask: which dest channels to touch (0xF = all).
    auto blendOver = [](uint8_t* dp, float sr, float sg, float sb, float sa, uint8_t writeMask = 0xF) {
        if (sa <= 0.f || writeMask == 0) return;
        float dr = dp[0] / 255.f, dg = dp[1] / 255.f, db = dp[2] / 255.f, da = dp[3] / 255.f;
        float outA = sa + da * (1.f - sa);
        if (outA <= 1e-6f && (writeMask & 8)) return;
        auto mix = [&](float s, float d) {
            return (s * sa + d * da * (1.f - sa)) / std::max(outA, 1e-6f);
        };
        if (writeMask & 1)
            dp[0] = (uint8_t)(std::clamp(mix(sr, dr), 0.f, 1.f) * 255.f + 0.5f);
        if (writeMask & 2)
            dp[1] = (uint8_t)(std::clamp(mix(sg, dg), 0.f, 1.f) * 255.f + 0.5f);
        if (writeMask & 4)
            dp[2] = (uint8_t)(std::clamp(mix(sb, db), 0.f, 1.f) * 255.f + 0.5f);
        if (writeMask & 8)
            dp[3] = (uint8_t)(std::clamp(outA, 0.f, 1.f) * 255.f + 0.5f);
    };

    for (const auto& layer : m_Layers) {
        if (!layer.visible || layer.isGroup || layer.parentGroupId >= 0) continue;
        if (!layer.ParticipatesInView(kind, false, texset::ChannelRole::None))
            continue;

        float solid[4] = {0, 0, 0, 1};
        bool useSolid = false;
        uint8_t writeMask = 0xF;
        if (layer.IsFill()) {
            if (!layer.fill.ResolveForMap(maps, kind, solid, &writeMask))
                continue;
            useSolid = true;
        }

        const float opacity = layer.opacity;
        const bool hasMask = layer.hasMask && layer.mask.size() == (size_t)m_Width * m_Height;

        for (int y = 0; y < outH; ++y) {
            // Map UV → canvas pixel (shared UV space)
            int cy = std::min(m_Height - 1, (int)((y + 0.5f) / outH * m_Height));
            for (int x = 0; x < outW; ++x) {
                int cx = std::min(m_Width - 1, (int)((x + 0.5f) / outW * m_Width));
                float sr, sg, sb, sa;
                if (useSolid) {
                    sr = solid[0]; sg = solid[1]; sb = solid[2]; sa = solid[3];
                } else if (layer.nativeMapCache && layer.nativeMapW > 0 && layer.nativeMapH > 0 &&
                   layer.nativeMapKind == kind) {
                    // Prefer native-resolution storage for this map
                    int nx = std::min(layer.nativeMapW - 1, (int)((x + 0.5f) / outW * layer.nativeMapW));
                    int ny = std::min(layer.nativeMapH - 1, (int)((y + 0.5f) / outH * layer.nativeMapH));
                    float px[4];
                    layer.nativeMapCache->GetPixelF(nx, ny, px);
                    sr = px[0]; sg = px[1]; sb = px[2]; sa = px[3];
                } else if (layer.tileCache) {
                    float px[4];
                    layer.tileCache->GetPixelF(cx, cy, px);
                    sr = px[0]; sg = px[1]; sb = px[2]; sa = px[3];
                } else {
                    continue;
                }
                sa *= opacity;
                if (hasMask) {
                    if (layer.maskTiles && layer.maskTiles->Valid())
                        sa *= layer.maskTiles->Get(cx, cy) / 255.f;
                    else if (layer.mask.size() == (size_t)m_Width * m_Height)
                        sa *= layer.mask[(size_t)cy * m_Width + cx] / 255.f;
                }
                if (sa <= 0.001f) continue;
                uint8_t* dp = outRgba.data() + ((size_t)y * outW + x) * 4;
                blendOver(dp, sr, sg, sb, sa, writeMask);
            }
        }
    }
    return true;
}

void Canvas::SampleActiveLayerPixel(int x, int y, float outColor[4]) const {
    outColor[0] = outColor[1] = outColor[2] = 0.f;
    outColor[3] = 0.f;
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return;
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    const Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup || !layer.tileCache) return;
    layer.tileCache->GetPixelF(x, y, outColor);
}

void Canvas::SampleCompositePixel(int x, int y, float outColor[4]) const {
    outColor[0] = 0.0f;
    outColor[1] = 0.0f;
    outColor[2] = 0.0f;
    outColor[3] = 0.0f;

    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return;

    // Match viewport: only layers that participate in the active Channels map.
    // Previously sampled ALL tileCache layers → pipette only looked right on NormalMap
    // (or whatever map happened to dominate the unfiltered blend).
    const texset::MapKind viewMap = m_ViewMapKind;
    const bool roleIso = m_ViewRoleIsolate;
    const texset::ChannelRole soloRole = m_ViewSoloRole;

    std::vector<const Layer*> vis;
    vis.reserve(m_Layers.size());
    for (const auto& layer : m_Layers) {
        if (!LayerEffectivelyVisible(m_Layers, layer)) continue;
        if (layer.isGroup) continue;
        if (!layer.ParticipatesInView(viewMap, roleIso, soloRole)) continue;
        vis.push_back(&layer);
    }
    if (vis.empty()) return;

    auto sampleStack = [&](float outF[4]) {
        outF[0] = outF[1] = outF[2] = outF[3] = 0.f;
        bool first = true;
        for (const Layer* layer : vis) {
            float rgba[4] = {};
            bool have = false;
            if (layer->IsFill()) {
                if (!m_ActiveSetMaps.empty()) {
                    if (!layer->fill.ResolveForMap(m_ActiveSetMaps, viewMap, rgba))
                        continue;
                } else {
                    layer->fill.ResolveRgba(rgba);
                }
                have = true;
            } else if (layer->tileCache) {
                layer->tileCache->GetPixelF(x, y, rgba);
                have = true;
            }
            if (!have) continue;

            const bool useMask = layer->hasMask && layer->mask.size() == (size_t)m_Width * m_Height;
            float op = layer->opacity;
            float mask = 1.f;
            if (useMask) mask = layer->mask[(size_t)y * m_Width + x] / 255.f;
            if (first) {
                outF[0] = rgba[0];
                outF[1] = rgba[1];
                outF[2] = rgba[2];
                outF[3] = rgba[3] * op * mask;
                first = false;
                continue;
            }
            float sa = rgba[3] * op * mask;
            if (sa <= 0.f) continue;
            BlendLayerPixelF(outF, rgba[0], rgba[1], rgba[2], sa, layer->blendMode, layer->alphaRewrite);
        }
    };

    sampleStack(outColor);
}

void Canvas::CreateLayerFromPixels(ID3D11Device* device, const std::string& name, const std::vector<float>& pixels, int width, int height) {
    if (pixels.empty() || width <= 0 || height <= 0) return;

    if (m_Layers.empty()) {
        m_Width = width;
        m_Height = height;
        if (device) {
            CreateCompositeResources(device);
        }
    }

    Layer newLayer;
    newLayer.name = name;
    newLayer.visible = true;
    newLayer.opacity = 1.0f;
    // First layer = document base (rewrite ON); pasted/extra = decal (A = coverage).
    newLayer.alphaRewrite = m_Layers.empty();
    newLayer.tileCache = std::make_unique<TileCache>();
    newLayer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);

    if (width == m_Width && height == m_Height) {
        newLayer.tileCache->ImportRGBA32F(pixels.data(), width, height);
    } else {
        // Center paste; keep source alpha (transparent PNG / UV layout).
        std::vector<float> resizedPixels((size_t)m_Width * m_Height * 4, 0.0f);
        int offsetX = (m_Width - width) / 2;
        int offsetY = (m_Height - height) / 2;
        for (int y = 0; y < height; ++y) {
            int targetY = y + offsetY;
            if (targetY < 0 || targetY >= m_Height) continue;
            for (int x = 0; x < width; ++x) {
                int targetX = x + offsetX;
                if (targetX < 0 || targetX >= m_Width) continue;

                int srcIdx = (y * width + x) * 4;
                int destIdx = (targetY * m_Width + targetX) * 4;
                std::memcpy(&resizedPixels[destIdx], &pixels[srcIdx], 4 * sizeof(float));
            }
        }
        newLayer.tileCache->ImportRGBA32F(resizedPixels.data(), m_Width, m_Height);
    }
    newLayer.tileCache->MarkAllDirty();
    newLayer.needsUpload = true;

    if (device) {
        RecreateLayerTexture(device, newLayer);
    }

    int insertIdx = m_ActiveLayerIdx + 1;
    if (insertIdx < 0 || insertIdx > static_cast<int>(m_Layers.size())) {
        insertIdx = static_cast<int>(m_Layers.size());
    }

    m_Layers.insert(m_Layers.begin() + insertIdx, std::move(newLayer));
    m_ActiveLayerIdx = insertIdx;
    m_CompositeDirty = true;

    ClearUndoHistory();
    m_IsDocumentModified = true;
    Logger::Get().Info("Created new layer from clipboard/drop: " + name +
        " " + std::to_string(width) + "x" + std::to_string(height));
}

bool Canvas::Undo() {
    UpdateScheduler::Get().CancelAll();
    bool res = m_UndoRedoManager.Undo(this);
    if (res) {
        m_IsDocumentModified = true;
        m_CompositeDirty = true;
        Logger::Get().DebugTag("gpu", "Undo applied — composite marked dirty");
    }
    return res;
}

bool Canvas::Redo() {
    UpdateScheduler::Get().CancelAll();
    bool res = m_UndoRedoManager.Redo(this);
    if (res) {
        m_IsDocumentModified = true;
        m_CompositeDirty = true;
        Logger::Get().DebugTag("gpu", "Redo applied — composite marked dirty");
    }
    return res;
}

bool Canvas::CanUndo() const {
    return m_UndoRedoManager.CanUndo();
}

bool Canvas::CanRedo() const {
    return m_UndoRedoManager.CanRedo();
}

std::string Canvas::GetUndoName() const {
    return m_UndoRedoManager.GetUndoName();
}

std::string Canvas::GetRedoName() const {
    return m_UndoRedoManager.GetRedoName();
}

void Canvas::ClearAllLayersNoUndo() {
    UpdateScheduler::Get().CancelAll();
    for (auto& L : m_Layers) {
        if (L.gpuSurfaceId) {
            m_GpuTiles.DestroySurface(L.gpuSurfaceId);
            L.gpuSurfaceId = 0;
        }
        if (L.texture) { L.texture->Release(); L.texture = nullptr; }
        if (L.srv) { L.srv->Release(); L.srv = nullptr; }
        if (L.maskTexture) { L.maskTexture->Release(); L.maskTexture = nullptr; }
        if (L.maskSRV) { L.maskSRV->Release(); L.maskSRV = nullptr; }
        if (L.thumbSRV) { L.thumbSRV->Release(); L.thumbSRV = nullptr; }
        if (L.thumbTex) { L.thumbTex->Release(); L.thumbTex = nullptr; }
    }
    m_Layers.clear();
    m_ActiveLayerIdx = -1;
    m_PaintTarget = PaintTarget::LayerContent;
    m_CompositeDirty = true;
}

void Canvas::ClearUndoHistory() {
    m_UndoRedoManager.Clear();
}

int Canvas::TrimUndoHistoryForPressure(bool extreme) {
    return m_UndoRedoManager.TrimForPressure(extreme);
}

size_t Canvas::GetUndoMemoryBytes() const {
    return m_UndoRedoManager.GetCurrentMemoryUsage();
}

// RAYP format:
//   v1: name/visible/opacity + zlib float RGBA pixels per layer
//   v2: + blend_mode, filters, groups, has_mask; after each layer pixels optional mask blob
static constexpr uint32_t kRaypVersionCurrent = 2;

static const char* BlendModeToString(BlendMode m) {
    switch (m) {
        case BlendMode::Normal:    return "Normal";
        case BlendMode::Multiply:  return "Multiply";
        case BlendMode::Screen:    return "Screen";
        case BlendMode::Overlay:   return "Overlay";
        case BlendMode::Add:       return "Add";
        case BlendMode::Subtract:  return "Subtract";
        case BlendMode::Darken:    return "Darken";
        case BlendMode::Lighten:   return "Lighten";
        case BlendMode::HardLight: return "HardLight";
        case BlendMode::SoftLight: return "SoftLight";
        default: return "Normal";
    }
}

static BlendMode BlendModeFromString(const std::string& s) {
    if (s == "Multiply")  return BlendMode::Multiply;
    if (s == "Screen")    return BlendMode::Screen;
    if (s == "Overlay")   return BlendMode::Overlay;
    if (s == "Add")       return BlendMode::Add;
    if (s == "Subtract")  return BlendMode::Subtract;
    if (s == "Darken")    return BlendMode::Darken;
    if (s == "Lighten")   return BlendMode::Lighten;
    if (s == "HardLight") return BlendMode::HardLight;
    if (s == "SoftLight") return BlendMode::SoftLight;
    return BlendMode::Normal;
}

static const char* FilterTypeToString(FilterType t) {
    switch (t) {
        case FilterType::Blur:        return "Blur";
        case FilterType::HSV:         return "HSV";
        case FilterType::Curves:      return "Curves";
        case FilterType::AlphaInvert: return "AlphaInvert";
        case FilterType::Noise:       return "Noise";
        default: return "Blur";
    }
}

static FilterType FilterTypeFromString(const std::string& s) {
    if (s == "HSV")         return FilterType::HSV;
    if (s == "Curves")      return FilterType::Curves;
    if (s == "AlphaInvert") return FilterType::AlphaInvert;
    if (s == "Noise")       return FilterType::Noise;
    return FilterType::Blur;
}

static const char* LayerTypeToString(Layer::Type t) {
    switch (t) {
    case Layer::Type::Group: return "group";
    case Layer::Type::SmartObject: return "smart_object";
    case Layer::Type::VectorSvg: return "vector"; // prefer "vector"; load still accepts vector_svg
    case Layer::Type::Fill: return "fill";
    default: return "raster";
    }
}
static Layer::Type LayerTypeFromString(const std::string& s) {
    if (s == "group") return Layer::Type::Group;
    if (s == "smart_object") return Layer::Type::SmartObject;
    if (s == "vector_svg" || s == "vector") return Layer::Type::VectorSvg;
    if (s == "fill") return Layer::Type::Fill;
    return Layer::Type::Raster;
}

static const char* StyleTypeToString(StyleType t) {
    switch (t) {
    case StyleType::Outline: return "Outline";
    default: return "Shadow";
    }
}
static StyleType StyleTypeFromString(const std::string& s) {
    if (s == "Outline" || s == "outline") return StyleType::Outline;
    return StyleType::Shadow;
}

static const char* FillTargetToString(FillChannelTarget t) {
    return FillChannelTargetName(t);
}
static FillChannelTarget FillTargetFromString(const std::string& s) {
    return FillChannelTargetFromName(s);
}

// Fill: workSpace = enabled maps (RGBA each). Roles are soft labels only.
void Layer::SyncWorkSpaceFromFillTarget(const texset::TextureSet* /*set*/) {
    fill.MigrateFromLegacy();
    fill.EnsureDefaults();

    workSpace = texset::LayerWorkSpace{};
    workSpace.mapMask = 0;
    workSpace.roleMask = 0; // 0 = don't filter by role
    workSpace.channelWriteMask = 0xF;

    for (int i = 0; i < (int)texset::MapKind::Count; ++i) {
        if (fill.mapColor[i].enabled)
            workSpace.SetMap((texset::MapKind)i, true);
    }
    if (workSpace.mapMask == 0)
        workSpace.SetMap(texset::MapKind::Diffuse, true);
}

bool Layer::ParticipatesInView(texset::MapKind viewMap, bool /*roleIsolate*/,
                               texset::ChannelRole /*soloRole*/) const {
    if (isGroup) return true;
    // Default paint layers (Diffuse-only workSpace) still show on Diffuse.
    // On other maps: only layers that opted into that map (Fill multi-map / activity).
    return workSpace.AffectsMap(viewMap);
}
static const char* FillModeToString(FillValueMode m) {
    switch (m) {
    case FillValueMode::Grayscale01: return "Grayscale01";
    case FillValueMode::GrayscaleSigned: return "GrayscaleSigned";
    default: return "RGB";
    }
}
static FillValueMode FillModeFromString(const std::string& s) {
    if (s == "Grayscale01") return FillValueMode::Grayscale01;
    if (s == "GrayscaleSigned") return FillValueMode::GrayscaleSigned;
    return FillValueMode::RGB;
}

static json LayerToJson(const Layer& layer) {
    json j;
    j["name"] = layer.name;
    j["visible"] = layer.visible;
    j["opacity"] = layer.opacity;
    j["blend_mode"] = BlendModeToString(layer.blendMode);
    j["alpha_rewrite"] = layer.alphaRewrite;
    j["is_group"] = layer.isGroup;
    j["parent_group_id"] = layer.parentGroupId;
    j["group_expanded"] = layer.groupExpanded;
    j["layer_type"] = LayerTypeToString(layer.isGroup ? Layer::Type::Group : layer.type);
    j["smart_source_path"] = layer.smartSourcePath;
    j["smart_scale"] = layer.smartScale;
    j["has_smart_source"] = !layer.smartSourceBytes.empty();
    if (!layer.smartAssetKey.empty())
        j["smart_asset_key"] = layer.smartAssetKey;
    j["has_mask"] = layer.hasMask && !layer.mask.empty();

    if (layer.vectorDoc) {
        try {
            j["vector"] = json::parse(vec::DocumentToJson(*layer.vectorDoc));
        } catch (...) {
            j["vector_json"] = vec::DocumentToJson(*layer.vectorDoc);
        }
    }

    if (layer.type == Layer::Type::Fill || layer.IsFill()) {
        json fj;
        fj["target"] = FillTargetToString(layer.fill.target);
        fj["mode"] = FillModeToString(layer.fill.mode);
        fj["color"] = { layer.fill.color[0], layer.fill.color[1], layer.fill.color[2], layer.fill.color[3] };
        fj["gray"] = layer.fill.gray;
        fj["use_texture"] = layer.fill.useTexture;
        fj["texture_path"] = layer.fill.texturePath;
        if (!layer.fill.textureAssetKey.empty())
            fj["texture_asset_key"] = layer.fill.textureAssetKey;
        fj["tex_scale"] = { layer.fill.texScale[0], layer.fill.texScale[1] };
        fj["tex_offset"] = { layer.fill.texOffset[0], layer.fill.texOffset[1] };
        // Per-map RGBA (primary model)
        json maps = json::array();
        for (int i = 0; i < (int)texset::MapKind::Count; ++i) {
            json mj;
            mj["kind"] = texset::MapKindName((texset::MapKind)i);
            mj["enabled"] = layer.fill.mapColor[i].enabled;
            mj["rgba"] = {
                layer.fill.mapColor[i].rgba[0], layer.fill.mapColor[i].rgba[1],
                layer.fill.mapColor[i].rgba[2], layer.fill.mapColor[i].rgba[3]
            };
            mj["channel_mask"] = (int)layer.fill.mapColor[i].channelMask;
            maps.push_back(mj);
        }
        fj["map_colors"] = maps;
        j["fill"] = fj;
    }

    // Texture-set work space (Plan 0)
    {
        json wj;
        wj["map_mask"] = layer.workSpace.mapMask;
        wj["role_mask"] = layer.workSpace.roleMask;
        wj["channel_write_mask"] = (int)layer.workSpace.channelWriteMask;
        j["work_space"] = wj;
    }

    json filters = json::array();
    for (const auto& f : layer.filters) {
        json fj;
        fj["type"] = FilterTypeToString(f.type);
        fj["enabled"] = f.enabled;
        fj["p"] = { f.p[0], f.p[1], f.p[2], f.p[3] };
        if (!f.lut.empty()) fj["lut"] = f.lut;
        if (!f.lutR.empty()) fj["lut_r"] = f.lutR;
        if (!f.lutG.empty()) fj["lut_g"] = f.lutG;
        if (!f.lutB.empty()) fj["lut_b"] = f.lutB;
        if (!f.lutA.empty()) fj["lut_a"] = f.lutA;
        if (f.type == FilterType::Curves)
            fj["curves_channels"] = (int)f.curvesChannels;
        filters.push_back(fj);
    }
    j["filters"] = filters;

    json styles = json::array();
    for (const auto& s : layer.styles) {
        json sj;
        sj["type"] = StyleTypeToString(s.type);
        sj["enabled"] = s.enabled;
        sj["opacity"] = s.opacity;
        sj["shadow_color"] = { s.shadowColor[0], s.shadowColor[1], s.shadowColor[2], s.shadowColor[3] };
        sj["distance"] = s.distance;
        sj["angle"] = s.angleDeg;
        sj["offset"] = { s.offsetX, s.offsetY };
        sj["spread"] = s.spread;
        sj["size"] = s.size;
        sj["outline_color"] = { s.outlineColor[0], s.outlineColor[1], s.outlineColor[2], s.outlineColor[3] };
        sj["outline_size"] = s.outlineSize;
        sj["outline_position"] = (int)s.outlinePos;
        sj["outline_fill_mode"] = (int)s.outlineFill;
        sj["outline_texture_path"] = s.outlineTexturePath;
        if (!s.outlineTextureAssetKey.empty())
            sj["outline_texture_asset_key"] = s.outlineTextureAssetKey;
        sj["outline_gradient_map"] = (int)s.outlineGradientMap;
        sj["outline_tex_scale"] = { s.outlineTexScale[0], s.outlineTexScale[1] };
        sj["outline_tex_offset"] = { s.outlineTexOffset[0], s.outlineTexOffset[1] };
        if (!s.outlineGradient.empty()) {
            json g = json::array();
            for (const auto& st : s.outlineGradient) {
                g.push_back({ {"t", st.t}, {"rgba", {st.rgba[0], st.rgba[1], st.rgba[2], st.rgba[3]}} });
            }
            sj["outline_gradient"] = g;
        }
        styles.push_back(sj);
    }
    j["styles"] = styles;
    return j;
}

static void LayerFromJson(Layer& layer, const json& j) {
    if (j.contains("name")) layer.name = j["name"].get<std::string>();
    if (j.contains("visible")) layer.visible = j["visible"].get<bool>();
    if (j.contains("opacity")) layer.opacity = j["opacity"].get<float>();

    if (j.contains("blend_mode")) {
        if (j["blend_mode"].is_string()) {
            layer.blendMode = BlendModeFromString(j["blend_mode"].get<std::string>());
        } else if (j["blend_mode"].is_number_integer()) {
            layer.blendMode = static_cast<BlendMode>(j["blend_mode"].get<int>());
        }
    }
    if (j.contains("alpha_rewrite")) layer.alphaRewrite = j["alpha_rewrite"].get<bool>();

    if (j.contains("is_group")) layer.isGroup = j["is_group"].get<bool>();
    if (j.contains("parent_group_id")) layer.parentGroupId = j["parent_group_id"].get<int>();
    if (j.contains("group_expanded")) layer.groupExpanded = j["group_expanded"].get<bool>();
    if (j.contains("layer_type")) layer.type = LayerTypeFromString(j["layer_type"].get<std::string>());
    else if (layer.isGroup) layer.type = Layer::Type::Group;
    if (j.contains("smart_source_path")) layer.smartSourcePath = j["smart_source_path"].get<std::string>();
    if (j.contains("smart_scale")) layer.smartScale = j["smart_scale"].get<float>();
    if (j.contains("smart_asset_key") && j["smart_asset_key"].is_string()) {
        layer.smartAssetKey = j["smart_asset_key"].get<std::string>();
        if (!layer.smartAssetKey.empty()) {
            if (!assets::AssetStore::Get().AddRef(layer.smartAssetKey)) {
                std::string k = assets::AssetStore::Get().AcquireKey(layer.smartAssetKey);
                if (!k.empty()) layer.smartAssetKey = k;
                else assets::AssetStore::Get().RequestLoad(layer.smartAssetKey);
            }
        }
    }
    if (j.contains("has_mask")) layer.hasMask = j["has_mask"].get<bool>();

    if (j.contains("vector") || j.contains("vector_json")) {
        layer.vectorDoc = std::make_unique<vec::Document>();
        std::string raw;
        if (j.contains("vector_json") && j["vector_json"].is_string())
            raw = j["vector_json"].get<std::string>();
        else if (j.contains("vector"))
            raw = j["vector"].dump();
        if (!raw.empty() && !vec::DocumentFromJson(raw, *layer.vectorDoc))
            layer.vectorDoc.reset();
        if (layer.vectorDoc)
            layer.type = Layer::Type::VectorSvg;
    }

    if (j.contains("fill") && j["fill"].is_object()) {
        const auto& fj = j["fill"];
        if (fj.contains("target") && fj["target"].is_string())
            layer.fill.target = FillTargetFromString(fj["target"].get<std::string>());
        if (fj.contains("mode") && fj["mode"].is_string())
            layer.fill.mode = FillModeFromString(fj["mode"].get<std::string>());
        if (fj.contains("color") && fj["color"].is_array()) {
            for (int i = 0; i < 4 && i < (int)fj["color"].size(); ++i)
                layer.fill.color[i] = fj["color"][i].get<float>();
        }
        if (fj.contains("gray")) layer.fill.gray = fj["gray"].get<float>();
        if (fj.contains("use_texture")) layer.fill.useTexture = fj["use_texture"].get<bool>();
        if (fj.contains("texture_path")) layer.fill.texturePath = fj["texture_path"].get<std::string>();
        if (fj.contains("texture_asset_key") && fj["texture_asset_key"].is_string())
            layer.fill.textureAssetKey = fj["texture_asset_key"].get<std::string>();
        if (fj.contains("tex_scale") && fj["tex_scale"].is_array() && fj["tex_scale"].size() >= 2) {
            layer.fill.texScale[0] = fj["tex_scale"][0].get<float>();
            layer.fill.texScale[1] = fj["tex_scale"][1].get<float>();
        }
        if (fj.contains("tex_offset") && fj["tex_offset"].is_array() && fj["tex_offset"].size() >= 2) {
            layer.fill.texOffset[0] = fj["tex_offset"][0].get<float>();
            layer.fill.texOffset[1] = fj["tex_offset"][1].get<float>();
        }
        // Resolve texture: prefer asset key (proj/user/core/ext), migrate path as fallback.
        if (layer.fill.useTexture) {
            bool resolved = false;
            if (!layer.fill.textureAssetKey.empty()) {
                std::string key = assets::AssetStore::Get().AcquireKey(layer.fill.textureAssetKey);
                if (key.empty()) {
                    // Async request; keep key and dims if known
                    assets::AssetStore::Get().RequestLoad(layer.fill.textureAssetKey);
                    assets::AssetStore::Get().AddRef(layer.fill.textureAssetKey);
                    key = layer.fill.textureAssetKey;
                }
                if (!key.empty()) {
                    layer.fill.textureAssetKey = key;
                    int tw = 0, th = 0;
                    if (assets::AssetStore::Get().GetDims(key, tw, th)) {
                        layer.fill.textureW = tw;
                        layer.fill.textureH = th;
                    }
                    layer.fill.textureRgba.clear();
                    resolved = (assets::AssetStore::Get().GetLoadState(key) == assets::AssetLoadState::Ready)
                               || tw > 0;
                }
            }
            if (!resolved && !layer.fill.texturePath.empty()) {
                // Prefer project import for portability on next save
                std::string key = assets::AssetManager::Get().ImportFileToProject(layer.fill.texturePath);
                if (key.empty())
                    key = assets::AssetStore::Get().AcquireFile(layer.fill.texturePath);
                if (!key.empty()) {
                    layer.fill.textureAssetKey = key;
                    int tw = 0, th = 0;
                    if (assets::AssetStore::Get().GetDims(key, tw, th)) {
                        layer.fill.textureW = tw;
                        layer.fill.textureH = th;
                    }
                    layer.fill.textureRgba.clear();
                    resolved = true;
                }
            }
            if (!resolved && layer.fill.textureAssetKey.empty()) {
                layer.fill.useTexture = false;
            }
        }
        layer.fill.roles.clear();
        for (int i = 0; i < (int)texset::MapKind::Count; ++i)
            layer.fill.mapColor[i] = FillMapColor{};
        if (fj.contains("map_colors") && fj["map_colors"].is_array()) {
            for (const auto& mj : fj["map_colors"]) {
                texset::MapKind mk = texset::MapKindFromName(mj.value("kind", "Diffuse"));
                int i = (int)mk;
                if (i < 0 || i >= (int)texset::MapKind::Count) continue;
                layer.fill.mapColor[i].enabled = mj.value("enabled", false);
                if (mj.contains("rgba") && mj["rgba"].is_array()) {
                    for (int c = 0; c < 4 && c < (int)mj["rgba"].size(); ++c)
                        layer.fill.mapColor[i].rgba[c] = mj["rgba"][c].get<float>();
                }
                if (mj.contains("channel_mask"))
                    layer.fill.mapColor[i].channelMask = (uint8_t)std::clamp(mj["channel_mask"].get<int>(), 0, 15);
            }
        } else if (fj.contains("roles") && fj["roles"].is_array()) {
            for (const auto& rj : fj["roles"]) {
                FillRoleSlot rs;
                rs.role = texset::ChannelRoleFromName(rj.value("role", "BaseColor"));
                rs.enabled = rj.value("enabled", false);
                rs.mode = FillModeFromString(rj.value("mode", "RGB"));
                if (rj.contains("rgba") && rj["rgba"].is_array()) {
                    for (int i = 0; i < 4 && i < (int)rj["rgba"].size(); ++i)
                        rs.rgba[i] = rj["rgba"][i].get<float>();
                }
                rs.gray = rj.value("gray", 1.f);
                layer.fill.roles.push_back(rs);
            }
        }
        layer.fill.MigrateFromLegacy();
        if (layer.type != Layer::Type::Group)
            layer.type = Layer::Type::Fill;
        layer.SyncWorkSpaceFromFillTarget(nullptr);
    }

    if (j.contains("work_space") && j["work_space"].is_object()) {
        const auto& wj = j["work_space"];
        if (wj.contains("map_mask")) layer.workSpace.mapMask = wj["map_mask"].get<uint32_t>();
        if (wj.contains("role_mask")) layer.workSpace.roleMask = wj["role_mask"].get<uint32_t>();
        if (wj.contains("channel_write_mask"))
            layer.workSpace.channelWriteMask = (uint8_t)wj["channel_write_mask"].get<int>();
    }

    layer.filters.clear();
    if (j.contains("filters") && j["filters"].is_array()) {
        for (const auto& fj : j["filters"]) {
            LayerFilter f;
            if (fj.contains("type")) {
                if (fj["type"].is_string()) f.type = FilterTypeFromString(fj["type"].get<std::string>());
                else if (fj["type"].is_number_integer()) f.type = static_cast<FilterType>(fj["type"].get<int>());
            }
            if (fj.contains("enabled")) f.enabled = fj["enabled"].get<bool>();
            if (fj.contains("p") && fj["p"].is_array()) {
                for (int i = 0; i < 4 && i < (int)fj["p"].size(); ++i) {
                    f.p[i] = fj["p"][i].get<float>();
                }
            }
            if (fj.contains("lut") && fj["lut"].is_array())
                f.lut = fj["lut"].get<std::vector<float>>();
            if (fj.contains("lut_r") && fj["lut_r"].is_array())
                f.lutR = fj["lut_r"].get<std::vector<float>>();
            if (fj.contains("lut_g") && fj["lut_g"].is_array())
                f.lutG = fj["lut_g"].get<std::vector<float>>();
            if (fj.contains("lut_b") && fj["lut_b"].is_array())
                f.lutB = fj["lut_b"].get<std::vector<float>>();
            if (fj.contains("lut_a") && fj["lut_a"].is_array())
                f.lutA = fj["lut_a"].get<std::vector<float>>();
            if (fj.contains("curves_channels"))
                f.curvesChannels = (uint8_t)fj["curves_channels"].get<int>();
            else if (f.type == FilterType::Curves)
                f.curvesChannels = 0x7; // RGB on, A off
            layer.filters.push_back(std::move(f));
        }
    }
    layer.filtersDirty = !layer.filters.empty();

    layer.styles.clear();
    if (j.contains("styles") && j["styles"].is_array()) {
        for (const auto& sj : j["styles"]) {
            LayerStyle s;
            if (sj.contains("type")) {
                if (sj["type"].is_string()) s.type = StyleTypeFromString(sj["type"].get<std::string>());
                else if (sj["type"].is_number_integer()) s.type = static_cast<StyleType>(sj["type"].get<int>());
            }
            if (sj.contains("enabled")) s.enabled = sj["enabled"].get<bool>();
            if (sj.contains("opacity")) s.opacity = sj["opacity"].get<float>();
            auto read4 = [&](const char* key, float* dst) {
                if (sj.contains(key) && sj[key].is_array()) {
                    for (int i = 0; i < 4 && i < (int)sj[key].size(); ++i)
                        dst[i] = sj[key][i].get<float>();
                }
            };
            read4("shadow_color", s.shadowColor);
            read4("outline_color", s.outlineColor);
            if (sj.contains("distance")) s.distance = sj["distance"].get<float>();
            if (sj.contains("angle")) s.angleDeg = sj["angle"].get<float>();
            if (sj.contains("offset") && sj["offset"].is_array() && sj["offset"].size() >= 2) {
                s.offsetX = sj["offset"][0].get<float>();
                s.offsetY = sj["offset"][1].get<float>();
            }
            if (sj.contains("spread")) s.spread = sj["spread"].get<float>();
            if (sj.contains("size")) s.size = sj["size"].get<float>();
            if (sj.contains("outline_size")) s.outlineSize = sj["outline_size"].get<float>();
            if (sj.contains("outline_position")) s.outlinePos = static_cast<OutlinePosition>(sj["outline_position"].get<int>());
            if (sj.contains("outline_fill_mode")) s.outlineFill = static_cast<OutlineFillMode>(sj["outline_fill_mode"].get<int>());
            if (sj.contains("outline_texture_path")) s.outlineTexturePath = sj["outline_texture_path"].get<std::string>();
            if (sj.contains("outline_texture_asset_key") && sj["outline_texture_asset_key"].is_string())
                s.outlineTextureAssetKey = sj["outline_texture_asset_key"].get<std::string>();
            if (sj.contains("outline_gradient_map")) s.outlineGradientMap = (uint8_t)sj["outline_gradient_map"].get<int>();
            if (sj.contains("outline_tex_scale") && sj["outline_tex_scale"].is_array() && sj["outline_tex_scale"].size() >= 2) {
                s.outlineTexScale[0] = sj["outline_tex_scale"][0].get<float>();
                s.outlineTexScale[1] = sj["outline_tex_scale"][1].get<float>();
            }
            if (sj.contains("outline_tex_offset") && sj["outline_tex_offset"].is_array() && sj["outline_tex_offset"].size() >= 2) {
                s.outlineTexOffset[0] = sj["outline_tex_offset"][0].get<float>();
                s.outlineTexOffset[1] = sj["outline_tex_offset"][1].get<float>();
            }
            if (sj.contains("outline_gradient") && sj["outline_gradient"].is_array()) {
                for (const auto& g : sj["outline_gradient"]) {
                    GradientStop st;
                    if (g.contains("t")) st.t = g["t"].get<float>();
                    if (g.contains("rgba") && g["rgba"].is_array()) {
                        for (int i = 0; i < 4 && i < (int)g["rgba"].size(); ++i)
                            st.rgba[i] = g["rgba"][i].get<float>();
                    }
                    s.outlineGradient.push_back(st);
                }
            }
            // Resolve outline texture: asset key first, path migration second
            if (s.outlineFill == OutlineFillMode::Texture) {
                bool resolved = false;
                if (!s.outlineTextureAssetKey.empty()) {
                    std::string key = assets::AssetStore::Get().AcquireKey(s.outlineTextureAssetKey);
                    if (key.empty()) {
                        assets::AssetStore::Get().RequestLoad(s.outlineTextureAssetKey);
                        assets::AssetStore::Get().AddRef(s.outlineTextureAssetKey);
                        key = s.outlineTextureAssetKey;
                    }
                    if (!key.empty()) {
                        s.outlineTextureAssetKey = key;
                        int tw = 0, th = 0;
                        if (assets::AssetStore::Get().GetDims(key, tw, th)) {
                            s.outlineTextureW = tw;
                            s.outlineTextureH = th;
                        }
                        s.outlineTextureRgba.clear();
                        resolved = true;
                    }
                }
                if (!resolved && !s.outlineTexturePath.empty()) {
                    std::string key = assets::AssetManager::Get().ImportFileToProject(s.outlineTexturePath);
                    if (key.empty())
                        key = assets::AssetStore::Get().AcquireFile(s.outlineTexturePath);
                    if (!key.empty()) {
                        s.outlineTextureAssetKey = key;
                        int tw = 0, th = 0;
                        if (assets::AssetStore::Get().GetDims(key, tw, th)) {
                            s.outlineTextureW = tw;
                            s.outlineTextureH = th;
                        }
                        s.outlineTextureRgba.clear();
                        resolved = true;
                    }
                }
                if (!resolved) {
                    // keep mode; bake will fall back to solid tint if no payload
                }
            }
            layer.styles.push_back(std::move(s));
        }
    }
    layer.stylesDirty = !layer.styles.empty();
    layer.presentationDirty = true;
}

static bool WriteZlibBlob(std::ostream& out, const void* data, size_t uncompressedBytes) {
    int compSize = 0;
    unsigned char* compData = stbi_zlib_compress(
        reinterpret_cast<unsigned char*>(const_cast<void*>(data)),
        static_cast<int>(uncompressedBytes),
        &compSize,
        8
    );
    if (!compData) return false;
    uint64_t uncompressedSize = uncompressedBytes;
    uint64_t compressedSize = (uint64_t)compSize;
    out.write(reinterpret_cast<const char*>(&uncompressedSize), sizeof(uncompressedSize));
    out.write(reinterpret_cast<const char*>(&compressedSize), sizeof(compressedSize));
    out.write(reinterpret_cast<const char*>(compData), compressedSize);
    free(compData);
    return true;
}

static bool ReadZlibBlob(std::istream& in, std::vector<uint8_t>& outBytes) {
    uint64_t uncompressedSize = 0;
    uint64_t compressedSize = 0;
    in.read(reinterpret_cast<char*>(&uncompressedSize), sizeof(uncompressedSize));
    in.read(reinterpret_cast<char*>(&compressedSize), sizeof(compressedSize));
    if (!in || compressedSize > (1ull << 32)) return false;

    std::vector<uint8_t> compressedBytes(compressedSize);
    in.read(reinterpret_cast<char*>(compressedBytes.data()), (std::streamsize)compressedSize);
    if (!in) return false;

    int decompSize = 0;
    char* decompData = stbi_zlib_decode_malloc(
        reinterpret_cast<const char*>(compressedBytes.data()),
        static_cast<int>(compressedSize),
        &decompSize
    );
    if (!decompData || static_cast<size_t>(decompSize) != uncompressedSize) {
        if (decompData) free(decompData);
        return false;
    }
    outBytes.resize(uncompressedSize);
    std::memcpy(outBytes.data(), decompData, uncompressedSize);
    free(decompData);
    return true;
}

bool Canvas::SaveCanvasRayp(const std::string& filepath) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save in RAYP.");
        return false;
    }

    try {
        json metadata;
        metadata["width"] = m_Width;
        metadata["height"] = m_Height;
        metadata["active_layer"] = m_ActiveLayerIdx;
        metadata["project_type"] =
            (m_ProjectType == ProjectType::Simple) ? "simple" :
            (m_ProjectType == ProjectType::AdvancedModMode) ? "advanced_mod" : "advanced";
        metadata["document_bit_depth"] = (m_DocumentBitDepth == DocumentBitDepth::F32) ? "f32"
            : (m_DocumentBitDepth == DocumentBitDepth::F16) ? "f16" : "u8";
        metadata["format_features"] = "blend_filters_mask_groups";

        metadata["export_path"] = m_ExportPath;
        metadata["export_container"] = (m_ExportContainer == ExportContainer::DDS) ? "dds" : "png";
        metadata["export_format"] = m_ExportFormat;
        metadata["export_advanced_mode"] = m_ExportAdvancedMode;
        metadata["export_compression_speed"] = m_ExportCompressionSpeed;
        metadata["export_generate_mip_maps"] = m_ExportGenerateMipMaps;
        metadata["export_mip_filter"] = m_ExportMipFilter;
        metadata["export_png_color_space"] = m_ExportPngColorSpace;
        metadata["export_icc_preset"] = IccPresetName(m_ExportIccPreset);
        metadata["brush_tip_id"] = m_BrushTipId;
        if (m_BrushTipId == "custom" && m_CustomBrushTipSize > 0 && !m_CustomBrushTipPixels.empty()) {
            metadata["brush_tip_custom_size"] = m_CustomBrushTipSize;
            // store as array of ints (json-friendly)
            metadata["brush_tip_custom_pixels"] = m_CustomBrushTipPixels;
        }

        // Advanced Mod Mode — optional paths (3D preview sources). Safe to omit on other types.
        if (m_ProjectType == ProjectType::AdvancedModMode ||
            !m_ModIniPath.empty() || !m_ModDumpPath.empty()) {
            json mod;
            mod["ini_path"] = m_ModIniPath;
            mod["dump_path"] = m_ModDumpPath;
            metadata["mod"] = mod;
        }

        // Texture Set library meta (Project injects before save)
        if (!m_TextureSetsMetaJson.empty()) {
            try {
                metadata["texture_sets"] = json::parse(m_TextureSetsMetaJson);
            } catch (...) {
                Logger::Get().Warn("Save RAYP: texture_sets meta JSON invalid — skipped");
            }
        }

        // Promote external (ext:) / path-bound assets into project session so .rayp is portable.
        auto promoteToProject = [&](std::string& key) {
            if (key.empty()) return;
            if (key.size() >= 5 && key.compare(0, 5, "proj:") == 0) return;
            if (key.size() >= 5 && (key.compare(0, 5, "core:") == 0 || key.compare(0, 5, "user:") == 0))
                return; // library-local; not packed (user must have library / app)
            // ext: or unknown → copy payload into proj:
            auto pay = assets::AssetStore::Get().GetPayload(key);
            std::string name = assets::AssetManager::Get().DisplayName(key);
            if (pay && !pay->rgba.empty()) {
                std::string newKey = assets::AssetManager::Get().RegisterProjectRgba(
                    name.empty() ? "texture" : name, pay->w, pay->h,
                    std::vector<uint8_t>(pay->rgba.begin(), pay->rgba.end()));
                if (!newKey.empty()) {
                    assets::AssetStore::Get().Release(key);
                    key = newKey; // Register left ref=1
                }
                return;
            }
            // Try re-acquire from resolved path
            std::string path = assets::AssetStore::ResolvePath(key);
            if (path.empty()) {
                if (const assets::TextureAsset* a = assets::AssetStore::Get().Get(key))
                    path = a->sourcePath;
            }
            if (!path.empty()) {
                std::string newKey = assets::AssetManager::Get().ImportFileToProject(path);
                if (!newKey.empty()) {
                    assets::AssetStore::Get().Release(key);
                    key = newKey;
                }
            }
        };
        for (auto& layer : m_Layers) {
            if (!layer.fill.textureAssetKey.empty())
                promoteToProject(layer.fill.textureAssetKey);
            if (!layer.smartAssetKey.empty())
                promoteToProject(layer.smartAssetKey);
            for (auto& st : layer.styles) {
                if (!st.outlineTextureAssetKey.empty())
                    promoteToProject(st.outlineTextureAssetKey);
            }
        }

        // Project assets referenced by fill / outline / smart object keys
        std::vector<std::string> refKeys;
        {
            std::unordered_set<std::string> uniq;
            for (const auto& layer : m_Layers) {
                if (!layer.fill.textureAssetKey.empty() &&
                    layer.fill.textureAssetKey.compare(0, 5, "proj:") == 0)
                    uniq.insert(layer.fill.textureAssetKey);
                if (!layer.smartAssetKey.empty() &&
                    layer.smartAssetKey.compare(0, 5, "proj:") == 0)
                    uniq.insert(layer.smartAssetKey);
                for (const auto& st : layer.styles) {
                    if (!st.outlineTextureAssetKey.empty() &&
                        st.outlineTextureAssetKey.compare(0, 5, "proj:") == 0)
                        uniq.insert(st.outlineTextureAssetKey);
                }
            }
            refKeys.assign(uniq.begin(), uniq.end());
        }
        auto projBlobs = assets::AssetManager::Get().CollectProjectBlobs(refKeys);
        json projArr = json::array();
        for (size_t i = 0; i < projBlobs.size(); ++i) {
            json pj;
            pj["key"] = projBlobs[i].key;
            pj["name"] = projBlobs[i].name;
            pj["kind"] = projBlobs[i].kind;
            pj["mime"] = projBlobs[i].mime;
            pj["w"] = projBlobs[i].w;
            pj["h"] = projBlobs[i].h;
            pj["is_rgba"] = projBlobs[i].isRgba;
            pj["blob_index"] = (int)i;
            projArr.push_back(std::move(pj));
        }
        if (!projArr.empty())
            metadata["project_assets"] = projArr;

        json layersArray = json::array();
        for (const auto& layer : m_Layers) {
            layersArray.push_back(LayerToJson(layer));
        }
        metadata["layers"] = layersArray;

        std::string metadataStr = metadata.dump();

#ifdef _WIN32
        std::ofstream out(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(filepath)), std::ios::binary);
#else
        std::ofstream out(filepath, std::ios::binary);
#endif
        if (!out.is_open()) {
            Logger::Get().Error("Could not open file for saving RAYP: " + filepath);
            return false;
        }

        out.write("RAYP", 4);
        uint32_t version = kRaypVersionCurrent;
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));

        uint64_t metadataSize = metadataStr.size();
        out.write(reinterpret_cast<const char*>(&metadataSize), sizeof(metadataSize));
        out.write(metadataStr.data(), metadataStr.size());

        // Project asset blobs BEFORE layer pixels so load can rehydrate keys first.
        for (const auto& blob : projBlobs) {
            if (!WriteZlibBlob(out, blob.bytes.data(), blob.bytes.size())) {
                Logger::Get().Error("Failed to compress project asset " + blob.key);
                return false;
            }
        }

        // Pixels (float RGBA) + optional mask blob per layer (v2)
        for (auto& layer : m_Layers) {
            std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);
            if (!WriteZlibBlob(out, layerPixels.data(), layerPixels.size() * sizeof(float))) {
                Logger::Get().Error("Failed to compress layer data for " + layer.name);
                return false;
            }

            // v2: mask follows pixels when present
            if (layer.hasMask && !layer.mask.empty()) {
                if (!WriteZlibBlob(out, layer.mask.data(), layer.mask.size())) {
                    Logger::Get().Error("Failed to compress mask for " + layer.name);
                    return false;
                }
            }
            // smart object / SVG source bytes (after mask)
            if (!layer.smartSourceBytes.empty()) {
                if (!WriteZlibBlob(out, layer.smartSourceBytes.data(), layer.smartSourceBytes.size())) {
                    Logger::Get().Error("Failed to compress smart source for " + layer.name);
                    return false;
                }
            }
        }

        m_IsDocumentModified = false;
        m_CurrentProjectFilePath = filepath;
        Logger::Get().Info("Successfully saved project to " + filepath +
            " (RAYP v" + std::to_string(kRaypVersionCurrent) +
            ", layers=" + std::to_string(m_Layers.size()) +
            ", project_assets=" + std::to_string(projBlobs.size()) + ")");
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Exception saving RAYP: " + std::string(e.what()));
        return false;
    }
}

bool Canvas::LoadCanvasRayp(const std::string& filepath, ID3D11Device* device, LoadProgressFn progress) {
    try {
        if (progress) progress(0.02f, "open");
        // 1. Open binary file for reading
#ifdef _WIN32
        std::ifstream in(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(filepath)), std::ios::binary);
#else
        std::ifstream in(filepath, std::ios::binary);
#endif
        if (!in.is_open()) {
            Logger::Get().Error("Could not open file for loading RAYP: " + filepath);
            return false;
        }

        // Read Magic header
        char magic[4];
        in.read(magic, 4);
        if (std::strncmp(magic, "RAYP", 4) != 0) {
            Logger::Get().Error("Invalid RAYP magic signature.");
            return false;
        }

        // Read format version (1 = basic, 2 = blend/filters/mask/groups)
        uint32_t version = 0;
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (version != 1 && version != 2) {
            Logger::Get().Error("Unsupported RAYP version: " + std::to_string(version));
            return false;
        }

        uint64_t metadataSize = 0;
        in.read(reinterpret_cast<char*>(&metadataSize), sizeof(metadataSize));

        std::string metadataStr;
        metadataStr.resize(metadataSize);
        in.read(&metadataStr[0], metadataSize);

        json metadata = json::parse(metadataStr);

        for (auto& layer : m_Layers) {
            if (!layer.fill.textureAssetKey.empty()) {
                assets::AssetStore::Get().Release(layer.fill.textureAssetKey);
                layer.fill.textureAssetKey.clear();
            }
            if (!layer.smartAssetKey.empty()) {
                assets::AssetStore::Get().Release(layer.smartAssetKey);
                layer.smartAssetKey.clear();
            }
            for (auto& st : layer.styles) {
                if (!st.outlineTextureAssetKey.empty()) {
                    assets::AssetStore::Get().Release(st.outlineTextureAssetKey);
                    st.outlineTextureAssetKey.clear();
                }
            }
            ReleaseFillPatternGpu(layer);
            if (layer.texture) layer.texture->Release();
            if (layer.srv) layer.srv->Release();
            if (layer.maskTexture) layer.maskTexture->Release();
            if (layer.maskSRV) layer.maskSRV->Release();
            if (layer.thumbSRV) { layer.thumbSRV->Release(); layer.thumbSRV = nullptr; }
            if (layer.thumbTex) { layer.thumbTex->Release(); layer.thumbTex = nullptr; }
        }
        m_Layers.clear();
        assets::AssetManager::Get().ClearProjectSession();

        m_Width = metadata["width"].get<int>();
        m_Height = metadata["height"].get<int>();
        m_ActiveLayerIdx = metadata["active_layer"].get<int>();
        m_CurrentProjectFilePath = filepath;
        m_CanvasFormat = CanvasPixelFormat::RGBA8;

        if (metadata.contains("project_type")) {
            std::string pt = metadata["project_type"].get<std::string>();
            if (pt == "simple") m_ProjectType = ProjectType::Simple;
            else if (pt == "advanced_mod") m_ProjectType = ProjectType::AdvancedModMode;
            else m_ProjectType = ProjectType::Advanced;
        } else {
            m_ProjectType = ProjectType::Advanced;
        }
        if (metadata.contains("document_bit_depth")) {
            std::string bd = metadata["document_bit_depth"].get<std::string>();
            if (bd == "f32") m_DocumentBitDepth = DocumentBitDepth::F32;
            else if (bd == "f16") m_DocumentBitDepth = DocumentBitDepth::F16;
            else m_DocumentBitDepth = DocumentBitDepth::U8;
            m_CanvasFormat = FormatForBitDepth(m_DocumentBitDepth);
        }

        if (metadata.contains("export_path")) m_ExportPath = metadata["export_path"].get<std::string>();
        if (metadata.contains("export_container")) {
            std::string ec = metadata["export_container"].get<std::string>();
            for (char& c : ec) c = (char)std::tolower((unsigned char)c);
            m_ExportContainer = (ec == "dds") ? ExportContainer::DDS : ExportContainer::PNG;
        } else if (metadata.contains("export_path")) {
            // Legacy: infer once from path, then container is authoritative
            std::string p = m_ExportPath;
            size_t d = p.find_last_of('.');
            std::string e = (d != std::string::npos) ? p.substr(d + 1) : "";
            for (char& c : e) c = (char)std::tolower((unsigned char)c);
            m_ExportContainer = (e == "dds") ? ExportContainer::DDS : ExportContainer::PNG;
        }
        if (metadata.contains("export_format")) m_ExportFormat = metadata["export_format"].get<std::string>();
        if (metadata.contains("export_advanced_mode")) m_ExportAdvancedMode = metadata["export_advanced_mode"].get<bool>();
        if (metadata.contains("export_compression_speed")) m_ExportCompressionSpeed = metadata["export_compression_speed"].get<std::string>();
        if (metadata.contains("export_generate_mip_maps")) m_ExportGenerateMipMaps = metadata["export_generate_mip_maps"].get<bool>();
        if (metadata.contains("export_mip_filter")) m_ExportMipFilter = metadata["export_mip_filter"].get<std::string>();
        if (metadata.contains("export_png_color_space")) m_ExportPngColorSpace = metadata["export_png_color_space"].get<std::string>();
        if (metadata.contains("export_icc_preset")) {
            m_ExportIccPreset = IccPresetFromName(metadata["export_icc_preset"].get<std::string>());
            m_ExportPngColorSpace = IccPresetName(m_ExportIccPreset);
        } else if (!m_ExportPngColorSpace.empty()) {
            // Migrate legacy free-text / name into preset enum
            m_ExportIccPreset = IccPresetFromName(m_ExportPngColorSpace);
        }
        SyncExportPathExtension();
        if (metadata.contains("brush_tip_id")) m_BrushTipId = metadata["brush_tip_id"].get<std::string>();

        // Texture Set library meta → Project reads after load
        m_TextureSetsMetaJson.clear();
        if (metadata.contains("texture_sets")) {
            try {
                m_TextureSetsMetaJson = metadata["texture_sets"].dump();
            } catch (...) {
                m_TextureSetsMetaJson.clear();
            }
        }

        // Mod preview sources (soft — never fail the load)
        m_ModIniPath.clear();
        m_ModDumpPath.clear();
        m_ModScene = modio::ModScene{};
        m_ModParseSummary.clear();
        if (metadata.contains("mod") && metadata["mod"].is_object()) {
            const auto& mod = metadata["mod"];
            try {
                if (mod.contains("ini_path")) m_ModIniPath = mod["ini_path"].get<std::string>();
                if (mod.contains("dump_path")) m_ModDumpPath = mod["dump_path"].get<std::string>();
            } catch (...) {
                Logger::Get().Warn("RAYP mod block present but unreadable — ignoring");
            }
            if (m_ProjectType == ProjectType::AdvancedModMode && !m_ModIniPath.empty()) {
                // Best-effort reparse; paint continues even if ini missing
                ApplyModIniParse();
            }
        }

        if (metadata.contains("brush_tip_custom_size") && metadata.contains("brush_tip_custom_pixels")) {
            m_CustomBrushTipSize = metadata["brush_tip_custom_size"].get<int>();
            m_CustomBrushTipPixels = metadata["brush_tip_custom_pixels"].get<std::vector<uint8_t>>();
        }

        CreateCompositeResources(device);
        if (progress) progress(0.12f, "metadata");

        // Rehydrate project assets (blobs sit before layer pixels when present)
        if (metadata.contains("project_assets") && metadata["project_assets"].is_array()) {
            std::vector<assets::AssetManager::ProjectAssetBlob> blobs;
            for (const auto& pj : metadata["project_assets"]) {
                assets::AssetManager::ProjectAssetBlob b;
                b.key = pj.value("key", "");
                b.name = pj.value("name", "");
                b.kind = pj.value("kind", "texture");
                b.mime = pj.value("mime", "image/png");
                b.w = pj.value("w", 0);
                b.h = pj.value("h", 0);
                b.isRgba = pj.value("is_rgba", false);
                std::vector<uint8_t> raw;
                if (!ReadZlibBlob(in, raw)) {
                    Logger::Get().Error("Failed to decompress project asset " + b.key);
                    return false;
                }
                b.bytes = std::move(raw);
                blobs.push_back(std::move(b));
            }
            assets::AssetManager::Get().LoadProjectBlobs(blobs);
            Logger::Get().Info("RAYP: rehydrated " + std::to_string(blobs.size()) + " project asset(s)");
        }
        if (progress) progress(0.15f, "assets");

        auto layersArray = metadata["layers"];
        const size_t layerCount = layersArray.size();
        for (size_t idx = 0; idx < layerCount; ++idx) {
            if (progress && layerCount > 0) {
                float t = 0.15f + 0.75f * ((float)idx / (float)layerCount);
                progress(t, "layer");
            }
            Layer layer;
            LayerFromJson(layer, layersArray[idx]);

            std::vector<uint8_t> pixelBytes;
            if (!ReadZlibBlob(in, pixelBytes)) {
                Logger::Get().Error("Failed to decompress layer data for " + layer.name);
                return false;
            }

            if (!layer.isGroup) {
                if (layer.IsFill()) {
                    // Pixel blob is unused (fill is parametric); still consumed for format sync.
                    layer.tileCache.reset();
                    layer.presentationDirty = true;
                    layer.stylesDirty = !layer.styles.empty();
                    layer.filtersDirty = !layer.filters.empty();
                    EnsureFillLayerGpu(device, layer);
                } else {
                    if (pixelBytes.size() % sizeof(float) != 0) {
                        Logger::Get().Error("Corrupt layer pixel blob for " + layer.name);
                        return false;
                    }
                    std::vector<float> layerPixels(pixelBytes.size() / sizeof(float));
                    std::memcpy(layerPixels.data(), pixelBytes.data(), pixelBytes.size());

                    layer.tileCache = std::make_unique<TileCache>();
                    layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
                    // Vector layers: prefer re-raster from geometry (sharp, editable).
                    if (layer.IsVector() && layer.vectorDoc) {
                        layer.vectorDoc->MarkAllDirty(m_Width, m_Height);
                        vec::RasterizeDocumentFull(*layer.vectorDoc, *layer.tileCache,
                                                   m_Width, m_Height, false);
                        layer.vectorDoc->rasterGen = layer.vectorDoc->generation;
                        layer.vectorDoc->ClearDirty();
                    } else {
                        layer.tileCache->ImportRGBA32F(layerPixels.data(), m_Width, m_Height);
                    }
                    layer.tileCache->MarkAllDirty();
                    layer.needsUpload = true;
                    layer.presentationDirty = true;
                    layer.stylesDirty = !layer.styles.empty();
                    RecreateLayerTexture(device, layer);
                }
            }

            // v2: mask blob after pixels when has_mask
            if (version >= 2 && layer.hasMask) {
                std::vector<uint8_t> maskBytes;
                if (!ReadZlibBlob(in, maskBytes)) {
                    Logger::Get().Error("Failed to decompress mask for " + layer.name);
                    return false;
                }
                layer.mask = std::move(maskBytes);
                layer.maskNeedsUpload = true;
            } else if (version < 2) {
                layer.hasMask = false;
            }

            // smart source bytes (SVG / smart object payload)
            bool hasSmart = layersArray[idx].value("has_smart_source", false);
            if (version >= 2 && hasSmart) {
                std::vector<uint8_t> smartBytes;
                if (!ReadZlibBlob(in, smartBytes)) {
                    Logger::Get().Error("Failed to decompress smart source for " + layer.name);
                    return false;
                }
                layer.smartSourceBytes = std::move(smartBytes);
            }

            m_Layers.push_back(std::move(layer));
            if (m_Layers.back().hasMask && !m_Layers.back().mask.empty() && device) {
                UpdateLayerMaskTexture(device, static_cast<int>(m_Layers.size()) - 1);
            }
        }
        m_CompositeDirty = true;

        m_UndoRedoManager.Clear();
        m_IsDocumentModified = false;
        m_CurrentProjectFilePath = filepath;
        m_PaintTarget = PaintTarget::LayerContent;
        if (progress) progress(0.98f, "finalize");
        MemoryStats::LogSnapshot("after_LoadCanvasRayp");
        Logger::Get().Info("Successfully loaded project from " + filepath +
            " (RAYP v" + std::to_string(version) +
            ", " + std::to_string(m_Width) + "x" + std::to_string(m_Height) +
            ", layers=" + std::to_string(m_Layers.size()) + ")");
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Exception loading RAYP: " + std::string(e.what()));
        return false;
    }
}

// Async autosave: snapshot full layer JSON + pixels + masks on the calling thread,
// then write on a worker (same v2 layout as SaveCanvasRayp).
struct RaypLayerSnapshot {
    json meta;
    std::vector<float> pixels;
    std::vector<uint8_t> mask;
    bool hasMask = false;
};

static bool SaveCanvasRaypFromSnapshots(
    const std::string& filepath,
    int width, int height, int activeLayerIdx,
    const std::string& projectType,
    const json& exportMeta,
    const std::vector<RaypLayerSnapshot>& layers)
{
    try {
        json metadata = exportMeta;
        metadata["width"] = width;
        metadata["height"] = height;
        metadata["active_layer"] = activeLayerIdx;
        metadata["project_type"] = projectType;
        metadata["format_features"] = "blend_filters_mask_groups";

        json layersArray = json::array();
        for (const auto& L : layers) layersArray.push_back(L.meta);
        metadata["layers"] = layersArray;

        std::string metadataStr = metadata.dump();
#ifdef _WIN32
        std::ofstream out(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(filepath)), std::ios::binary);
#else
        std::ofstream out(filepath, std::ios::binary);
#endif
        if (!out.is_open()) return false;

        out.write("RAYP", 4);
        uint32_t version = kRaypVersionCurrent;
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        uint64_t metadataSize = metadataStr.size();
        out.write(reinterpret_cast<const char*>(&metadataSize), sizeof(metadataSize));
        out.write(metadataStr.data(), metadataStr.size());

        for (const auto& L : layers) {
            if (!WriteZlibBlob(out, L.pixels.data(), L.pixels.size() * sizeof(float))) return false;
            if (L.hasMask && !L.mask.empty()) {
                if (!WriteZlibBlob(out, L.mask.data(), L.mask.size())) return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

void Canvas::SaveCanvasRaypAsync(const std::string& filepath,
                                 std::function<void(bool)> callback,
                                 const std::string& previewPngPath) {
    int width = m_Width;
    int height = m_Height;
    int activeLayer = m_ActiveLayerIdx;
    std::string projectType =
        (m_ProjectType == ProjectType::Simple) ? "simple" :
        (m_ProjectType == ProjectType::AdvancedModMode) ? "advanced_mod" : "advanced";

    json exportMeta;
    exportMeta["export_path"] = m_ExportPath;
    exportMeta["export_container"] = (m_ExportContainer == ExportContainer::DDS) ? "dds" : "png";
    exportMeta["export_format"] = m_ExportFormat;
    exportMeta["export_advanced_mode"] = m_ExportAdvancedMode;
    exportMeta["export_compression_speed"] = m_ExportCompressionSpeed;
    exportMeta["export_generate_mip_maps"] = m_ExportGenerateMipMaps;
    exportMeta["export_mip_filter"] = m_ExportMipFilter;
    exportMeta["export_png_color_space"] = m_ExportPngColorSpace;
    exportMeta["export_icc_preset"] = IccPresetName(m_ExportIccPreset);
    exportMeta["brush_tip_id"] = m_BrushTipId;
    if (!m_TextureSetsMetaJson.empty()) {
        try {
            exportMeta["texture_sets"] = json::parse(m_TextureSetsMetaJson);
        } catch (...) {}
    }

    std::vector<RaypLayerSnapshot> layers;
    layers.reserve(m_Layers.size());
    for (const auto& layer : m_Layers) {
        RaypLayerSnapshot snap;
        snap.meta = LayerToJson(layer);
        snap.pixels = ExportLayerF(layer, width, height);
        snap.hasMask = layer.hasMask && !layer.mask.empty();
        if (snap.hasMask) snap.mask = layer.mask;
        layers.push_back(std::move(snap));
    }

    std::thread([filepath, previewPngPath, width, height, activeLayer, projectType, exportMeta,
                 layers = std::move(layers), callback]() mutable {
        bool success = SaveCanvasRaypFromSnapshots(
            filepath, width, height, activeLayer, projectType, exportMeta, layers);

        // Small preview for Recent Autosaves UI (worker-side; does not touch live Canvas).
        if (success && !previewPngPath.empty() && width > 0 && height > 0 && !layers.empty()) {
            try {
                const int maxSide = 128;
                float scale = 1.f;
                if (width > maxSide || height > maxSide)
                    scale = (float)maxSide / (float)std::max(width, height);
                const int pw = std::max(1, (int)(width * scale + 0.5f));
                const int ph = std::max(1, (int)(height * scale + 0.5f));
                std::vector<uint8_t> out((size_t)pw * ph * 4, 0);

                auto sampleLayer = [&](const RaypLayerSnapshot& L, int dx, int dy,
                                       float& r, float& g, float& b, float& a) {
                    r = g = b = a = 0.f;
                    if (L.pixels.size() < (size_t)width * height * 4) return;
                    int sx = std::min(width - 1, (int)((dx + 0.5f) * width / (float)pw));
                    int sy = std::min(height - 1, (int)((dy + 0.5f) * height / (float)ph));
                    size_t i = ((size_t)sy * width + sx) * 4;
                    r = L.pixels[i + 0]; g = L.pixels[i + 1];
                    b = L.pixels[i + 2]; a = L.pixels[i + 3];
                    if (L.hasMask && L.mask.size() == (size_t)width * height)
                        a *= (L.mask[(size_t)sy * width + sx] / 255.f);
                };

                for (int y = 0; y < ph; ++y) {
                    for (int x = 0; x < pw; ++x) {
                        float dr = 0, dg = 0, db = 0, da = 0;
                        for (const auto& L : layers) {
                            bool visible = true;
                            try {
                                if (L.meta.contains("visible"))
                                    visible = L.meta["visible"].get<bool>();
                                if (L.meta.contains("isGroup") && L.meta["isGroup"].get<bool>())
                                    continue;
                            } catch (...) {}
                            if (!visible) continue;
                            float sr, sg, sb, sa;
                            sampleLayer(L, x, y, sr, sg, sb, sa);
                            // src-over
                            float outA = sa + da * (1.f - sa);
                            if (outA > 1e-6f) {
                                dr = (sr * sa + dr * da * (1.f - sa)) / outA;
                                dg = (sg * sa + dg * da * (1.f - sa)) / outA;
                                db = (sb * sa + db * da * (1.f - sa)) / outA;
                                da = outA;
                            }
                        }
                        size_t o = ((size_t)y * pw + x) * 4;
                        out[o + 0] = (uint8_t)std::clamp(dr * 255.f, 0.f, 255.f);
                        out[o + 1] = (uint8_t)std::clamp(dg * 255.f, 0.f, 255.f);
                        out[o + 2] = (uint8_t)std::clamp(db * 255.f, 0.f, 255.f);
                        out[o + 3] = (uint8_t)std::clamp(da * 255.f, 0.f, 255.f);
                    }
                }
                ImageManager::SaveRGBA8ToFile(previewPngPath, out.data(), pw, ph);
            } catch (...) {}
        }

        if (callback) callback(success);
    }).detach();
}

std::vector<float> Canvas::GetComposedPixels() {
    return ComposeVisibleLayers(m_Layers, m_Width, m_Height);
}

bool Canvas::BeginScriptPixelEdit(int layerIndex) {
    if (layerIndex < 0 || layerIndex >= (int)m_Layers.size()) {
        Logger::Get().ErrorTag("script", "BeginScriptPixelEdit: bad layer");
        return false;
    }
    auto& layer = m_Layers[layerIndex];
    if (layer.isGroup || layer.IsFill() || !layer.CanPaintContent()) {
        Logger::Get().ErrorTag("script", "BeginScriptPixelEdit: layer not paint-raster");
        return false;
    }
    if (m_ScriptPixelEditActive) {
        Logger::Get().WarnTag("script", "BeginScriptPixelEdit: previous session still open — cancelling it");
        CancelScriptPixelEdit();
    }
    if (m_IsStrokeActive) {
        Logger::Get().ErrorTag("script", "BeginScriptPixelEdit: paint stroke active — finish stroke first");
        return false;
    }
    m_ScriptPixelEditPrevActive = m_ActiveLayerIdx;
    m_ActiveLayerIdx = layerIndex;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    m_ScriptPixelEditActive = true;
    m_ScriptPixelEditLayer = layerIndex;
    return true;
}

bool Canvas::EndScriptPixelEdit(const std::string& actionName) {
    if (!m_ScriptPixelEditActive) {
        Logger::Get().ErrorTag("script", "EndScriptPixelEdit: no active session");
        return false;
    }
    const std::string name = actionName.empty() ? "Script Edit" : actionName;
    CommitActiveLayerMutation(name);
    m_ScriptPixelEditActive = false;
    m_ScriptPixelEditLayer = -1;
    if (m_ScriptPixelEditPrevActive >= 0 && m_ScriptPixelEditPrevActive < (int)m_Layers.size())
        m_ActiveLayerIdx = m_ScriptPixelEditPrevActive;
    m_ScriptPixelEditPrevActive = -1;
    return true;
}

void Canvas::CancelScriptPixelEdit() {
    if (!m_ScriptPixelEditActive) return;
    RestoreActiveLayerMutation();
    m_ScriptPixelEditActive = false;
    m_ScriptPixelEditLayer = -1;
    if (m_ScriptPixelEditPrevActive >= 0 && m_ScriptPixelEditPrevActive < (int)m_Layers.size())
        m_ActiveLayerIdx = m_ScriptPixelEditPrevActive;
    m_ScriptPixelEditPrevActive = -1;
}

void Canvas::CommitTransformation(const std::string& actionName) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);

    // Full-layer ops must call BackupAllActiveLayerTiles() first so every grid
    // slot is covered (including previously empty tiles that Import may create).
    // Paint strokes only backup touched tiles — do NOT invent empty-oldState
    // entries for untouched existing tiles (that would erase them on undo).

    std::vector<TileDelta> deltas = SealActiveStrokeDeltas(
        m_ActiveStrokeDeltas, layer.tileCache.get());

    if (!deltas.empty()) {
        auto cmd = std::make_shared<PaintStrokeCommand>(
            layer.name + " " + actionName, m_ActiveLayerIdx, std::move(deltas)
        );
        m_UndoRedoManager.PushCommand(cmd);
    }
    m_ActiveStrokeDeltas.clear();
    m_IsDocumentModified = true;
    layer.needsUpload = true;
    m_CompositeDirty = true;
    InvalidateWandSourceCache();
    m_QuickSelectEdgeValid = false;
}

void Canvas::FlipActiveLayerHorizontal(ID3D11Device* device) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);

    // Backup all tiles
    m_ActiveStrokeDeltas.clear();
    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;
    for (int ty = 0; ty < numTilesY; ++ty) {
        for (int tx = 0; tx < numTilesX; ++tx) {
            BackupTile(tx, ty);
        }
    }

    // Perform horizontal flip
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width / 2; ++x) {
            int leftIdx = (y * m_Width + x) * 4;
            int rightIdx = (y * m_Width + (m_Width - 1 - x)) * 4;
            for (int c = 0; c < 4; ++c) {
                std::swap(pixels[leftIdx + c], pixels[rightIdx + c]);
            }
        }
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);

    CommitTransformation("Horizontal Flip");
    (void)device;
}

void Canvas::FlipActiveLayerVertical(ID3D11Device* device) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);

    // Backup all tiles
    m_ActiveStrokeDeltas.clear();
    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;
    for (int ty = 0; ty < numTilesY; ++ty) {
        for (int tx = 0; tx < numTilesX; ++tx) {
            BackupTile(tx, ty);
        }
    }

    // Perform vertical flip
    for (int y = 0; y < m_Height / 2; ++y) {
        int targetY = m_Height - 1 - y;
        for (int x = 0; x < m_Width; ++x) {
            int topIdx = (y * m_Width + x) * 4;
            int bottomIdx = (targetY * m_Width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                std::swap(pixels[topIdx + c], pixels[bottomIdx + c]);
            }
        }
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);

    CommitTransformation("Vertical Flip");
    (void)device;
}

void Canvas::RotateCanvas90(ID3D11Device* device, bool clockwise) {
    int oldW = m_Width;
    int oldH = m_Height;
    int newW = oldH;
    int newH = oldW;

    // Clear undo history because resizing/rotating the entire canvas changes layout dimensions
    ClearUndoHistory();

    for (auto& layer : m_Layers) {
        auto pixels = ExportLayerF(layer, oldW, oldH);
        std::vector<float> rotated((size_t)newW * newH * 4, 0.0f);
        for (int y = 0; y < oldH; ++y) {
            for (int x = 0; x < oldW; ++x) {
                int dx = clockwise ? (oldH - 1 - y) : y;
                int dy = clockwise ? x : (oldW - 1 - x);
                int srcIdx = (y * oldW + x) * 4;
                int destIdx = (dy * newW + dx) * 4;
                for (int c = 0; c < 4; ++c) {
                    rotated[destIdx + c] = pixels[srcIdx + c];
                }
            }
        }
        if (!layer.tileCache) {
            layer.tileCache = std::make_unique<TileCache>();
        }
        layer.tileCache->Init(newW, newH, m_CanvasFormat);
        layer.tileCache->ImportRGBA32F(rotated.data(), newW, newH);
        layer.tileCache->MarkAllDirty();
        layer.needsUpload = true;
    }

    m_Width = newW;
    m_Height = newH;

    // Recreate resources
    if (device) {
        CreateCompositeResources(device);
        for (auto& layer : m_Layers) {
            RecreateLayerTexture(device, layer);
        }
    }

    m_IsDocumentModified = true;
    Logger::Get().Info("Rotated canvas 90 degrees " + std::string(clockwise ? "CW" : "CCW"));
}

void Canvas::FlipCanvasHorizontal(ID3D11Device* device) {
    ClearUndoHistory();
    for (auto& layer : m_Layers) {
        if (!LayerHasPixels(layer)) continue;
        auto pixels = ExportLayerF(layer, m_Width, m_Height);
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width / 2; ++x) {
                int leftIdx = (y * m_Width + x) * 4;
                int rightIdx = (y * m_Width + (m_Width - 1 - x)) * 4;
                for (int c = 0; c < 4; ++c) {
                    std::swap(pixels[leftIdx + c], pixels[rightIdx + c]);
                }
            }
        }
        SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
        if (device) {
            RecreateLayerTexture(device, layer);
        }
    }
    m_IsDocumentModified = true;
    Logger::Get().Info("Flipped canvas horizontally");
}

void Canvas::FlipCanvasVertical(ID3D11Device* device) {
    ClearUndoHistory();
    for (auto& layer : m_Layers) {
        if (!LayerHasPixels(layer)) continue;
        auto pixels = ExportLayerF(layer, m_Width, m_Height);
        for (int y = 0; y < m_Height / 2; ++y) {
            int targetY = m_Height - 1 - y;
            for (int x = 0; x < m_Width; ++x) {
                int topIdx = (y * m_Width + x) * 4;
                int bottomIdx = (targetY * m_Width + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    std::swap(pixels[topIdx + c], pixels[bottomIdx + c]);
                }
            }
        }
        SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
        if (device) {
            RecreateLayerTexture(device, layer);
        }
    }
    m_IsDocumentModified = true;
    Logger::Get().Info("Flipped canvas vertically");
}

bool Canvas::ApplyModIniParse() {
    m_ModParseSummary.clear();
    m_ModScene = modio::ModScene{};

    if (m_ModIniPath.empty()) {
        m_ModParseSummary = "No INI path set.";
        return false;
    }

    try {
        m_ModScene = modio::ModIniParser::ParseFile(m_ModIniPath);
        // Optional dump: real formats + semantic roles (TEXCOORD1 ≠ UV, etc.)
        if (!m_ModDumpPath.empty()) {
            modio::ModIniParser::ApplyDumpPath(m_ModScene, m_ModDumpPath, m_ModScene.gameHint);
        }
        m_ModParseSummary = modio::FormatSceneSummary(m_ModScene);
        // Append layout summary
        if (!m_ModScene.components.empty()) {
            m_ModParseSummary += "\n--- Layouts (roles) ---\n";
            m_ModParseSummary += modio::FormatLayoutSummary(m_ModScene.components[0].positionLayout);
            m_ModParseSummary += modio::FormatLayoutSummary(m_ModScene.components[0].texcoordLayout);
        }
        if (!m_ModScene.ok) {
            Logger::Get().Warn("Mod INI parse failed: " + m_ModScene.error);
            return false;
        }
        Logger::Get().Info("Mod INI applied:\n" + m_ModParseSummary);
        return true;
    } catch (const std::exception& e) {
        m_ModScene.ok = false;
        m_ModScene.error = e.what();
        m_ModParseSummary = std::string("Exception parsing INI: ") + e.what();
        Logger::Get().Error(m_ModParseSummary);
        return false;
    } catch (...) {
        m_ModScene.ok = false;
        m_ModScene.error = "Unknown exception";
        m_ModParseSummary = "Unknown exception while parsing INI";
        Logger::Get().Error(m_ModParseSummary);
        return false;
    }
}

bool Canvas::ApplyModDumpParse() {
    if (m_ModDumpPath.empty()) {
        m_ModParseSummary = "No dump path set.";
        return false;
    }
    if (!m_ModScene.ok && m_ModScene.components.empty()) {
        // Try ini first if available
        if (!m_ModIniPath.empty())
            ApplyModIniParse();
    }
    bool ok = modio::ModIniParser::ApplyDumpPath(m_ModScene, m_ModDumpPath, m_ModScene.gameHint);
    m_ModParseSummary = modio::FormatSceneSummary(m_ModScene);
    if (m_ModScene.dumpTexcoordLayout.valid)
        m_ModParseSummary += "\n" + modio::FormatLayoutSummary(m_ModScene.dumpTexcoordLayout);
    m_IsDocumentModified = true;
    return ok;
}

bool Canvas::SaveProjectAuto() {
    if (m_CurrentProjectFilePath.empty()) {
        Logger::Get().Error("Cannot auto-save: current project file path is empty.");
        return false;
    }

    if (m_ProjectType == ProjectType::Simple) {
        // Simple project "save" re-exports via hard container setting (not path guessing).
        if (!m_ExportPath.empty())
            m_CurrentProjectFilePath = m_ExportPath;
        else if (!m_CurrentProjectFilePath.empty())
            m_ExportPath = m_CurrentProjectFilePath;
        SyncExportPathExtension();
        m_CurrentProjectFilePath = m_ExportPath;
        bool success = ExportWithProjectSettings();
        if (success) {
            m_IsDocumentModified = false;
            Logger::Get().Info("Simple project exported: " + m_ExportPath);
        } else {
            Logger::Get().Error("Failed to export simple project: " + m_ExportPath);
        }
        return success;
    } else {
        bool success = SaveCanvasRayp(m_CurrentProjectFilePath);
        if (success) {
            m_IsDocumentModified = false;
            Logger::Get().Info("Advanced project saved to RAYP package: " + m_CurrentProjectFilePath);
        } else {
            Logger::Get().Error("Failed to save advanced project to RAYP package: " + m_CurrentProjectFilePath);
        }
        return success;
    }
}

void Canvas::ClearSelection() {
    if (!m_HasSelection && m_SelectionMask.empty()) return;
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    if (m_SelectionBoundsValid)
        ExpandSelectionGpuDirty(m_SelectionBoundsX, m_SelectionBoundsY,
            m_SelectionBoundsX + m_SelectionBoundsW - 1,
            m_SelectionBoundsY + m_SelectionBoundsH - 1);
    else
        m_SelectionGpuDirtyValid = false; // full clear upload

    m_SelectionMask.clear();
    m_HasSelection = false;
    m_SelectionBoundsValid = false;
    m_SelectionBoundsW = m_SelectionBoundsH = 0;
    m_SelectionMaskNeedsUpload = true;

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Deselect", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::FillSelection(const float rgba[4]) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;

    // Mask paint target: fill selection with grayscale of color (luma)
    if (m_PaintTarget == PaintTarget::LayerMask && layer.hasMask) {
        if (layer.mask.empty()) layer.mask.assign((size_t)m_Width * m_Height, 255);
        float gray = std::clamp(0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2], 0.f, 1.f);
        uint8_t g8 = (uint8_t)(gray * 255.f + 0.5f);
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
                if (sel < 1e-4f) continue;
                size_t i = (size_t)y * m_Width + x;
                float cur = layer.mask[i] / 255.f;
                float out = cur * (1.f - sel) + gray * sel;
                layer.mask[i] = (uint8_t)(std::clamp(out, 0.f, 1.f) * 255.f + 0.5f);
            }
        }
        layer.maskNeedsUpload = true;
        m_CompositeDirty = true;
        m_IsDocumentModified = true;
        Logger::Get().Info("FillSelection (mask)");
        return;
    }

    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * m_Width + x) * 4;
            float fa = rgba[3] * sel;
            float da = pixels[idx + 3];
            float outA = fa + da * (1.f - fa);
            if (outA > 1e-6f) {
                pixels[idx + 0] = (rgba[0] * fa + pixels[idx + 0] * da * (1.f - fa)) / outA;
                pixels[idx + 1] = (rgba[1] * fa + pixels[idx + 1] * da * (1.f - fa)) / outA;
                pixels[idx + 2] = (rgba[2] * fa + pixels[idx + 2] * da * (1.f - fa)) / outA;
                pixels[idx + 3] = outA;
            }
        }
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Fill");
    Logger::Get().Info("FillSelection");
}

void Canvas::DeleteSelectionContent() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;

    if (m_PaintTarget == PaintTarget::LayerMask && layer.hasMask) {
        if (layer.mask.empty()) layer.mask.assign((size_t)m_Width * m_Height, 255);
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
                if (sel < 1e-4f) continue;
                size_t i = (size_t)y * m_Width + x;
                float cur = layer.mask[i] / 255.f;
                layer.mask[i] = (uint8_t)(std::clamp(cur * (1.f - sel), 0.f, 1.f) * 255.f + 0.5f);
            }
        }
        layer.maskNeedsUpload = true;
        m_CompositeDirty = true;
        m_IsDocumentModified = true;
        Logger::Get().Info("DeleteSelectionContent (mask)");
        return;
    }

    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * m_Width + x) * 4;
            // Erase toward transparent
            float keep = 1.f - sel;
            pixels[idx + 0] *= keep;
            pixels[idx + 1] *= keep;
            pixels[idx + 2] *= keep;
            pixels[idx + 3] *= keep;
        }
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Delete");
    Logger::Get().Info("DeleteSelectionContent");
}

bool Canvas::CopyContentToClipboard() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return false;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return false;

    // Determine AABB: selection bounds or full canvas
    int x0 = 0, y0 = 0, x1 = m_Width - 1, y1 = m_Height - 1;
    if (m_HasSelection && !m_SelectionMask.empty()) {
        x0 = m_Width; y0 = m_Height; x1 = -1; y1 = -1;
        for (int y = 0; y < m_Height; ++y)
            for (int x = 0; x < m_Width; ++x)
                if (m_SelectionMask[(size_t)y * m_Width + x] > 0) {
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
        if (x1 < x0) return false;
    }
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    auto src = ExportLayerF(layer, m_Width, m_Height);
    m_ContentClipRGBA.assign((size_t)cw * ch * 4, 0.f);
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            int sx = x0 + x, sy = y0 + y;
            float sel = GetSelWeight(m_SelectionMask, m_Width, sx, sy, m_HasSelection);
            size_t si = ((size_t)sy * m_Width + sx) * 4;
            size_t di = ((size_t)y * cw + x) * 4;
            m_ContentClipRGBA[di + 0] = src[si + 0];
            m_ContentClipRGBA[di + 1] = src[si + 1];
            m_ContentClipRGBA[di + 2] = src[si + 2];
            m_ContentClipRGBA[di + 3] = src[si + 3] * sel;
        }
    }
    m_ContentClipW = cw;
    m_ContentClipH = ch;
    m_ContentClipboardValid = true;
    m_LayerClipboardValid = false; // content copy takes precedence for paste routing

    // Also push to system clipboard for other apps
    ClipboardHelper::CopyImageToClipboard(m_ContentClipRGBA, cw, ch);
    Logger::Get().Info("CopyContentToClipboard " + std::to_string(cw) + "x" + std::to_string(ch));
    return true;
}

bool Canvas::CopyLayersToClipboard(const std::vector<int>& indices) {
    if (indices.empty()) return false;
    m_LayerClipboard.clear();
    for (int idx : indices) {
        if (idx < 0 || idx >= (int)m_Layers.size()) continue;
        const Layer& src = m_Layers[idx];
        LayerClipboardEntry e;
        e.name = src.name;
        e.isGroup = src.isGroup;
        e.opacity = src.opacity;
        e.blendMode = src.blendMode;
        e.visible = src.visible;
        e.type = src.type;
        e.smartSourceBytes = src.smartSourceBytes;
        e.smartSourcePath = src.smartSourcePath;
        if (!src.isGroup) {
            e.pixels = ExportLayerF(src, m_Width, m_Height);
            e.hasMask = src.hasMask;
            if (src.hasMask) e.mask = src.mask;
        }
        m_LayerClipboard.push_back(std::move(e));
    }
    if (m_LayerClipboard.empty()) return false;
    m_LayerClipboardValid = true;
    // Prefer layer paste when this was intentional layer copy
    Logger::Get().Info("CopyLayersToClipboard count=" + std::to_string(m_LayerClipboard.size()));
    return true;
}

bool Canvas::PasteLayersFromClipboard(ID3D11Device* device) {
    if (!m_LayerClipboardValid || m_LayerClipboard.empty()) return false;
    int insertAt = m_ActiveLayerIdx + 1;
    if (insertAt < 0) insertAt = (int)m_Layers.size();
    int firstNew = insertAt;
    for (const auto& e : m_LayerClipboard) {
        Layer L;
        L.name = e.name + " copy";
        L.isGroup = e.isGroup;
        L.opacity = e.opacity;
        L.blendMode = e.blendMode;
        L.visible = e.visible;
        L.type = e.type;
        L.smartSourceBytes = e.smartSourceBytes;
        L.smartSourcePath = e.smartSourcePath;
        if (!L.isGroup) {
            L.tileCache = std::make_unique<TileCache>();
            L.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
            if (!e.pixels.empty())
                SetLayerPixelsF(L, e.pixels, m_Width, m_Height, m_CanvasFormat);
            if (e.hasMask && !e.mask.empty()) {
                L.hasMask = true;
                L.mask = e.mask;
                L.maskNeedsUpload = true;
            }
            if (device) RecreateLayerTexture(device, L);
        }
        m_Layers.insert(m_Layers.begin() + insertAt, std::move(L));
        for (int i = 0; i < (int)m_Layers.size(); ++i) {
            if (m_Layers[i].parentGroupId >= insertAt)
                m_Layers[i].parentGroupId++;
        }
        if (device && m_Layers[insertAt].hasMask)
            UpdateLayerMaskTexture(device, insertAt);
        insertAt++;
    }
    m_ActiveLayerIdx = insertAt - 1;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("PasteLayersFromClipboard inserted " + std::to_string(m_LayerClipboard.size()));
    (void)firstNew;
    return true;
}

// Resolve paste pixels: system clipboard wins when another app overwrote it
// (PNG with alpha from Blender/Chrome/PS). Internal buffer used only when we still own it.
static bool ResolvePastePixels(bool contentValid, const std::vector<float>& contentRgba,
                               int contentW, int contentH,
                               std::vector<float>& pixels, int& pw, int& ph) {
    const bool hasInternal = contentValid && !contentRgba.empty() && contentW > 0 && contentH > 0;
    const bool hasSystem = ClipboardHelper::HasClipboardImage();
    const bool systemNewer = ClipboardHelper::IsSystemClipboardNewerThanLastCopy();

    if (hasSystem && (systemNewer || !hasInternal)) {
        if (ClipboardHelper::PasteImageFromClipboard(pixels, pw, ph))
            return true;
    }
    if (hasInternal) {
        pixels = contentRgba;
        pw = contentW;
        ph = contentH;
        return true;
    }
    if (hasSystem)
        return ClipboardHelper::PasteImageFromClipboard(pixels, pw, ph);
    return false;
}

bool Canvas::PasteContentIntoActive(ID3D11Device* device) {
    std::vector<float> pixels;
    int pw = 0, ph = 0;
    if (!ResolvePastePixels(m_ContentClipboardValid, m_ContentClipRGBA, m_ContentClipW, m_ContentClipH,
                            pixels, pw, ph))
        return false;
    if (pixels.empty() || pw <= 0 || ph <= 0) return false;
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return false;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return false;

    int ox = (m_Width - pw) / 2;
    int oy = (m_Height - ph) / 2;

    if (m_PaintTarget == PaintTarget::LayerMask) {
        if (!layer.hasMask) {
            // allocate white mask
            layer.hasMask = true;
            layer.mask.assign((size_t)m_Width * m_Height, 255);
        }
        if (layer.mask.empty())
            layer.mask.assign((size_t)m_Width * m_Height, 255);
        for (int y = 0; y < ph; ++y) {
            int dy = y + oy;
            if (dy < 0 || dy >= m_Height) continue;
            for (int x = 0; x < pw; ++x) {
                int dx = x + ox;
                if (dx < 0 || dx >= m_Width) continue;
                size_t si = ((size_t)y * pw + x) * 4;
                float a = pixels[si + 3];
                if (a < 1e-4f) continue;
                float gray = 0.2126f * pixels[si] + 0.7152f * pixels[si + 1] + 0.0722f * pixels[si + 2];
                size_t di = (size_t)dy * m_Width + dx;
                float cur = layer.mask[di] / 255.f;
                float out = cur * (1.f - a) + gray * a;
                layer.mask[di] = (uint8_t)(std::clamp(out, 0.f, 1.f) * 255.f + 0.5f);
            }
        }
        layer.maskNeedsUpload = true;
        // Full mask changed — force full GPU upload (dirty rect invalid).
        layer.maskDirtyX0 = 0;
        layer.maskDirtyY0 = 0;
        layer.maskDirtyX1 = m_Width - 1;
        layer.maskDirtyY1 = m_Height - 1;
        if (device) UpdateLayerMaskTexture(device, m_ActiveLayerIdx);
        m_CompositeDirty = true;
        m_IsDocumentModified = true;
        Logger::Get().Info("PasteContentIntoActive (mask) " +
            std::to_string(pw) + "x" + std::to_string(ph));
        return true;
    }

    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto dest = ExportLayerF(layer, m_Width, m_Height);
    for (int y = 0; y < ph; ++y) {
        int dy = y + oy;
        if (dy < 0 || dy >= m_Height) continue;
        for (int x = 0; x < pw; ++x) {
            int dx = x + ox;
            if (dx < 0 || dx >= m_Width) continue;
            size_t si = ((size_t)y * pw + x) * 4;
            size_t di = ((size_t)dy * m_Width + dx) * 4;
            float sa = pixels[si + 3];
            if (sa < 1e-4f) continue;
            float da = dest[di + 3];
            float outA = sa + da * (1.f - sa);
            if (outA > 1e-6f) {
                dest[di + 0] = (pixels[si + 0] * sa + dest[di + 0] * da * (1.f - sa)) / outA;
                dest[di + 1] = (pixels[si + 1] * sa + dest[di + 1] * da * (1.f - sa)) / outA;
                dest[di + 2] = (pixels[si + 2] * sa + dest[di + 2] * da * (1.f - sa)) / outA;
                dest[di + 3] = outA;
            }
        }
    }
    SetLayerPixelsF(layer, dest, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Paste");
    Logger::Get().Info("PasteContentIntoActive");
    return true;
}

bool Canvas::PasteContentAsNewLayer(ID3D11Device* device, const std::string& name) {
    std::vector<float> pixels;
    int pw = 0, ph = 0;
    if (!ResolvePastePixels(m_ContentClipboardValid, m_ContentClipRGBA, m_ContentClipW, m_ContentClipH,
                            pixels, pw, ph))
        return false;
    if (pixels.empty() || pw <= 0 || ph <= 0) return false;
    CreateLayerFromPixels(device, name, pixels, pw, ph);
    return true;
}

void Canvas::SetSelectionMask(const std::vector<uint8_t>& mask) {
    // Empty = deselect (SelectionCommand undo/redo of ClearSelection).
    if (mask.empty()) {
        if (m_SelectionBoundsValid)
            ExpandSelectionGpuDirty(m_SelectionBoundsX, m_SelectionBoundsY,
                m_SelectionBoundsX + m_SelectionBoundsW - 1,
                m_SelectionBoundsY + m_SelectionBoundsH - 1);
        else
            m_SelectionGpuDirtyValid = false;
        m_SelectionMask.clear();
        m_HasSelection = false;
        m_SelectionBoundsValid = false;
        m_SelectionBoundsW = m_SelectionBoundsH = 0;
        m_SelectionMaskNeedsUpload = true;
        return;
    }
    if (mask.size() == (size_t)m_Width * (size_t)m_Height) {
        m_SelectionMask = mask;
        m_SelectionGpuDirtyValid = false; // full mask replaced
        // Seed AABB invalid so recompute does a full scan
        m_SelectionBoundsValid = false;
        RecomputeSelectionBoundsFromMask();
        m_SelectionMaskNeedsUpload = true;
    }
}

bool Canvas::EnsureSelectionMaskAllocated() {
    if (m_Width <= 0 || m_Height <= 0) return false;
    const size_t need = (size_t)m_Width * (size_t)m_Height;
    if (m_SelectionMask.size() == need) return true;
    // u8 mask: 16K = 256 MiB — soft budget only (not float composite refuse)
    const size_t est = need; // 1 byte/px
    if (MemoryStats::ExceedsRamBudget(est, 0.30))
        MemoryStats::LogSoftBudget("EnsureSelectionMaskAllocated", est, 0.30);
    if (MemoryStats::ExceedsHardRamCeiling(est)) {
        Logger::Get().ErrorTag("mem",
            "EnsureSelectionMaskAllocated hard ceiling " + MemoryStats::FormatBytes(est));
        return false;
    }
    try {
        m_SelectionMask.assign(need, 0);
    } catch (const std::bad_alloc&) {
        Logger::Get().ErrorTag("mem", "EnsureSelectionMaskAllocated bad_alloc");
        m_SelectionMask.clear();
        return false;
    }
    return true;
}

void Canvas::ExpandSelectionBounds(int x0, int y0, int x1, int y1) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(m_Width - 1, x1); y1 = std::min(m_Height - 1, y1);
    if (x1 < x0 || y1 < y0) return;
    if (!m_SelectionBoundsValid) {
        m_SelectionBoundsX = x0; m_SelectionBoundsY = y0;
        m_SelectionBoundsW = x1 - x0 + 1; m_SelectionBoundsH = y1 - y0 + 1;
        m_SelectionBoundsValid = true;
        return;
    }
    int bx0 = m_SelectionBoundsX, by0 = m_SelectionBoundsY;
    int bx1 = bx0 + m_SelectionBoundsW - 1, by1 = by0 + m_SelectionBoundsH - 1;
    bx0 = std::min(bx0, x0); by0 = std::min(by0, y0);
    bx1 = std::max(bx1, x1); by1 = std::max(by1, y1);
    m_SelectionBoundsX = bx0; m_SelectionBoundsY = by0;
    m_SelectionBoundsW = bx1 - bx0 + 1; m_SelectionBoundsH = by1 - by0 + 1;
}

void Canvas::ExpandSelectionGpuDirty(int x0, int y0, int x1, int y1) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(m_Width - 1, x1); y1 = std::min(m_Height - 1, y1);
    if (x1 < x0 || y1 < y0) return;
    if (!m_SelectionGpuDirtyValid) {
        m_SelectionGpuDirtyX0 = x0; m_SelectionGpuDirtyY0 = y0;
        m_SelectionGpuDirtyX1 = x1; m_SelectionGpuDirtyY1 = y1;
        m_SelectionGpuDirtyValid = true;
        return;
    }
    m_SelectionGpuDirtyX0 = std::min(m_SelectionGpuDirtyX0, x0);
    m_SelectionGpuDirtyY0 = std::min(m_SelectionGpuDirtyY0, y0);
    m_SelectionGpuDirtyX1 = std::max(m_SelectionGpuDirtyX1, x1);
    m_SelectionGpuDirtyY1 = std::max(m_SelectionGpuDirtyY1, y1);
}

void Canvas::RecomputeSelectionBoundsFromMask() {
    m_HasSelection = false;
    if (m_SelectionMask.size() != (size_t)m_Width * (size_t)m_Height) {
        m_SelectionBoundsX = m_SelectionBoundsY = 0;
        m_SelectionBoundsW = m_SelectionBoundsH = 0;
        m_SelectionBoundsValid = false;
        return;
    }
    int minX = m_Width, minY = m_Height, maxX = -1, maxY = -1;
    // Prefer previous AABB + pad as seed if valid; else full scan.
    // Capture seed BEFORE clearing valid (was a no-op bug: valid cleared first).
    int sx0 = 0, sy0 = 0, sx1 = m_Width - 1, sy1 = m_Height - 1;
    const bool hadSeed = m_SelectionBoundsValid && m_SelectionBoundsW > 0 && m_SelectionBoundsH > 0;
    if (hadSeed) {
        sx0 = std::max(0, m_SelectionBoundsX - 2);
        sy0 = std::max(0, m_SelectionBoundsY - 2);
        sx1 = std::min(m_Width - 1, m_SelectionBoundsX + m_SelectionBoundsW + 1);
        sy1 = std::min(m_Height - 1, m_SelectionBoundsY + m_SelectionBoundsH + 1);
    }
    m_SelectionBoundsValid = false;
    for (int y = sy0; y <= sy1; ++y) {
        const size_t row = (size_t)y * (size_t)m_Width;
        for (int x = sx0; x <= sx1; ++x) {
            if (m_SelectionMask[row + (size_t)x] == 0) continue;
            m_HasSelection = true;
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
        }
    }
    // Subtract may leave pixels outside seed — if empty seed found nothing but mask may still have bits
    if (!m_HasSelection && (sx0 > 0 || sy0 > 0 || sx1 < m_Width - 1 || sy1 < m_Height - 1)) {
        for (int y = 0; y < m_Height; ++y) {
            const size_t row = (size_t)y * (size_t)m_Width;
            for (int x = 0; x < m_Width; ++x) {
                if (m_SelectionMask[row + (size_t)x] == 0) continue;
                m_HasSelection = true;
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
    }
    if (!m_HasSelection) {
        m_SelectionBoundsX = m_SelectionBoundsY = 0;
        m_SelectionBoundsW = m_SelectionBoundsH = 0;
        m_SelectionBoundsValid = false;
        return;
    }
    m_SelectionBoundsX = minX; m_SelectionBoundsY = minY;
    m_SelectionBoundsW = maxX - minX + 1; m_SelectionBoundsH = maxY - minY + 1;
    m_SelectionBoundsValid = true;
}

void Canvas::CombineSelectionRoi(int x0, int y0, int x1, int y1, bool add, bool subtract,
                                 const std::function<uint8_t(int x, int y)>& sampleNew) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(m_Width - 1, x1); y1 = std::min(m_Height - 1, y1);
    if (x1 < x0 || y1 < y0) return;
    if (!EnsureSelectionMaskAllocated()) return;

    if (!add && !subtract) {
        // Replace: clear previous selection (only need previous AABB if we had one)
        if (m_SelectionBoundsValid && m_SelectionBoundsW > 0 && m_SelectionBoundsH > 0) {
            const int ox0 = m_SelectionBoundsX, oy0 = m_SelectionBoundsY;
            const int ox1 = ox0 + m_SelectionBoundsW - 1, oy1 = oy0 + m_SelectionBoundsH - 1;
            for (int y = oy0; y <= oy1; ++y) {
                uint8_t* row = m_SelectionMask.data() + (size_t)y * (size_t)m_Width;
                std::memset(row + ox0, 0, (size_t)(ox1 - ox0 + 1));
            }
            ExpandSelectionGpuDirty(ox0, oy0, ox1, oy1);
        } else if (m_HasSelection) {
            std::memset(m_SelectionMask.data(), 0, m_SelectionMask.size());
            m_SelectionGpuDirtyValid = false; // full upload
        }
        m_SelectionBoundsValid = false;
        m_HasSelection = false;
    }

    bool any = false;
    int bminX = m_Width, bminY = m_Height, bmaxX = -1, bmaxY = -1;
    for (int y = y0; y <= y1; ++y) {
        uint8_t* row = m_SelectionMask.data() + (size_t)y * (size_t)m_Width;
        for (int x = x0; x <= x1; ++x) {
            const uint8_t n = sampleNew(x, y);
            if (n == 0 && !subtract) continue;
            uint8_t& d = row[x];
            if (add) {
                d = (uint8_t)std::max(d, n);
            } else if (subtract) {
                if (n) d = 0;
            } else {
                d = n;
            }
            if (d > 0) {
                any = true;
                bminX = std::min(bminX, x); bmaxX = std::max(bmaxX, x);
                bminY = std::min(bminY, y); bmaxY = std::max(bmaxY, y);
            }
        }
    }
    ExpandSelectionGpuDirty(x0, y0, x1, y1);
    if (subtract) {
        RecomputeSelectionBoundsFromMask();
    } else if (any) {
        m_HasSelection = true;
        if (add && m_SelectionBoundsValid)
            ExpandSelectionBounds(bminX, bminY, bmaxX, bmaxY);
        else {
            m_SelectionBoundsX = bminX; m_SelectionBoundsY = bminY;
            m_SelectionBoundsW = bmaxX - bminX + 1;
            m_SelectionBoundsH = bmaxY - bminY + 1;
            m_SelectionBoundsValid = true;
        }
    } else if (!add) {
        m_HasSelection = false;
        m_SelectionBoundsValid = false;
    }
    m_SelectionMaskNeedsUpload = true;
}

void Canvas::UpdateSelectionMaskTexture(ID3D11Device* device) {
    if (!device || m_Width <= 0 || m_Height <= 0) return;

    if (m_SelectionMaskTexture) {
        D3D11_TEXTURE2D_DESC desc = {};
        m_SelectionMaskTexture->GetDesc(&desc);
        if (desc.Width != (UINT)m_Width || desc.Height != (UINT)m_Height || desc.Format != DXGI_FORMAT_R8_UNORM) {
            DeferReleaseTex(m_SelectionMaskTexture);
            m_SelectionMaskTexture = nullptr;
            if (m_SelectionMaskSRV) {
                DeferReleaseSRV(m_SelectionMaskSRV);
                m_SelectionMaskSRV = nullptr;
            }
        }
    }

    if (!m_SelectionMaskTexture) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_Width;
        desc.Height = m_Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_SelectionMaskTexture);
        if (SUCCEEDED(hr) && m_SelectionMaskTexture) {
            device->CreateShaderResourceView(m_SelectionMaskTexture, nullptr, &m_SelectionMaskSRV);
        }
        m_SelectionGpuDirtyValid = false; // force full upload into new texture
    }

    if (!m_SelectionMaskTexture) return;

    const size_t need = (size_t)m_Width * (size_t)m_Height;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return;

    if (m_HasSelection && m_SelectionMask.size() == need) {
        if (m_SelectionGpuDirtyValid) {
            const int x0 = std::max(0, m_SelectionGpuDirtyX0);
            const int y0 = std::max(0, m_SelectionGpuDirtyY0);
            const int x1 = std::min(m_Width - 1, m_SelectionGpuDirtyX1);
            const int y1 = std::min(m_Height - 1, m_SelectionGpuDirtyY1);
            if (x1 >= x0 && y1 >= y0) {
                D3D11_BOX box = {};
                box.left = (UINT)x0; box.top = (UINT)y0; box.front = 0;
                box.right = (UINT)(x1 + 1); box.bottom = (UINT)(y1 + 1); box.back = 1;
                const uint8_t* src = m_SelectionMask.data() + (size_t)y0 * (size_t)m_Width + (size_t)x0;
                context->UpdateSubresource(m_SelectionMaskTexture, 0, &box, src,
                    (UINT)(m_Width * sizeof(uint8_t)), 0);
            }
        } else {
            context->UpdateSubresource(m_SelectionMaskTexture, 0, nullptr, m_SelectionMask.data(),
                (UINT)(m_Width * sizeof(uint8_t)), 0);
        }
    } else {
        // Deselect / empty: clear dirty region or whole texture
        if (m_SelectionGpuDirtyValid) {
            const int x0 = std::max(0, m_SelectionGpuDirtyX0);
            const int y0 = std::max(0, m_SelectionGpuDirtyY0);
            const int x1 = std::min(m_Width - 1, m_SelectionGpuDirtyX1);
            const int y1 = std::min(m_Height - 1, m_SelectionGpuDirtyY1);
            if (x1 >= x0 && y1 >= y0) {
                const int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
                std::vector<uint8_t> zeros((size_t)rw * (size_t)rh, 0);
                D3D11_BOX box = {};
                box.left = (UINT)x0; box.top = (UINT)y0; box.front = 0;
                box.right = (UINT)(x1 + 1); box.bottom = (UINT)(y1 + 1); box.back = 1;
                context->UpdateSubresource(m_SelectionMaskTexture, 0, &box, zeros.data(),
                    (UINT)(rw * sizeof(uint8_t)), 0);
            }
        } else {
            std::vector<uint8_t> zeros(need, 0);
            context->UpdateSubresource(m_SelectionMaskTexture, 0, nullptr, zeros.data(),
                (UINT)(m_Width * sizeof(uint8_t)), 0);
        }
    }
    context->Release();
    m_SelectionMaskNeedsUpload = false;
    m_SelectionGpuDirtyValid = false;
}

void Canvas::ApplyRectSelection(int x1, int y1, int x2, int y2, bool add, bool subtract) {
    if (m_Width <= 0 || m_Height <= 0) return;
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    int xa = std::min(x1, x2), xb = std::max(x1, x2);
    int ya = std::min(y1, y2), yb = std::max(y1, y2);
    CombineSelectionRoi(xa, ya, xb, yb, add, subtract,
        [](int /*x*/, int /*y*/) -> uint8_t { return 255; });

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::ApplyEllipseSelection(int x1, int y1, int x2, int y2, bool add, bool subtract) {
    if (m_Width <= 0 || m_Height <= 0) return;
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    int xa = std::min(x1, x2), xb = std::max(x1, x2);
    int ya = std::min(y1, y2), yb = std::max(y1, y2);
    const float cx = (xa + xb) * 0.5f;
    const float cy = (ya + yb) * 0.5f;
    const float rx = std::max(0.5f, (xb - xa) * 0.5f);
    const float ry = std::max(0.5f, (yb - ya) * 0.5f);
    const float invRx2 = 1.f / (rx * rx);
    const float invRy2 = 1.f / (ry * ry);

    CombineSelectionRoi(xa, ya, xb, yb, add, subtract,
        [&](int x, int y) -> uint8_t {
            const float dx = (float)x + 0.5f - cx;
            const float dy = (float)y + 0.5f - cy;
            return (dx * dx * invRx2 + dy * dy * invRy2) <= 1.f ? (uint8_t)255 : (uint8_t)0;
        });

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::ApplyLassoSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract) {
    if (m_Width <= 0 || m_Height <= 0 || points.size() < 3) return;
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    int minX = m_Width, minY = m_Height, maxX = -1, maxY = -1;
    std::vector<cv::Point> cvPoints;
    cvPoints.reserve(points.size());
    for (const auto& p : points) {
        int x = std::clamp(p.first, 0, m_Width - 1);
        int y = std::clamp(p.second, 0, m_Height - 1);
        cvPoints.emplace_back(x, y);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
    }
    if (maxX < minX) return;

    // ROI-only OpenCV mat (not full 16K canvas)
    const int rw = maxX - minX + 1, rh = maxY - minY + 1;
    cv::Mat temp = cv::Mat::zeros(rh, rw, CV_8UC1);
    std::vector<cv::Point> local = cvPoints;
    for (auto& pt : local) { pt.x -= minX; pt.y -= minY; }
    std::vector<std::vector<cv::Point>> polys = { local };
    cv::fillPoly(temp, polys, cv::Scalar(255));

    CombineSelectionRoi(minX, minY, maxX, maxY, add, subtract,
        [&](int x, int y) -> uint8_t {
            return temp.at<uint8_t>(y - minY, x - minX);
        });

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::InvalidateWandSourceCache() {
    m_WandSourceRGBA.clear();
    m_WandSourceW = m_WandSourceH = 0;
    m_WandSourceLayerIdx = -1;
}

void Canvas::EnsureWandSourceCache() {
    if (m_WandSourceW == m_Width && m_WandSourceH == m_Height &&
        m_WandSourceLayerIdx == m_ActiveLayerIdx && !m_WandSourceRGBA.empty()) {
        return;
    }
    std::vector<float> srcPixels;
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size() &&
        !m_Layers[m_ActiveLayerIdx].isGroup) {
        srcPixels = ExportLayerF(m_Layers[m_ActiveLayerIdx], m_Width, m_Height);
    } else {
        srcPixels = GetCompositePixels();
    }
    // Full RGBA — empty/transparent pixels must not collapse to RGB black for wand.
    m_WandSourceRGBA.resize((size_t)m_Width * m_Height * 4);
    for (int i = 0; i < m_Width * m_Height; ++i) {
        m_WandSourceRGBA[i * 4 + 0] = (uint8_t)(std::clamp(srcPixels[i * 4 + 0], 0.f, 1.f) * 255.f + 0.5f);
        m_WandSourceRGBA[i * 4 + 1] = (uint8_t)(std::clamp(srcPixels[i * 4 + 1], 0.f, 1.f) * 255.f + 0.5f);
        m_WandSourceRGBA[i * 4 + 2] = (uint8_t)(std::clamp(srcPixels[i * 4 + 2], 0.f, 1.f) * 255.f + 0.5f);
        m_WandSourceRGBA[i * 4 + 3] = (uint8_t)(std::clamp(srcPixels[i * 4 + 3], 0.f, 1.f) * 255.f + 0.5f);
    }
    m_WandSourceW = m_Width;
    m_WandSourceH = m_Height;
    m_WandSourceLayerIdx = m_ActiveLayerIdx;
}

void Canvas::RunMagicWand(ID3D11Device* device, int startX, int startY, float tolerance,
                          bool add, bool subtract, bool contiguous, bool pushUndo) {
    if (startX < 0 || startX >= m_Width || startY < 0 || startY >= m_Height) return;

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    EnsureWandSourceCache();
    if (m_WandSourceRGBA.size() != (size_t)m_Width * m_Height * 4) return;

    const uint8_t* src = m_WandSourceRGBA.data();
    const uint8_t* seed = src + ((size_t)startY * m_Width + startX) * 4;
    // Photoshop-like: tolerance is max per-channel delta in 0..255.
    const int tol8 = std::max(0, std::min(255, (int)std::lround(std::clamp(tolerance, 0.f, 1.f) * 255.f)));

    // Clicked empty/transparent: match by alpha only (RGB of A=0 tiles is often 0,0,0
    // and must NOT select opaque dark paint).
    const bool seedIsTransparent = seed[3] <= tol8;

    auto matches = [&](int x, int y) -> bool {
        const uint8_t* p = src + ((size_t)y * m_Width + x) * 4;
        if (seedIsTransparent) {
            return std::abs((int)p[3] - (int)seed[3]) <= tol8;
        }
        // Opaque/semi seed: all channels including alpha (transparent holes stop the fill).
        int dr = std::abs((int)p[0] - (int)seed[0]);
        int dg = std::abs((int)p[1] - (int)seed[1]);
        int db = std::abs((int)p[2] - (int)seed[2]);
        int da = std::abs((int)p[3] - (int)seed[3]);
        return std::max(std::max(dr, dg), std::max(db, da)) <= tol8;
    };

    std::vector<uint8_t> temp((size_t)m_Width * m_Height, 0);

    if (contiguous) {
        std::vector<uint8_t> visited((size_t)m_Width * m_Height, 0);
        std::vector<std::pair<int, int>> q;
        q.reserve(1024);
        q.push_back({ startX, startY });
        visited[(size_t)startY * m_Width + startX] = 1;
        size_t head = 0;
        const int ndx[4] = { 1, -1, 0, 0 };
        const int ndy[4] = { 0, 0, 1, -1 };
        while (head < q.size()) {
            auto [x, y] = q[head++];
            if (!matches(x, y)) continue;
            temp[(size_t)y * m_Width + x] = 255;
            for (int n = 0; n < 4; ++n) {
                int nx = x + ndx[n], ny = y + ndy[n];
                if (nx < 0 || ny < 0 || nx >= m_Width || ny >= m_Height) continue;
                size_t ni = (size_t)ny * m_Width + nx;
                if (visited[ni]) continue;
                visited[ni] = 1;
                if (matches(nx, ny))
                    q.push_back({ nx, ny });
            }
        }
    } else {
        for (int y = 0; y < m_Height; ++y)
            for (int x = 0; x < m_Width; ++x)
                if (matches(x, y))
                    temp[(size_t)y * m_Width + x] = 255;
    }

    std::vector<uint8_t> current((size_t)m_Width * m_Height, 0);
    if (!m_SelectionMask.empty() && (int)m_SelectionMask.size() == m_Width * m_Height)
        current = m_SelectionMask;

    m_SelectionMask.resize((size_t)m_Width * m_Height);
    if (add) {
        for (size_t i = 0; i < m_SelectionMask.size(); ++i)
            m_SelectionMask[i] = (uint8_t)(current[i] | temp[i]);
    } else if (subtract) {
        for (size_t i = 0; i < m_SelectionMask.size(); ++i)
            m_SelectionMask[i] = (uint8_t)(current[i] & (uint8_t)~temp[i]);
    } else {
        m_SelectionMask = std::move(temp);
    }

    m_HasSelection = false;
    for (uint8_t val : m_SelectionMask) {
        if (val > 0) { m_HasSelection = true; break; }
    }
    m_SelectionMaskNeedsUpload = true;
    if (device) UpdateSelectionMaskTexture(device);

    if (pushUndo) {
        m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
            seedIsTransparent ? "Magic Wand (Transparent)" : "Magic Wand",
            std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
    }
}

void Canvas::ApplyMagicWandSelection(ID3D11Device* device, int startX, int startY, float tolerance, bool add, bool subtract, bool contiguous) {
    if (startX < 0 || startX >= m_Width || startY < 0 || startY >= m_Height) return;
    m_WandSeedX = startX;
    m_WandSeedY = startY;
    m_WandSeedValid = true;
    // Always re-sample layer at click time (cache can be stale after paint/edits).
    InvalidateWandSourceCache();
    EnsureWandSourceCache();
    RunMagicWand(device, startX, startY, tolerance, add, subtract, contiguous, true);
}

bool Canvas::PreviewWandFromSeed(ID3D11Device* device, float tolerance, bool add, bool subtract, bool contiguous) {
    if (!m_WandSeedValid) return false;
    // Preview replaces selection without stacking undo (UI should commit on mouse-up if needed).
    // For live scrub: re-run without push; last committed wand already on stack from click.
    RunMagicWand(device, m_WandSeedX, m_WandSeedY, tolerance, add, subtract, contiguous, false);
    return true;
}

void Canvas::ApplySmartSelectSelection(ID3D11Device* device, const std::vector<std::pair<int, int>>& points, bool add, bool subtract) {
    if (points.empty()) return;

    m_SmartSelectInProgress.store(true);
    m_SmartSelectCancelled.store(false);

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    std::thread t([this, device, points, add, subtract, oldMask, oldHasSelection]() {
        std::vector<float> srcPixels;
        if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
            srcPixels = ExportLayerF(m_Layers[m_ActiveLayerIdx], m_Width, m_Height);
        } else {
            srcPixels = GetCompositePixels();
        }

        cv::Mat mat = ImageManager::PixelsToMat8UC3(srcPixels, m_Width, m_Height);

        // Find bounding box
        int minX = m_Width;
        int maxX = 0;
        int minY = m_Height;
        int maxY = 0;
        for (const auto& p : points) {
            minX = std::min(minX, p.first);
            maxX = std::max(maxX, p.first);
            minY = std::min(minY, p.second);
            maxY = std::max(maxY, p.second);
        }

        // Add 15px margin for GrabCut context
        const int margin = 15;
        minX = std::max(0, minX - margin);
        minY = std::max(0, minY - margin);
        maxX = std::min(m_Width - 1, maxX + margin);
        maxY = std::min(m_Height - 1, maxY + margin);

        int rectW = maxX - minX + 1;
        int rectH = maxY - minY + 1;

        if (rectW <= 2 || rectH <= 2) {
            m_SmartSelectInProgress.store(false);
            return;
        }

        cv::Rect roiRect(minX, minY, rectW, rectH);
        cv::Mat croppedMat = mat(roiRect).clone();
        cv::Mat croppedMask = cv::Mat::zeros(rectH, rectW, CV_8UC1); // Initialized to cv::GC_BGD

        // Shift points relative to ROI
        std::vector<cv::Point> shiftedPoints;
        for (const auto& p : points) {
            shiftedPoints.push_back(cv::Point(
                std::clamp(p.first - minX, 0, rectW - 1),
                std::clamp(p.second - minY, 0, rectH - 1)
            ));
        }

        // Fill probable foreground area (inside lasso)
        if (shiftedPoints.size() >= 3) {
            std::vector<std::vector<cv::Point>> polys = { shiftedPoints };
            cv::fillPoly(croppedMask, polys, cv::Scalar(cv::GC_PR_FGD));
        } else {
            // Draw a thick line if too few points
            for (size_t i = 0; i < shiftedPoints.size() - 1; ++i) {
                cv::line(croppedMask, shiftedPoints[i], shiftedPoints[i+1], cv::Scalar(cv::GC_PR_FGD), 5);
            }
        }

        cv::Mat bgdModel, fgdModel;
        try {
            if (!m_SmartSelectCancelled.load()) {
                cv::grabCut(croppedMat, croppedMask, cv::Rect(), bgdModel, fgdModel, 2, cv::GC_INIT_WITH_MASK);
            }
        } catch (...) {
            // Ignore errors
        }

        if (m_SmartSelectCancelled.load()) {
            m_SmartSelectInProgress.store(false);
            return;
        }

        // Map back to full selection mask
        cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
        for (int y = 0; y < rectH; ++y) {
            for (int x = 0; x < rectW; ++x) {
                uint8_t val = croppedMask.at<uint8_t>(y, x);
                if (val == cv::GC_PR_FGD || val == cv::GC_FGD) {
                    temp.at<uint8_t>(minY + y, minX + x) = 255;
                }
            }
        }

        cv::Mat current(m_Height, m_Width, CV_8UC1);
        if (!oldMask.empty()) {
            std::memcpy(current.data, oldMask.data(), oldMask.size());
        } else {
            current = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
        }
        cv::Mat combined;
        if (add) {
            cv::bitwise_or(current, temp, combined);
        } else if (subtract) {
            cv::bitwise_and(current, ~temp, combined);
        } else {
            combined = temp.clone();
        }

        m_SelectionMask.resize((size_t)m_Width * m_Height);
        std::memcpy(m_SelectionMask.data(), combined.data, m_SelectionMask.size());
        m_HasSelection = false;
        for (uint8_t val : m_SelectionMask) {
            if (val > 0) {
                m_HasSelection = true;
                break;
            }
        }
        m_SelectionMaskNeedsUpload = true;

        UpdateSelectionMaskTexture(device);
        m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Smart Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
        m_SmartSelectInProgress.store(false);
    });
    t.detach();
}

void Canvas::ApplyBucketFill(int startX, int startY, float tolerance, const float color[4], bool contiguous) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    if (startX < 0 || startX >= m_Width || startY < 0 || startY >= m_Height) return;

    auto& layer = m_Layers[m_ActiveLayerIdx];
    std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);

    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    const bool floatDoc = (m_DocumentBitDepth != DocumentBitDepth::U8);

    if (floatDoc) {
        // Float flood-fill: compare linear RGB (no 8-bit quantize). Tolerance is
        // max channel abs-diff (same units as color, not 0..255).
        const size_t seedIdx = ((size_t)startY * m_Width + startX) * 4;
        const float seedR = layerPixels[seedIdx + 0];
        const float seedG = layerPixels[seedIdx + 1];
        const float seedB = layerPixels[seedIdx + 2];
        const float tol = std::max(0.f, tolerance); // user 0..1 scales poorly for HDR;
        // Map UI tolerance 0..1 → absolute float threshold; expand for HDR docs.
        const float thr = (tol <= 1.f) ? (tol * 0.05f + tol * tol * 0.5f) : tol;
        auto inRange = [&](int x, int y) {
            size_t i = ((size_t)y * m_Width + x) * 4;
            float dr = std::fabs(layerPixels[i + 0] - seedR);
            float dg = std::fabs(layerPixels[i + 1] - seedG);
            float db = std::fabs(layerPixels[i + 2] - seedB);
            return (dr <= thr && dg <= thr && db <= thr);
        };
        if (contiguous) {
            std::vector<uint8_t> visited((size_t)m_Width * m_Height, 0);
            std::vector<std::pair<int,int>> stack;
            stack.push_back({startX, startY});
            while (!stack.empty()) {
                auto [x, y] = stack.back();
                stack.pop_back();
                if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) continue;
                size_t vi = (size_t)y * m_Width + x;
                if (visited[vi]) continue;
                if (!inRange(x, y)) continue;
                visited[vi] = 1;
                temp.at<uint8_t>(y, x) = 255;
                stack.push_back({x + 1, y});
                stack.push_back({x - 1, y});
                stack.push_back({x, y + 1});
                stack.push_back({x, y - 1});
            }
        } else {
            for (int y = 0; y < m_Height; ++y)
                for (int x = 0; x < m_Width; ++x)
                    if (inRange(x, y)) temp.at<uint8_t>(y, x) = 255;
        }
    } else {
        cv::Mat mat = ImageManager::PixelsToMat8UC3(layerPixels, m_Width, m_Height);
        if (contiguous) {
            cv::Mat mask = cv::Mat::zeros(m_Height + 2, m_Width + 2, CV_8UC1);
            cv::Scalar loDiff(tolerance * 255.0f, tolerance * 255.0f, tolerance * 255.0f);
            cv::Scalar upDiff(tolerance * 255.0f, tolerance * 255.0f, tolerance * 255.0f);
            cv::floodFill(mat, mask, cv::Point(startX, startY), cv::Scalar(255), nullptr, loDiff, upDiff, 4 | cv::FLOODFILL_MASK_ONLY | (255 << 8));
            temp = mask(cv::Range(1, m_Height + 1), cv::Range(1, m_Width + 1)).clone();
        } else {
            cv::Vec3b seedColor = mat.at<cv::Vec3b>(startY, startX);
            for (int y = 0; y < m_Height; ++y) {
                for (int x = 0; x < m_Width; ++x) {
                    cv::Vec3b colorVal = mat.at<cv::Vec3b>(y, x);
                    float diff = std::sqrt(
                        std::pow(static_cast<float>(colorVal[0]) - seedColor[0], 2) +
                        std::pow(static_cast<float>(colorVal[1]) - seedColor[1], 2) +
                        std::pow(static_cast<float>(colorVal[2]) - seedColor[2], 2)
                    ) / 255.0f;
                    if (diff <= tolerance * std::sqrt(3.0f)) {
                        temp.at<uint8_t>(y, x) = 255;
                    }
                }
            }
        }
    }

    for (int ty = 0; ty < (m_Height + 255) / 256; ++ty) {
        for (int tx = 0; tx < (m_Width + 255) / 256; ++tx) {
            bool tileAffected = false;
            for (int y = ty * 256; y < std::min((ty + 1) * 256, m_Height); ++y) {
                for (int x = tx * 256; x < std::min((tx + 1) * 256, m_Width); ++x) {
                    if (temp.at<uint8_t>(y, x) > 0) {
                        tileAffected = true;
                        break;
                    }
                }
                if (tileAffected) break;
            }
            if (tileAffected) {
                BackupTile(tx, ty);
            }
        }
    }

    const bool useSel = m_HasSelection &&
        m_SelectionMask.size() == (size_t)m_Width * (size_t)m_Height;
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float selectionVal = useSel ? SelU82F(m_SelectionMask[(size_t)y * m_Width + x]) : 1.0f;
            float maskVal = temp.at<uint8_t>(y, x) / 255.0f;
            float fillAlpha = maskVal * selectionVal * color[3];

            if (fillAlpha > 0.0f) {
                size_t idx = ((size_t)y * m_Width + x) * 4;
                float destR = layerPixels[idx + 0];
                float destG = layerPixels[idx + 1];
                float destB = layerPixels[idx + 2];
                float destA = layerPixels[idx + 3];

                float outA = fillAlpha + destA * (1.0f - fillAlpha);
                if (outA > 0.0f) {
                    layerPixels[idx + 0] = (color[0] * fillAlpha + destR * destA * (1.0f - fillAlpha)) / outA;
                    layerPixels[idx + 1] = (color[1] * fillAlpha + destG * destA * (1.0f - fillAlpha)) / outA;
                    layerPixels[idx + 2] = (color[2] * fillAlpha + destB * destA * (1.0f - fillAlpha)) / outA;
                    layerPixels[idx + 3] = outA;
                }
            }
        }
    }

    SetLayerPixelsF(layer, layerPixels, m_Width, m_Height, m_CanvasFormat);
    m_IsDocumentModified = true;

    if (!m_ActiveStrokeDeltas.empty()) {
        auto deltas = SealActiveStrokeDeltas(m_ActiveStrokeDeltas, layer.tileCache.get());
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Bucket Fill", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }
}

void Canvas::ApplyGradient(int x1, int y1, int x2, int y2, const float startColor[4], const float endColor[4]) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;

    auto& layer = m_Layers[m_ActiveLayerIdx];
    std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);

    float dx = (float)(x2 - x1);
    float dy = (float)(y2 - y1);
    float lenSq = dx * dx + dy * dy;

    for (int ty = 0; ty < (m_Height + 255) / 256; ++ty) {
        for (int tx = 0; tx < (m_Width + 255) / 256; ++tx) {
            BackupTile(tx, ty);
        }
    }

    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float t = 0.0f;
            if (lenSq > 0.001f) {
                float vx = (float)(x - x1);
                float vy = (float)(y - y1);
                t = (vx * dx + vy * dy) / lenSq;
                t = std::clamp(t, 0.0f, 1.0f);
            }

            float lerpColor[4];
            lerpColor[0] = startColor[0] * (1.0f - t) + endColor[0] * t;
            lerpColor[1] = startColor[1] * (1.0f - t) + endColor[1] * t;
            lerpColor[2] = startColor[2] * (1.0f - t) + endColor[2] * t;
            lerpColor[3] = startColor[3] * (1.0f - t) + endColor[3] * t;

            float selectionVal = GetSelWeight(m_SelectionMask, m_Width, m_Height, x, y, m_HasSelection);
            float alphaVal = lerpColor[3] * selectionVal;

            if (alphaVal > 0.0f) {
                size_t idx = ((size_t)y * m_Width + x) * 4;
                float destR = layerPixels[idx + 0];
                float destG = layerPixels[idx + 1];
                float destB = layerPixels[idx + 2];
                float destA = layerPixels[idx + 3];

                float outA = alphaVal + destA * (1.0f - alphaVal);
                if (outA > 0.0f) {
                    layerPixels[idx + 0] = (lerpColor[0] * alphaVal + destR * destA * (1.0f - alphaVal)) / outA;
                    layerPixels[idx + 1] = (lerpColor[1] * alphaVal + destG * destA * (1.0f - alphaVal)) / outA;
                    layerPixels[idx + 2] = (lerpColor[2] * alphaVal + destB * destA * (1.0f - alphaVal)) / outA;
                    layerPixels[idx + 3] = outA;
                }
            }
        }
    }

    SetLayerPixelsF(layer, layerPixels, m_Width, m_Height, m_CanvasFormat);
    m_IsDocumentModified = true;

    if (!m_ActiveStrokeDeltas.empty()) {
        auto deltas = SealActiveStrokeDeltas(m_ActiveStrokeDeltas, layer.tileCache.get());
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Gradient", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }
}

bool Canvas::ComputeLayerContentBounds(int layerIdx, int& outMinX, int& outMinY,
                                       int& outMaxX, int& outMaxY,
                                       float alphaThreshold, bool respectSelection) const {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    if (m_Width <= 0 || m_Height <= 0) return false;
    const Layer& layer = m_Layers[layerIdx];
    if (!layer.tileCache || layer.tileCache->IsEmpty()) return false;

    const bool useSel = respectSelection && m_HasSelection &&
                        m_SelectionMask.size() == (size_t)m_Width * (size_t)m_Height;
    const float thr = std::max(0.f, alphaThreshold);

    // Seed scan region: selection AABB or union of existing tiles
    int scanMinX = m_Width, scanMinY = m_Height, scanMaxX = -1, scanMaxY = -1;
    if (useSel) {
        int bx = 0, by = 0, bw = 0, bh = 0;
        if (!GetSelectionBounds(bx, by, bw, bh) || bw <= 0 || bh <= 0)
            return false;
        scanMinX = bx; scanMinY = by;
        scanMaxX = bx + bw - 1; scanMaxY = by + bh - 1;
    } else {
        const int ntx = layer.tileCache->GetTilesX();
        const int nty = layer.tileCache->GetTilesY();
        for (int ty = 0; ty < nty; ++ty) {
            for (int tx = 0; tx < ntx; ++tx) {
                if (!layer.tileCache->HasTile(tx, ty)) continue;
                int x0 = tx * TILE_SIZE, y0 = ty * TILE_SIZE;
                int x1 = std::min(m_Width - 1, x0 + TILE_SIZE - 1);
                int y1 = std::min(m_Height - 1, y0 + TILE_SIZE - 1);
                scanMinX = std::min(scanMinX, x0);
                scanMinY = std::min(scanMinY, y0);
                scanMaxX = std::max(scanMaxX, x1);
                scanMaxY = std::max(scanMaxY, y1);
            }
        }
        if (scanMaxX < scanMinX) return false;
    }

    const int64_t scanArea = (int64_t)(scanMaxX - scanMinX + 1) * (int64_t)(scanMaxY - scanMinY + 1);
    // Fast path: no selection + large occupied region → tile AABB only.
    // Full per-pixel alpha on dense 16K was ~6s (Ctrl+T "smart border"). Tile AABB is
    // slightly loose (up to TILE_SIZE) but O(tiles) not O(pixels).
    constexpr int64_t kPixelScanBudget = 2048ll * 2048ll; // ~4M px max fine scan
    if (!useSel && scanArea > kPixelScanBudget) {
        outMinX = scanMinX;
        outMinY = scanMinY;
        outMaxX = scanMaxX;
        outMaxY = scanMaxY;
        return true;
    }

    // Alpha (× selection) scan — only tiles that intersect seed region
    int minX = m_Width, minY = m_Height, maxX = -1, maxY = -1;
    const auto fmt = layer.tileCache->GetFormat();
    const int bpp = layer.tileCache->GetBytesPerPixel();
    const int t0x = scanMinX / TILE_SIZE, t0y = scanMinY / TILE_SIZE;
    const int t1x = scanMaxX / TILE_SIZE, t1y = scanMaxY / TILE_SIZE;
    const int ntx = layer.tileCache->GetTilesX();
    const int nty = layer.tileCache->GetTilesY();

    // Coarse step for medium areas (selection 2K–4K): still tight, much faster
    const int step = (scanArea > 512ll * 512ll) ? 4 : 1;

    for (int ty = t0y; ty <= t1y && ty < nty; ++ty) {
        for (int tx = t0x; tx <= t1x && tx < ntx; ++tx) {
            if (!layer.tileCache->HasTile(tx, ty)) continue;
            const uint8_t* raw = layer.tileCache->GetTileData(tx, ty);
            if (!raw) continue;
            const int tileX0 = tx * TILE_SIZE, tileY0 = ty * TILE_SIZE;
            const int px0 = std::max(scanMinX, tileX0);
            const int py0 = std::max(scanMinY, tileY0);
            const int px1 = std::min(scanMaxX, tileX0 + TILE_SIZE - 1);
            const int py1 = std::min(scanMaxY, tileY0 + TILE_SIZE - 1);
            if (px1 < px0 || py1 < py0) continue;

            for (int y = py0; y <= py1; y += step) {
                const int lyT = y - tileY0;
                const uint8_t* row = raw + ((size_t)lyT * TILE_SIZE) * bpp;
                for (int x = px0; x <= px1; x += step) {
                    if (useSel) {
                        uint8_t sw = m_SelectionMask[(size_t)y * (size_t)m_Width + (size_t)x];
                        if (sw == 0) continue;
                    }
                    const int lxT = x - tileX0;
                    const uint8_t* p = row + (size_t)lxT * bpp;
                    float a = 0.f;
                    if (fmt == CanvasPixelFormat::RGBA8) {
                        a = p[3] / 255.f;
                    } else if (fmt == CanvasPixelFormat::RGBA16F) {
                        float rgba[4];
                        HalfFloat::LoadRGBA16F(p, rgba);
                        a = rgba[3];
                    } else {
                        a = reinterpret_cast<const float*>(p)[3];
                    }
                    if (useSel) {
                        float w = m_SelectionMask[(size_t)y * (size_t)m_Width + (size_t)x] / 255.f;
                        a *= w;
                    }
                    if (a <= thr) continue;
                    minX = std::min(minX, x);
                    minY = std::min(minY, y);
                    maxX = std::max(maxX, x);
                    maxY = std::max(maxY, y);
                }
            }
        }
    }

    if (maxX < minX) return false;
    // Expand coarse hits so soft edges are not clipped
    if (step > 1) {
        minX = std::max(scanMinX, minX - step);
        minY = std::max(scanMinY, minY - step);
        maxX = std::min(scanMaxX, maxX + step);
        maxY = std::min(scanMaxY, maxY + step);
    }
    outMinX = minX;
    outMinY = minY;
    outMaxX = maxX;
    outMaxY = maxY;
    return true;
}

void Canvas::StartMovePixels(ID3D11Device* device) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    if (m_Width <= 0 || m_Height <= 0) return;
    if (m_IsMovingPixels) {
        CommitMovePixels(device);
    }

    m_IsMovingPixels = true;
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_FloatingScaleX = 1.0f;
    m_FloatingScaleY = 1.0f;
    m_FloatingRotation = 0.0f;
    m_StartActiveLayerIdx = m_ActiveLayerIdx;
    m_ActiveStrokeDeltas.clear();

    Layer& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);

    // ---- Content AABB via alpha (not full canvas / not whole tiles) ----
    // If selection exists: pixels with selection>0 AND alpha high enough.
    // Else: any non-transparent content on the active layer.
    int minX = 0, minY = 0, maxX = m_Width - 1, maxY = m_Height - 1;
    {
        int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
        const bool respectSel = m_HasSelection && !m_SelectionMask.empty();
        if (!ComputeLayerContentBounds(m_ActiveLayerIdx, cx0, cy0, cx1, cy1,
                                       /*alphaThreshold=*/0.02f, respectSel)) {
            // Fallback: selection AABB only (may be empty of pixels)
            if (respectSel) {
                int bx = 0, by = 0, bw = 0, bh = 0;
                if (GetSelectionBounds(bx, by, bw, bh) && bw > 0 && bh > 0) {
                    minX = bx; minY = by; maxX = bx + bw - 1; maxY = by + bh - 1;
                    Logger::Get().InfoTag("transform",
                        "StartMovePixels: no opaque content in selection — using selection AABB");
                } else {
                    m_IsMovingPixels = false;
                    return;
                }
            } else {
                Logger::Get().InfoTag("transform",
                    "StartMovePixels: no opaque content on layer — abort");
                m_IsMovingPixels = false;
                return;
            }
        } else {
            minX = cx0; minY = cy0; maxX = cx1; maxY = cy1;
            Logger::Get().InfoTag("transform",
                "StartMovePixels: alpha bounds " + std::to_string(minX) + "," + std::to_string(minY) +
                " " + std::to_string(maxX - minX + 1) + "x" + std::to_string(maxY - minY + 1));
        }
        // pad for soft edges / bilinear
        minX = std::max(0, minX - 2); minY = std::max(0, minY - 2);
        maxX = std::min(m_Width - 1, maxX + 2); maxY = std::min(m_Height - 1, maxY + 2);
    }

    m_FloatingBBoxX = minX;
    m_FloatingBBoxY = minY;
    m_FloatingBBoxW = maxX - minX + 1;
    m_FloatingBBoxH = maxY - minY + 1;
    m_FloatingBufW = m_FloatingBBoxW;
    m_FloatingBufH = m_FloatingBBoxH;
    m_FloatingCenterX = (minX + maxX) * 0.5f;
    m_FloatingCenterY = (minY + maxY) * 0.5f;

    const size_t pixCount = (size_t)m_FloatingBufW * (size_t)m_FloatingBufH;
    const size_t estBytes = pixCount * 16ull; // RGBA32F
    // Soft: refuse absurd full-doc float lift (16K = 4 GiB) — use selection or crop first.
    constexpr size_t kMaxFloatLiftBytes = 512ull * 1024ull * 1024ull; // 512 MiB ≈ 4K×4K RGBA32F
    if (estBytes > kMaxFloatLiftBytes || MemoryStats::ExceedsHardRamCeiling(estBytes)) {
        Logger::Get().ErrorTag("mem",
            "StartMovePixels: floating buffer " + MemoryStats::FormatBytes(estBytes) +
            " too large — abort. Select a smaller region (marquee) before transform.");
        m_IsMovingPixels = false;
        m_FloatingPixels.clear();
        m_OriginalSelectionMask.clear();
        m_FloatingBufW = m_FloatingBufH = 0;
        m_FloatingBBoxW = m_FloatingBBoxH = 0;
        return;
    }
    if (MemoryStats::ExceedsRamBudget(estBytes, 0.35)) {
        MemoryStats::LogSoftBudget("StartMovePixels floating", estBytes, 0.35);
    }

    // Local-only buffers (not full document)
    m_FloatingPixels.assign(pixCount * 4, 0.0f);
    m_OriginalSelectionMask.assign(pixCount, 255);
    if (m_HasSelection && !m_SelectionMask.empty()) {
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                size_t li = (size_t)(y - minY) * (size_t)m_FloatingBufW + (size_t)(x - minX);
                m_OriginalSelectionMask[li] = m_SelectionMask[(size_t)y * (size_t)m_Width + (size_t)x];
            }
        }
    }

    // Undo backup only for tiles we will cut (source AABB)
    BackupTilesInRect(minX, minY, maxX, maxY);

    // Lift pixels tile-locked (no GetPixelF/SetPixelF thrash)
    {
        const auto fmt = layer.tileCache->GetFormat();
        const int bpp = layer.tileCache->GetBytesPerPixel();
        const int ntx = layer.tileCache->GetTilesX();
        const int nty = layer.tileCache->GetTilesY();
        const int t0x = minX / TILE_SIZE, t0y = minY / TILE_SIZE;
        const int t1x = maxX / TILE_SIZE, t1y = maxY / TILE_SIZE;
        for (int ty = t0y; ty <= t1y && ty < nty; ++ty) {
            for (int tx = t0x; tx <= t1x && tx < ntx; ++tx) {
                const int tileX0 = tx * TILE_SIZE, tileY0 = ty * TILE_SIZE;
                const int px0 = std::max(minX, tileX0);
                const int py0 = std::max(minY, tileY0);
                const int px1 = std::min(maxX, tileX0 + TILE_SIZE - 1);
                const int py1 = std::min(maxY, tileY0 + TILE_SIZE - 1);
                if (px1 < px0 || py1 < py0) continue;
                // Need a writable tile only if something is cut; still lock if any weight>0
                uint8_t* raw = layer.tileCache->LockTile(tx, ty);
                if (!raw) continue;
                for (int y = py0; y <= py1; ++y) {
                    const int lyT = y - tileY0;
                    uint8_t* row = raw + ((size_t)lyT * TILE_SIZE) * bpp;
                    for (int x = px0; x <= px1; ++x) {
                        size_t li = (size_t)(y - minY) * (size_t)m_FloatingBufW + (size_t)(x - minX);
                        float weight = SelU82F(m_OriginalSelectionMask[li]);
                        if (weight <= 0.0f) continue;
                        const int lxT = x - tileX0;
                        uint8_t* p = row + (size_t)lxT * bpp;
                        float rgba[4];
                        if (fmt == CanvasPixelFormat::RGBA8) {
                            rgba[0] = p[0] / 255.f; rgba[1] = p[1] / 255.f;
                            rgba[2] = p[2] / 255.f; rgba[3] = p[3] / 255.f;
                        } else if (fmt == CanvasPixelFormat::RGBA16F) {
                            HalfFloat::LoadRGBA16F(p, rgba);
                        } else {
                            const float* fp = reinterpret_cast<const float*>(p);
                            rgba[0] = fp[0]; rgba[1] = fp[1]; rgba[2] = fp[2]; rgba[3] = fp[3];
                        }
                        size_t pi = li * 4;
                        m_FloatingPixels[pi + 0] = rgba[0];
                        m_FloatingPixels[pi + 1] = rgba[1];
                        m_FloatingPixels[pi + 2] = rgba[2];
                        m_FloatingPixels[pi + 3] = rgba[3] * weight;
                        rgba[0] *= (1.0f - weight);
                        rgba[1] *= (1.0f - weight);
                        rgba[2] *= (1.0f - weight);
                        rgba[3] *= (1.0f - weight);
                        if (fmt == CanvasPixelFormat::RGBA8) {
                            p[0] = HalfFloat::FloatToU8(rgba[0]);
                            p[1] = HalfFloat::FloatToU8(rgba[1]);
                            p[2] = HalfFloat::FloatToU8(rgba[2]);
                            p[3] = HalfFloat::FloatToU8(rgba[3]);
                        } else if (fmt == CanvasPixelFormat::RGBA16F) {
                            HalfFloat::StoreRGBA16F(p, rgba);
                        } else {
                            float* fp = reinterpret_cast<float*>(p);
                            fp[0] = rgba[0]; fp[1] = rgba[1]; fp[2] = rgba[2]; fp[3] = rgba[3];
                        }
                    }
                }
            }
        }
    }
    layer.needsUpload = true;
    layer.thumbDirty = true;
    m_CompositeDirty = true;

    // Edge RGB pad for bilinear (local buffer only — 4 cheap dilate passes)
    const int fw = m_FloatingBufW, fh = m_FloatingBufH;
    for (int iter = 0; iter < 4; ++iter) {
        std::vector<float> next = m_FloatingPixels;
        for (int y = 0; y < fh; ++y) {
            for (int x = 0; x < fw; ++x) {
                size_t idx = ((size_t)y * fw + x) * 4;
                if (m_FloatingPixels[idx + 3] > 0.05f) continue;
                int dxs[] = {0, 0, -1, 1};
                int dys[] = {-1, 1, 0, 0};
                float bestA = 0.f, pr = 0.f, pg = 0.f, pb = 0.f;
                for (int n = 0; n < 4; ++n) {
                    int nx = x + dxs[n], ny = y + dys[n];
                    if (nx < 0 || ny < 0 || nx >= fw || ny >= fh) continue;
                    size_t nIdx = ((size_t)ny * fw + nx) * 4;
                    if (m_FloatingPixels[nIdx + 3] > bestA) {
                        bestA = m_FloatingPixels[nIdx + 3];
                        pr = m_FloatingPixels[nIdx + 0];
                        pg = m_FloatingPixels[nIdx + 1];
                        pb = m_FloatingPixels[nIdx + 2];
                    }
                }
                if (bestA > 0.05f) {
                    next[idx + 0] = pr;
                    next[idx + 1] = pg;
                    next[idx + 2] = pb;
                }
            }
        }
        m_FloatingPixels.swap(next);
    }

    if (device) {
        if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
        if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = (UINT)fw;
        desc.Height = (UINT)fh;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = m_FloatingPixels.data();
        initData.SysMemPitch = (UINT)(fw * sizeof(float) * 4);

        HRESULT hr = device->CreateTexture2D(&desc, &initData, &m_FloatingTexture);
        if (SUCCEEDED(hr))
            device->CreateShaderResourceView(m_FloatingTexture, nullptr, &m_FloatingSRV);

        if (layer.hasMask && !layer.mask.empty()) {
            if (m_FloatingMaskTexture) { m_FloatingMaskTexture->Release(); m_FloatingMaskTexture = nullptr; }
            if (m_FloatingMaskSRV) { m_FloatingMaskSRV->Release(); m_FloatingMaskSRV = nullptr; }

            std::vector<uint8_t> floatingMask(pixCount, 0);
            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    size_t li = (size_t)(y - minY) * (size_t)fw + (size_t)(x - minX);
                    if (m_OriginalSelectionMask[li] > 0) {
                        size_t di = (size_t)y * (size_t)m_Width + (size_t)x;
                        floatingMask[li] = layer.mask[di];
                        layer.mask[di] = 255;
                    }
                }
            }
            layer.maskNeedsUpload = true;

            D3D11_TEXTURE2D_DESC mDesc = {};
            mDesc.Width = (UINT)fw;
            mDesc.Height = (UINT)fh;
            mDesc.MipLevels = 1;
            mDesc.ArraySize = 1;
            mDesc.Format = DXGI_FORMAT_R8_UNORM;
            mDesc.SampleDesc.Count = 1;
            mDesc.Usage = D3D11_USAGE_DEFAULT;
            mDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA mInitData = {};
            mInitData.pSysMem = floatingMask.data();
            mInitData.SysMemPitch = (UINT)fw;

            hr = device->CreateTexture2D(&mDesc, &mInitData, &m_FloatingMaskTexture);
            if (SUCCEEDED(hr))
                device->CreateShaderResourceView(m_FloatingMaskTexture, nullptr, &m_FloatingMaskSRV);
        }
    }

    Logger::Get().Debug(
        std::string("[perf] StartMovePixels floating ") + std::to_string(fw) + "x" +
        std::to_string(fh) + " (" + MemoryStats::FormatBytes(estBytes) + ")");
}

void Canvas::UpdateMovePixels(ID3D11Device* device, int dx, int dy) {
    if (!m_IsMovingPixels) return;
    m_FloatingOffsetX = dx;
    m_FloatingOffsetY = dy;
}

static float sampleBilinearChannel(const std::vector<float>& pixels, int width, int height, float fx, float fy, int channel) {
    int x1 = (int)std::floor(fx);
    int y1 = (int)std::floor(fy);
    int x2 = x1 + 1;
    int y2 = y1 + 1;
    
    float tx = fx - x1;
    float ty = fy - y1;
    
    x1 = std::clamp(x1, 0, width - 1);
    x2 = std::clamp(x2, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    y2 = std::clamp(y2, 0, height - 1);
    
    float c00 = pixels[(y1 * width + x1) * 4 + channel];
    float c10 = pixels[(y1 * width + x2) * 4 + channel];
    float c01 = pixels[(y2 * width + x1) * 4 + channel];
    float c11 = pixels[(y2 * width + x2) * 4 + channel];
    
    float r1 = c00 * (1.0f - tx) + c10 * tx;
    float r2 = c01 * (1.0f - tx) + c11 * tx;
    return r1 * (1.0f - ty) + r2 * ty;
}

// Sample all 4 channels once (4× cheaper than 4× sampleBilinearChannel).
static void sampleBilinearRGBA(const std::vector<float>& pixels, int width, int height,
                               float fx, float fy, float out[4]) {
    int x1 = (int)std::floor(fx);
    int y1 = (int)std::floor(fy);
    int x2 = x1 + 1;
    int y2 = y1 + 1;
    float tx = fx - x1;
    float ty = fy - y1;
    x1 = std::clamp(x1, 0, width - 1);
    x2 = std::clamp(x2, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    y2 = std::clamp(y2, 0, height - 1);
    const size_t i00 = ((size_t)y1 * width + x1) * 4;
    const size_t i10 = ((size_t)y1 * width + x2) * 4;
    const size_t i01 = ((size_t)y2 * width + x1) * 4;
    const size_t i11 = ((size_t)y2 * width + x2) * 4;
    const float w00 = (1.f - tx) * (1.f - ty);
    const float w10 = tx * (1.f - ty);
    const float w01 = (1.f - tx) * ty;
    const float w11 = tx * ty;
    for (int c = 0; c < 4; ++c)
        out[c] = pixels[i00 + c] * w00 + pixels[i10 + c] * w10
               + pixels[i01 + c] * w01 + pixels[i11 + c] * w11;
}

static float sampleBilinearMask(const std::vector<uint8_t>& mask, int width, int height, float fx, float fy) {
    int x1 = (int)std::floor(fx);
    int y1 = (int)std::floor(fy);
    int x2 = x1 + 1;
    int y2 = y1 + 1;
    
    float tx = fx - x1;
    float ty = fy - y1;
    
    x1 = std::clamp(x1, 0, width - 1);
    x2 = std::clamp(x2, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    y2 = std::clamp(y2, 0, height - 1);
    
    float c00 = SelU82F(mask[y1 * width + x1]);
    float c10 = SelU82F(mask[y1 * width + x2]);
    float c01 = SelU82F(mask[y2 * width + x1]);
    float c11 = SelU82F(mask[y2 * width + x2]);
    
    float r1 = c00 * (1.0f - tx) + c10 * tx;
    float r2 = c01 * (1.0f - tx) + c11 * tx;
    return r1 * (1.0f - ty) + r2 * ty;
}

void Canvas::CommitMovePixels(ID3D11Device* device) {
    if (!m_IsMovingPixels) return;

    if (m_StartActiveLayerIdx >= 0 && m_StartActiveLayerIdx < (int)m_Layers.size() &&
        m_FloatingBufW > 0 && m_FloatingBufH > 0 && !m_FloatingPixels.empty()) {
        Layer& layer = m_Layers[m_StartActiveLayerIdx];
        EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);

        const float cx = m_FloatingCenterX;
        const float cy = m_FloatingCenterY;
        const int fw = m_FloatingBufW;
        const int fh = m_FloatingBufH;
        const int ox = m_FloatingBBoxX;
        const int oy = m_FloatingBBoxY;

        // Destination AABB of transformed source corners (with pad)
        auto xformCorner = [&](float sx, float sy, float& dx, float& dy) {
            float px = (sx - cx) * m_FloatingScaleX;
            float py = (sy - cy) * m_FloatingScaleY;
            float cosA = std::cos(m_FloatingRotation);
            float sinA = std::sin(m_FloatingRotation);
            dx = px * cosA - py * sinA + cx + (float)m_FloatingOffsetX;
            dy = px * sinA + py * cosA + cy + (float)m_FloatingOffsetY;
        };
        float cxs[4], cys[4];
        xformCorner((float)ox, (float)oy, cxs[0], cys[0]);
        xformCorner((float)(ox + fw - 1), (float)oy, cxs[1], cys[1]);
        xformCorner((float)(ox + fw - 1), (float)(oy + fh - 1), cxs[2], cys[2]);
        xformCorner((float)ox, (float)(oy + fh - 1), cxs[3], cys[3]);
        float dMinX = cxs[0], dMaxX = cxs[0], dMinY = cys[0], dMaxY = cys[0];
        for (int i = 1; i < 4; ++i) {
            dMinX = std::min(dMinX, cxs[i]); dMaxX = std::max(dMaxX, cxs[i]);
            dMinY = std::min(dMinY, cys[i]); dMaxY = std::max(dMaxY, cys[i]);
        }
        int destX0 = std::max(0, (int)std::floor(dMinX) - 2);
        int destY0 = std::max(0, (int)std::floor(dMinY) - 2);
        int destX1 = std::min(m_Width - 1, (int)std::ceil(dMaxX) + 2);
        int destY1 = std::min(m_Height - 1, (int)std::ceil(dMaxY) + 2);

        // Also cover source cut region (already backed up) and destination for undo
        int prevActive = m_ActiveLayerIdx;
        m_ActiveLayerIdx = m_StartActiveLayerIdx;
        BackupTilesInRect(destX0, destY0, destX1, destY1);

        const float invCos = std::cos(-m_FloatingRotation);
        const float invSin = std::sin(-m_FloatingRotation);
        const float invSx = (m_FloatingScaleX > 1e-4f) ? (1.f / m_FloatingScaleX) : 1.f;
        const float invSy = (m_FloatingScaleY > 1e-4f) ? (1.f / m_FloatingScaleY) : 1.f;

        // Tile-locked commit (no GetPixelF/SetPixelF per pixel)
        const auto fmt = layer.tileCache->GetFormat();
        const int bpp = layer.tileCache->GetBytesPerPixel();
        const int ntx = layer.tileCache->GetTilesX();
        const int nty = layer.tileCache->GetTilesY();
        const int tx0 = destX0 / TILE_SIZE, ty0 = destY0 / TILE_SIZE;
        const int tx1 = destX1 / TILE_SIZE, ty1 = destY1 / TILE_SIZE;

        for (int ty = ty0; ty <= ty1 && ty < nty; ++ty) {
            for (int tx = tx0; tx <= tx1 && tx < ntx; ++tx) {
                const int tileX0 = tx * TILE_SIZE, tileY0 = ty * TILE_SIZE;
                const int px0 = std::max(destX0, tileX0);
                const int py0 = std::max(destY0, tileY0);
                const int px1 = std::min(destX1, tileX0 + TILE_SIZE - 1);
                const int py1 = std::min(destY1, tileY0 + TILE_SIZE - 1);
                if (px1 < px0 || py1 < py0) continue;

                uint8_t* raw = layer.tileCache->LockTile(tx, ty);
                if (!raw) continue;

                for (int y = py0; y <= py1; ++y) {
                    const int lyT = y - tileY0;
                    uint8_t* row = raw + ((size_t)lyT * TILE_SIZE) * bpp;
                    for (int x = px0; x <= px1; ++x) {
                        float px = (float)x - cx - (float)m_FloatingOffsetX;
                        float py = (float)y - cy - (float)m_FloatingOffsetY;
                        float rx = px * invCos - py * invSin;
                        float ry = px * invSin + py * invCos;
                        float srcX = rx * invSx + cx;
                        float srcY = ry * invSy + cy;
                        float lx = srcX - (float)ox;
                        float ly = srcY - (float)oy;
                        if (lx < -0.5f || ly < -0.5f || lx > (float)fw - 0.5f || ly > (float)fh - 0.5f)
                            continue;

                        float src[4];
                        sampleBilinearRGBA(m_FloatingPixels, fw, fh, lx, ly, src);
                        if (src[3] <= 0.0f) continue;
                        const float srcAlpha = src[3];

                        const int lxT = x - tileX0;
                        uint8_t* p = row + (size_t)lxT * bpp;
                        float dest[4];
                        if (fmt == CanvasPixelFormat::RGBA8) {
                            dest[0] = p[0] / 255.f; dest[1] = p[1] / 255.f;
                            dest[2] = p[2] / 255.f; dest[3] = p[3] / 255.f;
                        } else if (fmt == CanvasPixelFormat::RGBA16F) {
                            HalfFloat::LoadRGBA16F(p, dest);
                        } else {
                            const float* fp = reinterpret_cast<const float*>(p);
                            dest[0] = fp[0]; dest[1] = fp[1]; dest[2] = fp[2]; dest[3] = fp[3];
                        }

                        float destAlpha = dest[3];
                        float outAlpha = srcAlpha + destAlpha * (1.0f - srcAlpha);
                        if (outAlpha > 0.0f) {
                            dest[0] = (src[0] * srcAlpha + dest[0] * destAlpha * (1.0f - srcAlpha)) / outAlpha;
                            dest[1] = (src[1] * srcAlpha + dest[1] * destAlpha * (1.0f - srcAlpha)) / outAlpha;
                            dest[2] = (src[2] * srcAlpha + dest[2] * destAlpha * (1.0f - srcAlpha)) / outAlpha;
                        }
                        dest[3] = outAlpha;

                        if (fmt == CanvasPixelFormat::RGBA8) {
                            p[0] = HalfFloat::FloatToU8(dest[0]);
                            p[1] = HalfFloat::FloatToU8(dest[1]);
                            p[2] = HalfFloat::FloatToU8(dest[2]);
                            p[3] = HalfFloat::FloatToU8(dest[3]);
                        } else if (fmt == CanvasPixelFormat::RGBA16F) {
                            HalfFloat::StoreRGBA16F(p, dest);
                        } else {
                            float* fp = reinterpret_cast<float*>(p);
                            fp[0] = dest[0]; fp[1] = dest[1]; fp[2] = dest[2]; fp[3] = dest[3];
                        }
                    }
                }
            }
        }
        layer.needsUpload = true;
        layer.thumbDirty = true;
        // Deferred FX: mark dirty only if filters present (don't force full filtersDirty rebuild)
        if (LayerFilterListHasEnabled(layer.filters)) {
            for (int ty = ty0; ty <= ty1 && ty < nty; ++ty)
                for (int tx = tx0; tx <= tx1 && tx < ntx; ++tx)
                    layer.tileCache->MarkDirty(tx, ty);
            layer.filtersDirty = false; // incremental path in compose
        } else {
            layer.filtersDirty = false;
        }
        m_CompositeDirty = true;

        CommitTransformation("Transform");
        m_ActiveLayerIdx = prevActive;

        if (layer.hasMask && m_FloatingMaskSRV && !layer.mask.empty()) {
            for (int y = destY0; y <= destY1; ++y) {
                for (int x = destX0; x <= destX1; ++x) {
                    float px = (float)x - cx - (float)m_FloatingOffsetX;
                    float py = (float)y - cy - (float)m_FloatingOffsetY;
                    float rx = px * invCos - py * invSin;
                    float ry = px * invSin + py * invCos;
                    float srcX = rx * invSx + cx;
                    float srcY = ry * invSy + cy;
                    float lx = srcX - (float)ox;
                    float ly = srcY - (float)oy;
                    if (lx < 0.f || ly < 0.f || lx > (float)(fw - 1) || ly > (float)(fh - 1))
                        continue;
                    float maskWeight = sampleBilinearMask(m_OriginalSelectionMask, fw, fh, lx, ly);
                    if (maskWeight > 0.0f)
                        layer.mask[(size_t)y * (size_t)m_Width + (size_t)x] = SelF2U8(maskWeight);
                }
            }
            layer.maskNeedsUpload = true;
        }

        // Move selection with content (undoable) — previously silent, so Ctrl+Z stack
        // desynced vs pixels and further redo/undo left inconsistent ants/clip.
        if (m_HasSelection && !m_OriginalSelectionMask.empty()) {
            std::vector<uint8_t> oldSel = m_SelectionMask;
            const bool oldHas = m_HasSelection;
            std::vector<uint8_t> shiftedSelection((size_t)m_Width * (size_t)m_Height, 0);
            for (int y = destY0; y <= destY1; ++y) {
                for (int x = destX0; x <= destX1; ++x) {
                    float px = (float)x - cx - (float)m_FloatingOffsetX;
                    float py = (float)y - cy - (float)m_FloatingOffsetY;
                    float rx = px * invCos - py * invSin;
                    float ry = px * invSin + py * invCos;
                    float srcX = rx * invSx + cx;
                    float srcY = ry * invSy + cy;
                    float lx = srcX - (float)ox;
                    float ly = srcY - (float)oy;
                    if (lx < 0.f || ly < 0.f || lx > (float)(fw - 1) || ly > (float)(fh - 1))
                        continue;
                    float maskWeight = sampleBilinearMask(m_OriginalSelectionMask, fw, fh, lx, ly);
                    shiftedSelection[(size_t)y * (size_t)m_Width + (size_t)x] = SelF2U8(maskWeight);
                }
            }
            m_SelectionMask = std::move(shiftedSelection);
            m_SelectionBoundsValid = false;
            RecomputeSelectionBoundsFromMask();
            m_SelectionGpuDirtyValid = false;
            m_SelectionMaskNeedsUpload = true;
            UpdateSelectionMaskTexture(device);
            m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
                "Move Selection", std::move(oldSel), oldHas, m_SelectionMask, m_HasSelection));
        }
    }

    if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
    if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }
    if (m_FloatingMaskTexture) { m_FloatingMaskTexture->Release(); m_FloatingMaskTexture = nullptr; }
    if (m_FloatingMaskSRV) { m_FloatingMaskSRV->Release(); m_FloatingMaskSRV = nullptr; }
    m_FloatingPixels.clear();
    m_OriginalSelectionMask.clear();
    m_FloatingBufW = m_FloatingBufH = 0;
    m_WarpMode = WarpOperatorMode::None;
    m_WarpControls.clear();
    m_WarpSourcePixels.clear();

    m_IsMovingPixels = false;
}

void Canvas::CancelMovePixels(ID3D11Device* device) {
    if (!m_IsMovingPixels) return;

    // Restore pre-move tile snapshots (no undo entry).
    {
        int prevActive = m_ActiveLayerIdx;
        if (m_StartActiveLayerIdx >= 0 && m_StartActiveLayerIdx < (int)m_Layers.size())
            m_ActiveLayerIdx = m_StartActiveLayerIdx;
        RestoreActiveLayerMutation();
        m_ActiveLayerIdx = prevActive;
    }
    
    if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
    if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }
    if (m_FloatingMaskTexture) { m_FloatingMaskTexture->Release(); m_FloatingMaskTexture = nullptr; }
    if (m_FloatingMaskSRV) { m_FloatingMaskSRV->Release(); m_FloatingMaskSRV = nullptr; }
    m_FloatingPixels.clear();
    m_OriginalSelectionMask.clear();
    m_FloatingBufW = m_FloatingBufH = 0;
    
    m_IsMovingPixels = false;
    m_WarpMode = WarpOperatorMode::None;
    m_WarpControls.clear();
    m_WarpSourcePixels.clear();
    m_CompositeDirty = true;
}

// ---------------------------------------------------------------------------
// Perspective / Mesh Warp operators
// ---------------------------------------------------------------------------

// Inverse bilinear: map point p into unit square given corners TL,TR,BR,BL.
static bool InvBilinear(float px, float py,
                        float ax, float ay, float bx, float by,
                        float cx, float cy, float dx, float dy,
                        float& u, float& v) {
    // e=B-A, f=D-A, g=A-B+C-D, h=P-A  (Eberly)
    const float eps = 1e-6f;
    float ex = bx - ax, ey = by - ay;
    float fx = dx - ax, fy = dy - ay;
    float gx = ax - bx + cx - dx, gy = ay - by + cy - dy;
    float hx = px - ax, hy = py - ay;

    float k2 = gx * fy - gy * fx;
    float k1 = ex * fy - ey * fx + hx * gy - hy * gx;
    float k0 = hx * ey - hy * ex;

    auto solveU = [&](float vv) -> float {
        float denx = ex + gx * vv;
        float deny = ey + gy * vv;
        if (std::fabs(denx) > std::fabs(deny))
            return (std::fabs(denx) > eps) ? (hx - fx * vv) / denx : 0.f;
        return (std::fabs(deny) > eps) ? (hy - fy * vv) / deny : 0.f;
    };

    if (std::fabs(k2) < 1e-8f) {
        if (std::fabs(k1) < eps) return false;
        v = -k0 / k1;
        u = solveU(v);
    } else {
        float disc = k1 * k1 - 4.f * k2 * k0;
        if (disc < 0.f) disc = 0.f;
        float sd = std::sqrt(disc);
        float v1 = (-k1 - sd) / (2.f * k2);
        float v2 = (-k1 + sd) / (2.f * k2);
        bool v1ok = (v1 >= -0.05f && v1 <= 1.05f);
        bool v2ok = (v2 >= -0.05f && v2 <= 1.05f);
        if (v1ok && !v2ok) v = v1;
        else if (v2ok && !v1ok) v = v2;
        else if (v1ok && v2ok) v = (std::fabs(v1 - 0.5f) < std::fabs(v2 - 0.5f)) ? v1 : v2;
        else v = v1;
        u = solveU(v);
    }
    return true;
}

static float SampleBilinearFloat(const std::vector<float>& buf, int w, int h, float x, float y, int c) {
    if (buf.empty() || w < 1 || h < 1) return 0.f;
    x = std::clamp(x, 0.f, (float)(w - 1));
    y = std::clamp(y, 0.f, (float)(h - 1));
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    float tx = x - x0, ty = y - y0;
    auto at = [&](int ix, int iy) { return buf[((size_t)iy * w + ix) * 4 + c]; };
    float a = at(x0, y0), b = at(x1, y0), d = at(x0, y1), e = at(x1, y1);
    return a * (1 - tx) * (1 - ty) + b * tx * (1 - ty) + d * (1 - tx) * ty + e * tx * ty;
}

void Canvas::StartWarpOperator(ID3D11Device* device, WarpOperatorMode mode) {
    if (mode == WarpOperatorMode::None) return;
    if (m_IsMovingPixels)
        CancelMovePixels(device);
    StartMovePixels(device);
    if (!m_IsMovingPixels) return;

    m_WarpMode = mode;
    m_WarpSourcePixels = m_FloatingPixels;
    m_WarpBBoxX = m_FloatingBBoxX;
    m_WarpBBoxY = m_FloatingBBoxY;
    m_WarpBBoxW = m_FloatingBBoxW;
    m_WarpBBoxH = m_FloatingBBoxH;
    if (m_WarpBBoxW < 2) m_WarpBBoxW = 2;
    if (m_WarpBBoxH < 2) m_WarpBBoxH = 2;

    float x0 = (float)m_WarpBBoxX, y0 = (float)m_WarpBBoxY;
    float x1 = (float)(m_WarpBBoxX + m_WarpBBoxW - 1);
    float y1 = (float)(m_WarpBBoxY + m_WarpBBoxH - 1);
    m_WarpSourceCorners = { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} }; // TL TR BR BL

    m_WarpControls.clear();
    if (mode == WarpOperatorMode::Perspective) {
        m_WarpControls = m_WarpSourceCorners;
    } else {
        m_WarpMeshN = 4;
        const int n = m_WarpMeshN;
        m_WarpControls.resize((size_t)n * n);
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                float u = (n == 1) ? 0.f : (float)i / (float)(n - 1);
                float v = (n == 1) ? 0.f : (float)j / (float)(n - 1);
                m_WarpControls[(size_t)j * n + i] = {
                    x0 + u * (x1 - x0),
                    y0 + v * (y1 - y0)
                };
            }
        }
    }
    // Identity offset while warping — geometry is in control points
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_FloatingScaleX = 1.f;
    m_FloatingScaleY = 1.f;
    m_FloatingRotation = 0.f;
    PreviewWarpOperator(device);
}

void Canvas::SetWarpControlPoint(int index, float canvasX, float canvasY) {
    if (index < 0 || index >= (int)m_WarpControls.size()) return;
    m_WarpControls[index] = { canvasX, canvasY };
}

int Canvas::HitTestWarpControl(float canvasX, float canvasY, float threshPx) const {
    float best = threshPx * threshPx;
    int hit = -1;
    for (int i = 0; i < (int)m_WarpControls.size(); ++i) {
        float dx = m_WarpControls[i].first - canvasX;
        float dy = m_WarpControls[i].second - canvasY;
        float d2 = dx * dx + dy * dy;
        if (d2 <= best) { best = d2; hit = i; }
    }
    return hit;
}

bool Canvas::GetWarpControl(int index, float& outX, float& outY) const {
    if (index < 0 || index >= (int)m_WarpControls.size()) return false;
    outX = m_WarpControls[index].first;
    outY = m_WarpControls[index].second;
    return true;
}

void Canvas::RebuildWarpPreviewTexture(ID3D11Device* device) {
    if (!device || m_WarpMode == WarpOperatorMode::None || m_WarpSourcePixels.empty()) return;
    const int srcW = m_WarpBBoxW;
    const int srcH = m_WarpBBoxH;
    if (srcW < 1 || srcH < 1) return;
    if ((int)m_WarpSourcePixels.size() < srcW * srcH * 4) return;

    // Dest hull from control points → tight floating buffer (not full canvas)
    float minXf = 1e9f, minYf = 1e9f, maxXf = -1e9f, maxYf = -1e9f;
    for (auto& p : m_WarpControls) {
        minXf = std::min(minXf, p.first); maxXf = std::max(maxXf, p.first);
        minYf = std::min(minYf, p.second); maxYf = std::max(maxYf, p.second);
    }
    int x0 = std::max(0, (int)std::floor(minXf) - 1);
    int y0 = std::max(0, (int)std::floor(minYf) - 1);
    int x1 = std::min(m_Width - 1, (int)std::ceil(maxXf) + 1);
    int y1 = std::min(m_Height - 1, (int)std::ceil(maxYf) + 1);
    if (x1 < x0 || y1 < y0) return;

    const int outW = x1 - x0 + 1;
    const int outH = y1 - y0 + 1;
    m_FloatingBBoxX = x0;
    m_FloatingBBoxY = y0;
    m_FloatingBBoxW = outW;
    m_FloatingBBoxH = outH;
    m_FloatingBufW = outW;
    m_FloatingBufH = outH;
    m_FloatingCenterX = (x0 + x1) * 0.5f;
    m_FloatingCenterY = (y0 + y1) * 0.5f;
    m_FloatingPixels.assign((size_t)outW * (size_t)outH * 4, 0.f);

    float sx0 = m_WarpSourceCorners[0].first, sy0 = m_WarpSourceCorners[0].second;
    float sx1c = m_WarpSourceCorners[1].first, sy1c = m_WarpSourceCorners[2].second;
    float srcSpanW = std::max(1.f, sx1c - sx0), srcSpanH = std::max(1.f, sy1c - sy0);

    auto writeSample = [&](int x, int y, float sx, float sy) {
        // Source floating buffer is local to original warp bbox
        float lx = sx - (float)m_WarpBBoxX;
        float ly = sy - (float)m_WarpBBoxY;
        int dx = x - x0, dy = y - y0;
        if (dx < 0 || dy < 0 || dx >= outW || dy >= outH) return;
        size_t di = ((size_t)dy * outW + dx) * 4;
        for (int c = 0; c < 4; ++c)
            m_FloatingPixels[di + c] = SampleBilinearFloat(m_WarpSourcePixels, srcW, srcH, lx, ly, c);
    };

    if (m_WarpMode == WarpOperatorMode::Perspective && m_WarpControls.size() >= 4) {
        auto A = m_WarpControls[0], B = m_WarpControls[1], C = m_WarpControls[2], D = m_WarpControls[3];
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                float u, v;
                if (!InvBilinear((float)x, (float)y,
                                 A.first, A.second, B.first, B.second,
                                 C.first, C.second, D.first, D.second, u, v))
                    continue;
                if (u < -0.01f || u > 1.01f || v < -0.01f || v > 1.01f) continue;
                u = std::clamp(u, 0.f, 1.f);
                v = std::clamp(v, 0.f, 1.f);
                writeSample(x, y, sx0 + u * srcSpanW, sy0 + v * srcSpanH);
            }
        }
    } else if (m_WarpMode == WarpOperatorMode::Mesh && m_WarpMeshN >= 2) {
        const int n = m_WarpMeshN;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                bool found = false;
                float su = 0, sv = 0;
                for (int j = 0; j < n - 1 && !found; ++j) {
                    for (int i = 0; i < n - 1 && !found; ++i) {
                        auto TL = m_WarpControls[(size_t)j * n + i];
                        auto TR = m_WarpControls[(size_t)j * n + i + 1];
                        auto BR = m_WarpControls[(size_t)(j + 1) * n + i + 1];
                        auto BL = m_WarpControls[(size_t)(j + 1) * n + i];
                        float u, v;
                        if (!InvBilinear((float)x, (float)y,
                                         TL.first, TL.second, TR.first, TR.second,
                                         BR.first, BR.second, BL.first, BL.second, u, v))
                            continue;
                        if (u < -0.02f || u > 1.02f || v < -0.02f || v > 1.02f) continue;
                        u = std::clamp(u, 0.f, 1.f);
                        v = std::clamp(v, 0.f, 1.f);
                        float gu0 = (float)i / (float)(n - 1);
                        float gv0 = (float)j / (float)(n - 1);
                        float gu1 = (float)(i + 1) / (float)(n - 1);
                        float gv1 = (float)(j + 1) / (float)(n - 1);
                        su = gu0 + u * (gu1 - gu0);
                        sv = gv0 + v * (gv1 - gv0);
                        found = true;
                    }
                }
                if (!found) continue;
                writeSample(x, y, sx0 + su * srcSpanW, sy0 + sv * srcSpanH);
            }
        }
    }

    // Upload tight floating texture
    if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
    if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)outW; desc.Height = (UINT)outH; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = m_FloatingPixels.data();
    init.SysMemPitch = (UINT)(outW * 4 * sizeof(float));
    if (SUCCEEDED(device->CreateTexture2D(&desc, &init, &m_FloatingTexture)) && m_FloatingTexture)
        device->CreateShaderResourceView(m_FloatingTexture, nullptr, &m_FloatingSRV);
    m_CompositeDirty = true;
}

void Canvas::PreviewWarpOperator(ID3D11Device* device) {
    RebuildWarpPreviewTexture(device);
}

void Canvas::CommitWarpOperator(ID3D11Device* device) {
    if (m_WarpMode == WarpOperatorMode::None) return;
    PreviewWarpOperator(device);
    // Floating buffer holds warped result — commit via existing move path
    m_WarpMode = WarpOperatorMode::None;
    m_WarpControls.clear();
    m_WarpSourcePixels.clear();
    CommitMovePixels(device);
}

void Canvas::CancelWarpOperator(ID3D11Device* device) {
    if (m_WarpMode == WarpOperatorMode::None) {
        if (m_IsMovingPixels) CancelMovePixels(device);
        return;
    }
    m_WarpMode = WarpOperatorMode::None;
    m_WarpControls.clear();
    m_WarpSourcePixels.clear();
    CancelMovePixels(device);
}

void Canvas::DrawWarpGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen) {
    if (m_WarpMode == WarpOperatorMode::None || m_WarpControls.empty()) return;
    ImU32 lineCol = IM_COL32(0, 200, 255, 220);
    ImU32 handleCol = IM_COL32(255, 255, 255, 240);
    ImU32 handleOut = IM_COL32(0, 120, 200, 255);

    if (m_WarpMode == WarpOperatorMode::Perspective && m_WarpControls.size() >= 4) {
        ImVec2 p[4];
        for (int i = 0; i < 4; ++i)
            p[i] = canvasToScreen(m_WarpControls[i].first, m_WarpControls[i].second);
        for (int i = 0; i < 4; ++i)
            dl->AddLine(p[i], p[(i + 1) % 4], lineCol, 2.f);
        for (int i = 0; i < 4; ++i) {
            dl->AddCircleFilled(p[i], 5.f, handleCol);
            dl->AddCircle(p[i], 5.f, handleOut, 0, 1.5f);
        }
    } else if (m_WarpMode == WarpOperatorMode::Mesh) {
        const int n = m_WarpMeshN;
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                ImVec2 p = canvasToScreen(m_WarpControls[(size_t)j * n + i].first,
                                          m_WarpControls[(size_t)j * n + i].second);
                if (i + 1 < n) {
                    ImVec2 p2 = canvasToScreen(m_WarpControls[(size_t)j * n + i + 1].first,
                                               m_WarpControls[(size_t)j * n + i + 1].second);
                    dl->AddLine(p, p2, lineCol, 1.5f);
                }
                if (j + 1 < n) {
                    ImVec2 p2 = canvasToScreen(m_WarpControls[(size_t)(j + 1) * n + i].first,
                                               m_WarpControls[(size_t)(j + 1) * n + i].second);
                    dl->AddLine(p, p2, lineCol, 1.5f);
                }
                dl->AddCircleFilled(p, 4.f, handleCol);
                dl->AddCircle(p, 4.f, handleOut, 0, 1.2f);
            }
        }
    }
}

void Canvas::DrawMoveGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen,
                           bool showHandles) {
    if (!m_IsMovingPixels) return;
    if (m_WarpMode != WarpOperatorMode::None) {
        DrawWarpGizmo(dl, canvasToScreen);
        return;
    }
    
    // Use cached floating bbox (local buffer) — never scan full canvas for gizmo
    if (m_FloatingBBoxW <= 0 || m_FloatingBBoxH <= 0) return;
    int minX = m_FloatingBBoxX;
    int minY = m_FloatingBBoxY;
    int maxX = m_FloatingBBoxX + m_FloatingBBoxW - 1;
    int maxY = m_FloatingBBoxY + m_FloatingBBoxH - 1;

    {
        float cx = m_FloatingCenterX;
        float cy = m_FloatingCenterY;
        float cosA = std::cos(m_FloatingRotation);
        float sinA = std::sin(m_FloatingRotation);
        
        // Forward transform: scale then rotate around bounding box center, then translate
        auto transformCorner = [&](float px, float py) -> ImVec2 {
            float rx = (px - cx) * m_FloatingScaleX;
            float ry = (py - cy) * m_FloatingScaleY;
            float tx = rx * cosA - ry * sinA + cx + (float)m_FloatingOffsetX;
            float ty = rx * sinA + ry * cosA + cy + (float)m_FloatingOffsetY;
            return canvasToScreen(tx, ty);
        };
        
        ImVec2 p1 = transformCorner((float)minX, (float)minY); // TL
        ImVec2 p2 = transformCorner((float)maxX, (float)minY); // TR
        ImVec2 p3 = transformCorner((float)maxX, (float)maxY); // BR
        ImVec2 p4 = transformCorner((float)minX, (float)maxY); // BL
        
        // Midpoints for edge handles
        ImVec2 mT = { (p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f };
        ImVec2 mR = { (p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f };
        ImVec2 mB = { (p3.x + p4.x) * 0.5f, (p3.y + p4.y) * 0.5f };
        ImVec2 mL = { (p4.x + p1.x) * 0.5f, (p4.y + p1.y) * 0.5f };
        
        ImU32 gizmoCol  = IM_COL32(0, 120, 215, 255);
        ImU32 shadowCol = IM_COL32(0, 0, 0, 120);
        float thickness = 2.0f;

        // Shadow
        dl->AddLine(ImVec2(p1.x+1,p1.y+1), ImVec2(p2.x+1,p2.y+1), shadowCol, thickness);
        dl->AddLine(ImVec2(p2.x+1,p2.y+1), ImVec2(p3.x+1,p3.y+1), shadowCol, thickness);
        dl->AddLine(ImVec2(p3.x+1,p3.y+1), ImVec2(p4.x+1,p4.y+1), shadowCol, thickness);
        dl->AddLine(ImVec2(p4.x+1,p4.y+1), ImVec2(p1.x+1,p1.y+1), shadowCol, thickness);
        // Border
        dl->AddLine(p1, p2, gizmoCol, thickness);
        dl->AddLine(p2, p3, gizmoCol, thickness);
        dl->AddLine(p3, p4, gizmoCol, thickness);
        dl->AddLine(p4, p1, gizmoCol, thickness);
        
        if (showHandles) {
            float hs = 5.0f;
            auto drawHandle = [&](ImVec2 p) {
                dl->AddRectFilled(ImVec2(p.x - hs, p.y - hs), ImVec2(p.x + hs, p.y + hs), IM_COL32(255, 255, 255, 230));
                dl->AddRect(ImVec2(p.x - hs, p.y - hs), ImVec2(p.x + hs, p.y + hs), gizmoCol, 0.0f, 0, 1.5f);
            };
            auto drawEdgeHandle = [&](ImVec2 p) {
                float hs2 = 4.0f;
                dl->AddRectFilled(ImVec2(p.x - hs2, p.y - hs2), ImVec2(p.x + hs2, p.y + hs2), IM_COL32(200, 220, 255, 200));
                dl->AddRect(ImVec2(p.x - hs2, p.y - hs2), ImVec2(p.x + hs2, p.y + hs2), gizmoCol, 0.0f, 0, 1.0f);
            };
            drawHandle(p1); drawHandle(p2); drawHandle(p3); drawHandle(p4);
            drawEdgeHandle(mT); drawEdgeHandle(mR); drawEdgeHandle(mB); drawEdgeHandle(mL);
        }
    }
}

// ============================================================
//  Helpers (color/blur shared via layer_fx — no local duplicates)
// ============================================================
#include <random>

// Monotone cubic spline LUT — called from UI curves editor
std::vector<float> Canvas_BuildSplineLUT(const std::vector<std::pair<float,float>>& pts) {
    std::vector<float> lut(256);
    if (pts.size() < 2) { for(int i=0;i<256;++i) lut[i]=(float)i/255.f; return lut; }
    int n=(int)pts.size();
    std::vector<float> d(n-1),m2(n,0.f);
    for (int i=0;i<n-1;++i) d[i]=(pts[i+1].second-pts[i].second)/(pts[i+1].first-pts[i].first+1e-9f);
    m2[0]=d[0];
    for (int i=1;i<n-1;++i) m2[i]=(d[i-1]+d[i])*0.5f;
    m2[n-1]=d[n-2];
    for (int i=0;i<n-1;++i) {
        if (fabsf(d[i])<1e-9f){m2[i]=m2[i+1]=0.f;continue;}
        float a=m2[i]/d[i], b=m2[i+1]/d[i];
        float ab2=a*a+b*b;
        if (ab2>9.f){float s2=3.f/sqrtf(ab2);m2[i]=s2*a*d[i];m2[i+1]=s2*b*d[i];}
    }
    for (int xi=0;xi<256;++xi) {
        float t=(float)xi/255.f;
        t=std::clamp(t,pts.front().first,pts.back().first);
        int seg=0; for(int i=0;i<n-2;++i) if(t>=pts[i+1].first) seg=i+1;
        float hh=(pts[seg+1].first-pts[seg].first);
        float u=(t-pts[seg].first)/(hh+1e-9f);
        float u2=u*u,u3=u2*u;
        float vv=(2*u3-3*u2+1)*pts[seg].second+(u3-2*u2+u)*hh*m2[seg]
                +(-2*u3+3*u2)*pts[seg+1].second+(u3-u2)*hh*m2[seg+1];
        lut[xi]=std::clamp(vv,0.f,1.f);
    }
    return lut;
}

// ============================================================
//  Destructive Operations
// ============================================================

void Canvas::SelectAll() {
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHas = m_HasSelection;
    m_SelectionMask.assign((size_t)m_Width * (size_t)m_Height, 255);
    m_HasSelection = true;
    m_SelectionBoundsX = 0;
    m_SelectionBoundsY = 0;
    m_SelectionBoundsW = m_Width;
    m_SelectionBoundsH = m_Height;
    m_SelectionBoundsValid = true;
    m_SelectionGpuDirtyValid = false; // full upload
    m_SelectionMaskNeedsUpload = true;
    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Select All", std::move(oldMask), oldHas, m_SelectionMask, m_HasSelection));
    Logger::Get().Info("SelectAll");
}

void Canvas::SelectOpaquePixels(int layerIdx) {
    if (layerIdx < 0) layerIdx = m_ActiveLayerIdx;
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[layerIdx];
    if (layer.isGroup || !layer.tileCache) return;

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHas = m_HasSelection;
    m_SelectionMask.assign((size_t)m_Width * m_Height, 0);
    m_HasSelection = false;

    // Walk tiles: alpha > 0 → selected
    const int ntx = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    const int nty = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    for (int ty = 0; ty < nty; ++ty) {
        for (int tx = 0; tx < ntx; ++tx) {
            if (!layer.tileCache->HasTile(tx, ty)) continue;
            const int x0 = tx * TILE_SIZE;
            const int y0 = ty * TILE_SIZE;
            const int x1 = std::min(x0 + TILE_SIZE, m_Width);
            const int y1 = std::min(y0 + TILE_SIZE, m_Height);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    float rgba[4];
                    layer.tileCache->GetPixelF(x, y, rgba);
                    if (rgba[3] > 0.001f) {
                        m_SelectionMask[(size_t)y * m_Width + x] = 255;
                        m_HasSelection = true;
                    }
                }
            }
        }
    }
    m_SelectionBoundsValid = false;
    RecomputeSelectionBoundsFromMask();
    m_SelectionGpuDirtyValid = false;
    m_SelectionMaskNeedsUpload = true;
    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Select Layer Pixels", std::move(oldMask), oldHas, m_SelectionMask, m_HasSelection));
    Logger::Get().Info("SelectOpaquePixels layer=" + std::to_string(layerIdx));
}

void Canvas::SelectFromLayerMask(int layerIdx) {
    if (layerIdx < 0) layerIdx = m_ActiveLayerIdx;
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[layerIdx];
    if (layer.isGroup || !layer.hasMask || layer.mask.empty()) return;

    const size_t need = (size_t)m_Width * (size_t)m_Height;
    if (layer.mask.size() != need) {
        Logger::Get().Warn("SelectFromLayerMask: mask size mismatch");
        return;
    }

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHas = m_HasSelection;
    // White / high mask value = selected (PS: load selection from mask).
    m_SelectionMask = layer.mask;
    m_SelectionBoundsValid = false;
    RecomputeSelectionBoundsFromMask();
    m_SelectionGpuDirtyValid = false;
    m_SelectionMaskNeedsUpload = true;
    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Select From Mask", std::move(oldMask), oldHas, m_SelectionMask, m_HasSelection));
    Logger::Get().Info("SelectFromLayerMask layer=" + std::to_string(layerIdx));
}

void Canvas::InvertSelection() {
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHas = m_HasSelection;
    if (!m_HasSelection) {
        m_SelectionMask.assign((size_t)m_Width * (size_t)m_Height, 255);
        m_HasSelection = true;
        m_SelectionBoundsX = 0; m_SelectionBoundsY = 0;
        m_SelectionBoundsW = m_Width; m_SelectionBoundsH = m_Height;
        m_SelectionBoundsValid = true;
    } else {
        if (m_SelectionMask.empty() ||
            m_SelectionMask.size() != (size_t)m_Width * (size_t)m_Height)
            m_SelectionMask.assign((size_t)m_Width * (size_t)m_Height, 0);
        for (auto& v : m_SelectionMask) v = (uint8_t)(255 - v);
        m_SelectionBoundsValid = false;
        RecomputeSelectionBoundsFromMask();
    }
    m_SelectionGpuDirtyValid = false;
    m_SelectionMaskNeedsUpload = true;
    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Invert Selection", std::move(oldMask), oldHas, m_SelectionMask, m_HasSelection));
    Logger::Get().Info("InvertSelection");
}

void Canvas::InvertAlpha() {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<0.5f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        pixels[idx+3]=1.f-pixels[idx+3];
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Invert Alpha");
    Logger::Get().Info("InvertAlpha");
}

void Canvas::InvertColors() {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<0.5f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        // Invert RGB; leave alpha unchanged
        pixels[idx+0]=1.f-pixels[idx+0];
        pixels[idx+1]=1.f-pixels[idx+1];
        pixels[idx+2]=1.f-pixels[idx+2];
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Invert Colors");
    Logger::Get().Info("InvertColors");
}

// Shared buffer ops for Apply* + live adjust preview (active layer + selection).
static void BufferApplyBlur(std::vector<float>& pixels, int w, int h, float radius,
                            const std::vector<uint8_t>& selMask, bool hasSel) {
    int r = std::max(1, (int)radius);
    // Premul path (same as layer_fx filter blur) — no white fringe on transparency
    std::vector<float> blurred = pixels;
    const int nPx = w * h;
    for (int i = 0; i < nPx; ++i) {
        size_t idx = (size_t)i * 4;
        float a = std::clamp(blurred[idx + 3], 0.f, 1.f);
        blurred[idx + 0] *= a;
        blurred[idx + 1] *= a;
        blurred[idx + 2] *= a;
        blurred[idx + 3] = a;
    }
    layer_fx::BoxBlur(blurred, w, h, r, 4, 3);
    for (int i = 0; i < nPx; ++i) {
        size_t idx = (size_t)i * 4;
        float a = std::clamp(blurred[idx + 3], 0.f, 1.f);
        if (a > 1e-6f) {
            float inv = 1.f / a;
            blurred[idx + 0] = std::clamp(blurred[idx + 0] * inv, 0.f, 1.f);
            blurred[idx + 1] = std::clamp(blurred[idx + 1] * inv, 0.f, 1.f);
            blurred[idx + 2] = std::clamp(blurred[idx + 2] * inv, 0.f, 1.f);
        } else {
            blurred[idx + 0] = blurred[idx + 1] = blurred[idx + 2] = 0.f;
        }
        blurred[idx + 3] = a;
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sel = GetSelWeight(selMask, w, x, y, hasSel);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * w + x) * 4;
            for (int c = 0; c < 4; ++c)
                pixels[idx + c] = pixels[idx + c] * (1.f - sel) + blurred[idx + c] * sel;
        }
}

static void BufferApplyHSV(std::vector<float>& pixels, int w, int h, float dH, float dS, float dV,
                           const std::vector<uint8_t>& selMask, bool hasSel) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sel = GetSelWeight(selMask, w, x, y, hasSel);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * w + x) * 4;
            float rr = pixels[idx], gg = pixels[idx + 1], bb = pixels[idx + 2];
            float hv, s, v;
            layer_fx::RGBtoHSV(rr, gg, bb, hv, s, v);
            hv = fmodf(hv + dH + 1.f, 1.f);
            s = std::clamp(s + dS, 0.f, 1.f);
            v = std::clamp(v + dV, 0.f, 1.f);
            float nr, ng, nb;
            layer_fx::HSVtoRGB(hv, s, v, nr, ng, nb);
            pixels[idx]     = pixels[idx]     * (1.f - sel) + nr * sel;
            pixels[idx + 1] = pixels[idx + 1] * (1.f - sel) + ng * sel;
            pixels[idx + 2] = pixels[idx + 2] * (1.f - sel) + nb * sel;
        }
}

static void BufferApplyCurves(std::vector<float>& pixels, int w, int h,
                              const std::vector<float>& lutRGB, const std::vector<float>& lutAlpha,
                              const std::vector<uint8_t>& selMask, bool hasSel) {
    auto sample = [&](const std::vector<float>& lut, float v) -> float {
        float fi = v * 255.f;
        int i = std::clamp((int)fi, 0, 254);
        float t = fi - i;
        return lut[i] * (1.f - t) + lut[i + 1] * t;
    };
    const bool hasA = (int)lutAlpha.size() >= 256;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sel = GetSelWeight(selMask, w, x, y, hasSel);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * w + x) * 4;
            for (int c = 0; c < 3; ++c)
                pixels[idx + c] = pixels[idx + c] * (1.f - sel) + sample(lutRGB, pixels[idx + c]) * sel;
            if (hasA)
                pixels[idx + 3] = pixels[idx + 3] * (1.f - sel) + sample(lutAlpha, pixels[idx + 3]) * sel;
        }
}

static void BufferApplyNoise(std::vector<float>& pixels, int w, int h, float strength, bool colorNoise,
                             uint32_t seed, const std::vector<uint8_t>& selMask, bool hasSel) {
    std::mt19937 rng(seed ? seed : 1u);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sel = GetSelWeight(selMask, w, x, y, hasSel);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * w + x) * 4;
            if (colorNoise) {
                for (int c = 0; c < 3; ++c)
                    pixels[idx + c] = std::clamp(pixels[idx + c] + dist(rng) * strength * sel, 0.f, 1.f);
            } else {
                float n = dist(rng) * strength * sel;
                for (int c = 0; c < 3; ++c)
                    pixels[idx + c] = std::clamp(pixels[idx + c] + n, 0.f, 1.f);
            }
        }
}

bool Canvas::BeginAdjustPreview() {
    if (m_AdjustPreviewActive) CancelAdjustPreview();
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return false;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup || layer.IsFill()) return false;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles(); // undo base for Commit
    m_AdjustPreviewBase = ExportLayerF(layer, m_Width, m_Height);
    m_AdjustPreviewLayerIdx = m_ActiveLayerIdx;
    m_AdjustPreviewActive = true;
    m_AdjustPreviewNoiseSeed = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() | 1u;
    return true;
}

void Canvas::CancelAdjustPreview() {
    if (!m_AdjustPreviewActive) return;
    if (m_AdjustPreviewLayerIdx >= 0 && m_AdjustPreviewLayerIdx < (int)m_Layers.size() &&
        m_AdjustPreviewLayerIdx == m_ActiveLayerIdx) {
        RestoreActiveLayerMutation();
    } else {
        m_ActiveStrokeDeltas.clear();
    }
    m_AdjustPreviewActive = false;
    m_AdjustPreviewLayerIdx = -1;
    m_AdjustPreviewBase.clear();
    m_CompositeDirty = true;
}

void Canvas::CommitAdjustPreview(const std::string& actionName) {
    if (!m_AdjustPreviewActive) return;
    // Current pixels are already the preview result; backup still holds pre-adjust tiles.
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
        auto& layer = m_Layers[m_ActiveLayerIdx];
        layer.filtersDirty = true;
        layer.thumbDirty = true;
        if (layer.HasEnabledStyles()) {
            layer.presentationDirty = true;
            layer.stylesDirty = true;
        }
    }
    CommitActiveLayerMutation(actionName);
    m_AdjustPreviewActive = false;
    m_AdjustPreviewLayerIdx = -1;
    m_AdjustPreviewBase.clear();
    m_CompositeDirty = true;
    m_ChannelPreviewDirty = true;
    Logger::Get().Info(actionName + " (commit preview)");
}

void Canvas::UpdateAdjustPreviewHSV(float dH, float dS, float dV) {
    if (!m_AdjustPreviewActive || m_ActiveLayerIdx != m_AdjustPreviewLayerIdx) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    auto pixels = m_AdjustPreviewBase;
    BufferApplyHSV(pixels, m_Width, m_Height, dH, dS, dV, m_SelectionMask, m_HasSelection);
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat, /*markFiltersDirty=*/false);
    m_CompositeDirty = true;
}

void Canvas::UpdateAdjustPreviewCurves(const std::vector<float>& lutRGB, const std::vector<float>& lutAlpha) {
    if (!m_AdjustPreviewActive || m_ActiveLayerIdx != m_AdjustPreviewLayerIdx) return;
    if ((int)lutRGB.size() < 256) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    auto pixels = m_AdjustPreviewBase;
    BufferApplyCurves(pixels, m_Width, m_Height, lutRGB, lutAlpha, m_SelectionMask, m_HasSelection);
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat, /*markFiltersDirty=*/false);
    m_CompositeDirty = true;
}

void Canvas::UpdateAdjustPreviewBlur(float radius) {
    if (!m_AdjustPreviewActive || m_ActiveLayerIdx != m_AdjustPreviewLayerIdx) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    auto pixels = m_AdjustPreviewBase;
    BufferApplyBlur(pixels, m_Width, m_Height, radius, m_SelectionMask, m_HasSelection);
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat, /*markFiltersDirty=*/false);
    m_CompositeDirty = true;
}

void Canvas::UpdateAdjustPreviewNoise(float strength, bool colorNoise) {
    if (!m_AdjustPreviewActive || m_ActiveLayerIdx != m_AdjustPreviewLayerIdx) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    auto pixels = m_AdjustPreviewBase;
    BufferApplyNoise(pixels, m_Width, m_Height, strength, colorNoise, m_AdjustPreviewNoiseSeed,
                     m_SelectionMask, m_HasSelection);
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat, /*markFiltersDirty=*/false);
    m_CompositeDirty = true;
}

void Canvas::ApplyBlur(float radius) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    BufferApplyBlur(pixels, m_Width, m_Height, radius, m_SelectionMask, m_HasSelection);
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Blur");
    Logger::Get().Info("ApplyBlur r=" + std::to_string(std::max(1, (int)radius)));
}

void Canvas::ApplyHSV(float dH, float dS, float dV) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    BufferApplyHSV(pixels, m_Width, m_Height, dH, dS, dV, m_SelectionMask, m_HasSelection);
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("HSV");
    Logger::Get().Info("ApplyHSV");
}

void Canvas::ApplyCurves(const std::vector<float>& lutRGB, const std::vector<float>& lutAlpha) {
    if ((int)lutRGB.size() < 256 || m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    BufferApplyCurves(pixels, m_Width, m_Height, lutRGB, lutAlpha, m_SelectionMask, m_HasSelection);
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Curves");
    Logger::Get().Info("ApplyCurves");
}

void Canvas::ApplyNoise(float strength, bool colorNoise) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    uint32_t seed = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() | 1u;
    BufferApplyNoise(pixels, m_Width, m_Height, strength, colorNoise, seed, m_SelectionMask, m_HasSelection);
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Noise");
    Logger::Get().Info("ApplyNoise");
}

// ============================================================
//  Clone Stamp
// ============================================================

void Canvas::StampSetSource(float canvasX, float canvasY) {
    m_StampSrcX = canvasX;
    m_StampSrcY = canvasY;
    m_StampHasSource = true;
    m_StampHasOffset = false; // new source resets offset until first dab
    Logger::Get().InfoTag("stamp",
        "Source set at " + std::to_string((int)canvasX) + "," + std::to_string((int)canvasY));
}

void Canvas::StampClearSource() {
    m_StampHasSource = false;
    m_StampHasOffset = false;
}

void Canvas::StampLockOffsetFromDab(float dabX, float dabY) {
    if (!m_StampHasSource || m_StampHasOffset) return;
    // Sample at (dest - offset) == source when dest == first dab
    m_StampOffsetX = dabX - m_StampSrcX;
    m_StampOffsetY = dabY - m_StampSrcY;
    m_StampHasOffset = true;
    Logger::Get().InfoTag("stamp",
        "Offset locked dx=" + std::to_string(m_StampOffsetX) +
        " dy=" + std::to_string(m_StampOffsetY));
}

bool Canvas::ApplyContentAwareFill(ID3D11Device* /*device*/) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return false;
    if (!m_HasSelection || m_SelectionMask.empty()) {
        Logger::Get().Warn("Content-Aware Fill requires an active selection (hole region).");
        return false;
    }
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.CanPaintContent()) {
        Logger::Get().Warn("Content-Aware Fill: active layer is not paintable.");
        return false;
    }
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);

    caf::CafImage img;
    img.w = m_Width;
    img.h = m_Height;
    img.rgba = ExportLayerF(layer, m_Width, m_Height);

    caf::CafMask hole;
    hole.w = m_Width;
    hole.h = m_Height;
    hole.hole = m_SelectionMask; // non-zero = synthesize

    caf::CafParams params;
    params.patchSize = 7;
    params.multiScaleLevels = 4;
    params.searchIters = 5;

    caf::CafResult res = caf::ContentAwareFill(img, hole, params);
    if (!res.ok) {
        Logger::Get().Error("Content-Aware Fill: " + res.error);
        return false;
    }
    if ((int)res.filled.rgba.size() != m_Width * m_Height * 4) {
        Logger::Get().Error("Content-Aware Fill: bad output size");
        return false;
    }

    BackupAllActiveLayerTiles();
    SetLayerPixelsF(layer, res.filled.rgba, m_Width, m_Height, m_CanvasFormat, true);
    CommitActiveLayerMutation("Content-Aware Fill");
    m_CompositeDirty = true;
    m_ChannelPreviewDirty = true;
    return true;
}

// ============================================================
//  Smudge Tool — finger smear (push pixels along stroke)
// ============================================================

void Canvas::SmudgeOnActiveLayer(float x, float y, StrokePhase phase, const SmudgeSettings& s) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.CanPaintContent()) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);

    const int r = std::max(1, (int)std::lround(s.radius));
    const int diam = r * 2 + 1;
    const float strength = std::clamp(s.strength, 0.f, 1.f);

    auto backupBrushTiles = [&](float cx, float cy) {
        int numTilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
        int numTilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
        int minTX = std::max(0, (int)(cx - r) / TILE_SIZE);
        int maxTX = std::min(numTilesX - 1, (int)(cx + r) / TILE_SIZE);
        int minTY = std::max(0, (int)(cy - r) / TILE_SIZE);
        int maxTY = std::min(numTilesY - 1, (int)(cy + r) / TILE_SIZE);
        for (int ty = minTY; ty <= maxTY; ++ty)
            for (int tx = minTX; tx <= maxTX; ++tx)
                BackupTile(tx, ty);
    };

    if (phase == StrokePhase::Begin) {
        m_IsStrokeActive = true;
        m_ActiveStrokeDeltas.clear();
        m_SmudgeLastX = x; m_SmudgeLastY = y; m_SmudgeDistAcc = 0.f;
        m_SmudgePickupValid = true;
        // Seed finger buffer from local neighborhood at press
        m_SmudgeFinger.assign((size_t)diam * diam * 4, 0.f);
        for (int ky = -r; ky <= r; ++ky) {
            for (int kx = -r; kx <= r; ++kx) {
                if (kx * kx + ky * ky > r * r) continue;
                int px = std::clamp((int)std::lround(x) + kx, 0, m_Width - 1);
                int py = std::clamp((int)std::lround(y) + ky, 0, m_Height - 1);
                float rgba[4];
                layer.tileCache->GetPixelF(px, py, rgba);
                size_t fi = ((size_t)(ky + r) * diam + (kx + r)) * 4;
                for (int c = 0; c < 4; ++c) m_SmudgeFinger[fi + c] = rgba[c];
            }
        }
        backupBrushTiles(x, y);
        return;
    }

    if (phase == StrokePhase::End) {
        m_SmudgePickupValid = false;
        m_IsStrokeActive = false;
        if (!m_ActiveStrokeDeltas.empty()) {
            auto deltas = SealActiveStrokeDeltas(m_ActiveStrokeDeltas, layer.tileCache.get());
            m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>(
                "Smudge", m_ActiveLayerIdx, std::move(deltas)));
            m_ActiveStrokeDeltas.clear();
            m_IsDocumentModified = true;
        }
        m_SmudgeFinger.clear();
        m_CompositeDirty = true;
        return;
    }

    if (!m_SmudgePickupValid || phase != StrokePhase::Update) return;

    float ddx = x - m_SmudgeLastX, ddy = y - m_SmudgeLastY;
    float dist = std::sqrt(ddx * ddx + ddy * ddy);
    if (dist < 1e-4f) return;
    m_SmudgeDistAcc += dist;
    float minDist = std::max(0.5f, s.radius * std::max(0.02f, s.spacing));
    if (m_SmudgeDistAcc < minDist) return;
    m_SmudgeDistAcc = 0.f;

    // Unit direction of stroke — sample "upstream" (pixels pulled from behind the finger)
    float invD = 1.f / dist;
    float dirX = ddx * invD, dirY = ddy * invD;
    // How far to pull source samples (fraction of radius)
    float pull = std::max(1.f, s.radius * 0.35f);

    backupBrushTiles(x, y);
    backupBrushTiles(m_SmudgeLastX, m_SmudgeLastY);

    // For each pixel under brush: mix current with source from upstream + finger paint
    for (int ky = -r; ky <= r; ++ky) {
        for (int kx = -r; kx <= r; ++kx) {
            float d2 = std::sqrt((float)(kx * kx + ky * ky));
            if (d2 > (float)r) continue;
            float falloff = 1.f - d2 / (float)r;
            falloff = falloff * falloff; // softer center weight like oil smear
            float w = strength * falloff;
            if (w < 1e-4f) continue;

            int px = (int)std::lround(x) + kx;
            int py = (int)std::lround(y) + ky;
            if (px < 0 || py < 0 || px >= m_Width || py >= m_Height) continue;
            float sel = GetSelWeight(m_SelectionMask, m_Width, px, py, m_HasSelection);
            if (sel < 1e-4f) continue;
            w *= sel;

            // Upstream sample (pixel that is being dragged into this spot)
            float sx = (float)px - dirX * pull;
            float sy = (float)py - dirY * pull;
            int isx = std::clamp((int)std::lround(sx), 0, m_Width - 1);
            int isy = std::clamp((int)std::lround(sy), 0, m_Height - 1);

            float cur[4], src[4];
            layer.tileCache->GetPixelF(px, py, cur);
            layer.tileCache->GetPixelF(isx, isy, src);

            // Finger buffer contribution (color carried from stroke start / previous dabs)
            size_t fi = ((size_t)(ky + r) * diam + (kx + r)) * 4;
            float finger[4] = {0, 0, 0, 0};
            if (fi + 3 < m_SmudgeFinger.size()) {
                for (int c = 0; c < 4; ++c) finger[c] = m_SmudgeFinger[fi + c];
            }

            // Mix: mostly push upstream canvas + finger paint
            float out[4];
            for (int c = 0; c < 4; ++c) {
                float pushed = src[c] * 0.55f + finger[c] * 0.45f;
                out[c] = cur[c] * (1.f - w) + pushed * w;
            }
            layer.tileCache->SetPixelF(px, py, out);

            // Update finger: absorb a bit of what we just painted (oil picks up paint)
            if (fi + 3 < m_SmudgeFinger.size()) {
                for (int c = 0; c < 4; ++c)
                    m_SmudgeFinger[fi + c] = finger[c] * (1.f - 0.35f * w) + out[c] * (0.35f * w);
            }
        }
    }

    m_SmudgeLastX = x;
    m_SmudgeLastY = y;
    layer.needsUpload = true;
    layer.thumbDirty = true;
    m_CompositeDirty = true;
    m_ChannelPreviewDirty = true;
}

// ============================================================
//  Blur Tool — local brush blur (Tool | Operator | Filter triad)
// ============================================================

void Canvas::BlurToolOnActiveLayer(float x, float y, StrokePhase phase, const SmudgeSettings& s) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.CanPaintContent()) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);

    const int r = std::max(1, (int)std::lround(s.radius));
    const float strength = std::clamp(s.strength, 0.f, 1.f);
    // Blur kernel radius independent of brush size (small local blur under dab)
    const int kr = std::clamp((int)std::lround(std::max(1.f, s.radius * 0.25f)), 1, 24);

    auto backupBrushTiles = [&](float cx, float cy) {
        int numTilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
        int numTilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
        int minTX = std::max(0, (int)(cx - r) / TILE_SIZE);
        int maxTX = std::min(numTilesX - 1, (int)(cx + r) / TILE_SIZE);
        int minTY = std::max(0, (int)(cy - r) / TILE_SIZE);
        int maxTY = std::min(numTilesY - 1, (int)(cy + r) / TILE_SIZE);
        for (int ty = minTY; ty <= maxTY; ++ty)
            for (int tx = minTX; tx <= maxTX; ++tx)
                BackupTile(tx, ty);
    };

    if (phase == StrokePhase::Begin) {
        m_IsStrokeActive = true;
        m_ActiveStrokeDeltas.clear();
        m_SmudgeLastX = x; m_SmudgeLastY = y; m_SmudgeDistAcc = 0.f;
        m_SmudgePickupValid = true;
        backupBrushTiles(x, y);
        // Fall through: first dab immediately
    }
    if (phase == StrokePhase::End) {
        m_SmudgePickupValid = false;
        m_IsStrokeActive = false;
        if (!m_ActiveStrokeDeltas.empty()) {
            auto deltas = SealActiveStrokeDeltas(m_ActiveStrokeDeltas, layer.tileCache.get());
            m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>(
                "Blur Tool", m_ActiveLayerIdx, std::move(deltas)));
            m_ActiveStrokeDeltas.clear();
            m_IsDocumentModified = true;
        }
        m_CompositeDirty = true;
        return;
    }
    if (!m_SmudgePickupValid && phase != StrokePhase::Begin) return;

    if (phase == StrokePhase::Update) {
        float ddx = x - m_SmudgeLastX, ddy = y - m_SmudgeLastY;
        float dist = std::sqrt(ddx * ddx + ddy * ddy);
        m_SmudgeDistAcc += dist;
        float minDist = std::max(0.5f, s.radius * std::max(0.05f, s.spacing));
        if (m_SmudgeDistAcc < minDist && phase != StrokePhase::Begin) return;
        m_SmudgeDistAcc = 0.f;
    }

    backupBrushTiles(x, y);
    m_SmudgeLastX = x;
    m_SmudgeLastY = y;

    // Sample neighborhood into temp, then write blurred premul mix
    const int sampleR = r + kr;
    for (int ky = -r; ky <= r; ++ky) {
        for (int kx = -r; kx <= r; ++kx) {
            float d2 = std::sqrt((float)(kx * kx + ky * ky));
            if (d2 > (float)r) continue;
            float falloff = 1.f - d2 / (float)r;
            float w = strength * falloff;
            if (w < 1e-4f) continue;

            int px = (int)std::lround(x) + kx;
            int py = (int)std::lround(y) + ky;
            if (px < 0 || py < 0 || px >= m_Width || py >= m_Height) continue;
            float sel = GetSelWeight(m_SelectionMask, m_Width, px, py, m_HasSelection);
            if (sel < 1e-4f) continue;
            w *= sel;

            // Premul box blur kernel around pixel
            float acc[4] = {0, 0, 0, 0};
            int cnt = 0;
            for (int jy = -kr; jy <= kr; ++jy) {
                for (int jx = -kr; jx <= kr; ++jx) {
                    int qx = std::clamp(px + jx, 0, m_Width - 1);
                    int qy = std::clamp(py + jy, 0, m_Height - 1);
                    float srgba[4];
                    layer.tileCache->GetPixelF(qx, qy, srgba);
                    float a = std::clamp(srgba[3], 0.f, 1.f);
                    acc[0] += srgba[0] * a;
                    acc[1] += srgba[1] * a;
                    acc[2] += srgba[2] * a;
                    acc[3] += a;
                    ++cnt;
                }
            }
            if (cnt < 1) continue;
            float inv = 1.f / (float)cnt;
            float ba = acc[3] * inv;
            float br = 0, bg = 0, bb = 0;
            if (ba > 1e-6f) {
                br = std::clamp((acc[0] * inv) / ba, 0.f, 1.f);
                bg = std::clamp((acc[1] * inv) / ba, 0.f, 1.f);
                bb = std::clamp((acc[2] * inv) / ba, 0.f, 1.f);
            }
            float cur[4];
            layer.tileCache->GetPixelF(px, py, cur);
            float out[4] = {
                cur[0] * (1.f - w) + br * w,
                cur[1] * (1.f - w) + bg * w,
                cur[2] * (1.f - w) + bb * w,
                cur[3] * (1.f - w) + ba * w
            };
            layer.tileCache->SetPixelF(px, py, out);
        }
    }

    layer.needsUpload = true;
    layer.thumbDirty = true;
    m_CompositeDirty = true;
    m_ChannelPreviewDirty = true;
    (void)sampleR;
}

// ============================================================
//  Non-destructive Filters + Styles presentation
// ============================================================

std::vector<float> Canvas::ResolveLayerContentF(const Layer& layer) const {
    if (layer.IsFill()) {
        std::vector<float> buf;
        layer_fx::FillSolidBuffer(buf, m_Width, m_Height, layer.fill);
        return buf;
    }
    return ExportLayerF(layer, m_Width, m_Height);
}

bool Canvas::IsTopLevelLayer(const Layer& layer) {
    return layer.parentGroupId < 0;
}

void Canvas::RebuildFilteredPixels(Layer& layer) {
    if (!layer.filtersDirty) return;
    RefreshFilteredCache(layer, /*onlyDirtyTiles=*/false);
}

void Canvas::RefreshFilteredCache(Layer& layer, bool onlyDirtyTiles) {
    if (layer.filters.empty() || (!LayerHasPixels(layer) && !layer.IsFill())) {
        layer.filteredCache.reset();
        layer.filtersDirty = false;
        layer.presentationDirty = true;
        return;
    }
    if (!m_EffectsPreviewEnabled) {
        // Keep caches but don't thrash while preview is off
        return;
    }

    const int halo = layer_fx::MaxFilterSupportRadius(layer.filters);
    const bool needFull = layer.filtersDirty || !layer.filteredCache ||
                          layer.filteredCache->IsEmpty() ||
                          layer.filteredCache->GetWidth() != m_Width ||
                          layer.filteredCache->GetHeight() != m_Height ||
                          !onlyDirtyTiles;

    if (!layer.filteredCache) {
        layer.filteredCache = std::make_unique<TileCache>();
        layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
    } else if (layer.filteredCache->GetWidth() != m_Width ||
               layer.filteredCache->GetHeight() != m_Height) {
        layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
    }

    // Collect content tiles to process
    std::vector<std::pair<int, int>> work;
    work.reserve(64);

    auto addTile = [&](int tx, int ty) {
        if (tx < 0 || ty < 0) return;
        if (layer.tileCache) {
            if (tx >= layer.tileCache->GetTilesX() || ty >= layer.tileCache->GetTilesY()) return;
        } else if (layer.IsFill()) {
            int txx = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
            int tyy = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
            if (tx >= txx || ty >= tyy) return;
        } else return;
        work.emplace_back(tx, ty);
    };

    if (needFull) {
        if (layer.IsFill() && (!layer.tileCache || layer.tileCache->IsEmpty())) {
            // Solid/texture fill without raster tiles: one full-buffer pass (proxy-safe size check)
            if (!CanAllocateFlatComposite(m_Width, m_Height, "RefreshFilteredCache(fill)")) {
                layer.filtersDirty = false;
                return;
            }
            std::vector<float> tmp = ResolveLayerContentF(layer);
            layer_fx::ApplyPixelFilters(tmp, m_Width, m_Height, layer.filters, 0, 0);
            layer.filteredCache->Clear();
            layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
            layer.filteredCache->ImportRGBA32F(tmp.data(), m_Width, m_Height);
            layer.filteredCache->MarkAllDirty();
            layer.filtersDirty = false;
            layer.presentationDirty = true;
            return;
        }
        if (!layer.tileCache) {
            layer.filtersDirty = false;
            return;
        }
        for (int ty = 0; ty < layer.tileCache->GetTilesY(); ++ty)
            for (int tx = 0; tx < layer.tileCache->GetTilesX(); ++tx)
                if (layer.tileCache->HasTile(tx, ty))
                    addTile(tx, ty);
        // Full rebuild: drop stale filtered tiles first
        layer.filteredCache->Clear();
        layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
    } else {
        // Incremental: content dirty tiles + blur halo neighborhood
        if (!layer.tileCache) {
            layer.filtersDirty = false;
            return;
        }
        std::vector<std::pair<int, int>> seeds;
        for (int ty = 0; ty < layer.tileCache->GetTilesY(); ++ty) {
            for (int tx = 0; tx < layer.tileCache->GetTilesX(); ++tx) {
                if (layer.tileCache->IsDirty(tx, ty) && layer.tileCache->HasTile(tx, ty))
                    seeds.emplace_back(tx, ty);
            }
        }
        if (seeds.empty()) {
            layer.filtersDirty = false;
            return;
        }
        const int expand = (halo + TILE_SIZE - 1) / TILE_SIZE;
        std::unordered_set<uint32_t> seen;
        for (auto [sx, sy] : seeds) {
            for (int dy = -expand; dy <= expand; ++dy) {
                for (int dx = -expand; dx <= expand; ++dx) {
                    int tx = sx + dx, ty = sy + dy;
                    if (tx < 0 || ty < 0 || tx >= layer.tileCache->GetTilesX() ||
                        ty >= layer.tileCache->GetTilesY())
                        continue;
                    // Only process tiles that have content (or seed empty→paint created)
                    if (!layer.tileCache->HasTile(tx, ty) && !(dx == 0 && dy == 0))
                        continue;
                    uint32_t key = (uint32_t)tx | ((uint32_t)ty << 16);
                    if (!seen.insert(key).second) continue;
                    addTile(tx, ty);
                }
            }
        }
    }

    // Fast path: no halo (Curves/HSV/Noise/AlphaInvert) — convert whole 256² tile from raw bytes
    auto exportTileF = [&](int tx, int ty, std::vector<float>& out, int& outW, int& outH) -> bool {
        if (!layer.tileCache || !layer.tileCache->HasTile(tx, ty)) return false;
        const int x0 = tx * TILE_SIZE, y0 = ty * TILE_SIZE;
        outW = std::min(TILE_SIZE, m_Width - x0);
        outH = std::min(TILE_SIZE, m_Height - y0);
        if (outW <= 0 || outH <= 0) return false;
        out.assign((size_t)TILE_SIZE * TILE_SIZE * 4, 0.f);
        const uint8_t* raw = layer.tileCache->GetTileData(tx, ty);
        if (!raw) return false;
        const auto fmt = layer.tileCache->GetFormat();
        for (int ly = 0; ly < outH; ++ly) {
            for (int lx = 0; lx < outW; ++lx) {
                size_t di = ((size_t)ly * TILE_SIZE + lx) * 4; // dense TILE_SIZE pitch in out
                // Store with outW pitch for ApplyPixelFilters
                size_t oi = ((size_t)ly * outW + lx) * 4;
                if (fmt == CanvasPixelFormat::RGBA8) {
                    size_t si = ((size_t)ly * TILE_SIZE + lx) * 4;
                    out[oi + 0] = raw[si + 0] / 255.f;
                    out[oi + 1] = raw[si + 1] / 255.f;
                    out[oi + 2] = raw[si + 2] / 255.f;
                    out[oi + 3] = raw[si + 3] / 255.f;
                } else if (fmt == CanvasPixelFormat::RGBA16F) {
                    float px[4];
                    HalfFloat::LoadRGBA16F(raw + ((size_t)ly * TILE_SIZE + lx) * 8, px);
                    out[oi + 0] = px[0]; out[oi + 1] = px[1];
                    out[oi + 2] = px[2]; out[oi + 3] = px[3];
                } else {
                    const float* fp = reinterpret_cast<const float*>(raw);
                    size_t si = ((size_t)ly * TILE_SIZE + lx) * 4;
                    out[oi + 0] = fp[si + 0]; out[oi + 1] = fp[si + 1];
                    out[oi + 2] = fp[si + 2]; out[oi + 3] = fp[si + 3];
                }
                (void)di;
            }
        }
        out.resize((size_t)outW * outH * 4);
        return true;
    };

    for (auto [tx, ty] : work) {
        const int x0 = tx * TILE_SIZE;
        const int y0 = ty * TILE_SIZE;
        const int tileW = std::min(TILE_SIZE, m_Width - x0);
        const int tileH = std::min(TILE_SIZE, m_Height - y0);
        if (tileW <= 0 || tileH <= 0 || !layer.tileCache) continue;

        if (halo == 0) {
            std::vector<float> tileF;
            int tw = 0, th = 0;
            if (!exportTileF(tx, ty, tileF, tw, th)) continue;
            layer_fx::ApplyPixelFilters(tileF, tw, th, layer.filters, x0, y0);
            layer.filteredCache->ImportRGBA32F(tileF.data(), tw, th, x0, y0);
            continue;
        }

        const int rx0 = std::max(0, x0 - halo);
        const int ry0 = std::max(0, y0 - halo);
        const int rx1 = std::min(m_Width, x0 + tileW + halo);
        const int ry1 = std::min(m_Height, y0 + tileH + halo);
        const int rw = rx1 - rx0;
        const int rh = ry1 - ry0;
        if (rw <= 0 || rh <= 0) continue;

        // Bulk tile-row export (no per-pixel GetPixelF — was catastrophic for blur halo)
        std::vector<float> region((size_t)rw * rh * 4, 0.f);
        const auto fmt = layer.tileCache->GetFormat();
        const int bpp = layer.tileCache->GetBytesPerPixel();
        for (int y = 0; y < rh; ++y) {
            const int docY = ry0 + y;
            const int sty = docY / TILE_SIZE;
            const int sly = docY - sty * TILE_SIZE;
            int x = 0;
            while (x < rw) {
                const int docX = rx0 + x;
                const int stx = docX / TILE_SIZE;
                const int slx0 = docX - stx * TILE_SIZE;
                const int run = std::min(rw - x, TILE_SIZE - slx0);
                const uint8_t* raw = layer.tileCache->GetTileData(stx, sty);
                if (raw) {
                    for (int k = 0; k < run; ++k) {
                        const uint8_t* p = raw + ((size_t)sly * TILE_SIZE + (slx0 + k)) * bpp;
                        size_t i = ((size_t)y * rw + x + k) * 4;
                        if (fmt == CanvasPixelFormat::RGBA8) {
                            region[i + 0] = p[0] / 255.f; region[i + 1] = p[1] / 255.f;
                            region[i + 2] = p[2] / 255.f; region[i + 3] = p[3] / 255.f;
                        } else if (fmt == CanvasPixelFormat::RGBA16F) {
                            float px[4]; HalfFloat::LoadRGBA16F(p, px);
                            region[i + 0] = px[0]; region[i + 1] = px[1];
                            region[i + 2] = px[2]; region[i + 3] = px[3];
                        } else {
                            const float* fp = reinterpret_cast<const float*>(p);
                            region[i + 0] = fp[0]; region[i + 1] = fp[1];
                            region[i + 2] = fp[2]; region[i + 3] = fp[3];
                        }
                    }
                }
                x += run;
            }
        }

        // CPU filters only — GPU blur preview was producing tile seams / ghost frames.
        layer_fx::ApplyPixelFilters(region, rw, rh, layer.filters, rx0, ry0);

        const int ox = x0 - rx0;
        const int oy = y0 - ry0;
        std::vector<float> tileF((size_t)tileW * tileH * 4);
        for (int y = 0; y < tileH; ++y) {
            const size_t siBase = ((size_t)(oy + y) * rw + ox) * 4;
            const size_t diBase = ((size_t)y * tileW) * 4;
            std::memcpy(tileF.data() + diBase, region.data() + siBase, (size_t)tileW * 4 * sizeof(float));
        }
        layer.filteredCache->ImportRGBA32F(tileF.data(), tileW, tileH, x0, y0);
    }

    // Content dirty was consumed for refilter; clear so we don't refilter every frame.
    // GPU upload will use filteredCache dirty flags set by ImportRGBA32F.
    if (layer.tileCache && onlyDirtyTiles) {
        for (auto [tx, ty] : work)
            layer.tileCache->ClearDirty(tx, ty);
    } else if (layer.tileCache && needFull) {
        layer.tileCache->ClearAllDirty();
    }

    layer.filtersDirty = false;
    layer.presentationDirty = true;
}

void Canvas::RebuildLayerPresentation(Layer& layer, bool fullQuality) {
    if (!fullQuality && !layer.presentationDirty && !layer.stylesDirty && !layer.filtersDirty)
        return;
    // Never full-rebake styles mid-stroke — paint stays live without styles until End.
    if (!fullQuality && m_IsStrokeActive) {
        return;
    }
    // Debounce interactive rebuilds (slider drag / add FX)
    if (!fullQuality && m_PresentationRebuildDeferred) {
        if (std::chrono::steady_clock::now() < m_PresentationRebuildNotBefore)
            return;
        m_PresentationRebuildDeferred = false;
    }
    if (m_Width <= 0 || m_Height <= 0) {
        layer.stylesDirty = false;
        layer.presentationDirty = false;
        return;
    }
    if (layer.filtersDirty)
        RebuildFilteredPixels(layer);

    if (!layer.HasEnabledStyles()) {
        layer.presentationCache.reset();
        layer.stylesDirty = false;
        layer.presentationDirty = false;
        return;
    }

    // Guard flat float buffers (4K RGBA32F content alone ≈ 256 MiB; styles allocate more).
    if (!CanAllocateFlatComposite(m_Width, m_Height, "RebuildLayerPresentation")) {
        Logger::Get().ErrorTag("fx",
            "Layer styles bake refused at " + std::to_string(m_Width) + "x" +
            std::to_string(m_Height) + " — reduce document size or disable styles.");
        layer.stylesDirty = false;
        layer.presentationDirty = false;
        return;
    }

    // Content after filters (or raw). Prefer proxy path for large docs —
    // sample at bake res only (never allocate full 4K float then downsample).
    int bakeW = m_Width, bakeH = m_Height;
    if (!fullQuality)
        ComputeCompositePreviewSize(m_Width, m_Height, bakeW, bakeH);

    const bool useProxy = (bakeW != m_Width || bakeH != m_Height);
    const float scaleX = useProxy ? (float)bakeW / (float)m_Width : 1.f;
    const float scaleY = useProxy ? (float)bakeH / (float)m_Height : 1.f;

    std::vector<float> bakeContent;
    std::vector<uint8_t> bakeMask;
    try {
        bakeContent.resize((size_t)bakeW * bakeH * 4, 0.f);
        const TileCache* srcCache = nullptr;
        if (layer.filteredCache && !layer.filteredCache->IsEmpty() && !layer.filters.empty())
            srcCache = layer.filteredCache.get();
        else if (layer.tileCache && !layer.tileCache->IsEmpty())
            srcCache = layer.tileCache.get();

        if (layer.IsFill()) {
            float c[4];
            layer.fill.ResolveRgba(c);
            for (size_t i = 0, n = (size_t)bakeW * bakeH; i < n; ++i) {
                bakeContent[i * 4 + 0] = c[0];
                bakeContent[i * 4 + 1] = c[1];
                bakeContent[i * 4 + 2] = c[2];
                bakeContent[i * 4 + 3] = c[3];
            }
        } else if (srcCache) {
            for (int y = 0; y < bakeH; ++y) {
                int sy = useProxy
                    ? std::min(m_Height - 1, (int)(y / scaleY + 0.5f))
                    : y;
                for (int x = 0; x < bakeW; ++x) {
                    int sx = useProxy
                        ? std::min(m_Width - 1, (int)(x / scaleX + 0.5f))
                        : x;
                    float px[4];
                    srcCache->GetPixelF(sx, sy, px);
                    size_t di = ((size_t)y * bakeW + x) * 4;
                    bakeContent[di + 0] = px[0];
                    bakeContent[di + 1] = px[1];
                    bakeContent[di + 2] = px[2];
                    bakeContent[di + 3] = px[3];
                }
            }
        }
        if (layer.hasMask) {
            bakeMask.resize((size_t)bakeW * bakeH, 255);
            for (int y = 0; y < bakeH; ++y) {
                int sy = useProxy
                    ? std::min(m_Height - 1, (int)(y / scaleY + 0.5f))
                    : y;
                for (int x = 0; x < bakeW; ++x) {
                    int sx = useProxy
                        ? std::min(m_Width - 1, (int)(x / scaleX + 0.5f))
                        : x;
                    uint8_t mv = 255;
                    if (layer.maskTiles && layer.maskTiles->Valid())
                        mv = layer.maskTiles->Get(sx, sy);
                    else if (layer.mask.size() == (size_t)m_Width * m_Height)
                        mv = layer.mask[(size_t)sy * m_Width + sx];
                    bakeMask[(size_t)y * bakeW + x] = mv;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        Logger::Get().ErrorTag("fx", "OOM preparing style bake buffer");
        layer.stylesDirty = false;
        layer.presentationDirty = false;
        return;
    }

    // Scale style geometry to bake resolution
    std::vector<LayerStyle> scaledStyles = layer.styles;
    for (auto& st : scaledStyles) {
        st.distance *= scaleX; // approx isotropic
        st.offsetX *= scaleX;
        st.offsetY *= scaleY;
        st.size *= (scaleX + scaleY) * 0.5f;
        st.outlineSize *= (scaleX + scaleY) * 0.5f;
    }

    layer_fx::PresentationParams pp;
    pp.fillOpacity = layer.opacity;
    pp.bakeFillOpacity = true;
    pp.hasMask = useProxy ? !bakeMask.empty() : (layer.hasMask && !layer.mask.empty());
    pp.mask = useProxy ? (bakeMask.empty() ? nullptr : bakeMask.data())
                       : (pp.hasMask ? layer.mask.data() : nullptr);
    pp.maskBytes = useProxy ? bakeMask.size() : layer.mask.size();
    pp.previewQuality = !fullQuality;

    std::vector<float> presSmall;
    try {
        presSmall = layer_fx::BuildPresentation(
            bakeContent, bakeW, bakeH, {}, scaledStyles, pp);
    } catch (const std::bad_alloc&) {
        Logger::Get().ErrorTag("fx", "OOM during style bake " +
            std::to_string(bakeW) + "x" + std::to_string(bakeH));
        layer.stylesDirty = false;
        layer.presentationDirty = false;
        return;
    }

    // Upsample back to document size for GPU layer texture
    std::vector<float> presFull;
    if (useProxy) {
        try {
            presFull.resize((size_t)m_Width * m_Height * 4);
        } catch (const std::bad_alloc&) {
            Logger::Get().ErrorTag("fx", "OOM upsampling style presentation");
            // Keep proxy-sized presentation cache only
            if (!layer.presentationCache)
                layer.presentationCache = std::make_unique<TileCache>();
            layer.presentationCache->Init(bakeW, bakeH, m_CanvasFormat);
            layer.presentationCache->ImportRGBA32F(presSmall.data(), bakeW, bakeH);
            layer.presentationCache->MarkAllDirty();
            layer.stylesDirty = false;
            layer.presentationDirty = false;
            layer.needsUpload = true;
            return;
        }
        for (int y = 0; y < m_Height; ++y) {
            int sy = std::min(bakeH - 1, (int)(y * scaleY));
            for (int x = 0; x < m_Width; ++x) {
                int sx = std::min(bakeW - 1, (int)(x * scaleX));
                size_t di = ((size_t)y * m_Width + x) * 4;
                size_t si = ((size_t)sy * bakeW + sx) * 4;
                presFull[di + 0] = presSmall[si + 0];
                presFull[di + 1] = presSmall[si + 1];
                presFull[di + 2] = presSmall[si + 2];
                presFull[di + 3] = presSmall[si + 3];
            }
        }
    } else {
        presFull = std::move(presSmall);
    }

    if (!layer.presentationCache)
        layer.presentationCache = std::make_unique<TileCache>();
    layer.presentationCache->Init(m_Width, m_Height, m_CanvasFormat);
    layer.presentationCache->ImportRGBA32F(presFull.data(), m_Width, m_Height);
    layer.presentationCache->MarkAllDirty();
    layer.stylesDirty = false;
    layer.presentationDirty = false;
    layer.needsUpload = true;
}

ID3D11BlendState* Canvas::GetFillWriteBlend(ID3D11Device* device, uint8_t channelMask, bool replace) {
    if (!device) return replace ? m_LayerBlendStateReplace : m_LayerBlendState;
    uint8_t m = channelMask & 0xF;
    if (m == 0xF)
        return replace ? m_LayerBlendStateReplace : m_LayerBlendState;

    ID3D11BlendState** slot = replace ? &m_FillWriteMaskReplace[m] : &m_FillWriteMaskBlend[m];
    if (*slot) return *slot;

    UINT write = 0;
    if (m & 1) write |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (m & 2) write |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (m & 4) write |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (m & 8) write |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    if (write == 0) return replace ? m_LayerBlendStateReplace : m_LayerBlendState;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    if (replace) {
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    } else {
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    }
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)write;
    if (FAILED(device->CreateBlendState(&bd, slot)) || !*slot)
        return replace ? m_LayerBlendStateReplace : m_LayerBlendState;
    return *slot;
}

void Canvas::SuspendGpuResources() {
    if (m_GpuSuspended) return;
    // Do not leave interactive transform mid-flight on a tab we are freezing.
    if (m_IsMovingPixels) {
        m_IsMovingPixels = false;
        m_FloatingPixels.clear();
        m_OriginalSelectionMask.clear();
        m_FloatingBufW = m_FloatingBufH = 0;
    }
    if (m_IsStrokeActive) {
        m_IsStrokeActive = false;
        m_ActiveStrokeDeltas.clear();
    }

    ReleaseCompositeResources();

    for (auto& L : m_Layers) {
        ReleaseFillPatternGpu(L);
        if (L.gpuSurfaceId) {
            m_GpuTiles.DestroySurface(L.gpuSurfaceId);
            L.gpuSurfaceId = 0;
        }
        if (L.texture) { DeferReleaseTex(L.texture); L.texture = nullptr; }
        if (L.srv) { DeferReleaseSRV(L.srv); L.srv = nullptr; }
        if (L.maskTexture) { DeferReleaseTex(L.maskTexture); L.maskTexture = nullptr; }
        if (L.maskSRV) { DeferReleaseSRV(L.maskSRV); L.maskSRV = nullptr; }
        if (L.thumbSRV) { DeferReleaseSRV(L.thumbSRV); L.thumbSRV = nullptr; }
        if (L.thumbTex) { DeferReleaseTex(L.thumbTex); L.thumbTex = nullptr; }
        L.needsUpload = true;
        L.thumbDirty = true;
        L.maskNeedsUpload = L.hasMask;
    }

    if (m_SelectionMaskTexture) {
        DeferReleaseTex(m_SelectionMaskTexture);
        m_SelectionMaskTexture = nullptr;
    }
    if (m_SelectionMaskSRV) {
        DeferReleaseSRV(m_SelectionMaskSRV);
        m_SelectionMaskSRV = nullptr;
    }
    m_SelectionMaskNeedsUpload = m_HasSelection;
    m_SelectionGpuDirtyValid = false;

    // Drop rebuildable CPU caches (filters/styles presentation) — save RAM while idle.
    // TileCache pixel data is kept (source of truth for RESTORING).
    for (auto& L : m_Layers) {
        L.filteredCache.reset();
        L.presentationCache.reset();
        L.filtersDirty = !L.filters.empty();
        L.stylesDirty = L.HasEnabledStyles();
        L.presentationDirty = true;
    }

    m_CompositeDirty = true;
    m_CompositeDirtyFull = true;
    m_GpuSuspended = true;
    // Inactive tab is not in ImGui draw list — free deferred GPU now (avoid leak until switch).
    FlushDeferredGpuReleases();
    Logger::Get().InfoTag("mem",
        "GPU dormancy: suspended canvas " + std::to_string(m_Width) + "x" +
        std::to_string(m_Height) + " layers=" + std::to_string(m_Layers.size()) +
        " (CPU tiles retained, FX caches dropped)");
}

bool Canvas::EnsureGpuAwake(ID3D11Device* device) {
    if (m_DiskHibernated) return false; // must RestoreFromHibernateFile first
    if (!m_GpuSuspended) return false;
    if (!device || m_Width <= 0 || m_Height <= 0) {
        m_GpuSuspended = false;
        return false;
    }

    auto t0 = std::chrono::steady_clock::now();
    Logger::Get().InfoTag("mem", "GPU dormancy: RESTORING …");

    CreateCompositeResources(device);
    for (auto& L : m_Layers) {
        if (L.isGroup) continue;
        L.needsUpload = true;
        if (L.IsFill()) {
            EnsureFillLayerGpu(device, L);
        } else if (L.tileCache && !L.tileCache->IsEmpty()) {
            RecreateLayerTexture(device, L);
        }
        if (L.hasMask)
            L.maskNeedsUpload = true;
    }
    if (m_HasSelection)
        UpdateSelectionMaskTexture(device);

    m_CompositeDirty = true;
    m_CompositeDirtyFull = true;
    m_GpuSuspended = false;

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    Logger::Get().InfoTag("mem",
        "GPU dormancy: RESTORING done in " + std::to_string(ms) + " ms");
    return true;
}

void Canvas::StripHeavyMemoryAfterHibernate() {
    SuspendGpuResources();
    ClearUndoHistory();
    for (auto& L : m_Layers) {
        L.tileCache.reset();
        L.filteredCache.reset();
        L.presentationCache.reset();
        L.nativeMapCache.reset();
        L.mask.clear();
        L.maskTiles.reset();
        L.hasMask = false;
        L.fill.textureRgba.clear();
        L.fill.textureW = L.fill.textureH = 0;
        L.smartSourceBytes.clear();
        L.needsUpload = true;
    }
    m_SelectionMask.clear();
    m_HasSelection = false;
    m_SelectionBoundsValid = false;
    m_WandSourceRGBA.clear();
    m_WandSourceW = m_WandSourceH = 0;
    m_FloatingPixels.clear();
    m_OriginalSelectionMask.clear();
    m_DiskHibernated = true;
    m_GpuSuspended = true; // still need GPU rebuild after load
    Logger::Get().InfoTag("mem",
        "Disk hibernate: stripped CPU tiles/masks/undo (shell kept, reload from snapshot)");
}

bool Canvas::RestoreFromHibernateFile(ID3D11Device* device, const std::string& raypPath) {
    if (!device || raypPath.empty()) return false;
    Logger::Get().InfoTag("mem", "Disk hibernate: RESTORING from " + raypPath);
    const bool ok = LoadCanvasRayp(raypPath, device);
    m_DiskHibernated = false;
    m_GpuSuspended = false;
    if (ok) {
        m_CompositeDirty = true;
        m_CompositeDirtyFull = true;
    }
    return ok;
}

void Canvas::DeferReleaseSRV(ID3D11ShaderResourceView* srv) {
    if (srv) m_DeferredSrvRelease.push_back(srv);
}

void Canvas::DeferReleaseTex(ID3D11Texture2D* tex) {
    if (tex) m_DeferredTexRelease.push_back(tex);
}

void Canvas::FlushDeferredGpuReleases() {
    for (ID3D11ShaderResourceView* s : m_DeferredSrvRelease) {
        if (s) s->Release();
    }
    m_DeferredSrvRelease.clear();
    for (ID3D11Texture2D* t : m_DeferredTexRelease) {
        if (t) t->Release();
    }
    m_DeferredTexRelease.clear();
}

void Canvas::ReleaseFillPatternGpu(Layer& layer) {
    // Deferred: ImGui may still reference fillPatternSRV as ImTextureID this frame.
    if (layer.fillPatternSRV) {
        DeferReleaseSRV(layer.fillPatternSRV);
        layer.fillPatternSRV = nullptr;
    }
    if (layer.fillPatternTex) {
        DeferReleaseTex(layer.fillPatternTex);
        layer.fillPatternTex = nullptr;
    }
    layer.fillPatternUploadedKey.clear();
}

bool Canvas::EnsureFillPatternGpu(ID3D11Device* device, Layer& layer) {
    if (!device || !layer.IsFill() || !layer.fill.HasTexture()) return false;
    const std::string& key = layer.fill.textureAssetKey;
    if (key.empty()) return false;

    // Already uploaded this asset
    if (layer.fillPatternSRV && layer.fillPatternUploadedKey == key)
        return true;

    auto pay = assets::AssetStore::Get().GetPayload(key);
    if (!pay || pay->rgba.empty() || pay->w <= 0 || pay->h <= 0) {
        // Kick async load; compose will solid-fallback until ready
        assets::AssetStore::Get().RequestLoad(key);
        return false;
    }

    ReleaseFillPatternGpu(layer);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)pay->w;
    td.Height = (UINT)pay->h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pay->rgba.data();
    init.SysMemPitch = (UINT)pay->w * 4;

    if (FAILED(device->CreateTexture2D(&td, &init, &layer.fillPatternTex)) || !layer.fillPatternTex)
        return false;
    if (FAILED(device->CreateShaderResourceView(layer.fillPatternTex, nullptr, &layer.fillPatternSRV)) ||
        !layer.fillPatternSRV) {
        ReleaseFillPatternGpu(layer);
        return false;
    }
    layer.fillPatternUploadedKey = key;
    layer.fill.textureW = pay->w;
    layer.fill.textureH = pay->h;
    return true;
}

void Canvas::EnsureFillLayerGpu(ID3D11Device* device, Layer& layer) {
    if (!device || !layer.IsFill()) return;

    // Interactive: FX bake only when Effects Preview is ON.
    // Texture without FX → GPU pattern sample (source-res), NEVER full-doc CPU bake.
    const bool fxActive = LayerFxPreviewActive(layer);

    if (fxActive) {
        // Styles/filters need baked presentation (proxy or full via RebuildLayerPresentation)
        RebuildLayerPresentation(layer);
        if (!layer.texture)
            RecreateLayerTexture(device, layer);
        if (layer.texture) {
            D3D11_TEXTURE2D_DESC desc{};
            layer.texture->GetDesc(&desc);
            if (desc.Width != (UINT)m_Width || desc.Height != (UINT)m_Height)
                RecreateLayerTexture(device, layer);
        }
        return;
    }

    // Texture fill: ensure pattern GPU; keep 1×1 tint as layer.srv fallback
    if (layer.fill.HasTexture() || layer.fill.useTexture) {
        EnsureFillPatternGpu(device, layer);
        // Drop any leftover full-doc presentation from older builds / FX-off toggle
        if (layer.presentationCache) {
            layer.presentationCache.reset();
        }
    } else {
        ReleaseFillPatternGpu(layer);
    }

    // Cheap path: 1×1 solid color texture (tint / solid fill).
    // Multi-target Fill: pack enabled roles into current view map.
    float c[4] = {0,0,0,1};
    if (!m_ActiveSetMaps.empty()) {
        if (!layer.fill.ResolveForMap(m_ActiveSetMaps, m_ViewMapKind, c)) {
            c[0] = c[1] = c[2] = 0.f; c[3] = 0.f;
        }
    } else {
        layer.fill.ResolveRgba(c);
    }
    if (layer.texture) {
        D3D11_TEXTURE2D_DESC desc{};
        layer.texture->GetDesc(&desc);
        if (desc.Width != 1 || desc.Height != 1) {
            DeferReleaseTex(layer.texture); layer.texture = nullptr;
            if (layer.srv) { DeferReleaseSRV(layer.srv); layer.srv = nullptr; }
        }
    }
    if (!layer.texture) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 1; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = GetLayerDxgiFormat();
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &layer.texture)) || !layer.texture)
            return;
        if (FAILED(device->CreateShaderResourceView(layer.texture, nullptr, &layer.srv)) ||
            !layer.srv) {
            DeferReleaseTex(layer.texture);
            layer.texture = nullptr;
            layer.srv = nullptr;
            return;
        }
    }
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (ctx && layer.texture) {
        if (m_CanvasFormat == CanvasPixelFormat::RGBA8) {
            uint8_t px[4] = {
                (uint8_t)(std::clamp(c[0],0.f,1.f)*255.f+0.5f),
                (uint8_t)(std::clamp(c[1],0.f,1.f)*255.f+0.5f),
                (uint8_t)(std::clamp(c[2],0.f,1.f)*255.f+0.5f),
                (uint8_t)(std::clamp(c[3],0.f,1.f)*255.f+0.5f)
            };
            ctx->UpdateSubresource(layer.texture, 0, nullptr, px, 4, 0);
        } else {
            ctx->UpdateSubresource(layer.texture, 0, nullptr, c, 16, 0);
        }
        ctx->Release();
    }
    // MUST clear dirty flags — sticky presentationDirty forced full recomposite every frame.
    layer.needsUpload = false;
    layer.presentationDirty = false;
    layer.stylesDirty = false;
    layer.filtersDirty = false;
}

bool Canvas::LoadFillTexture(int layerIdx, const std::string& filepath) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    if (!m_Layers[layerIdx].IsFill()) return false;

    if (filepath.empty())
        return BindFillTextureAsset(layerIdx, {});

    // Import into Project session so .rayp packs without absolute paths (ref=1).
    std::string key = assets::AssetManager::Get().ImportFileToProject(filepath);
    if (key.empty()) {
        key = assets::AssetStore::Get().AcquireFile(filepath);
        if (key.empty()) {
            Logger::Get().Error("LoadFillTexture failed: " + filepath);
            return false;
        }
    }
    // Key already held with refcount 1 — assign without second Acquire.
    Layer& layer = m_Layers[layerIdx];
    if (!layer.fill.textureAssetKey.empty()) {
        assets::AssetStore::Get().Release(layer.fill.textureAssetKey);
        layer.fill.textureAssetKey.clear();
    }
    ReleaseFillPatternGpu(layer);
    layer.fill.textureRgba.clear();
    layer.fill.useTexture = true;
    layer.fill.textureAssetKey = key;
    layer.fill.texturePath.clear();
    int tw = 0, th = 0;
    if (assets::AssetStore::Get().GetDims(key, tw, th)) {
        layer.fill.textureW = tw;
        layer.fill.textureH = th;
    } else {
        layer.fill.textureW = layer.fill.textureH = 1;
    }
    layer.needsUpload = true;
    layer.presentationDirty = true;
    layer.presentationCache.reset();
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("Fill texture (import) key=" + key);
    return true;
}

bool Canvas::BindFillTextureAsset(int layerIdx, const std::string& assetKey) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    Layer& layer = m_Layers[layerIdx];
    if (!layer.IsFill()) return false;

    auto releasePrev = [&]() {
        if (!layer.fill.textureAssetKey.empty()) {
            assets::AssetStore::Get().Release(layer.fill.textureAssetKey);
            layer.fill.textureAssetKey.clear();
        }
        layer.fill.textureRgba.clear();
        layer.fill.textureW = layer.fill.textureH = 0;
        layer.fill.texturePath.clear();
    };

    if (assetKey.empty()) {
        releasePrev();
        ReleaseFillPatternGpu(layer);
        layer.fill.useTexture = false;
        layer.needsUpload = true;
        layer.presentationDirty = true;
        m_CompositeDirty = true;
        m_IsDocumentModified = true;
        return true;
    }

    if (!assets::AssetManager::Get().IsKindAllowed(assetKey, assets::AssetKind::Texture)) {
        // Unknown kind on unindexed keys: still allow if it looks like a texture key
        assets::AssetCategory cat;
        std::string rest;
        bool okKind = false;
        if (assets::ParseKey(assetKey, cat, rest)) {
            auto k = assets::GuessKindFromPath(rest);
            okKind = (k == assets::AssetKind::Texture || k == assets::AssetKind::Unknown);
        }
        if (!okKind) {
            Logger::Get().Error("BindFillTextureAsset: kind not Texture for " + assetKey);
            return false;
        }
    }

    // Ensure payload requested; pin ref for layer.
    std::string key = assets::AssetStore::Get().AcquireKey(assetKey);
    if (key.empty()) {
        assets::AssetStore::Get().RequestLoad(assetKey);
        if (!assets::AssetStore::Get().AddRef(assetKey)) {
            // Ensure meta entry exists
            assets::AssetStore::Get().EnsureMeta(assetKey, assets::AssetKind::Texture, {}, {});
            assets::AssetStore::Get().AddRef(assetKey);
        }
        key = assetKey;
    }

    releasePrev();
    ReleaseFillPatternGpu(layer); // force re-upload of new pattern
    layer.fill.useTexture = true;
    layer.fill.textureAssetKey = key;
    layer.fill.texturePath.clear(); // library/project identity — not path-bound
    int tw = 0, th = 0;
    if (assets::AssetStore::Get().GetDims(key, tw, th)) {
        layer.fill.textureW = tw;
        layer.fill.textureH = th;
    } else {
        // Placeholder dims until async ready
        layer.fill.textureW = layer.fill.textureH = 1;
    }
    layer.fill.textureRgba.clear();
    layer.needsUpload = true;
    layer.presentationDirty = true;
    layer.presentationCache.reset();
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("Fill texture bound key=" + key + " " +
                       std::to_string(layer.fill.textureW) + "x" +
                       std::to_string(layer.fill.textureH));
    return true;
}

bool Canvas::LoadOutlineTexture(int layerIdx, int styleIdx, const std::string& filepath) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    if (styleIdx < 0 || styleIdx >= (int)m_Layers[layerIdx].styles.size()) return false;

    if (filepath.empty())
        return BindOutlineTextureAsset(layerIdx, styleIdx, {});

    // Import into Project so .rayp packs without absolute paths (ref=1).
    std::string key = assets::AssetManager::Get().ImportFileToProject(filepath);
    if (key.empty()) {
        key = assets::AssetStore::Get().AcquireFile(filepath);
        if (key.empty()) {
            Logger::Get().Error("LoadOutlineTexture failed: " + filepath);
            return false;
        }
    }
    LayerStyle& st = m_Layers[layerIdx].styles[styleIdx];
    if (!st.outlineTextureAssetKey.empty()) {
        assets::AssetStore::Get().Release(st.outlineTextureAssetKey);
        st.outlineTextureAssetKey.clear();
    }
    st.outlineTextureRgba.clear();
    st.outlineTexturePath.clear();
    st.outlineTextureAssetKey = key; // keep import/acquire ref
    int tw = 0, th = 0;
    if (assets::AssetStore::Get().GetDims(key, tw, th)) {
        st.outlineTextureW = tw;
        st.outlineTextureH = th;
    } else {
        st.outlineTextureW = st.outlineTextureH = 1;
    }
    st.outlineFill = OutlineFillMode::Texture;
    RequestPresentationRebuild(layerIdx);
    m_IsDocumentModified = true;
    Logger::Get().Info("Outline texture (import) key=" + key);
    return true;
}

bool Canvas::BindOutlineTextureAsset(int layerIdx, int styleIdx, const std::string& assetKey) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    Layer& layer = m_Layers[layerIdx];
    if (styleIdx < 0 || styleIdx >= (int)layer.styles.size()) return false;
    LayerStyle& st = layer.styles[styleIdx];

    auto releasePrev = [&]() {
        if (!st.outlineTextureAssetKey.empty()) {
            assets::AssetStore::Get().Release(st.outlineTextureAssetKey);
            st.outlineTextureAssetKey.clear();
        }
        st.outlineTextureRgba.clear();
        st.outlineTextureW = st.outlineTextureH = 0;
        st.outlineTexturePath.clear();
    };

    if (assetKey.empty()) {
        releasePrev();
        RequestPresentationRebuild(layerIdx);
        m_IsDocumentModified = true;
        return true;
    }

    if (!assets::AssetManager::Get().IsKindAllowed(assetKey, assets::AssetKind::Texture)) {
        assets::AssetCategory cat;
        std::string rest;
        bool okKind = false;
        if (assets::ParseKey(assetKey, cat, rest)) {
            auto k = assets::GuessKindFromPath(rest);
            okKind = (k == assets::AssetKind::Texture || k == assets::AssetKind::Unknown);
        }
        if (!okKind) {
            Logger::Get().Error("BindOutlineTextureAsset: kind not Texture for " + assetKey);
            return false;
        }
    }

    std::string key = assets::AssetStore::Get().AcquireKey(assetKey);
    if (key.empty()) {
        assets::AssetStore::Get().RequestLoad(assetKey);
        if (!assets::AssetStore::Get().AddRef(assetKey)) {
            assets::AssetStore::Get().EnsureMeta(assetKey, assets::AssetKind::Texture, {}, {});
            assets::AssetStore::Get().AddRef(assetKey);
        }
        key = assetKey;
    }

    releasePrev();
    st.outlineTextureAssetKey = key;
    st.outlineFill = OutlineFillMode::Texture;
    int tw = 0, th = 0;
    if (assets::AssetStore::Get().GetDims(key, tw, th)) {
        st.outlineTextureW = tw;
        st.outlineTextureH = th;
    } else {
        st.outlineTextureW = st.outlineTextureH = 1;
    }
    RequestPresentationRebuild(layerIdx);
    m_IsDocumentModified = true;
    Logger::Get().Info("Outline texture bound key=" + key);
    return true;
}

void Canvas::CreateFillLayer(ID3D11Device* device, const std::string& name, const FillLayerParams& params) {
    Layer layer;
    layer.name = name;
    layer.visible = true;
    layer.opacity = 1.f;
    layer.type = Layer::Type::Fill;
    layer.isGroup = false;
    layer.fill = params;
    layer.fill.EnsureDefaults();
    layer.alphaRewrite = true;
    layer.SyncWorkSpaceFromFillTarget(nullptr);
    // No paint tiles for fill content
    layer.tileCache.reset();
    layer.filtersDirty = false;
    layer.stylesDirty = false;
    layer.presentationDirty = false;

    if (device && m_Width > 0 && m_Height > 0) {
        if (!m_CompositeTexture) CreateCompositeResources(device);
        EnsureFillLayerGpu(device, layer);
    }

    int insertAt = (int)m_Layers.size();
    m_Layers.push_back(std::move(layer));
    m_ActiveLayerIdx = insertAt;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    {
        auto snap = CaptureLayerSnap(m_Layers[insertAt], insertAt, m_Width, m_Height, m_CanvasFormat);
        m_UndoRedoManager.PushCommand(std::make_shared<LayerStackCommand>(
            "New Fill Layer", LayerStackCommand::Kind::Insert, std::move(snap)));
    }
    Logger::Get().Info("Created fill layer: " + name);
}

bool Canvas::IsFillLayer(int layerIdx) const {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    return m_Layers[layerIdx].IsFill();
}

bool Canvas::CanPaintLayerContent(int layerIdx) const {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    return m_Layers[layerIdx].CanPaintContent();
}

void Canvas::RefreshCanvas(ID3D11Device* device) {
    // Kill GPU/tile ghosting: force full re-upload path + recompose.
    for (auto& L : m_Layers) {
        if (L.isGroup) continue;
        if (L.tileCache && !L.tileCache->IsEmpty())
            L.tileCache->MarkAllDirty();
        if (L.filteredCache && !L.filteredCache->IsEmpty())
            L.filteredCache->MarkAllDirty();
        if (L.presentationCache && !L.presentationCache->IsEmpty())
            L.presentationCache->MarkAllDirty();
        L.needsUpload = true;
        L.thumbDirty = true;
        L.maskNeedsUpload = L.hasMask;
        L.gpuDisplayKind = 0xFF;
        if (m_EffectsPreviewEnabled) {
            if (LayerFilterListHasEnabled(L.filters))
                L.filtersDirty = true;
            if (L.HasEnabledStyles()) {
                L.stylesDirty = true;
                L.presentationDirty = true;
            }
        }
        // Drop GPU layer textures + sparse surfaces so ComposeLayers recreates clean views
        if (device) {
            if (L.texture) { L.texture->Release(); L.texture = nullptr; }
            if (L.srv) { L.srv->Release(); L.srv = nullptr; }
            if (L.gpuSurfaceId) {
                m_GpuTiles.DestroySurface(L.gpuSurfaceId);
                L.gpuSurfaceId = 0;
            }
        }
    }
    MarkCompositeDirty();
    m_ChannelPreviewDirty = true;
    m_SelectionMaskNeedsUpload = m_HasSelection;
    Logger::Get().InfoTag("gpu", "RefreshCanvas: forced tile re-upload + recompose");
}

void Canvas::SetEffectsPreviewEnabled(bool enabled) {
    if (m_EffectsPreviewEnabled == enabled) return;
    m_EffectsPreviewEnabled = enabled;
    if (enabled) {
        // Re-bake everything that has FX after temporary disable.
        for (auto& L : m_Layers) {
            if (L.HasEnabledStyles() || LayerFilterListHasEnabled(L.filters)) {
                L.stylesDirty = true;
                L.presentationDirty = true;
                L.filtersDirty = true;
                L.needsUpload = true;
            }
        }
        Logger::Get().Info("Effects preview ON — rebuilding presentations");
    } else {
        Logger::Get().Info("Effects preview OFF — raw content (no CPU FX bake)");
    }
    // Full refresh so FX on/off never leaves ghost tiles
    RefreshCanvas(nullptr);
}

bool Canvas::LayerStylesPreviewActive(const Layer& layer) const {
    return m_EffectsPreviewEnabled && layer.HasEnabledStyles();
}

bool Canvas::LayerFxPreviewActive(const Layer& layer) const {
    return m_EffectsPreviewEnabled &&
        (layer.HasEnabledStyles() || LayerFilterListHasEnabled(layer.filters));
}

int Canvas::AddLayerStyle(int layerIdx, StyleType type) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return -1;
    Layer& layer = m_Layers[layerIdx];
    auto before = CaptureLayerProps(layer);
    LayerStyle s;
    s.type = type;
    s.enabled = true;
    if (type == StyleType::Shadow) {
        s.opacity = 0.75f;
        s.distance = 8.f;
        s.size = 8.f;
        s.angleDeg = 120.f;
    } else {
        s.opacity = 1.f;
        s.outlineSize = 2.f;
        s.outlinePos = OutlinePosition::Outside;
    }
    layer.styles.push_back(s);
    layer.gpuDisplayKind = 0xFF;
    m_IsDocumentModified = true;
    CommitLayerPropsEdit(layerIdx, before, "Add Layer Style");
    // Debounced bake — never full-res float composite on the same click (crash/lag on 4K+)
    RequestPresentationRebuild(layerIdx);
    return (int)layer.styles.size() - 1;
}

void Canvas::RemoveLayerStyle(int layerIdx, int styleIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[layerIdx];
    if (styleIdx < 0 || styleIdx >= (int)layer.styles.size()) return;
    auto before = CaptureLayerProps(layer);
    layer.styles.erase(layer.styles.begin() + styleIdx);
    layer.stylesDirty = true;
    layer.presentationDirty = true;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    CommitLayerPropsEdit(layerIdx, before, "Remove Layer Style");
}

void Canvas::MarkLayerStylesDirty(int layerIdx) {
    RequestPresentationRebuild(layerIdx);
}

void Canvas::RequestPresentationRebuild(int layerIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
    m_Layers[layerIdx].stylesDirty = true;
    m_Layers[layerIdx].presentationDirty = true;
    m_CompositeDirty = true;
    // Debounce CPU style bake while user drags sliders
    m_PresentationRebuildNotBefore =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
    m_PresentationRebuildDeferred = true;
    // Group FX above this layer must re-flatten after style edits.
    int p = m_Layers[layerIdx].parentGroupId;
    while (p >= 0 && p < (int)m_Layers.size()) {
        if (m_Layers[p].isGroup) {
            m_Layers[p].presentationDirty = true;
            m_Layers[p].stylesDirty = true;
            m_Layers[p].filtersDirty = true;
        }
        p = m_Layers[p].parentGroupId;
    }
}

void Canvas::NotifyLayerVisualsChanged(int layerIdx, bool contentPixelsChanged,
                                       bool markFiltersDirty) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return;
    Layer& L = m_Layers[layerIdx];
    L.thumbDirty = true;
    bool touchedGroupFx = false;
    if (contentPixelsChanged) {
        L.gpuDisplayKind = 0xFF; // raw ↔ filtered ↔ presentation re-bind
        L.needsUpload = true;
        if (markFiltersDirty && LayerFilterListHasEnabled(L.filters))
            L.filtersDirty = true;
        if (L.HasEnabledStyles()) {
            L.presentationDirty = true;
            L.stylesDirty = true;
        }
        if (L.isGroup) {
            L.presentationDirty = true;
            L.stylesDirty = true;
            L.filtersDirty = true;
            touchedGroupFx = true;
        }
    }
    // Ancestor groups: always re-compose (no-FX GPU path) or re-bake (FX path).
    int p = L.parentGroupId;
    while (p >= 0 && p < (int)m_Layers.size()) {
        if (m_Layers[p].isGroup) {
            m_Layers[p].presentationDirty = true;
            m_Layers[p].stylesDirty = true;
            m_Layers[p].filtersDirty = true;
            m_Layers[p].thumbDirty = true;
            m_Layers[p].needsUpload = true;
            touchedGroupFx = true;
        }
        p = m_Layers[p].parentGroupId;
    }
    // Membership / structure must rebuild on the next frame — do not wait slider debounce
    // (otherwise compose keeps drawing the previous group presentation bake → "layer vanished").
    if (touchedGroupFx || (contentPixelsChanged && L.isGroup)) {
        m_PresentationRebuildDeferred = false;
        m_PresentationRebuildNotBefore = std::chrono::steady_clock::now();
    }
    MarkCompositeDirty();
}

bool Canvas::IsLayerUnderGroup(int layerIdx, int groupIdx) const {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    if (groupIdx < 0 || groupIdx >= (int)m_Layers.size()) return false;
    int p = m_Layers[layerIdx].parentGroupId;
    while (p >= 0 && p < (int)m_Layers.size()) {
        if (p == groupIdx) return true;
        p = m_Layers[p].parentGroupId;
    }
    return false;
}

void Canvas::RebuildGroupPresentation(int groupIdx, bool fullQuality) {
    if (groupIdx < 0 || groupIdx >= (int)m_Layers.size()) return;
    Layer& group = m_Layers[groupIdx];
    if (!group.isGroup) return;

    if (!fullQuality && m_IsStrokeActive) return;
    if (!fullQuality && m_PresentationRebuildDeferred) {
        if (std::chrono::steady_clock::now() < m_PresentationRebuildNotBefore)
            return;
        m_PresentationRebuildDeferred = false;
    }
    if (m_Width <= 0 || m_Height <= 0) {
        group.stylesDirty = group.presentationDirty = group.filtersDirty = false;
        return;
    }

    // Ensure children presentations/filters are ready (also proxy when interactive)
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        if (i == groupIdx || !IsLayerUnderGroup(i, groupIdx)) continue;
        Layer& ch = m_Layers[i];
        if (ch.isGroup) continue;
        if (ch.filtersDirty) RebuildFilteredPixels(ch);
        if (ch.HasEnabledStyles() && (ch.presentationDirty || ch.stylesDirty || !ch.presentationCache))
            RebuildLayerPresentation(ch, fullQuality);
    }

    // P2: interactive group FX always at proxy res on large docs (was full float 16K → multi‑GiB / multi‑s).
    int bakeW = m_Width, bakeH = m_Height;
    if (!fullQuality)
        ComputeCompositePreviewSize(m_Width, m_Height, bakeW, bakeH);
    const bool useProxy = (bakeW != m_Width || bakeH != m_Height);
    const float invScaleX = useProxy ? (float)m_Width / (float)bakeW : 1.f;
    const float invScaleY = useProxy ? (float)m_Height / (float)bakeH : 1.f;
    const float scaleX = useProxy ? (float)bakeW / (float)m_Width : 1.f;
    const float scaleY = useProxy ? (float)bakeH / (float)m_Height : 1.f;

    if (fullQuality && !CanAllocateFlatComposite(m_Width, m_Height, "RebuildGroupPresentation")) {
        Logger::Get().WarnTag("fx",
            "Group FX full bake refused — falling back to proxy for " +
            std::to_string(m_Width) + "x" + std::to_string(m_Height));
        ComputeCompositePreviewSize(m_Width, m_Height, bakeW, bakeH);
    }

    std::vector<float> acc;
    try {
        acc.assign((size_t)bakeW * (size_t)bakeH * 4u, 0.f);
    } catch (const std::bad_alloc&) {
        Logger::Get().ErrorTag("fx", "OOM group flatten buffer");
        group.stylesDirty = group.presentationDirty = group.filtersDirty = false;
        return;
    }

    auto sampleChildToBake = [&](Layer& L, std::vector<float>& outBake) {
        outBake.assign((size_t)bakeW * (size_t)bakeH * 4u, 0.f);
        const TileCache* src = nullptr;
        if (L.HasEnabledStyles() && L.presentationCache && !L.presentationCache->IsEmpty())
            src = L.presentationCache.get();
        else if (L.filteredCache && !L.filteredCache->IsEmpty() && LayerFilterListHasEnabled(L.filters))
            src = L.filteredCache.get();
        else if (L.tileCache && !L.tileCache->IsEmpty())
            src = L.tileCache.get();

        if (L.IsFill() && !src) {
            float c[4];
            L.fill.ResolveRgba(c);
            for (size_t i = 0, n = (size_t)bakeW * bakeH; i < n; ++i) {
                outBake[i * 4 + 0] = c[0];
                outBake[i * 4 + 1] = c[1];
                outBake[i * 4 + 2] = c[2];
                outBake[i * 4 + 3] = c[3] * L.opacity;
            }
            return;
        }
        if (!src) return;

        const int sw = src->GetWidth();
        const int sh = src->GetHeight();
        for (int y = 0; y < bakeH; ++y) {
            int syDoc = useProxy
                ? std::min(m_Height - 1, (int)(y * invScaleY + 0.5f))
                : y;
            int sy = (sh == m_Height) ? syDoc
                : std::min(sh - 1, (int)((float)syDoc * ((float)sh / (float)std::max(1, m_Height)) + 0.5f));
            for (int x = 0; x < bakeW; ++x) {
                int sxDoc = useProxy
                    ? std::min(m_Width - 1, (int)(x * invScaleX + 0.5f))
                    : x;
                int sx = (sw == m_Width) ? sxDoc
                    : std::min(sw - 1, (int)((float)sxDoc * ((float)sw / (float)std::max(1, m_Width)) + 0.5f));
                float px[4];
                src->GetPixelF(sx, sy, px);
                // Child styles already bake fill opacity; raw path apply layer opacity.
                if (!(L.HasEnabledStyles() && L.presentationCache && !L.presentationCache->IsEmpty())) {
                    px[3] *= L.opacity;
                }
                size_t di = ((size_t)y * bakeW + x) * 4;
                outBake[di + 0] = px[0];
                outBake[di + 1] = px[1];
                outBake[di + 2] = px[2];
                outBake[di + 3] = px[3];
            }
        }
    };

    // Flatten children bottom → top at bake resolution
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        Layer& L = m_Layers[i];
        if (L.isGroup || !L.visible || !IsLayerUnderGroup(i, groupIdx)) continue;
        std::vector<float> content;
        try {
            sampleChildToBake(L, content);
        } catch (const std::bad_alloc&) {
            Logger::Get().ErrorTag("fx", "OOM sampling group child");
            continue;
        }
        if (!content.empty())
            layer_fx::CompositeOver(acc.data(), content.data(), bakeW * bakeH);
    }

    // Group filters + styles on flattened sum (geometry scaled to bake res)
    layer_fx::PresentationParams gpp;
    gpp.fillOpacity = group.opacity;
    gpp.bakeFillOpacity = true;
    gpp.previewQuality = !fullQuality;

    std::vector<uint8_t> bakeMask;
    if (group.hasMask) {
        bakeMask.resize((size_t)bakeW * bakeH, 255);
        for (int y = 0; y < bakeH; ++y) {
            int sy = useProxy ? std::min(m_Height - 1, (int)(y * invScaleY + 0.5f)) : y;
            for (int x = 0; x < bakeW; ++x) {
                int sx = useProxy ? std::min(m_Width - 1, (int)(x * invScaleX + 0.5f)) : x;
                uint8_t mv = 255;
                if (group.maskTiles && group.maskTiles->Valid())
                    mv = group.maskTiles->Get(sx, sy);
                else if (group.mask.size() == (size_t)m_Width * m_Height)
                    mv = group.mask[(size_t)sy * m_Width + sx];
                bakeMask[(size_t)y * bakeW + x] = mv;
            }
        }
        gpp.hasMask = true;
        gpp.mask = bakeMask.data();
        gpp.maskBytes = bakeMask.size();
    } else {
        gpp.hasMask = false;
        gpp.mask = nullptr;
        gpp.maskBytes = 0;
    }

    if (!group.filters.empty() || group.HasEnabledStyles()) {
        std::vector<LayerStyle> scaledStyles = group.styles;
        for (auto& st : scaledStyles) {
            st.distance *= scaleX;
            st.offsetX *= scaleX;
            st.offsetY *= scaleY;
            st.size *= (scaleX + scaleY) * 0.5f;
            st.outlineSize *= (scaleX + scaleY) * 0.5f;
        }
        try {
            acc = layer_fx::BuildPresentation(acc, bakeW, bakeH, group.filters, scaledStyles, gpp);
        } catch (const std::bad_alloc&) {
            Logger::Get().ErrorTag("fx", "OOM group style bake");
            group.stylesDirty = group.presentationDirty = group.filtersDirty = false;
            return;
        }
    } else {
        for (size_t i = 0, n = (size_t)bakeW * bakeH; i < n; ++i)
            acc[i * 4 + 3] = std::clamp(acc[i * 4 + 3] * group.opacity, 0.f, 1.f);
    }

    // Prefer full-doc presentation only when fullQuality and RAM allows; else keep proxy
    // (draw stretches proxy texture over canvas — fine for interactive preview).
    int outW = bakeW, outH = bakeH;
    std::vector<float> out;
    if (fullQuality && bakeW == m_Width && bakeH == m_Height) {
        out = std::move(acc);
        outW = m_Width;
        outH = m_Height;
    } else if (fullQuality && CanAllocateFlatComposite(m_Width, m_Height, "RebuildGroupPresentation.upsample")) {
        try {
            out.resize((size_t)m_Width * m_Height * 4);
            for (int y = 0; y < m_Height; ++y) {
                int sy = std::min(bakeH - 1, (int)(y * scaleY));
                for (int x = 0; x < m_Width; ++x) {
                    int sx = std::min(bakeW - 1, (int)(x * scaleX));
                    size_t di = ((size_t)y * m_Width + x) * 4;
                    size_t si = ((size_t)sy * bakeW + x) * 4;
                    // fix: sx not x
                    si = ((size_t)sy * bakeW + sx) * 4;
                    out[di + 0] = acc[si + 0];
                    out[di + 1] = acc[si + 1];
                    out[di + 2] = acc[si + 2];
                    out[di + 3] = acc[si + 3];
                }
            }
            outW = m_Width;
            outH = m_Height;
        } catch (const std::bad_alloc&) {
            out = std::move(acc);
            outW = bakeW;
            outH = bakeH;
        }
    } else {
        out = std::move(acc);
        outW = bakeW;
        outH = bakeH;
    }

    if (!group.presentationCache)
        group.presentationCache = std::make_unique<TileCache>();
    group.presentationCache->Init(outW, outH, m_CanvasFormat);
    group.presentationCache->ImportRGBA32F(out.data(), outW, outH);
    group.presentationCache->MarkAllDirty();
    group.stylesDirty = false;
    group.presentationDirty = false;
    group.filtersDirty = false;
    group.needsUpload = true;
    // Force classic texture recreate to match proxy presentation size next compose.
    if (group.texture && (outW != m_Width || outH != m_Height)) {
        DeferReleaseTex(group.texture);
        group.texture = nullptr;
        if (group.srv) { DeferReleaseSRV(group.srv); group.srv = nullptr; }
    }
}

// ============================================================
//  Layer Groups
// ============================================================

void Canvas::CreateGroupCompositeResources(ID3D11Device* device) {
    ReleaseGroupCompositeResources();
    // Match proxy composite size (not full 16K) — same as main composite RT.
    const int gw = std::max(1, m_CompositeWidth > 0 ? m_CompositeWidth : m_Width);
    const int gh = std::max(1, m_CompositeHeight > 0 ? m_CompositeHeight : m_Height);
    D3D11_TEXTURE2D_DESC desc={};
    desc.Width=(UINT)gw; desc.Height=(UINT)gh; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=GetLayerDxgiFormat(); desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&m_GroupCompositeTexture))) {
        device->CreateRenderTargetView(m_GroupCompositeTexture,nullptr,&m_GroupCompositeRTV);
        device->CreateShaderResourceView(m_GroupCompositeTexture,nullptr,&m_GroupCompositeSRV);
    }
}
void Canvas::ReleaseGroupCompositeResources() {
    if (m_GroupCompositeTexture){m_GroupCompositeTexture->Release();m_GroupCompositeTexture=nullptr;}
    if (m_GroupCompositeRTV){m_GroupCompositeRTV->Release();m_GroupCompositeRTV=nullptr;}
    if (m_GroupCompositeSRV){m_GroupCompositeSRV->Release();m_GroupCompositeSRV=nullptr;}
}
void Canvas::CreateLayerGroup(ID3D11Device* device, const std::string& name) {
    Layer grp; grp.name=name; grp.isGroup=true; grp.type=Layer::Type::Group;
    grp.visible=true; grp.opacity=1.f; grp.blendMode=BlendMode::Normal;
    m_Layers.push_back(std::move(grp));
    m_ActiveLayerIdx=(int)m_Layers.size()-1;
    m_CompositeDirty = true;
    Logger::Get().Info("Created layer group: "+name);
}
void Canvas::AddLayerToGroup(int layerIdx, int groupLayerIdx) {
    if (layerIdx<0||layerIdx>=(int)m_Layers.size()||groupLayerIdx<0||groupLayerIdx>=(int)m_Layers.size()) return;
    if (!m_Layers[groupLayerIdx].isGroup) return;
    if (layerIdx == groupLayerIdx) return;
    m_Layers[layerIdx].parentGroupId=groupLayerIdx;
    // Group isolation path + FX bake must see new membership immediately.
    NotifyLayerVisualsChanged(layerIdx, /*contentPixelsChanged=*/false, /*markFiltersDirty=*/false);
    NotifyLayerVisualsChanged(groupLayerIdx, /*contentPixelsChanged=*/true, /*markFiltersDirty=*/true);
}
void Canvas::RemoveLayerFromGroup(int layerIdx) {
    if (layerIdx>=0&&layerIdx<(int)m_Layers.size()) {
        const int oldParent = m_Layers[layerIdx].parentGroupId;
        m_Layers[layerIdx].parentGroupId=-1;
        NotifyLayerVisualsChanged(layerIdx, /*contentPixelsChanged=*/false, /*markFiltersDirty=*/false);
        if (oldParent >= 0)
            NotifyLayerVisualsChanged(oldParent, /*contentPixelsChanged=*/true, /*markFiltersDirty=*/true);
    }
}

static int MapLayerIndexAfterReorder(int j, int fromIdx, int toIdx) {
    if (fromIdx == toIdx) return j;
    if (fromIdx < toIdx) {
        if (j == fromIdx) return toIdx;
        if (j > fromIdx && j <= toIdx) return j - 1;
        return j;
    }
    if (j == fromIdx) return toIdx;
    if (j >= toIdx && j < fromIdx) return j + 1;
    return j;
}

int Canvas::ReorderLayer(int fromIdx, int toIdx) {
    auto& layers = m_Layers;
    if (fromIdx == toIdx || fromIdx < 0 || fromIdx >= (int)layers.size() ||
        toIdx < 0 || toIdx >= (int)layers.size()) return fromIdx;

    // Capture parents before reorder — groups with FX keep a baked presentationCache.
    // Without invalidation, old bake is drawn and the moved layer "vanishes" until stroke.
    const int oldParent = layers[fromIdx].parentGroupId;

    const int n = (int)layers.size();
    std::vector<int> identity(n), parentIdent(n, -1);
    for (int i = 0; i < n; ++i) {
        identity[i] = i;
        int pid = layers[i].parentGroupId;
        parentIdent[i] = (pid >= 0 && pid < n) ? pid : -1;
    }
    if (fromIdx < toIdx) {
        std::rotate(layers.begin() + fromIdx, layers.begin() + fromIdx + 1, layers.begin() + toIdx + 1);
        std::rotate(identity.begin() + fromIdx, identity.begin() + fromIdx + 1, identity.begin() + toIdx + 1);
        std::rotate(parentIdent.begin() + fromIdx, parentIdent.begin() + fromIdx + 1, parentIdent.begin() + toIdx + 1);
    } else {
        std::rotate(layers.begin() + toIdx, layers.begin() + fromIdx, layers.begin() + fromIdx + 1);
        std::rotate(identity.begin() + toIdx, identity.begin() + fromIdx, identity.begin() + fromIdx + 1);
        std::rotate(parentIdent.begin() + toIdx, parentIdent.begin() + fromIdx, parentIdent.begin() + fromIdx + 1);
    }
    std::vector<int> identToNew(n, -1);
    for (int i = 0; i < n; ++i) identToNew[identity[i]] = i;
    for (int i = 0; i < n; ++i)
        layers[i].parentGroupId = (parentIdent[i] >= 0) ? identToNew[parentIdent[i]] : -1;

    int newIdx = identToNew[fromIdx];
    if (m_ActiveLayerIdx == fromIdx) m_ActiveLayerIdx = newIdx;
    else m_ActiveLayerIdx = MapLayerIndexAfterReorder(m_ActiveLayerIdx, fromIdx, toIdx);

    const int newParent = layers[newIdx].parentGroupId;
    const int mappedOldParent = (oldParent >= 0 && oldParent < n) ? identToNew[oldParent] : -1;
    // Moved layer + both old/new group FX chains
    NotifyLayerVisualsChanged(newIdx, /*contentPixelsChanged=*/false, /*markFiltersDirty=*/false);
    if (mappedOldParent >= 0)
        NotifyLayerVisualsChanged(mappedOldParent, /*contentPixelsChanged=*/true, /*markFiltersDirty=*/true);
    if (newParent >= 0 && newParent != mappedOldParent)
        NotifyLayerVisualsChanged(newParent, /*contentPixelsChanged=*/true, /*markFiltersDirty=*/true);
    return newIdx;
}

int Canvas::MoveLayerIntoGroup(int layerIdx, int groupIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size() ||
        groupIdx < 0 || groupIdx >= (int)m_Layers.size()) return layerIdx;
    if (layerIdx == groupIdx || !m_Layers[groupIdx].isGroup) return layerIdx;
    if (m_Layers[layerIdx].isGroup) return layerIdx;

    const int oldParent = m_Layers[layerIdx].parentGroupId;

    int targetIdx = (layerIdx > groupIdx) ? groupIdx
                                         : ((groupIdx > 0) ? groupIdx - 1 : 0);
    int newLayerIdx = ReorderLayer(layerIdx, targetIdx);
    int newGroupIdx = MapLayerIndexAfterReorder(groupIdx, layerIdx, targetIdx);
    if (newGroupIdx >= 0 && newGroupIdx < (int)m_Layers.size() && m_Layers[newGroupIdx].isGroup)
        m_Layers[newLayerIdx].parentGroupId = newGroupIdx;
    else
        m_Layers[newLayerIdx].parentGroupId = -1;

    // Force group FX re-flatten (ReorderLayer may not see the final parent yet).
    NotifyLayerVisualsChanged(newLayerIdx, /*contentPixelsChanged=*/false, /*markFiltersDirty=*/false);
    if (newGroupIdx >= 0)
        NotifyLayerVisualsChanged(newGroupIdx, /*contentPixelsChanged=*/true, /*markFiltersDirty=*/true);
    if (oldParent >= 0) {
        const int mappedOld = MapLayerIndexAfterReorder(oldParent, layerIdx, targetIdx);
        if (mappedOld >= 0 && mappedOld != newGroupIdx)
            NotifyLayerVisualsChanged(mappedOld, /*contentPixelsChanged=*/true, /*markFiltersDirty=*/true);
    }
    return newLayerIdx;
}

void Canvas::ApplyPolygonalLassoSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract) {
    // Same polygon fill as freehand lasso; UI supplies straight-line vertices.
    ApplyLassoSelection(points, add, subtract);
}

// ---------------------------------------------------------------------------
// Quick Selection (non-AI constrained region grow)
// ---------------------------------------------------------------------------
static void RgbToLabApprox(float r, float g, float b, float& L, float& a, float& bb) {
    // Cheap linear approx good enough for relative distance
    L  = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    a  = r - g;
    bb = 0.5f * (r + g) - b;
}

void Canvas::BeginQuickSelectStroke() {
    const size_t n = (size_t)std::max(0, m_Width) * (size_t)std::max(0, m_Height);
    if (n == 0) return;

    // Snapshot selection at stroke start (progressive base + undo "old")
    m_QuickSelectBaseHas = m_HasSelection;
    if (m_HasSelection && m_SelectionMask.size() == n)
        m_QuickSelectBaseMask = m_SelectionMask;
    else
        m_QuickSelectBaseMask.assign(n, 0);

    m_QuickSelectMask.assign(n, 0);
    m_QuickSelectSampleCount = 0;
    m_QuickSelectLabMean[0] = m_QuickSelectLabMean[1] = m_QuickSelectLabMean[2] = 0.f;
    m_QuickSelectSubtract = false;
    m_QuickSelectStrokeActive = true;

    // Fresh wand source each stroke (layer may have changed)
    InvalidateWandSourceCache();
    EnsureWandSourceCache();

    if (!m_QuickSelectEdgeValid) {
        std::vector<float> src;
        if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size() && !m_Layers[m_ActiveLayerIdx].isGroup)
            src = ExportLayerF(m_Layers[m_ActiveLayerIdx], m_Width, m_Height);
        else
            src = GetCompositePixels();
        cv::Mat gray(m_Height, m_Width, CV_8UC1);
        for (int i = 0; i < m_Width * m_Height; ++i) {
            float y = 0.2126f * src[i * 4] + 0.7152f * src[i * 4 + 1] + 0.0722f * src[i * 4 + 2];
            gray.data[i] = (uint8_t)(std::clamp(y, 0.f, 1.f) * 255.f + 0.5f);
        }
        cv::Mat gradX, gradY, mag;
        cv::Sobel(gray, gradX, CV_32F, 1, 0, 3);
        cv::Sobel(gray, gradY, CV_32F, 0, 1, 3);
        cv::magnitude(gradX, gradY, mag);
        double minV, maxV;
        cv::minMaxLoc(mag, &minV, &maxV);
        m_QuickSelectEdge.assign(n, 0);
        float inv = (maxV > 1e-6) ? (1.0f / (float)maxV) : 1.0f;
        for (int i = 0; i < m_Width * m_Height; ++i) {
            float e = mag.at<float>(i / m_Width, i % m_Width) * inv;
            m_QuickSelectEdge[i] = (uint8_t)(std::clamp(e, 0.f, 1.f) * 255.f + 0.5f);
        }
        m_QuickSelectEdgeValid = true;
    }
}

void Canvas::StrokeQuickSelect(ID3D11Device* device, const std::vector<std::pair<int, int>>& points,
                               float radius, bool subtract) {
    if (points.empty() || m_Width <= 0 || m_Height <= 0) return;
    if (!m_QuickSelectStrokeActive || m_QuickSelectMask.size() != (size_t)m_Width * m_Height)
        BeginQuickSelectStroke();

    m_QuickSelectSubtract = subtract;
    EnsureWandSourceCache();
    const int r = std::max(1, (int)std::lround(radius));
    const int r2 = r * r;

    // Process only the newest segment tip (last point) for progressive feel
    const auto& pt = points.back();
    int cx = pt.first, cy = pt.second;

    // Stamp brush disc into stroke mask
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r2) continue;
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) continue;
            size_t idx = (size_t)y * m_Width + x;
            if (subtract) {
                // Alt: mark pixels to carve out of base selection
                m_QuickSelectMask[idx] = 255;
                continue;
            }
            m_QuickSelectMask[idx] = 255;
            if (m_WandSourceRGBA.size() < (idx + 1) * 4) continue;
            float rf = m_WandSourceRGBA[idx * 4 + 0] / 255.f;
            float gf = m_WandSourceRGBA[idx * 4 + 1] / 255.f;
            float bf = m_WandSourceRGBA[idx * 4 + 2] / 255.f;
            float L, a, b;
            RgbToLabApprox(rf, gf, bf, L, a, b);
            int n = m_QuickSelectSampleCount;
            m_QuickSelectLabMean[0] = (m_QuickSelectLabMean[0] * n + L) / (n + 1);
            m_QuickSelectLabMean[1] = (m_QuickSelectLabMean[1] * n + a) / (n + 1);
            m_QuickSelectLabMean[2] = (m_QuickSelectLabMean[2] * n + b) / (n + 1);
            m_QuickSelectSampleCount = n + 1;
        }
    }

    // Add mode: region grow from brush seeds (PS quick-select feel)
    if (!subtract && m_QuickSelectSampleCount > 0) {
        const float colorThresh = 0.18f;
        const float edgeStop = 0.45f;
        std::vector<std::pair<int, int>> q;
        q.reserve(256);
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r2) continue;
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) continue;
                if (m_QuickSelectMask[(size_t)y * m_Width + x]) q.push_back({x, y});
            }
        const int margin = r * 3;
        int minX = std::max(0, cx - margin), maxX = std::min(m_Width - 1, cx + margin);
        int minY = std::max(0, cy - margin), maxY = std::min(m_Height - 1, cy + margin);
        size_t head = 0;
        const int ndx[4] = {1, -1, 0, 0};
        const int ndy[4] = {0, 0, 1, -1};
        while (head < q.size()) {
            auto [x, y] = q[head++];
            for (int n = 0; n < 4; ++n) {
                int nx = x + ndx[n], ny = y + ndy[n];
                if (nx < minX || ny < minY || nx > maxX || ny > maxY) continue;
                size_t nidx = (size_t)ny * m_Width + nx;
                if (m_QuickSelectMask[nidx]) continue;
                float edge = m_QuickSelectEdge.empty() ? 0.f : m_QuickSelectEdge[nidx] / 255.f;
                if (edge >= edgeStop) continue;
                if (m_WandSourceRGBA.size() < (nidx + 1) * 4) continue;
                float rf = m_WandSourceRGBA[nidx * 4 + 0] / 255.f;
                float gf = m_WandSourceRGBA[nidx * 4 + 1] / 255.f;
                float bf = m_WandSourceRGBA[nidx * 4 + 2] / 255.f;
                float L, a, b;
                RgbToLabApprox(rf, gf, bf, L, a, b);
                float dL = L - m_QuickSelectLabMean[0];
                float da = a - m_QuickSelectLabMean[1];
                float db = b - m_QuickSelectLabMean[2];
                float dist = std::sqrt(dL * dL + da * da + db * db);
                if (dist > colorThresh * (1.0f + edge)) continue;
                m_QuickSelectMask[nidx] = 255;
                q.push_back({nx, ny});
            }
        }
    }

    // Live publish → marching ants update every dab
    {
        const size_t n = m_QuickSelectMask.size();
        if (n > 0 && m_QuickSelectBaseMask.size() == n) {
            m_SelectionMask.resize(n);
            m_HasSelection = false;
            for (size_t i = 0; i < n; ++i) {
                uint8_t v = subtract
                    ? (uint8_t)(m_QuickSelectBaseMask[i] & (uint8_t)~m_QuickSelectMask[i])
                    : (uint8_t)(m_QuickSelectBaseMask[i] | m_QuickSelectMask[i]);
                m_SelectionMask[i] = v;
                if (v) m_HasSelection = true;
            }
            m_SelectionMaskNeedsUpload = true;
        }
    }
    if (device) UpdateSelectionMaskTexture(device);
}

void Canvas::CancelQuickSelectStroke() {
    if (!m_QuickSelectStrokeActive) {
        m_QuickSelectMask.clear();
        return;
    }
    // Restore selection as it was at stroke begin
    m_SelectionMask = m_QuickSelectBaseMask;
    m_HasSelection = m_QuickSelectBaseHas;
    m_SelectionMaskNeedsUpload = true;
    m_QuickSelectMask.clear();
    m_QuickSelectBaseMask.clear();
    m_QuickSelectSampleCount = 0;
    m_QuickSelectLabMean[0] = m_QuickSelectLabMean[1] = m_QuickSelectLabMean[2] = 0.f;
    m_QuickSelectStrokeActive = false;
}

void Canvas::EndQuickSelectStroke(ID3D11Device* device, bool subtract) {
    if (!m_QuickSelectStrokeActive || m_QuickSelectMask.size() != (size_t)m_Width * m_Height) {
        m_QuickSelectStrokeActive = false;
        return;
    }
    // Prefer mode used during stroke (Alt may be released before mouse-up)
    const bool sub = m_QuickSelectSubtract || subtract;

    // Light morpho on stroke contribution, then re-merge with base
    if (!sub) {
        cv::Mat m(m_Height, m_Width, CV_8UC1);
        std::memcpy(m.data, m_QuickSelectMask.data(), m_QuickSelectMask.size());
        cv::Mat smoothed;
        cv::morphologyEx(m, smoothed, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
        cv::morphologyEx(smoothed, smoothed, cv::MORPH_OPEN,
                         cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
        std::memcpy(m_QuickSelectMask.data(), smoothed.data, m_QuickSelectMask.size());
    }

    std::vector<uint8_t> oldMask = m_QuickSelectBaseMask;
    bool oldHas = m_QuickSelectBaseHas;
    {
        const size_t n = m_QuickSelectMask.size();
        m_SelectionMask.resize(n);
        m_HasSelection = false;
        for (size_t i = 0; i < n; ++i) {
            uint8_t v = sub
                ? (uint8_t)(m_QuickSelectBaseMask[i] & (uint8_t)~m_QuickSelectMask[i])
                : (uint8_t)(m_QuickSelectBaseMask[i] | m_QuickSelectMask[i]);
            m_SelectionMask[i] = v;
            if (v) m_HasSelection = true;
        }
        m_SelectionMaskNeedsUpload = true;
    }
    if (device) UpdateSelectionMaskTexture(device);

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        sub ? "Quick Select Subtract" : "Quick Select",
        std::move(oldMask), oldHas, m_SelectionMask, m_HasSelection));

    m_QuickSelectMask.clear();
    m_QuickSelectBaseMask.clear();
    m_QuickSelectSampleCount = 0;
    m_QuickSelectStrokeActive = false;
}

// ---------------------------------------------------------------------------
// Canvas Edit / Crop
// ---------------------------------------------------------------------------
bool Canvas::GetSelectionBounds(int& outX, int& outY, int& outW, int& outH) const {
    if (!m_HasSelection || m_SelectionMask.empty()) return false;
    if (m_SelectionBoundsValid && m_SelectionBoundsW > 0 && m_SelectionBoundsH > 0) {
        outX = m_SelectionBoundsX; outY = m_SelectionBoundsY;
        outW = m_SelectionBoundsW; outH = m_SelectionBoundsH;
        return true;
    }
    // Slow path (const): scan full mask — avoid in hot paths by keeping cache valid.
    int minX = m_Width, minY = m_Height, maxX = -1, maxY = -1;
    for (int y = 0; y < m_Height; ++y) {
        const size_t row = (size_t)y * (size_t)m_Width;
        for (int x = 0; x < m_Width; ++x) {
            if (m_SelectionMask[row + (size_t)x] == 0) continue;
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY) return false;
    outX = minX; outY = minY;
    outW = maxX - minX + 1;
    outH = maxY - minY + 1;
    return true;
}

static DocumentGeometryCommand::DocSnap CaptureGeometrySnap(Canvas& canvas) {
    DocumentGeometryCommand::DocSnap snap;
    snap.width = canvas.GetWidth();
    snap.height = canvas.GetHeight();
    snap.selection = canvas.GetSelectionMask();
    snap.hasSelection = canvas.HasSelection();
    const int ntx = (snap.width  + TILE_SIZE - 1) / TILE_SIZE;
    const int nty = (snap.height + TILE_SIZE - 1) / TILE_SIZE;
    auto& layers = canvas.GetLayers();
    for (int i = 0; i < (int)layers.size(); ++i) {
        auto& layer = layers[i];
        if (layer.isGroup || !layer.tileCache) continue;
        DocumentGeometryCommand::LayerTiles lt;
        lt.layerIdx = i;
        lt.hasMask = layer.hasMask;
        lt.mask = layer.mask;
        for (int ty = 0; ty < nty; ++ty) {
            for (int tx = 0; tx < ntx; ++tx) {
                if (!layer.tileCache->HasTile(tx, ty)) continue;
                TileDelta d;
                d.layerIdx = i;
                d.tileX = tx;
                d.tileY = ty;
                d.newState = layer.tileCache->SnapshotTile(tx, ty);
                lt.tiles.push_back(std::move(d));
            }
        }
        snap.layers.push_back(std::move(lt));
    }
    return snap;
}

bool Canvas::CropCanvasToSelection(ID3D11Device* device) {
    int x, y, w, h;
    if (!GetSelectionBounds(x, y, w, h)) return false;
    return CropCanvasToRect(device, x, y, w, h);
}

bool Canvas::CropCanvasToRect(ID3D11Device* device, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    x = std::clamp(x, 0, m_Width - 1);
    y = std::clamp(y, 0, m_Height - 1);
    w = std::min(w, m_Width - x);
    h = std::min(h, m_Height - y);
    if (w <= 0 || h <= 0) return false;
    if (x == 0 && y == 0 && w == m_Width && h == m_Height) return false;

    auto oldSnap = CaptureGeometrySnap(*this);
    const int oldW = m_Width, oldH = m_Height;
    Logger::Get().Info("CropCanvasToRect " + std::to_string(x) + "," + std::to_string(y) +
                       " " + std::to_string(w) + "x" + std::to_string(h));

    // Capture selection crop
    std::vector<uint8_t> newSel;
    bool newHasSel = false;
    if (m_HasSelection && !m_SelectionMask.empty()) {
        newSel.assign((size_t)w * h, 0);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                uint8_t v = m_SelectionMask[(size_t)(y + yy) * oldW + (x + xx)];
                newSel[(size_t)yy * w + xx] = v;
                if (v) newHasSel = true;
            }
    }

    for (auto& layer : m_Layers) {
        if (layer.isGroup) continue;
        if (!layer.tileCache) continue;
        auto pixels = ExportLayerF(layer, oldW, oldH);
        std::vector<float> cropped((size_t)w * h * 4, 0.f);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                size_t si = ((size_t)(y + yy) * oldW + (x + xx)) * 4;
                size_t di = ((size_t)yy * w + xx) * 4;
                for (int c = 0; c < 4; ++c) cropped[di + c] = pixels[si + c];
            }
        if (layer.hasMask && !layer.mask.empty()) {
            std::vector<uint8_t> cm((size_t)w * h, 255);
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < w; ++xx)
                    cm[(size_t)yy * w + xx] = layer.mask[(size_t)(y + yy) * oldW + (x + xx)];
            layer.mask = std::move(cm);
            layer.maskNeedsUpload = true;
        }
        // Store cropped in temp; after size change import
        layer.tileCache->Clear();
        layer.tileCache->Init(w, h, m_CanvasFormat);
        layer.tileCache->ImportRGBA32F(cropped.data(), w, h);
        layer.tileCache->MarkAllDirty();
        layer.needsUpload = true;
        layer.filtersDirty = true;
    }

    m_Width = w;
    m_Height = h;
    m_SelectionMask = std::move(newSel);
    m_HasSelection = newHasSel;
    m_SelectionMaskNeedsUpload = true;

    if (device) {
        CreateCompositeResources(device);
        for (size_t i = 0; i < m_Layers.size(); ++i) {
            if (!m_Layers[i].isGroup)
                RecreateLayerTexture(device, m_Layers[i]);
            if (m_Layers[i].hasMask)
                UpdateLayerMaskTexture(device, (int)i);
        }
        UpdateSelectionMaskTexture(device);
    }

    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    auto newSnap = CaptureGeometrySnap(*this);
    m_UndoRedoManager.PushCommand(std::make_shared<DocumentGeometryCommand>(
        "Crop", std::move(oldSnap), std::move(newSnap)));
    InvalidateWandSourceCache();
    m_QuickSelectEdgeValid = false;
    return true;
}

bool Canvas::EditCanvas(ID3D11Device* device, CanvasEditMode mode, int newW, int newH,
                        ResampleFilter filter, float anchorX, float anchorY) {
    newW = std::clamp(newW, 1, 16384);
    newH = std::clamp(newH, 1, 16384);
    if (newW == m_Width && newH == m_Height) return false;

    auto oldSnap = CaptureGeometrySnap(*this);
    const int oldW = m_Width, oldH = m_Height;
    anchorX = std::clamp(anchorX, 0.f, 1.f);
    anchorY = std::clamp(anchorY, 0.f, 1.f);

    int interp = cv::INTER_LINEAR;
    if (filter == ResampleFilter::Nearest) interp = cv::INTER_NEAREST;
    else if (filter == ResampleFilter::Lanczos) interp = cv::INTER_LANCZOS4;

    if (mode == CanvasEditMode::Extend) {
        // Place old content at offset based on anchor
        int offX = (int)std::lround((newW - oldW) * anchorX);
        int offY = (int)std::lround((newH - oldH) * anchorY);

        for (auto& layer : m_Layers) {
            if (layer.isGroup) continue;
            if (!layer.tileCache) continue;
            auto pixels = ExportLayerF(layer, oldW, oldH);
            std::vector<float> next((size_t)newW * newH * 4, 0.f);
            for (int y = 0; y < oldH; ++y) {
                int dy = y + offY;
                if (dy < 0 || dy >= newH) continue;
                for (int x = 0; x < oldW; ++x) {
                    int dx = x + offX;
                    if (dx < 0 || dx >= newW) continue;
                    size_t si = ((size_t)y * oldW + x) * 4;
                    size_t di = ((size_t)dy * newW + dx) * 4;
                    for (int c = 0; c < 4; ++c) next[di + c] = pixels[si + c];
                }
            }
            if (layer.hasMask && !layer.mask.empty()) {
                std::vector<uint8_t> nm((size_t)newW * newH, 255);
                for (int y = 0; y < oldH; ++y) {
                    int dy = y + offY;
                    if (dy < 0 || dy >= newH) continue;
                    for (int x = 0; x < oldW; ++x) {
                        int dx = x + offX;
                        if (dx < 0 || dx >= newW) continue;
                        nm[(size_t)dy * newW + dx] = layer.mask[(size_t)y * oldW + x];
                    }
                }
                layer.mask = std::move(nm);
                layer.maskNeedsUpload = true;
            }
            layer.tileCache->Clear();
            layer.tileCache->Init(newW, newH, m_CanvasFormat);
            layer.tileCache->ImportRGBA32F(next.data(), newW, newH);
            layer.tileCache->MarkAllDirty();
            layer.needsUpload = true;
            layer.filtersDirty = true;
        }

        // Selection
        if (!m_SelectionMask.empty()) {
            std::vector<uint8_t> ns((size_t)newW * newH, 0);
            bool has = false;
            for (int y = 0; y < oldH; ++y) {
                int dy = y + offY;
                if (dy < 0 || dy >= newH) continue;
                for (int x = 0; x < oldW; ++x) {
                    int dx = x + offX;
                    if (dx < 0 || dx >= newW) continue;
                    uint8_t v = m_SelectionMask[(size_t)y * oldW + x];
                    ns[(size_t)dy * newW + dx] = v;
                    if (v) has = true;
                }
            }
            m_SelectionMask = std::move(ns);
            m_HasSelection = has;
            m_SelectionMaskNeedsUpload = true;
        }

        m_Width = newW;
        m_Height = newH;
    } else {
        // Resize content
        for (auto& layer : m_Layers) {
            if (layer.isGroup) continue;
            if (!layer.tileCache) continue;
            auto pixels = ExportLayerF(layer, oldW, oldH);
            cv::Mat src(oldH, oldW, CV_32FC4, pixels.data());
            cv::Mat dst;
            cv::resize(src, dst, cv::Size(newW, newH), 0, 0, interp);
            std::vector<float> next((size_t)newW * newH * 4);
            std::memcpy(next.data(), dst.ptr<float>(), next.size() * sizeof(float));
            if (layer.hasMask && !layer.mask.empty()) {
                cv::Mat msrc(oldH, oldW, CV_8UC1, layer.mask.data());
                cv::Mat mdst;
                cv::resize(msrc, mdst, cv::Size(newW, newH), 0, 0, interp);
                layer.mask.assign(mdst.datastart, mdst.dataend);
                layer.maskNeedsUpload = true;
            }
            layer.tileCache->Clear();
            layer.tileCache->Init(newW, newH, m_CanvasFormat);
            layer.tileCache->ImportRGBA32F(next.data(), newW, newH);
            layer.tileCache->MarkAllDirty();
            layer.needsUpload = true;
            layer.filtersDirty = true;
        }
        if (!m_SelectionMask.empty()) {
            cv::Mat msrc(oldH, oldW, CV_8UC1);
            if ((int)m_SelectionMask.size() == oldW * oldH)
                std::memcpy(msrc.data, m_SelectionMask.data(), m_SelectionMask.size());
            else
                msrc = cv::Mat::zeros(oldH, oldW, CV_8UC1);
            cv::Mat mdst;
            cv::resize(msrc, mdst, cv::Size(newW, newH), 0, 0, interp);
            m_SelectionMask.assign(mdst.datastart, mdst.dataend);
            m_HasSelection = false;
            for (uint8_t v : m_SelectionMask) { if (v) { m_HasSelection = true; break; } }
            m_SelectionMaskNeedsUpload = true;
        }
        m_Width = newW;
        m_Height = newH;
    }

    if (device) {
        CreateCompositeResources(device);
        for (size_t i = 0; i < m_Layers.size(); ++i) {
            if (!m_Layers[i].isGroup)
                RecreateLayerTexture(device, m_Layers[i]);
            if (m_Layers[i].hasMask)
                UpdateLayerMaskTexture(device, (int)i);
        }
        if (m_HasSelection) UpdateSelectionMaskTexture(device);
    }

    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    auto newSnap = CaptureGeometrySnap(*this);
    m_UndoRedoManager.PushCommand(std::make_shared<DocumentGeometryCommand>(
        mode == CanvasEditMode::Extend ? "Canvas Extend" : "Canvas Resize",
        std::move(oldSnap), std::move(newSnap)));
    InvalidateWandSourceCache();
    m_QuickSelectEdgeValid = false;
    Logger::Get().Info(std::string("EditCanvas ") +
        (mode == CanvasEditMode::Extend ? "Extend" : "Resize") + " -> " +
        std::to_string(newW) + "x" + std::to_string(newH));
    return true;
}

// ---------------------------------------------------------------------------
// ICC presets
// ---------------------------------------------------------------------------
const char* Canvas::IccPresetName(IccPreset p) {
    return IccProfiles::Name(static_cast<IccProfiles::Preset>(p));
}

Canvas::IccPreset Canvas::IccPresetFromName(const std::string& name) {
    if (name == "None" || name == "none") return IccPreset::None;
    if (name == "Display P3" || name == "DisplayP3" || name == "P3") return IccPreset::DisplayP3;
    if (name == "Adobe RGB" || name == "AdobeRGB") return IccPreset::AdobeRGB;
    if (name == "Linear" || name == "linear" || name == "Linear RGB") return IccPreset::Linear;
    if (name == "sRGB" || name == "srgb") return IccPreset::sRGB;
    // Legacy free-text path → treat as sRGB default (UI no longer exposes path)
    return IccPreset::sRGB;
}

const std::vector<uint8_t>& Canvas::GetIccPresetBytes(IccPreset p) {
    return IccProfiles::GetProfileBytes(static_cast<IccProfiles::Preset>(p));
}

void Canvas::SetExportIccPreset(IccPreset p) {
    m_ExportIccPreset = p;
    m_ExportPngColorSpace = IccPresetName(p);
}

void Canvas::SetDocumentBitDepth(DocumentBitDepth d) {
    if (d == m_DocumentBitDepth && FormatForBitDepth(d) == m_CanvasFormat) return;

    const DocumentBitDepth prev = m_DocumentBitDepth;
    const CanvasPixelFormat target = FormatForBitDepth(d);

    // Capture pre-convert tiles (deep) for undo — convert is lossy U8←float.
    auto captureSnap = [&]() -> DocumentBitDepthCommand::Snap {
        DocumentBitDepthCommand::Snap s;
        s.bitDepth = (uint8_t)m_DocumentBitDepth;
        s.pixelFormat = (uint8_t)m_CanvasFormat;
        for (int i = 0; i < (int)m_Layers.size(); ++i) {
            auto& layer = m_Layers[i];
            if (layer.isGroup || !layer.tileCache) continue;
            DocumentBitDepthCommand::LayerTiles lt;
            lt.layerIdx = i;
            const int ntx = layer.tileCache->GetTilesX();
            const int nty = layer.tileCache->GetTilesY();
            for (int ty = 0; ty < nty; ++ty) {
                for (int tx = 0; tx < ntx; ++tx) {
                    if (!layer.tileCache->HasTile(tx, ty)) continue;
                    TileDelta d;
                    d.layerIdx = i;
                    d.tileX = tx; d.tileY = ty;
                    d.newState = TileCache::DeepCopySnapshot(layer.tileCache->SnapshotTile(tx, ty));
                    lt.tiles.push_back(std::move(d));
                }
            }
            s.layers.push_back(std::move(lt));
        }
        return s;
    };

    DocumentBitDepthCommand::Snap oldSnap = captureSnap();

    size_t tilesConverted = 0;
    size_t bytesEst = 0;

    for (auto& layer : m_Layers) {
        if (layer.isGroup) continue;
        if (layer.tileCache) {
            tilesConverted += layer.tileCache->GetTileCount();
            layer.tileCache->ConvertFormat(target);
            layer.tileCache->MarkAllDirty();
            layer.needsUpload = true;
            bytesEst += layer.tileCache->EstimateUniquePixelBytes();
        }
        if (layer.filteredCache) {
            layer.filteredCache.reset();
            layer.filtersDirty = !layer.filters.empty();
        }
        layer.presentationCache.reset();
        layer.presentationDirty = layer.HasEnabledStyles();
        // Drop GPU textures — recreated on next compose with new DXGI format.
        if (layer.gpuSurfaceId) {
            m_GpuTiles.DestroySurface(layer.gpuSurfaceId);
            layer.gpuSurfaceId = 0;
        }
        if (layer.texture) { DeferReleaseTex(layer.texture); layer.texture = nullptr; }
        if (layer.srv)     { DeferReleaseSRV(layer.srv);     layer.srv = nullptr; }
        if (layer.thumbSRV) { DeferReleaseSRV(layer.thumbSRV); layer.thumbSRV = nullptr; }
        if (layer.thumbTex) { DeferReleaseTex(layer.thumbTex); layer.thumbTex = nullptr; }
        layer.thumbDirty = true;
        layer.gpuDisplayKind = 0xFF;
    }

    m_DocumentBitDepth = d;
    m_CanvasFormat = target;
    m_CompositeDirty = true;
    m_ChannelPreviewDirty = true;
    m_IsDocumentModified = true;

    DocumentBitDepthCommand::Snap newSnap = captureSnap();
    m_UndoRedoManager.PushCommand(std::make_shared<DocumentBitDepthCommand>(
        "Bit Depth Convert", std::move(oldSnap), std::move(newSnap)));

    // Composite RT is always U8 proxy; no rebuild required for bit depth alone.
    Logger::Get().Info(std::string("DocumentBitDepth ") +
        (prev == DocumentBitDepth::F32 ? "F32" : prev == DocumentBitDepth::F16 ? "F16" : "U8") +
        " → " +
        (d == DocumentBitDepth::F32 ? "F32" : d == DocumentBitDepth::F16 ? "F16" : "U8") +
        " tiles=" + std::to_string(tilesConverted) +
        " estCPU=" + MemoryStats::FormatBytes(bytesEst) +
        " storage=" + std::to_string(BytesPerPixel(target)) + "B/px");
}

void Canvas::SetCustomBrushTip(int size, const std::vector<uint8_t>& pixels) {
    if (size <= 0 || (int)pixels.size() < size * size) {
        m_CustomBrushTipSize = 0;
        m_CustomBrushTipPixels.clear();
        return;
    }
    m_CustomBrushTipSize = size;
    m_CustomBrushTipPixels = pixels;
    m_BrushTipId = "custom";
}

bool Canvas::GetCustomBrushTip(int& outSize, std::vector<uint8_t>& outPixels) const {
    if (m_CustomBrushTipSize <= 0 || m_CustomBrushTipPixels.empty()) return false;
    outSize = m_CustomBrushTipSize;
    outPixels = m_CustomBrushTipPixels;
    return true;
}

bool Canvas::SaveCanvasStandard(const std::string& filepath, IccPreset preset) {
    m_ExportIccPreset = preset;
    m_ExportPngColorSpace = IccPresetName(preset);

    if (m_Layers.empty()) {
        Logger::Get().Error("SaveCanvasStandard: no layers.");
        return false;
    }
    ScopedTimer saveTimer("SaveCanvasStandard " + filepath);
    MemoryStats::LogSnapshot("before_SaveCanvasStandard");

    // Ensure filters rebuilt before export
    for (auto& layer : m_Layers) {
        if (layer.isGroup) continue;
        if (!layer.filters.empty() && layer.filtersDirty)
            RebuildFilteredPixels(layer);
    }

    std::vector<uint8_t> rgba8;
    if (!ComposeVisibleLayersRGBA8(m_Layers, m_Width, m_Height, rgba8)) {
        Logger::Get().Error("SaveCanvasStandard: RGBA8 composite failed.");
        return false;
    }

    bool ok = false;
    if (preset == IccPreset::None) {
        ok = ImageManager::SaveRGBA8ToFile(filepath, rgba8.data(), m_Width, m_Height, m_Width * 4, std::string());
    } else {
        const auto& icc = GetIccPresetBytes(preset);
        ok = ImageManager::SaveRGBA8ToFile(
            filepath, rgba8.data(), m_Width, m_Height, m_Width * 4,
            icc.data(), icc.size(), IccPresetName(preset));
    }
    MemoryStats::LogSnapshot("after_SaveCanvasStandard");
    return ok;
}

// ---------------------------------------------------------------------------
// Channel solo setters — must rebuild composite (A-off forces opaque RGB compose)
// ---------------------------------------------------------------------------
void Canvas::SetChannelR(bool r) {
    if (m_ChannelR == r) return;
    m_ChannelR = r;
    MarkCompositeDirty();
}
void Canvas::SetChannelG(bool g) {
    if (m_ChannelG == g) return;
    m_ChannelG = g;
    MarkCompositeDirty();
}
void Canvas::SetChannelB(bool b) {
    if (m_ChannelB == b) return;
    m_ChannelB = b;
    MarkCompositeDirty();
}
void Canvas::SetChannelA(bool a) {
    if (m_ChannelA == a) return;
    m_ChannelA = a;
    // Viewport PSMain: A-off shows composite RGB ignoring coverage (buffer view).
    MarkCompositeDirty();
}

void Canvas::SetViewMapKind(texset::MapKind k) {
    if (m_ViewMapKind == k) return;
    m_ViewMapKind = k;
    // Multi-target Fill resolves different packed colors per map — re-upload GPU solids
    for (auto& L : m_Layers) {
        if (L.IsFill()) {
            L.needsUpload = true;
            L.presentationDirty = true;
        }
    }
    MarkCompositeDirty();
}

void Canvas::SetActiveSetMaps(const std::vector<texset::MapSlot>& maps) {
    m_ActiveSetMaps = maps;
}

void Canvas::ClearViewMapUnderlay() {
    if (m_ViewUnderlaySRV) { m_ViewUnderlaySRV->Release(); m_ViewUnderlaySRV = nullptr; }
    if (m_ViewUnderlayTex) { m_ViewUnderlayTex->Release(); m_ViewUnderlayTex = nullptr; }
    m_ViewUnderlayW = m_ViewUnderlayH = 0;
}

void Canvas::SetViewMapUnderlay(ID3D11Device* device, const TileCache* cache) {
    if (!device || !cache || cache->GetWidth() <= 0 || cache->GetHeight() <= 0) {
        ClearViewMapUnderlay();
        MarkCompositeDirty();
        return;
    }
    const int w = cache->GetWidth();
    const int h = cache->GetHeight();
    // Rebuild GPU tex if size changed
    if (!m_ViewUnderlayTex || m_ViewUnderlayW != w || m_ViewUnderlayH != h) {
        ClearViewMapUnderlay();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_ViewUnderlayTex)))
            return;
        device->CreateShaderResourceView(m_ViewUnderlayTex, nullptr, &m_ViewUnderlaySRV);
        m_ViewUnderlayW = w;
        m_ViewUnderlayH = h;
    }
    // Upload full image (map sizes are usually 1k–2k — acceptable for map switch)
    std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u);
    cache->ExportRGBA8(rgba.data(), w, h);
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (ctx && m_ViewUnderlayTex) {
        ctx->UpdateSubresource(m_ViewUnderlayTex, 0, nullptr, rgba.data(), (UINT)(w * 4), 0);
        ctx->Release();
    }
    MarkCompositeDirty();
}

void Canvas::SetViewRoleIsolate(bool on, texset::ChannelRole role) {
    if (m_ViewRoleIsolate == on && m_ViewSoloRole == role) return;
    m_ViewRoleIsolate = on;
    m_ViewSoloRole = role;
    MarkCompositeDirty();
}

void Canvas::ApplyViewRoleToChannelMasks(const texset::MapSlot* slot) {
    if (!m_ViewRoleIsolate || !slot) {
        // Full map RGBA
        m_ChannelR = m_ChannelG = m_ChannelB = true;
        m_ChannelA = true;
        MarkCompositeDirty();
        return;
    }
    if (m_ViewSoloRole == texset::ChannelRole::BaseColor) {
        m_ChannelR = m_ChannelG = m_ChannelB = true;
        m_ChannelA = false;
        MarkCompositeDirty();
        return;
    }
    int ch = texset::ResolveChannelForRole(*slot, m_ViewSoloRole);
    m_ChannelR = m_ChannelG = m_ChannelB = m_ChannelA = false;
    if (ch == 0) m_ChannelR = true;
    else if (ch == 1) m_ChannelG = true;
    else if (ch == 2) m_ChannelB = true;
    else if (ch == 3) m_ChannelA = true;
    else {
        // Role not packed — grayscale R as fallback
        m_ChannelR = true;
    }
    MarkCompositeDirty();
}

// ---------------------------------------------------------------------------
// Channel preview thumbs — sample *layers* (buffer semantics: RGB visible if A=0)
// ---------------------------------------------------------------------------
void Canvas::ReleaseChannelPreviewResources() {
    for (int i = 0; i < 4; ++i) {
        if (m_ChannelPreviewSRV[i]) { m_ChannelPreviewSRV[i]->Release(); m_ChannelPreviewSRV[i] = nullptr; }
        if (m_ChannelPreviewTex[i]) { m_ChannelPreviewTex[i]->Release(); m_ChannelPreviewTex[i] = nullptr; }
    }
    m_ChannelPreviewW = m_ChannelPreviewH = 0;
    m_ChannelPreviewDirty = true;
}

void Canvas::RebuildChannelPreviews(ID3D11Device* device) {
    if (!device || m_Width <= 0 || m_Height <= 0)
        return;

    // Proxy size (same as composite) so thumbs stay cheap
    int w = m_CompositeWidth > 0 ? m_CompositeWidth : 256;
    int h = m_CompositeHeight > 0 ? m_CompositeHeight : 256;
    if (w <= 0) w = 256;
    if (h <= 0) h = 256;
    w = std::min(w, 512);
    h = std::min(h, 512);

    std::vector<const Layer*> vis;
    vis.reserve(m_Layers.size());
    for (const auto& layer : m_Layers) {
        if (LayerEffectivelyVisible(m_Layers, layer)) vis.push_back(&layer);
    }

    std::vector<uint8_t> ch[4];
    for (int c = 0; c < 4; ++c) ch[c].assign((size_t)w * h, 0);

    // Same rules as viewport/export: first layer inits full RGBA (RGB if A=0);
    // later layers use A as strength; Alpha Rewrite OFF keeps A.
    for (int y = 0; y < h; ++y) {
        const int srcY = (int)((int64_t)y * m_Height / h);
        for (int x = 0; x < w; ++x) {
            const int srcX = (int)((int64_t)x * m_Width / w);
            float acc[4] = { 0.f, 0.f, 0.f, 0.f };
            bool first = true;
            for (const Layer* layer : vis) {
                const TileCache* cache = LayerExportCache(*layer);
                if (!cache) continue;
                float rgba[4] = {};
                cache->GetPixelF(srcX, srcY, rgba);
                float op = std::clamp(layer->opacity, 0.f, 1.f);
                float mask = 1.f;
                if (layer->hasMask && layer->mask.size() == (size_t)m_Width * m_Height)
                    mask = layer->mask[(size_t)srcY * m_Width + srcX] / 255.f;
                if (first) {
                    acc[0] = rgba[0];
                    acc[1] = rgba[1];
                    acc[2] = rgba[2];
                    acc[3] = rgba[3] * op * mask;
                    first = false;
                    continue;
                }
                float sa = op * rgba[3] * mask;
                if (sa <= 0.f) continue;
                BlendLayerPixelF(acc, rgba[0], rgba[1], rgba[2], sa, layer->blendMode, layer->alphaRewrite);
            }
            float accR = acc[0], accG = acc[1], accB = acc[2], accA = acc[3];
            size_t di = (size_t)y * w + x;
            ch[0][di] = HalfFloat::FloatToU8(accR);
            ch[1][di] = HalfFloat::FloatToU8(accG);
            ch[2][di] = HalfFloat::FloatToU8(accB);
            ch[3][di] = HalfFloat::FloatToU8(accA);
        }
    }

    ReleaseChannelPreviewResources();
    m_ChannelPreviewW = w;
    m_ChannelPreviewH = h;

    for (int c = 0; c < 4; ++c) {
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        for (int i = 0; i < w * h; ++i) {
            uint8_t v = ch[c][(size_t)i];
            rgba[(size_t)i * 4 + 0] = v;
            rgba[(size_t)i * 4 + 1] = v;
            rgba[(size_t)i * 4 + 2] = v;
            rgba[(size_t)i * 4 + 3] = 255; // ImGui must not hide by alpha
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = rgba.data();
        init.SysMemPitch = w * 4;

        if (SUCCEEDED(device->CreateTexture2D(&td, &init, &m_ChannelPreviewTex[c])) && m_ChannelPreviewTex[c]) {
            device->CreateShaderResourceView(m_ChannelPreviewTex[c], nullptr, &m_ChannelPreviewSRV[c]);
        }
    }
    m_ChannelPreviewDirty = false;
}

ID3D11ShaderResourceView* Canvas::GetChannelPreviewSRV(ID3D11Device* device, ChannelPreview ch) {
    if (!device) return nullptr;
    int idx = (int)ch;
    if (idx < 0 || idx > 3) return nullptr;
    // Defer rebuild during stroke/move — return last good preview.
    if (m_ChannelPreviewDirty && (m_IsStrokeActive || m_IsMovingPixels) && m_ChannelPreviewSRV[idx])
        return m_ChannelPreviewSRV[idx];
    if (m_ChannelPreviewDirty || !m_ChannelPreviewSRV[idx] ||
        m_ChannelPreviewW <= 0 || m_ChannelPreviewH <= 0) {
        RebuildChannelPreviews(device);
    }
    return m_ChannelPreviewSRV[idx];
}

ID3D11ShaderResourceView* Canvas::GetLayerThumbSRV(ID3D11Device* device, int layerIdx, int size) {
    if (!device || layerIdx < 0 || layerIdx >= (int)m_Layers.size() || size <= 0)
        return nullptr;
    Layer& layer = m_Layers[layerIdx];
    if (layer.isGroup || !layer.tileCache)
        return nullptr;

    if (!layer.thumbDirty && layer.thumbSRV && layer.thumbSize == size)
        return layer.thumbSRV;

    // Idle-only rebuild: during stroke/move keep stale thumb (avoids O(size²) thrash).
    if (layer.thumbDirty && layer.thumbSRV && layer.thumbSize == size &&
        (m_IsStrokeActive || m_IsMovingPixels))
        return layer.thumbSRV;

    // Stale thumb while idle: rebuild sync (scheduler path deferred — keep hot path simple).
    // ScheduleThumbJob remains available for future batching.

    // Rebuild thumb (sync). Deferred free — old thumb may still be in ImGui draw list.
    if (layer.thumbSRV) { DeferReleaseSRV(layer.thumbSRV); layer.thumbSRV = nullptr; }
    if (layer.thumbTex) { DeferReleaseTex(layer.thumbTex); layer.thumbTex = nullptr; }

    const TileCache* cache = LayerExportCache(layer);
    if (!cache) return nullptr;

    std::vector<uint8_t> rgba((size_t)size * size * 4, 0);
    for (int y = 0; y < size; ++y) {
        const int sy = (m_Height > 0) ? (int)((int64_t)y * m_Height / size) : 0;
        for (int x = 0; x < size; ++x) {
            const int sx = (m_Width > 0) ? (int)((int64_t)x * m_Width / size) : 0;
            float f[4] = {};
            cache->GetPixelF(sx, sy, f);
            size_t di = ((size_t)y * size + x) * 4;
            // Always force A=255 so ImGui ImageButton never hides A=0 buffer RGB.
            // When channel A is on, viewport still uses real alpha; list is diagnostic.
            rgba[di + 0] = HalfFloat::FloatToU8(f[0]);
            rgba[di + 1] = HalfFloat::FloatToU8(f[1]);
            rgba[di + 2] = HalfFloat::FloatToU8(f[2]);
            rgba[di + 3] = 255;
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = size;
    td.Height = size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba.data();
    init.SysMemPitch = size * 4;
    if (FAILED(device->CreateTexture2D(&td, &init, &layer.thumbTex)) || !layer.thumbTex)
        return nullptr;
    device->CreateShaderResourceView(layer.thumbTex, nullptr, &layer.thumbSRV);
    layer.thumbSize = size;
    layer.thumbDirty = false;
    return layer.thumbSRV;
}

// ---------------------------------------------------------------------------
// SVG → editable Vector layer (nanosvg parse + path model + tile raster)
// ---------------------------------------------------------------------------
bool Canvas::ImportSvgAsVectorLayer(ID3D11Device* device, const std::string& filepath) {
    if (m_Width <= 0 || m_Height <= 0) {
        Logger::Get().Error("ImportSvgAsVectorLayer: no document size");
        return false;
    }
    vec::Document doc;
    std::string err;
    if (!vec::LoadSvgFile(filepath, doc, &err)) {
        Logger::Get().Error("ImportSvgAsVectorLayer: " + err);
        return false;
    }
    Layer layer;
    size_t slash = filepath.find_last_of("/\\");
    layer.name = (slash == std::string::npos) ? filepath : filepath.substr(slash + 1);
    layer.type = Layer::Type::VectorSvg;
    layer.visible = true;
    layer.opacity = 1.f;
    layer.vectorDoc = std::make_unique<vec::Document>(std::move(doc));
    layer.vectorDoc->MarkAllDirty(m_Width, m_Height);
    {
#ifdef _WIN32
        std::ifstream in(PathUtil::Utf8ToWide(filepath), std::ios::binary);
#else
        std::ifstream in(filepath, std::ios::binary);
#endif
        if (in) {
            std::ostringstream ss; ss << in.rdbuf();
            std::string s = ss.str();
            layer.smartSourceBytes.assign(s.begin(), s.end());
            layer.smartSourcePath = filepath;
            layer.vectorDoc->sourceSvg = std::move(s);
        }
    }
    layer.tileCache = std::make_unique<TileCache>();
    layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
    vec::RasterizeDocumentFull(*layer.vectorDoc, *layer.tileCache, m_Width, m_Height, false);
    layer.vectorDoc->rasterGen = layer.vectorDoc->generation;
    layer.vectorDoc->ClearDirty();
    layer.needsUpload = true;

    if (device && m_Width > 0 && m_Height > 0) {
        if (!m_CompositeTexture) CreateCompositeResources(device);
        RecreateLayerTexture(device, layer);
    }

    int insertAt = (int)m_Layers.size();
    m_Layers.push_back(std::move(layer));
    m_ActiveLayerIdx = insertAt;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    auto snap = CaptureLayerSnap(m_Layers[insertAt], insertAt, m_Width, m_Height, m_CanvasFormat);
    m_UndoRedoManager.PushCommand(std::make_shared<LayerStackCommand>(
        "Import SVG Vector", LayerStackCommand::Kind::Insert, std::move(snap)));
    Logger::Get().Info("Imported SVG as vector layer: " + filepath);
    return true;
}

bool Canvas::ImportSvgAsSmartObject(ID3D11Device* device, const std::string& filepath) {
    return ImportSvgAsVectorLayer(device, filepath);
}

void Canvas::CreateVectorLayer(ID3D11Device* device, const std::string& name) {
    Layer layer;
    layer.name = name.empty() ? "Vector" : name;
    layer.type = Layer::Type::VectorSvg;
    layer.visible = true;
    layer.opacity = 1.f;
    layer.vectorDoc = std::make_unique<vec::Document>();
    layer.vectorDoc->antialias = true;
    layer.tileCache = std::make_unique<TileCache>();
    if (m_Width > 0 && m_Height > 0)
        layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);

    if (device && m_Width > 0 && m_Height > 0) {
        if (!m_CompositeTexture) CreateCompositeResources(device);
        RecreateLayerTexture(device, layer);
    }

    int insertAt = (int)m_Layers.size();
    m_Layers.push_back(std::move(layer));
    m_ActiveLayerIdx = insertAt;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    auto snap = CaptureLayerSnap(m_Layers[insertAt], insertAt, m_Width, m_Height, m_CanvasFormat);
    m_UndoRedoManager.PushCommand(std::make_shared<LayerStackCommand>(
        "New Vector Layer", LayerStackCommand::Kind::Insert, std::move(snap)));
    Logger::Get().Info("Created vector layer: " + layer.name);
}

int Canvas::EnsureActiveVectorLayer(ID3D11Device* device) {
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
        auto& L = m_Layers[m_ActiveLayerIdx];
        if (L.IsVector()) {
            if (!L.vectorDoc)
                L.vectorDoc = std::make_unique<vec::Document>();
            return m_ActiveLayerIdx;
        }
    }
    CreateVectorLayer(device, "Vector");
    return m_ActiveLayerIdx;
}

bool Canvas::EnsureVectorRaster(int layerIdx, bool coarse, bool forceFull) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    auto& L = m_Layers[layerIdx];
    if (!L.IsVector() || !L.vectorDoc) return false;
    if (!L.tileCache) {
        L.tileCache = std::make_unique<TileCache>();
        L.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
    }
    bool wrote = false;
    if (forceFull) {
        wrote = vec::RasterizeDocumentFull(*L.vectorDoc, *L.tileCache, m_Width, m_Height, coarse);
        L.vectorDoc->rasterGen = L.vectorDoc->generation;
        L.vectorDoc->ClearDirty();
        if (L.tileCache) L.tileCache->MarkAllDirty();
        NotifyLayerVisualsChanged(layerIdx, /*contentPixelsChanged=*/true, /*markFiltersDirty=*/true);
        return wrote;
    }
    if (L.vectorDoc->HasDirty() || L.vectorDoc->rasterGen != L.vectorDoc->generation ||
        L.tileCache->IsEmpty()) {
        if (!L.vectorDoc->HasDirty())
            L.vectorDoc->MarkAllDirty(m_Width, m_Height);
        wrote = vec::RasterizeDocument(*L.vectorDoc, *L.tileCache, m_Width, m_Height, coarse);
        L.vectorDoc->rasterGen = L.vectorDoc->generation;
        L.vectorDoc->ClearDirty();
        if (wrote)
            NotifyLayerVisualsChanged(layerIdx, /*contentPixelsChanged=*/true, /*markFiltersDirty=*/true);
    }
    return wrote;
}

void Canvas::CommitVectorEdit(int layerIdx, const std::string& beforeJson,
                              const std::string& afterJson, const char* actionName) {
    if (beforeJson == afterJson) return;
    m_UndoRedoManager.PushCommand(std::make_shared<VectorEditCommand>(
        actionName ? actionName : "Vector Edit", layerIdx, beforeJson, afterJson));
    m_IsDocumentModified = true;
    EnsureVectorRaster(layerIdx, false);
}

vec::Document* Canvas::GetVectorDocument(int layerIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return nullptr;
    return m_Layers[layerIdx].vectorDoc.get();
}
const vec::Document* Canvas::GetVectorDocument(int layerIdx) const {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return nullptr;
    return m_Layers[layerIdx].vectorDoc.get();
}

bool Canvas::ExportVectorLayerSvg(int layerIdx, const std::string& path) {
    auto* doc = GetVectorDocument(layerIdx);
    if (!doc) return false;
    std::string err;
    if (!vec::SaveSvgFile(path, *doc, m_Width, m_Height, &err)) {
        Logger::Get().Error("ExportVectorLayerSvg: " + err);
        return false;
    }
    return true;
}

bool Canvas::ConvertLayerToSmartObject(ID3D11Device* device, int layerIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    Layer& layer = m_Layers[layerIdx];
    if (layer.isGroup || layer.IsFill()) return false;
    if (layer.type == Layer::Type::SmartObject || layer.type == Layer::Type::VectorSvg)
        return true; // already smart

    // Export current display pixels as project texture asset
    std::vector<float> pxF = ExportLayerF(layer, m_Width, m_Height);
    if (pxF.empty() || m_Width <= 0 || m_Height <= 0) return false;
    std::vector<uint8_t> rgba((size_t)m_Width * m_Height * 4);
    for (size_t i = 0, n = (size_t)m_Width * m_Height; i < n; ++i) {
        auto q = [](float v) -> uint8_t {
            v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            return (uint8_t)(v * 255.f + 0.5f);
        };
        rgba[i * 4 + 0] = q(pxF[i * 4 + 0]);
        rgba[i * 4 + 1] = q(pxF[i * 4 + 1]);
        rgba[i * 4 + 2] = q(pxF[i * 4 + 2]);
        rgba[i * 4 + 3] = q(pxF[i * 4 + 3]);
    }
    std::string key = assets::AssetManager::Get().RegisterProjectRgba(
        layer.name + ".png", m_Width, m_Height, std::move(rgba));
    if (key.empty()) return false;

    if (!layer.smartAssetKey.empty())
        assets::AssetStore::Get().Release(layer.smartAssetKey);
    layer.smartAssetKey = key; // Register left ref=1
    layer.type = Layer::Type::SmartObject;
    layer.smartSourcePath.clear();
    // Keep tileCache as display proxy (current pixels already there)
    layer.needsUpload = true;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("ConvertLayerToSmartObject: " + layer.name + " key=" + key);
    (void)device;
    return true;
}

bool Canvas::ReplaceSmartObjectSource(ID3D11Device* device, int layerIdx, const std::string& assetKey) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    Layer& layer = m_Layers[layerIdx];
    if (layer.type != Layer::Type::SmartObject && layer.type != Layer::Type::VectorSvg)
        return false;
    if (assetKey.empty()) return false;
    if (!assets::AssetManager::Get().IsKindAllowed(assetKey, assets::AssetKind::Texture) &&
        assets::AssetManager::Get().GetKind(assetKey) != assets::AssetKind::Unknown) {
        Logger::Get().Error("ReplaceSmartObjectSource: expected Texture kind");
        return false;
    }

    std::string key = assets::AssetStore::Get().AcquireKey(assetKey);
    if (key.empty()) {
        assets::AssetStore::Get().RequestLoad(assetKey);
        assets::AssetStore::Get().AddRef(assetKey);
        key = assetKey;
    }
    auto pay = assets::AssetStore::Get().GetPayload(key);
    if (!pay || pay->rgba.empty()) {
        // Keep binding; display unchanged until load completes
        if (!layer.smartAssetKey.empty() && layer.smartAssetKey != key)
            assets::AssetStore::Get().Release(layer.smartAssetKey);
        layer.smartAssetKey = key;
        layer.type = Layer::Type::SmartObject;
        m_IsDocumentModified = true;
        return true;
    }

    // Rebuild tile cache from asset (centered / scaled to document)
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
    }
    std::vector<float> full((size_t)m_Width * m_Height * 4, 0.f);
    const int tw = pay->w, th = pay->h;
    int dx = (m_Width - tw) / 2;
    int dy = (m_Height - th) / 2;
    for (int y = 0; y < th; ++y) {
        int yy = y + dy;
        if (yy < 0 || yy >= m_Height) continue;
        for (int x = 0; x < tw; ++x) {
            int xx = x + dx;
            if (xx < 0 || xx >= m_Width) continue;
            size_t si = ((size_t)y * tw + x) * 4;
            size_t di = ((size_t)yy * m_Width + xx) * 4;
            full[di + 0] = pay->rgba[si + 0] / 255.f;
            full[di + 1] = pay->rgba[si + 1] / 255.f;
            full[di + 2] = pay->rgba[si + 2] / 255.f;
            full[di + 3] = pay->rgba[si + 3] / 255.f;
        }
    }
    layer.tileCache->ImportRGBA32F(full.data(), m_Width, m_Height);
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    if (!layer.smartAssetKey.empty() && layer.smartAssetKey != key)
        assets::AssetStore::Get().Release(layer.smartAssetKey);
    layer.smartAssetKey = key;
    layer.type = Layer::Type::SmartObject;
    if (device) RecreateLayerTexture(device, layer);
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    return true;
}

bool Canvas::RasterizeLayer(ID3D11Device* device, int layerIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    auto& layer = m_Layers[layerIdx];
    if (layer.isGroup)
        return RasterizeGroup(device, layerIdx);

    // Already plain raster with nothing to bake
    if (layer.type == Layer::Type::Raster && layer.filters.empty() && layer.styles.empty() && !layer.IsFill())
        return true;

    auto captureMeta = [&](const Layer& L) -> RasterizeCommand::LayerMeta {
        RasterizeCommand::LayerMeta m;
        m.name = L.name;
        m.type = (uint8_t)L.type;
        m.isGroup = L.isGroup;
        m.opacity = L.opacity;
        m.blendMode = L.blendMode;
        m.alphaRewrite = L.alphaRewrite;
        m.parentGroupId = L.parentGroupId;
        m.groupExpanded = L.groupExpanded;
        m.hasMask = L.hasMask;
        m.mask = L.mask;
        m.smartPath = L.smartSourcePath;
        m.smartBytes = L.smartSourceBytes;
        m.smartScale = L.smartScale;
        m.fill = L.fill;
        m.filters = L.filters;
        m.styles = L.styles;
        if (L.tileCache && !L.tileCache->IsEmpty()) {
            m.pixels = ExportLayerF(L, m_Width, m_Height);
            m.pixelsValid = true;
        } else if (L.IsFill()) {
            layer_fx::FillSolidBuffer(m.pixels, m_Width, m_Height, L.fill);
            m.pixelsValid = true;
        }
        return m;
    };

    RasterizeCommand::LayerMeta oldMeta = captureMeta(layer);

    // Snapshot old tiles (grid)
    std::vector<TileDelta> deltas;
    const int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    const int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    deltas.reserve((size_t)tilesX * tilesY);
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            TileDelta d;
            d.layerIdx = layerIdx;
            d.tileX = tx; d.tileY = ty;
            d.oldState = layer.tileCache ? layer.tileCache->SnapshotTile(tx, ty) : TileSnapshot{};
            deltas.push_back(std::move(d));
        }
    }

    // Bake content + filters + styles into pixels (full quality)
    if (layer.filtersDirty) RebuildFilteredPixels(layer);
    if (layer.HasEnabledStyles()) {
        layer.presentationDirty = true;
        RebuildLayerPresentation(layer, /*fullQuality=*/true);
    }

    std::vector<float> baked;
    if (layer.HasEnabledStyles() && layer.presentationCache && !layer.presentationCache->IsEmpty()) {
        baked.resize((size_t)m_Width * m_Height * 4);
        layer.presentationCache->ExportRGBA32F(baked.data(), m_Width, m_Height);
    } else if (!layer.filters.empty() && layer.filteredCache && !layer.filteredCache->IsEmpty()) {
        baked.resize((size_t)m_Width * m_Height * 4);
        layer.filteredCache->ExportRGBA32F(baked.data(), m_Width, m_Height);
        for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i)
            baked[i * 4 + 3] *= layer.opacity;
        if (layer.hasMask && layer.mask.size() == (size_t)m_Width * m_Height) {
            for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i)
                baked[i * 4 + 3] *= layer.mask[i] / 255.f;
        }
    } else if (layer.IsFill()) {
        layer_fx::FillSolidBuffer(baked, m_Width, m_Height, layer.fill);
        for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i)
            baked[i * 4 + 3] *= layer.opacity;
        if (layer.hasMask && layer.mask.size() == (size_t)m_Width * m_Height) {
            for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i)
                baked[i * 4 + 3] *= layer.mask[i] / 255.f;
        }
    } else {
        baked = ExportLayerF(layer, m_Width, m_Height);
    }

    if (!layer.smartAssetKey.empty()) {
        assets::AssetStore::Get().Release(layer.smartAssetKey);
        layer.smartAssetKey.clear();
    }
    layer.type = Layer::Type::Raster;
    layer.isGroup = false;
    layer.smartSourceBytes.clear();
    layer.smartSourcePath.clear();
    layer.vectorDoc.reset(); // live geometry discarded after bake
    layer.fill = FillLayerParams{};
    layer.filters.clear();
    layer.styles.clear();
    layer.filteredCache.reset();
    layer.presentationCache.reset();
    layer.filtersDirty = false;
    layer.stylesDirty = false;
    layer.presentationDirty = false;

    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
    }
    layer.tileCache->ImportRGBA32F(baked.data(), m_Width, m_Height);
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    layer.opacity = 1.f;

    for (auto& d : deltas)
        d.newState = layer.tileCache->SnapshotTile(d.tileX, d.tileY);

    RasterizeCommand::LayerMeta newMeta = captureMeta(layer);
    m_UndoRedoManager.PushCommand(std::make_shared<RasterizeCommand>(
        "Rasterize Layer", layerIdx, std::move(oldMeta), std::move(newMeta), std::move(deltas)));

    if (device) RecreateLayerTexture(device, layer);

    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("RasterizeLayer baked: " + layer.name);
    return true;
}

bool Canvas::RasterizeGroup(ID3D11Device* device, int groupIdx) {
    if (groupIdx < 0 || groupIdx >= (int)m_Layers.size()) return false;
    if (!m_Layers[groupIdx].isGroup) return false;

    const std::string groupName = m_Layers[groupIdx].name;
    const float groupOpacity = m_Layers[groupIdx].opacity;
    const auto groupFilters = m_Layers[groupIdx].filters;
    const auto groupStyles = m_Layers[groupIdx].styles;
    const bool groupHasMask = m_Layers[groupIdx].hasMask;
    std::vector<uint8_t> groupMask = m_Layers[groupIdx].mask;

    auto isUnderGroup = [&](int layerIdx, int gIdx) {
        int p = m_Layers[layerIdx].parentGroupId;
        while (p >= 0 && p < (int)m_Layers.size()) {
            if (p == gIdx) return true;
            p = m_Layers[p].parentGroupId;
        }
        return false;
    };

    auto captureMeta = [&](const Layer& L, int insertAt) -> RasterizeCommand::LayerMeta {
        RasterizeCommand::LayerMeta m;
        m.name = L.name;
        m.type = (uint8_t)L.type;
        m.isGroup = L.isGroup;
        m.opacity = L.opacity;
        m.blendMode = L.blendMode;
        m.alphaRewrite = L.alphaRewrite;
        m.parentGroupId = L.parentGroupId;
        m.groupExpanded = L.groupExpanded;
        m.hasMask = L.hasMask;
        m.mask = L.mask;
        m.smartPath = L.smartSourcePath;
        m.smartBytes = L.smartSourceBytes;
        m.smartScale = L.smartScale;
        m.fill = L.fill;
        m.filters = L.filters;
        m.styles = L.styles;
        m.insertAt = insertAt;
        if (L.IsFill()) {
            layer_fx::FillSolidBuffer(m.pixels, m_Width, m_Height, L.fill);
            m.pixelsValid = true;
        } else if (L.tileCache && !L.tileCache->IsEmpty()) {
            m.pixels = ExportLayerF(L, m_Width, m_Height);
            m.pixelsValid = true;
        }
        return m;
    };

    RasterizeCommand::LayerMeta oldGroupMeta = captureMeta(m_Layers[groupIdx], groupIdx);
    std::vector<RasterizeCommand::LayerMeta> removedChildren;
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        if (i != groupIdx && isUnderGroup(i, groupIdx))
            removedChildren.push_back(captureMeta(m_Layers[i], i));
    }

    // Bake children presentations first
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        if (i == groupIdx || m_Layers[i].isGroup || !isUnderGroup(i, groupIdx)) continue;
        if (m_Layers[i].filtersDirty) RebuildFilteredPixels(m_Layers[i]);
        if (m_Layers[i].HasEnabledStyles()) {
            m_Layers[i].presentationDirty = true;
            RebuildLayerPresentation(m_Layers[i], /*fullQuality=*/true);
        }
    }

    std::vector<float> acc((size_t)m_Width * m_Height * 4, 0.f);
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        Layer& L = m_Layers[i];
        if (L.isGroup || !L.visible || !isUnderGroup(i, groupIdx)) continue;

        std::vector<float> content;
        if (L.HasEnabledStyles() && L.presentationCache && !L.presentationCache->IsEmpty()) {
            content.resize((size_t)m_Width * m_Height * 4);
            L.presentationCache->ExportRGBA32F(content.data(), m_Width, m_Height);
        } else {
            content = ResolveLayerContentF(L);
            if (!L.filters.empty() && L.filteredCache && !L.filteredCache->IsEmpty()) {
                content.resize((size_t)m_Width * m_Height * 4);
                L.filteredCache->ExportRGBA32F(content.data(), m_Width, m_Height);
            }
            layer_fx::PresentationParams pp;
            pp.fillOpacity = L.opacity;
            pp.bakeFillOpacity = true;
            pp.hasMask = L.hasMask && !L.mask.empty();
            pp.mask = pp.hasMask ? L.mask.data() : nullptr;
            pp.maskBytes = L.mask.size();
            content = layer_fx::BuildPresentation(content, m_Width, m_Height, {}, {}, pp);
        }
        layer_fx::CompositeOver(acc.data(), content.data(), m_Width * m_Height);
    }

    if (!groupFilters.empty() || LayerStyleListHasEnabled(groupStyles)) {
        layer_fx::PresentationParams pp;
        pp.fillOpacity = groupOpacity;
        pp.bakeFillOpacity = true;
        pp.hasMask = groupHasMask && !groupMask.empty();
        pp.mask = pp.hasMask ? groupMask.data() : nullptr;
        pp.maskBytes = groupMask.size();
        acc = layer_fx::BuildPresentation(acc, m_Width, m_Height, groupFilters, groupStyles, pp);
    } else {
        for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i)
            acc[i * 4 + 3] *= groupOpacity;
    }

    // Collect children indices, delete high→low (DeleteLayer remaps parent ids)
    std::vector<int> childIdx;
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        if (i != groupIdx && isUnderGroup(i, groupIdx))
            childIdx.push_back(i);
    }
    std::sort(childIdx.begin(), childIdx.end(), std::greater<int>());
    int deletedBelow = 0;
    for (int di : childIdx) {
        if (di < groupIdx) deletedBelow++;
        DeleteLayer(di);
    }
    groupIdx -= deletedBelow;

    if (groupIdx < 0 || groupIdx >= (int)m_Layers.size() ||
        !(m_Layers[groupIdx].isGroup && m_Layers[groupIdx].name == groupName)) {
        groupIdx = -1;
        for (int i = 0; i < (int)m_Layers.size(); ++i)
            if (m_Layers[i].isGroup && m_Layers[i].name == groupName) { groupIdx = i; break; }
    }
    if (groupIdx < 0) return false;

    Layer& g = m_Layers[groupIdx];
    g.isGroup = false;
    g.type = Layer::Type::Raster;
    g.filters.clear();
    g.styles.clear();
    g.filteredCache.reset();
    g.presentationCache.reset();
    g.filtersDirty = false;
    g.stylesDirty = false;
    g.presentationDirty = false;
    g.opacity = 1.f;
    g.tileCache = std::make_unique<TileCache>();
    g.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
    g.tileCache->ImportRGBA32F(acc.data(), m_Width, m_Height);
    g.tileCache->MarkAllDirty();
    g.needsUpload = true;

    // Tile deltas for undo (old empty → new)
    std::vector<TileDelta> deltas;
    const int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    const int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            TileDelta d;
            d.layerIdx = groupIdx;
            d.tileX = tx; d.tileY = ty;
            d.oldState = TileSnapshot{};
            d.newState = g.tileCache->SnapshotTile(tx, ty);
            deltas.push_back(std::move(d));
        }
    }
    RasterizeCommand::LayerMeta newMeta = captureMeta(g, groupIdx);
    m_UndoRedoManager.PushCommand(std::make_shared<RasterizeCommand>(
        "Rasterize Group", groupIdx, std::move(oldGroupMeta),
        std::move(removedChildren), std::move(newMeta), std::move(deltas)));

    if (device) RecreateLayerTexture(device, g);

    m_ActiveLayerIdx = groupIdx;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("RasterizeGroup flattened: " + g.name);
    return true;
}
