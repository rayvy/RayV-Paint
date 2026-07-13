#include "Canvas.h"
#include "core/UndoRedoManager.h"
#include "layer/LayerStyles.h"
#include "core/TileCache.h"
#include "core/HalfFloat.h"
#include "core/Logger.h"
#include "core/MemoryStats.h"
#include "core/ImageManager.h"
#include "core/IccProfiles.h"
#include "core/PathUtil.h"
#include "core/ClipboardHelper.h"
#include "assets/AssetStore.h"
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

#ifdef _WIN32
#include <windows.h>
static std::wstring UTF8ToWString(const std::string& str) {
    return PathUtil::Utf8ToWide(str);
}
#endif

// Explicitly declare stbi_zlib_compress which is defined in ImageManager.cpp (via stb_image_write implementation)
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);
extern "C" char* stbi_zlib_decode_malloc(const char* buffer, int len, int* outlen);

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
static float GetSelWeight(const std::vector<uint8_t>& mask, int w, int x, int y, bool hasSel) {
    if (!hasSel || mask.empty()) return 1.f;
    return SelU82F(mask[(size_t)y * w + x]);
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
    if (MemoryStats::ExceedsRamBudget(est, 0.40)) {
        Logger::Get().ErrorTag("mem",
            std::string(context) + ": estimated " + MemoryStats::FormatBytes(est) +
            " exceeds 40% of system RAM budget.");
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
    if (MemoryStats::ExceedsRamBudget(bytes, 0.50)) {
        Logger::Get().ErrorTag("mem",
            "ComposeVisibleLayersRGBA8: " + MemoryStats::FormatBytes(bytes) +
            " exceeds 50% RAM budget.");
        return false;
    }

    Logger::Get().InfoTag("io",
        "Export composite RGBA8 " + std::to_string(w) + "x" + std::to_string(h) +
        " est=" + MemoryStats::FormatBytes(bytes) + " (filters+blend modes)");
    MemoryStats::LogSnapshot("export_rgba8_alloc");

    out.assign(bytes, 0);

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
                if (solidFill)
                    layer->fill.ResolveRgba(fillSolid);

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
    hr = device->CreateRasterizerState(&rd, &m_RasterizerState);
    if (FAILED(hr)) {
        return false;
    }

    // Composite targets and default layer are created lazily on first use.

    return true;
}

void Canvas::Shutdown() {
    ReleaseCompositeResources();

    if (m_VertexBuffer) { m_VertexBuffer->Release(); m_VertexBuffer = nullptr; }
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

    if (m_SelectionMaskTexture) { m_SelectionMaskTexture->Release(); m_SelectionMaskTexture = nullptr; }
    if (m_SelectionMaskSRV) { m_SelectionMaskSRV->Release(); m_SelectionMaskSRV = nullptr; }

    for (auto& layer : m_Layers) {
        if (layer.texture) layer.texture->Release();
        if (layer.srv) layer.srv->Release();
        if (layer.maskTexture) layer.maskTexture->Release();
        if (layer.maskSRV) layer.maskSRV->Release();
        if (layer.thumbSRV) { layer.thumbSRV->Release(); layer.thumbSRV = nullptr; }
        if (layer.thumbTex) { layer.thumbTex->Release(); layer.thumbTex = nullptr; }
    }
    m_Layers.clear();
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
    if (MemoryStats::ExceedsRamBudget(maskBytes, 0.25)) {
        Logger::Get().ErrorTag("mem", "CreateLayerMask refused: would exceed RAM budget.");
        return;
    }

    bool oldHas = layer.hasMask;
    std::vector<MaskTileSnapshot> oldTiles;
    if (oldHas && layer.maskTiles && layer.maskTiles->Valid())
        oldTiles = layer.maskTiles->SnapshotAll();
    // Photoshop default: white mask = fully reveal (sparse tiles = empty = default 255)
    layer.maskTiles = std::make_unique<MaskTiles>();
    layer.maskTiles->Init(m_Width, m_Height, 255);
    layer.mask.assign((size_t)m_Width * m_Height, 255);
    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    layer.maskDirtyX0 = 0; layer.maskDirtyY0 = 0;
    layer.maskDirtyX1 = m_Width - 1; layer.maskDirtyY1 = m_Height - 1;
    m_PaintTarget = PaintTarget::LayerMask;
    if (m_ActiveLayerIdx != index) m_ActiveLayerIdx = index;

    if (device) {
        UpdateLayerMaskTexture(device, index);
    }
    m_CompositeDirty = true;
    // White mask = empty tile set (cheap undo)
    m_UndoRedoManager.PushCommand(std::make_shared<LayerMaskCommand>(
        "Add Mask", index, oldHas, std::move(oldTiles),
        true, std::vector<MaskTileSnapshot>{}, m_Width, m_Height));
    m_IsDocumentModified = true;
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
    m_CompositeDirty = true;
    m_UndoRedoManager.PushCommand(std::make_shared<LayerMaskCommand>(
        "Add Mask from Selection", index, oldHas, std::move(oldTiles),
        true, std::move(newTiles), m_Width, m_Height));
    m_IsDocumentModified = true;

    Logger::Get().Info("Created layer mask from selection for layer: " + layer.name);
}

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
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newState = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : TileSnapshot{};
            deltas.push_back(std::move(delta));
        }
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

    // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION is typically 16384.
    constexpr UINT kMaxTexDim = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if ((UINT)m_Width > kMaxTexDim || (UINT)m_Height > kMaxTexDim) {
        Logger::Get().ErrorTag("gpu",
            "RecreateLayerTexture: canvas " + std::to_string(m_Width) + "x" +
            std::to_string(m_Height) + " exceeds D3D11 max texture dim " +
            std::to_string(kMaxTexDim) + ". Need tiled GPU layers (B1b).");
        return;
    }

    const size_t estGpu = MemoryStats::EstimateImageBytes(m_Width, m_Height, BytesPerPixel(m_CanvasFormat));
    Logger::Get().InfoTag("gpu",
        "RecreateLayerTexture " + std::to_string(m_Width) + "x" + std::to_string(m_Height) +
        " estVRAM~" + MemoryStats::FormatBytes(estGpu));

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

    // Upload existing TileCache data if available
    if (layer.tileCache) {
        layer.tileCache->MarkAllDirty();
        layer.needsUpload = true;
    }
    layer.needsUpload = false; // will be handled by ComposeLayers dirty loop
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
        if (const assets::TextureAsset* a = assets::AssetStore::Get().Get(s.fill.textureAssetKey)) {
            if (!a->rgba.empty() && a->w > 0 && a->h > 0) {
                s.fill.textureRgba = a->rgba;
                s.fill.textureW = a->w;
                s.fill.textureH = a->h;
            }
        }
    }
    s.filters = L.filters;
    s.styles = L.styles;
    // Outline textures also live as embedded rgba on style — already copied via styles.
    s.smartPath = L.smartSourcePath;
    s.smartBytes = L.smartSourceBytes;
    s.smartScale = L.smartScale;
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
            if (const assets::TextureAsset* a = assets::AssetStore::Get().Get(p.fill.textureAssetKey)) {
                if (!a->rgba.empty() && a->w > 0 && a->h > 0) {
                    p.fill.textureRgba = a->rgba;
                    p.fill.textureW = a->w;
                    p.fill.textureH = a->h;
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
            if (a.outlineGradient.size() != b.outlineGradient.size()) return false;
        }
        return true;
    };
    if (same()) return;
    m_UndoRedoManager.PushCommand(std::make_shared<LayerPropsCommand>(
        actionName ? actionName : "Layer Edit", layerIdx, before, std::move(after)));
    m_IsDocumentModified = true;
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
    if (m_Layers[index].texture) m_Layers[index].texture->Release();
    if (m_Layers[index].srv) m_Layers[index].srv->Release();
    if (m_Layers[index].maskTexture) m_Layers[index].maskTexture->Release();
    if (m_Layers[index].maskSRV) m_Layers[index].maskSRV->Release();
    if (m_Layers[index].thumbSRV) { m_Layers[index].thumbSRV->Release(); m_Layers[index].thumbSRV = nullptr; }
    if (m_Layers[index].thumbTex) { m_Layers[index].thumbTex->Release(); m_Layers[index].thumbTex = nullptr; }

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
    dup.smartScale = src.smartScale;
    dup.fill = src.fill;
    // Shared fill texture: bump AssetStore ref (copy does not Acquire).
    if (!dup.fill.textureAssetKey.empty())
        assets::AssetStore::Get().AddRef(dup.fill.textureAssetKey);
    dup.workSpace = src.workSpace; // map participation
    dup.filters = src.filters;
    dup.filtersDirty = true;
    dup.styles = src.styles;
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
        }
        layer.needsUpload = true;
        layer.filtersDirty = true;
    }
    m_ActiveStrokeDeltas.clear();
    m_CompositeDirty = true;
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
        PaintEngine::DrawStamp(*m_Layers[m_ActiveLayerIdx].tileCache,
                               currRawX, currRawY, activeBrush,
                               m_MirrorHorizontal, m_MirrorVertical,
                               m_HasSelection ? m_SelectionMask : std::vector<uint8_t>{});
        m_Layers[m_ActiveLayerIdx].needsUpload = true;
        // Do NOT set filtersDirty — ComposeLayers refilters only dirty tiles (keeps FX live).
        m_Layers[m_ActiveLayerIdx].thumbDirty = true;
        m_CompositeDirty = true;
        m_ChannelPreviewDirty = true;
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

        // Draw stroke segment
        PaintEngine::DrawStrokeSegment(*m_Layers[m_ActiveLayerIdx].tileCache,
                                       m_PrevStabilizedX, m_PrevStabilizedY,
                                       stabilizedX, stabilizedY,
                                       activeBrush, m_StrokeDistanceAccumulator,
                                       m_LastDabX, m_LastDabY,
                                       m_MirrorHorizontal, m_MirrorVertical,
                                       m_HasSelection ? m_SelectionMask : std::vector<uint8_t>{});

        m_PrevStabilizedX = stabilizedX;
        m_PrevStabilizedY = stabilizedY;
        m_Layers[m_ActiveLayerIdx].needsUpload = true;
        m_Layers[m_ActiveLayerIdx].thumbDirty = true;
        m_CompositeDirty = true; // recompose every dab (A-off RGB view stays live)
        m_ChannelPreviewDirty = true;
    }
    else if (phase == StrokePhase::End) {
        m_IsStrokeActive = false;
        m_CompositeDirty = true;
        m_ChannelPreviewDirty = true;
        if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
            auto& al = m_Layers[m_ActiveLayerIdx];
            // Final pass: catch any remaining dirty tiles (esp. blur halo).
            if (m_EffectsPreviewEnabled && LayerFilterListHasEnabled(al.filters))
                RefreshFilteredCache(al, /*onlyDirtyTiles=*/true);
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

            for (auto& pair : m_ActiveStrokeDeltas) {
                auto& delta = pair.second;
                // Shared snapshot AFTER the stroke (shares live TileData).
                delta.newState = layer.tileCache
                    ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                    : TileSnapshot{};
                deltas.push_back(std::move(delta));
            }

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

            // Fill layer: solid 1×1 or full presentation
            if (layer.IsFill()) {
                bool fillDirty = layer.needsUpload || layer.stylesDirty || layer.presentationDirty ||
                                 layer.filtersDirty || !layer.texture || !layer.srv;
                // Invisible fill must not force a full recomposite every frame.
                // Sticky presentationDirty used to re-upload forever (lag only dies on delete).
                if (fillDirty) {
                    if (layer.visible) {
                        EnsureFillLayerGpu(device, layer);
                        needsCompositeRebuild = true;
                        layer.thumbDirty = true;
                        m_ChannelPreviewDirty = true;
                    } else {
                        // Defer GPU; clear sticky flags that would thrash compose while hidden.
                        const bool needsFull = layer.HasEnabledStyles() ||
                            LayerFilterListHasEnabled(layer.filters) || layer.fill.HasTexture();
                        if (!needsFull) {
                            layer.needsUpload = false;
                            layer.presentationDirty = false;
                            layer.stylesDirty = false;
                        }
                    }
                }
                // Full-buffer path: styles, filters, or fill texture (visible only).
                // When Effects Preview is OFF, skip FX bake (raw/solid/texture only).
                if (layer.visible && (LayerFxPreviewActive(layer) || layer.fill.HasTexture())) {
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
                        src->ForEachDirtyTile([&](int tx, int ty, const uint8_t* data, int) {
                            D3D11_BOX box;
                            box.left = tx * TILE_SIZE; box.top = ty * TILE_SIZE; box.front = 0;
                            box.right = std::min(box.left + TILE_SIZE, (UINT)m_Width);
                            box.bottom = std::min(box.top + TILE_SIZE, (UINT)m_Height);
                            box.back = 1;
                            context->UpdateSubresource(layer.texture, 0, &box, data,
                                TILE_SIZE * (UINT)src->GetBytesPerPixel(), 0);
                        });
                        src->ClearAllDirty();
                        layer.needsUpload = false;
                        needsCompositeRebuild = true;
                    }
                }
                if (layer.visible && layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV)) {
                    UpdateLayerMaskTexture(device, static_cast<int>(i));
                    needsCompositeRebuild = true;
                }
                continue;
            }

            if (!layer.texture) {
                if (layer.needsUpload || (layer.tileCache && layer.tileCache->GetTileCount() > 0) ||
                    layer.HasEnabledStyles()) {
                    RecreateLayerTexture(device, layer);
                }
                if (!layer.texture) continue;
            }

            // Filters: sparse tile refresh (full when filtersDirty; dirty-only while painting).
            // Styles: full presentation bake is expensive — skip mid-stroke, keep filters live.
            bool filtersWereDirty = m_EffectsPreviewEnabled && LayerFilterListHasEnabled(layer.filters) &&
                (layer.filtersDirty || (layer.tileCache && layer.tileCache->HasPendingGpuWork()));
            bool stylesWereDirty = LayerStylesPreviewActive(layer) &&
                (layer.stylesDirty || layer.presentationDirty);

            if (m_EffectsPreviewEnabled && LayerFilterListHasEnabled(layer.filters)) {
                if (layer.filtersDirty || !layer.filteredCache || layer.filteredCache->IsEmpty())
                    RefreshFilteredCache(layer, /*onlyDirtyTiles=*/false);
                else if (layer.tileCache && layer.tileCache->HasPendingGpuWork())
                    // Dirty content after paint / undo — sparse refilter (FX stays live mid-stroke)
                    RefreshFilteredCache(layer, /*onlyDirtyTiles=*/true);
            }
            if (LayerStylesPreviewActive(layer) && !m_IsStrokeActive)
                RebuildLayerPresentation(layer);

            // Pick source: presentation (styles, idle) > filtered (filters live, incl. stroke) > raw
            TileCache* src = nullptr;
            bool layerNeedsUpload = layer.needsUpload;
            const bool usePres = LayerStylesPreviewActive(layer) && !m_IsStrokeActive &&
                layer.presentationCache && !layer.presentationCache->IsEmpty();
            if (usePres)
                src = layer.presentationCache.get();
            else if (m_EffectsPreviewEnabled && LayerFilterListHasEnabled(layer.filters) &&
                     layer.filteredCache && !layer.filteredCache->IsEmpty() && !layer.filtersDirty)
                src = layer.filteredCache.get();
            if (!src) src = layer.tileCache.get();
            if (!src) continue;

            bool layerHadUploads = false;
            size_t dirtyUploaded = 0;
            const bool hadPending = src->HasPendingGpuWork() || layerNeedsUpload;
            auto uploadStart = std::chrono::high_resolution_clock::now();
            src->ForEachDirtyTile([&](int tx, int ty, const uint8_t* data, int /*pitch*/) {
                layerHadUploads = true;
                ++dirtyUploaded;
                D3D11_BOX box;
                box.left   = tx * TILE_SIZE;
                box.top    = ty * TILE_SIZE;
                box.front  = 0;
                box.right  = std::min(box.left + TILE_SIZE, (UINT)m_Width);
                box.bottom = std::min(box.top  + TILE_SIZE, (UINT)m_Height);
                box.back   = 1;
                UINT pitch = TILE_SIZE * (UINT)src->GetBytesPerPixel();
                context->UpdateSubresource(layer.texture, 0, &box, data, pitch, 0);
                if (dirtyUploaded == 1 || (dirtyUploaded % 512) == 0) {
                    Logger::Get().InfoTag("gpu",
                        "Upload dirty tiles progress " + std::to_string(dirtyUploaded));
                }
            });
            src->ClearAllDirty();
            layer.needsUpload = false;
            if (layerHadUploads && dirtyUploaded > 0) {
                double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - uploadStart).count();
                Logger::Get().InfoTag("gpu",
                    "Uploaded " + std::to_string(dirtyUploaded) + " dirty tiles in " +
                    std::to_string(ms) + " ms");
            }
            if (layerHadUploads || filtersWereDirty || stylesWereDirty || layerNeedsUpload || hadPending) {
                needsCompositeRebuild = true;
                layer.thumbDirty = true;
                m_ChannelPreviewDirty = true;
            }

            if (layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV)) {
                UpdateLayerMaskTexture(device, static_cast<int>(i));
                needsCompositeRebuild = true;
            }
        }

        // Selection mask upload
        if (m_SelectionMaskNeedsUpload && m_SelectionMaskTexture && !m_SelectionMask.empty()) {
            context->UpdateSubresource(m_SelectionMaskTexture, 0, nullptr,
                m_SelectionMask.data(), m_Width * sizeof(uint8_t), 0);
            m_SelectionMaskNeedsUpload = false;
        }

        device->Release();
    }

    if (!m_CompositeRTV || !m_CompositeSRV) return;
    if (!needsCompositeRebuild) return;

    // Rebuild the proxy composite only when visible content changed.
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // Clear to transparent!
    context->ClearRenderTargetView(m_CompositeRTV, clearColor);

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
    auto drawLayerSrv = [&](Layer& layer, ID3D11ShaderResourceView* srv, bool useMask, float opacityMul) {
        if (!srv) return false;
        const bool isFirst = firstVisible && m_LayerBlendStateReplace;
        ID3D11BlendState* blend = nullptr;
        // Fill: per-map channel write mask (unchecked = leave underlay; never black-fill)
        uint8_t fillWrite = 0xF;
        if (layer.IsFill()) {
            float tmp[4];
            if (!layer.fill.ResolveForMap(m_ActiveSetMaps, m_ViewMapKind, tmp, &fillWrite))
                return false;
            // First layer + partial mask: still use replace only on enabled channels
            // so disabled channels keep clear-color underlay (typically transparent).
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
            lb->transformParams = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);
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
                if (layer.presentationCache && !layer.presentationCache->IsEmpty()) {
                    if (!layer.texture)
                        RecreateLayerTexture(groupDev, layer);
                    if (layer.texture) {
                        TileCache* src = layer.presentationCache.get();
                        src->ForEachDirtyTile([&](int tx, int ty, const uint8_t* data, int) {
                            D3D11_BOX box;
                            box.left = tx * TILE_SIZE; box.top = ty * TILE_SIZE; box.front = 0;
                            box.right = std::min(box.left + TILE_SIZE, (UINT)m_Width);
                            box.bottom = std::min(box.top + TILE_SIZE, (UINT)m_Height);
                            box.back = 1;
                            context->UpdateSubresource(layer.texture, 0, &box, data,
                                TILE_SIZE * (UINT)src->GetBytesPerPixel(), 0);
                        });
                        src->ClearAllDirty();
                        layer.needsUpload = false;
                    }
                }
                if (layer.srv) {
                    // Presentation already bakes group fill + styles → draw at opacity 1
                    const bool gFirst = firstVisible && m_LayerBlendStateReplace;
                    ID3D11BlendState* gBlend = gFirst ? m_LayerBlendStateReplace : m_LayerBlendState;
                    context->OMSetBlendState(gBlend, nullptr, 0xFFFFFFFF);
                    context->OMSetRenderTargets(1, &m_CompositeRTV, nullptr);
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        LayerBuffer* lb = (LayerBuffer*)mapped.pData;
                        lb->layerParams = DirectX::XMFLOAT4(1.f, 0.f, 0.f, 0.f);
                        lb->transformParams = DirectX::XMFLOAT4(1.f, 1.f, 0.f, 0.f);
                        float flags = 1.f + (gFirst ? 2.f : 0.f);
                        lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)layer.blendMode, flags);
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
                }
                continue;
            }

            // No group FX: GPU isolate children into group RT, then blend with group opacity
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
                if (!child.srv) continue;

                const bool childBaked = LayerStylesPreviewActive(child) && !m_IsStrokeActive &&
                    child.presentationCache && !child.presentationCache->IsEmpty();
                float childOp = childBaked ? 1.f : child.opacity;
                bool childMask = !childBaked && child.hasMask && child.maskSRV;

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
            ID3D11BlendState* gBlend = gFirst ? m_LayerBlendStateReplace : m_LayerBlendState;
            context->OMSetBlendState(gBlend, nullptr, 0xFFFFFFFF);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                LayerBuffer* lb = (LayerBuffer*)mapped.pData;
                lb->layerParams = DirectX::XMFLOAT4(layer.opacity, 0.f, 0.f, 0.f);
                lb->transformParams = DirectX::XMFLOAT4(1.f, 1.f, 0.f, 0.f);
                float flags = 1.f + (gFirst ? 2.f : 0.f);
                lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)layer.blendMode, flags);
                context->Unmap(m_LayerConstantBuffer, 0);
            }
            context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
            context->PSSetShaderResources(0, 1, &m_GroupCompositeSRV);
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
            continue;
        }

        if (layer.srv) {
            if (layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV)) {
                ID3D11Device* device = nullptr;
                context->GetDevice(&device);
                if (device) {
                    UpdateLayerMaskTexture(device, static_cast<int>(i));
                    device->Release();
                }
            }

            // Styles baked → opacity=1, mask already in presentation
            // During stroke / FX preview OFF: live content path with fill opacity
            const bool baked = LayerStylesPreviewActive(layer) && !m_IsStrokeActive &&
                layer.presentationCache && !layer.presentationCache->IsEmpty();
            const float op = baked ? 1.f : layer.opacity;
            const bool useMask = !baked && layer.hasMask && layer.maskSRV;
            drawLayerSrv(layer, layer.srv, useMask, op);

            if (m_IsMovingPixels && i == m_StartActiveLayerIdx && m_FloatingSRV) {
                float uOff = (float)m_FloatingOffsetX / (float)m_Width;
                float vOff = (float)m_FloatingOffsetY / (float)m_Height;
                
                // Calculate bounding box center of floating selection
                int minX = m_Width, maxX = 0, minY = m_Height, maxY = 0;
                bool hasPixels = false;
                for (int y = 0; y < m_Height; ++y) {
                    for (int x = 0; x < m_Width; ++x) {
                        if (m_OriginalSelectionMask[y * m_Width + x] > 0) {
                            if (x < minX) minX = x;
                            if (x > maxX) maxX = x;
                            if (y < minY) minY = y;
                            if (y > maxY) maxY = y;
                            hasPixels = true;
                        }
                    }
                }
                float cx_box = hasPixels ? (minX + maxX) * 0.5f : m_Width * 0.5f;
                float cy_box = hasPixels ? (minY + maxY) * 0.5f : m_Height * 0.5f;
                float centerX = cx_box / (float)m_Width;
                float centerY = cy_box / (float)m_Height;

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
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_CompositeDirty = false;
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

    // 3.5 Draw selection outline overlay if active
    if (m_HasSelection && m_SelectionMaskSRV) {
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

        context->PSSetShader(m_SelectionOutlinePixelShader, nullptr, 0);
        context->PSSetShaderResources(1, 1, &m_SelectionMaskSRV);
        
        context->RSSetState(m_RasterizerState);
        context->DrawIndexed(6, 0, 0);
        context->RSSetState(nullptr);
        
        ID3D11ShaderResourceView* nullSRV1 = nullptr;
        context->PSSetShaderResources(1, 1, &nullSRV1);
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
    if (!imported.texture) {
        Logger::Get().ErrorTag("gpu",
            "LoadImageToLayer: layer GPU texture missing after RecreateLayerTexture. "
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
    bool res = m_UndoRedoManager.Undo(this);
    if (res) {
        m_IsDocumentModified = true;
        // Always force composite rebuild after history travel even if a
        // command forgot to mark dirty (belt-and-suspenders with PaintStrokeCommand).
        m_CompositeDirty = true;
        Logger::Get().DebugTag("gpu", "Undo applied — composite marked dirty");
    }
    return res;
}

bool Canvas::Redo() {
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
    for (auto& L : m_Layers) {
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
    case Layer::Type::VectorSvg: return "vector_svg";
    case Layer::Type::Fill: return "fill";
    default: return "raster";
    }
}
static Layer::Type LayerTypeFromString(const std::string& s) {
    if (s == "group") return Layer::Type::Group;
    if (s == "smart_object") return Layer::Type::SmartObject;
    if (s == "vector_svg") return Layer::Type::VectorSvg;
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
    j["has_mask"] = layer.hasMask && !layer.mask.empty();

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
    if (j.contains("has_mask")) layer.hasMask = j["has_mask"].get<bool>();

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
        // Rehydrate texture via shared AssetStore (no private multi-MB copy).
        if (layer.fill.useTexture && !layer.fill.texturePath.empty()) {
            std::string key = assets::AssetStore::Get().AcquireFile(layer.fill.texturePath);
            if (!key.empty()) {
                layer.fill.textureAssetKey = key;
                if (const assets::TextureAsset* t = assets::AssetStore::Get().Get(key)) {
                    layer.fill.textureW = t->w;
                    layer.fill.textureH = t->h;
                }
                layer.fill.textureRgba.clear();
            } else {
                layer.fill.useTexture = false;
                layer.fill.textureAssetKey.clear();
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
            // Texture pixels reloaded on demand when path set (not embedded in .rayp for size)
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
            ", layers=" + std::to_string(m_Layers.size()) + ")");
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
            if (layer.texture) layer.texture->Release();
            if (layer.srv) layer.srv->Release();
            if (layer.maskTexture) layer.maskTexture->Release();
            if (layer.maskSRV) layer.maskSRV->Release();
            if (layer.thumbSRV) { layer.thumbSRV->Release(); layer.thumbSRV = nullptr; }
            if (layer.thumbTex) { layer.thumbTex->Release(); layer.thumbTex = nullptr; }
        }
        m_Layers.clear();

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
        if (progress) progress(0.15f, "metadata");

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
                    layer.tileCache->ImportRGBA32F(layerPixels.data(), m_Width, m_Height);
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

void Canvas::SaveCanvasRaypAsync(const std::string& filepath, std::function<void(bool)> callback) {
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

    std::thread([filepath, width, height, activeLayer, projectType, exportMeta,
                 layers = std::move(layers), callback]() mutable {
        bool success = SaveCanvasRaypFromSnapshots(
            filepath, width, height, activeLayer, projectType, exportMeta, layers);
        if (callback) callback(success);
    }).detach();
}

std::vector<float> Canvas::GetComposedPixels() {
    return ComposeVisibleLayers(m_Layers, m_Width, m_Height);
}

void Canvas::CommitTransformation(const std::string& actionName) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);

    // Full-layer ops must call BackupAllActiveLayerTiles() first so every grid
    // slot is covered (including previously empty tiles that Import may create).
    // Paint strokes only backup touched tiles — do NOT invent empty-oldState
    // entries for untouched existing tiles (that would erase them on undo).

    std::vector<TileDelta> deltas;
    deltas.reserve(m_ActiveStrokeDeltas.size());

    for (auto& pair : m_ActiveStrokeDeltas) {
        auto& delta = pair.second;
        delta.newState = layer.tileCache
            ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
            : TileSnapshot{};
        // Skip no-op (shared blob unchanged = no write / COW did not occur).
        if (delta.oldState.data && delta.newState.data &&
            delta.oldState.data.get() == delta.newState.data.get()) {
            continue;
        }
        if (!delta.oldState.data && !delta.newState.data) continue;
        deltas.push_back(std::move(delta));
    }

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

    m_SelectionMask.clear();
    m_HasSelection = false;
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
    if (mask.size() == (size_t)m_Width * m_Height) {
        m_SelectionMask = mask;
        m_HasSelection = false;
        for (uint8_t val : m_SelectionMask) {
            if (val > 0) {
                m_HasSelection = true;
                break;
            }
        }
        m_SelectionMaskNeedsUpload = true;
    }
}

void Canvas::UpdateSelectionMaskTexture(ID3D11Device* device) {
    if (!device || m_Width <= 0 || m_Height <= 0) return;

    if (m_SelectionMaskTexture) {
        D3D11_TEXTURE2D_DESC desc = {};
        m_SelectionMaskTexture->GetDesc(&desc);
        if (desc.Width != (UINT)m_Width || desc.Height != (UINT)m_Height || desc.Format != DXGI_FORMAT_R8_UNORM) {
            m_SelectionMaskTexture->Release();
            m_SelectionMaskTexture = nullptr;
            if (m_SelectionMaskSRV) {
                m_SelectionMaskSRV->Release();
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
    }

    if (!m_SelectionMaskTexture) return;

    // Never pass empty-vector .data() (nullptr) into UpdateSubresource — Ctrl+D / Deselect crash.
    const size_t need = (size_t)m_Width * (size_t)m_Height;
    const uint8_t* src = nullptr;
    std::vector<uint8_t> zeros;
    if (m_HasSelection && m_SelectionMask.size() == need) {
        src = m_SelectionMask.data();
    } else {
        zeros.assign(need, 0);
        src = zeros.data();
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context) {
        context->UpdateSubresource(m_SelectionMaskTexture, 0, nullptr, src,
            (UINT)(m_Width * sizeof(uint8_t)), 0);
        context->Release();
    }
    m_SelectionMaskNeedsUpload = false;
}

void Canvas::ApplyRectSelection(int x1, int y1, int x2, int y2, bool add, bool subtract) {
    if (!CanAllocateFlatComposite(m_Width, m_Height, "ApplyRectSelection")) {
        // Reuse composite guard threshold; selection is also full-frame today.
        Logger::Get().ErrorTag("mem", "Selection tools require flat masks; disabled for huge documents.");
        return;
    }
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    cv::rectangle(temp, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(255), -1);

    cv::Mat current(m_Height, m_Width, CV_8UC1);
    if (!m_SelectionMask.empty()) {
        std::memcpy(current.data, m_SelectionMask.data(), m_SelectionMask.size());
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

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::ApplyEllipseSelection(int x1, int y1, int x2, int y2, bool add, bool subtract) {
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    cv::Point center((x1 + x2) / 2, (y1 + y2) / 2);
    cv::Size axes(std::abs(x2 - x1) / 2, std::abs(y2 - y1) / 2);
    if (axes.width > 0 && axes.height > 0) {
        cv::ellipse(temp, center, axes, 0.0, 0.0, 360.0, cv::Scalar(255), -1);
    }

    cv::Mat current(m_Height, m_Width, CV_8UC1);
    if (!m_SelectionMask.empty()) {
        std::memcpy(current.data, m_SelectionMask.data(), m_SelectionMask.size());
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

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::ApplyLassoSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract) {
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    if (points.size() >= 3) {
        std::vector<cv::Point> cvPoints;
        for (const auto& p : points) {
            cvPoints.push_back(cv::Point(p.first, p.second));
        }
        std::vector<std::vector<cv::Point>> polys = { cvPoints };
        cv::fillPoly(temp, polys, cv::Scalar(255));
    }

    cv::Mat current(m_Height, m_Width, CV_8UC1);
    if (!m_SelectionMask.empty()) {
        std::memcpy(current.data, m_SelectionMask.data(), m_SelectionMask.size());
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

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
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

    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float selectionVal = m_HasSelection ? SelU82F(m_SelectionMask[(size_t)y * m_Width + x]) : 1.0f;
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
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newState = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : TileSnapshot{};
            deltas.push_back(std::move(delta));
        }
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

            float selectionVal = m_HasSelection ? SelU82F(m_SelectionMask[(size_t)y * m_Width + x]) : 1.0f;
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
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newState = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : TileSnapshot{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Gradient", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }
}

void Canvas::StartMovePixels(ID3D11Device* device) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= m_Layers.size()) return;
    if (m_IsMovingPixels) {
        CommitMovePixels(device);
    }

    BackupAllActiveLayerTiles();
    
    m_IsMovingPixels = true;
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_FloatingScaleX = 1.0f;
    m_FloatingScaleY = 1.0f;
    m_FloatingRotation = 0.0f;
    m_StartActiveLayerIdx = m_ActiveLayerIdx;
    
    m_OriginalSelectionMask.assign(m_Width * m_Height, 255);
    if (m_HasSelection && !m_SelectionMask.empty()) {
        std::copy(m_SelectionMask.begin(), m_SelectionMask.end(), m_OriginalSelectionMask.begin());
    }
    
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    // Floating buffer layout matches full document for shader UV compatibility,
    // but we only fill the selection AABB (padded) to reduce CPU work on 4K+.
    m_FloatingPixels.assign((size_t)m_Width * m_Height * 4, 0.0f);
    m_FloatingBufW = m_Width;
    m_FloatingBufH = m_Height;

    int minX = 0, minY = 0, maxX = m_Width - 1, maxY = m_Height - 1;
    if (m_HasSelection) {
        minX = m_Width; minY = m_Height; maxX = -1; maxY = -1;
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                if (m_OriginalSelectionMask[(size_t)y * m_Width + x] > 0) {
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
            }
        }
        if (maxX < minX) { minX = 0; minY = 0; maxX = m_Width - 1; maxY = m_Height - 1; }
        // pad for soft edges / bilinear
        minX = std::max(0, minX - 2); minY = std::max(0, minY - 2);
        maxX = std::min(m_Width - 1, maxX + 2); maxY = std::min(m_Height - 1, maxY + 2);
    }
    m_FloatingBBoxX = minX; m_FloatingBBoxY = minY;
    m_FloatingBBoxW = maxX - minX + 1; m_FloatingBBoxH = maxY - minY + 1;
    
    std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);
    
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            size_t maskIdx = (size_t)y * m_Width + x;
            float weight = SelU82F(m_OriginalSelectionMask[maskIdx]);
            if (weight > 0.0f) {
                size_t pixelIdx = maskIdx * 4;
                m_FloatingPixels[pixelIdx + 0] = layerPixels[pixelIdx + 0];
                m_FloatingPixels[pixelIdx + 1] = layerPixels[pixelIdx + 1];
                m_FloatingPixels[pixelIdx + 2] = layerPixels[pixelIdx + 2];
                m_FloatingPixels[pixelIdx + 3] = layerPixels[pixelIdx + 3] * weight;
                
                layerPixels[pixelIdx + 0] *= (1.0f - weight);
                layerPixels[pixelIdx + 1] *= (1.0f - weight);
                layerPixels[pixelIdx + 2] *= (1.0f - weight);
                layerPixels[pixelIdx + 3] *= (1.0f - weight);
            }
        }
    }
    
    SetLayerPixelsF(layer, layerPixels, m_Width, m_Height, m_CanvasFormat);
    
    for (int iter = 0; iter < 4; ++iter) {
        std::vector<float> nextFloating = m_FloatingPixels;
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                size_t idx = (y * m_Width + x) * 4;
                if (m_FloatingPixels[idx + 3] <= 0.05f) {
                    int dxs[] = { 0, 0, -1, 1 };
                    int dys[] = { -1, 1, 0, 0 };
                    float bestAlpha = 0.0f;
                    float padR = 0.0f, padG = 0.0f, padB = 0.0f;
                    for (int n = 0; n < 4; ++n) {
                        int nx = x + dxs[n];
                        int ny = y + dys[n];
                        if (nx >= 0 && nx < m_Width && ny >= 0 && ny < m_Height) {
                            size_t nIdx = (ny * m_Width + nx) * 4;
                            if (m_FloatingPixels[nIdx + 3] > bestAlpha) {
                                bestAlpha = m_FloatingPixels[nIdx + 3];
                                padR = m_FloatingPixels[nIdx + 0];
                                padG = m_FloatingPixels[nIdx + 1];
                                padB = m_FloatingPixels[nIdx + 2];
                            }
                        }
                    }
                    if (bestAlpha > 0.05f) {
                        nextFloating[idx + 0] = padR;
                        nextFloating[idx + 1] = padG;
                        nextFloating[idx + 2] = padB;
                    }
                }
            }
        }
        m_FloatingPixels = nextFloating;
    }
    
    if (device) {
        if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
        if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }
        
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_Width;
        desc.Height = m_Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = m_FloatingPixels.data();
        initData.SysMemPitch = m_Width * sizeof(float) * 4;
        
        HRESULT hr = device->CreateTexture2D(&desc, &initData, &m_FloatingTexture);
        if (SUCCEEDED(hr)) {
            device->CreateShaderResourceView(m_FloatingTexture, nullptr, &m_FloatingSRV);
        }
        
        if (layer.hasMask) {
            if (m_FloatingMaskTexture) { m_FloatingMaskTexture->Release(); m_FloatingMaskTexture = nullptr; }
            if (m_FloatingMaskSRV) { m_FloatingMaskSRV->Release(); m_FloatingMaskSRV = nullptr; }
            
            std::vector<uint8_t> floatingMask(m_Width * m_Height, 0);
            for (int y = 0; y < m_Height; ++y) {
                for (int x = 0; x < m_Width; ++x) {
                    size_t idx = y * m_Width + x;
                    if (m_OriginalSelectionMask[idx] > 0) {
                        floatingMask[idx] = layer.mask[idx];
                        layer.mask[idx] = 255;
                    }
                }
            }
            layer.maskNeedsUpload = true;
            
            D3D11_TEXTURE2D_DESC mDesc = {};
            mDesc.Width = m_Width;
            mDesc.Height = m_Height;
            mDesc.MipLevels = 1;
            mDesc.ArraySize = 1;
            mDesc.Format = DXGI_FORMAT_R8_UNORM;
            mDesc.SampleDesc.Count = 1;
            mDesc.SampleDesc.Quality = 0;
            mDesc.Usage = D3D11_USAGE_DEFAULT;
            mDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            
            D3D11_SUBRESOURCE_DATA mInitData = {};
            mInitData.pSysMem = floatingMask.data();
            mInitData.SysMemPitch = m_Width * sizeof(uint8_t);
            
            hr = device->CreateTexture2D(&mDesc, &mInitData, &m_FloatingMaskTexture);
            if (SUCCEEDED(hr)) {
                device->CreateShaderResourceView(m_FloatingMaskTexture, nullptr, &m_FloatingMaskSRV);
            }
        }
    }
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
    
    if (m_StartActiveLayerIdx >= 0 && m_StartActiveLayerIdx < m_Layers.size()) {
        Layer& layer = m_Layers[m_StartActiveLayerIdx];
        
        // Calculate bounding box center of floating selection
        int minX = m_Width, maxX = 0, minY = m_Height, maxY = 0;
        bool hasPixels = false;
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                if (m_OriginalSelectionMask[y * m_Width + x] > 0) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                    hasPixels = true;
                }
            }
        }
        float cx = hasPixels ? (minX + maxX) * 0.5f : m_Width * 0.5f;
        float cy = hasPixels ? (minY + maxY) * 0.5f : m_Height * 0.5f;
        
        std::vector<float> finalPixels = ExportLayerF(layer, m_Width, m_Height);
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                // Compute inverse transformation
                float px = (float)x - cx;
                float py = (float)y - cy;
                
                // Inverse translation
                px -= (float)m_FloatingOffsetX;
                py -= (float)m_FloatingOffsetY;
                
                // Inverse rotation
                float angle = -m_FloatingRotation;
                float cosA = std::cos(angle);
                float sinA = std::sin(angle);
                float rx = px * cosA - py * sinA;
                float ry = px * sinA + py * cosA;
                
                // Inverse scale
                float sx = rx;
                float sy = ry;
                if (m_FloatingScaleX > 0.0001f) sx /= m_FloatingScaleX;
                if (m_FloatingScaleY > 0.0001f) sy /= m_FloatingScaleY;
                
                // Translate back to center
                float srcX = sx + cx;
                float srcY = sy + cy;
                
                if (srcX >= 0.0f && srcX <= (float)m_Width - 1.0f && srcY >= 0.0f && srcY <= (float)m_Height - 1.0f) {
                    float srcAlpha = sampleBilinearChannel(m_FloatingPixels, m_Width, m_Height, srcX, srcY, 3);
                    if (srcAlpha > 0.0f) {
                        size_t destIdx = (y * m_Width + x) * 4;
                        float destAlpha = finalPixels[destIdx + 3];
                        float outAlpha = srcAlpha + destAlpha * (1.0f - srcAlpha);
                        if (outAlpha > 0.0f) {
                            float srcR = sampleBilinearChannel(m_FloatingPixels, m_Width, m_Height, srcX, srcY, 0);
                            float srcG = sampleBilinearChannel(m_FloatingPixels, m_Width, m_Height, srcX, srcY, 1);
                            float srcB = sampleBilinearChannel(m_FloatingPixels, m_Width, m_Height, srcX, srcY, 2);
                            
                            finalPixels[destIdx + 0] = (srcR * srcAlpha + finalPixels[destIdx + 0] * destAlpha * (1.0f - srcAlpha)) / outAlpha;
                            finalPixels[destIdx + 1] = (srcG * srcAlpha + finalPixels[destIdx + 1] * destAlpha * (1.0f - srcAlpha)) / outAlpha;
                            finalPixels[destIdx + 2] = (srcB * srcAlpha + finalPixels[destIdx + 2] * destAlpha * (1.0f - srcAlpha)) / outAlpha;
                        }
                        finalPixels[destIdx + 3] = outAlpha;
                    }
                }
            }
        }
        SetLayerPixelsF(layer, finalPixels, m_Width, m_Height, m_CanvasFormat);

        // Register transform in undo (StartMovePixels already captured oldState for all tiles).
        {
            int prevActive = m_ActiveLayerIdx;
            m_ActiveLayerIdx = m_StartActiveLayerIdx;
            CommitTransformation("Transform");
            m_ActiveLayerIdx = prevActive;
        }
        
        if (layer.hasMask && m_FloatingMaskSRV) {
            std::vector<uint8_t> finalMask = layer.mask;
            for (int y = 0; y < m_Height; ++y) {
                for (int x = 0; x < m_Width; ++x) {
                    float px = (float)x - cx;
                    float py = (float)y - cy;
                    px -= (float)m_FloatingOffsetX;
                    py -= (float)m_FloatingOffsetY;
                    
                    float angle = -m_FloatingRotation;
                    float cosA = std::cos(angle);
                    float sinA = std::sin(angle);
                    float rx = px * cosA - py * sinA;
                    float ry = px * sinA + py * cosA;
                    
                    float sx = rx;
                    float sy = ry;
                    if (m_FloatingScaleX > 0.0001f) sx /= m_FloatingScaleX;
                    if (m_FloatingScaleY > 0.0001f) sy /= m_FloatingScaleY;
                    
                    float srcX = sx + cx;
                    float srcY = sy + cy;
                    
                    if (srcX >= 0.0f && srcX <= (float)m_Width - 1.0f && srcY >= 0.0f && srcY <= (float)m_Height - 1.0f) {
                        float maskWeight = sampleBilinearMask(m_OriginalSelectionMask, m_Width, m_Height, srcX, srcY);
                        if (maskWeight > 0.0f) {
                            size_t destIdx = y * m_Width + x;
                            finalMask[destIdx] = SelF2U8(maskWeight);
                        }
                    }
                }
            }
            layer.mask = finalMask;
            layer.maskNeedsUpload = true;
        }
        
        if (m_HasSelection) {
            std::vector<uint8_t> shiftedSelection(m_Width * m_Height, 0);
            for (int y = 0; y < m_Height; ++y) {
                for (int x = 0; x < m_Width; ++x) {
                    float px = (float)x - cx;
                    float py = (float)y - cy;
                    px -= (float)m_FloatingOffsetX;
                    py -= (float)m_FloatingOffsetY;
                    
                    float angle = -m_FloatingRotation;
                    float cosA = std::cos(angle);
                    float sinA = std::sin(angle);
                    float rx = px * cosA - py * sinA;
                    float ry = px * sinA + py * cosA;
                    
                    float sx = rx;
                    float sy = ry;
                    if (m_FloatingScaleX > 0.0001f) sx /= m_FloatingScaleX;
                    if (m_FloatingScaleY > 0.0001f) sy /= m_FloatingScaleY;
                    
                    float srcX = sx + cx;
                    float srcY = sy + cy;
                    
                    if (srcX >= 0.0f && srcX <= (float)m_Width - 1.0f && srcY >= 0.0f && srcY <= (float)m_Height - 1.0f) {
                        float maskWeight = sampleBilinearMask(m_OriginalSelectionMask, m_Width, m_Height, srcX, srcY);
                        shiftedSelection[y * m_Width + x] = SelF2U8(maskWeight);
                    }
                }
            }
            m_SelectionMask = shiftedSelection;
            UpdateSelectionMaskTexture(device);
        }
    }
    
    if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
    if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }
    if (m_FloatingMaskTexture) { m_FloatingMaskTexture->Release(); m_FloatingMaskTexture = nullptr; }
    if (m_FloatingMaskSRV) { m_FloatingMaskSRV->Release(); m_FloatingMaskSRV = nullptr; }
    m_FloatingPixels.clear();
    m_OriginalSelectionMask.clear();
    
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
    m_CompositeDirty = true;
}

void Canvas::DrawMoveGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen,
                           bool showHandles) {
    if (!m_IsMovingPixels) return;
    
    int minX = m_Width, maxX = 0, minY = m_Height, maxY = 0;
    bool hasPixels = false;
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            if (m_OriginalSelectionMask[y * m_Width + x] > 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
                hasPixels = true;
            }
        }
    }
    
    if (hasPixels) {
        float cx = (minX + maxX) * 0.5f;
        float cy = (minY + maxY) * 0.5f;
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
    m_SelectionMask.assign((size_t)m_Width * m_Height, 255);
    m_HasSelection = true;
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
    m_HasSelection = false;
    for (uint8_t v : m_SelectionMask) {
        if (v > 0) { m_HasSelection = true; break; }
    }
    m_SelectionMaskNeedsUpload = true;
    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Select From Mask", std::move(oldMask), oldHas, m_SelectionMask, m_HasSelection));
    Logger::Get().Info("SelectFromLayerMask layer=" + std::to_string(layerIdx));
}

void Canvas::InvertSelection() {
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHas = m_HasSelection;
    if (!m_HasSelection) {
        m_SelectionMask.assign((size_t)m_Width * m_Height, 255);
        m_HasSelection = true;
    } else {
        if (m_SelectionMask.empty())
            m_SelectionMask.assign((size_t)m_Width * m_Height, 0);
        for (auto& v : m_SelectionMask) v = (uint8_t)(255 - v);
        m_HasSelection = false;
        for (uint8_t v : m_SelectionMask) { if (v) { m_HasSelection = true; break; } }
    }
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
//  Smudge Tool
// ============================================================

void Canvas::SmudgeOnActiveLayer(float x, float y, StrokePhase phase, const SmudgeSettings& s) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    if (!layer.CanPaintContent()) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    int r=std::max(1,(int)s.radius);

    auto pickupAt=[&](float cx, float cy){
        float acc[4]={0,0,0,0}; int cnt=0;
        for (int dy=-r;dy<=r;++dy) for (int dx=-r;dx<=r;++dx) {
            if (dx*dx+dy*dy>r*r) continue;
            int px=std::clamp((int)cx+dx,0,m_Width-1);
            int py=std::clamp((int)cy+dy,0,m_Height-1);
            float rgba[4];
            layer.tileCache->GetPixelF(px, py, rgba);
            for(int c=0;c<4;++c) acc[c]+=rgba[c]; ++cnt;
        }
        if (cnt>0) for(int c=0;c<4;++c) m_SmudgePickup[c]=acc[c]/(float)cnt;
    };

    if (phase==StrokePhase::Begin) {
        m_SmudgePickupValid=false; m_SmudgeLastX=x; m_SmudgeLastY=y; m_SmudgeDistAcc=0.f;
        pickupAt(x,y); m_SmudgePickupValid=true; return;
    }
    if (phase==StrokePhase::End) { m_SmudgePickupValid=false; return; }
    if (!m_SmudgePickupValid) return;

    float ddx=x-m_SmudgeLastX, ddy=y-m_SmudgeLastY;
    float dist=sqrtf(ddx*ddx+ddy*ddy);
    m_SmudgeDistAcc+=dist;
    float minDist=s.radius*s.spacing;
    if (m_SmudgeDistAcc<minDist) return;
    m_SmudgeDistAcc=0.f; m_SmudgeLastX=x; m_SmudgeLastY=y;

    for (int ky=-r;ky<=r;++ky) for (int kx=-r;kx<=r;++kx) {
        float d2=sqrtf((float)(kx*kx+ky*ky));
        if (d2>(float)r) continue;
        float falloff=1.f-d2/(float)r;
        float blend=s.strength*falloff;
        int px=std::clamp((int)x+kx,0,m_Width-1);
        int py=std::clamp((int)y+ky,0,m_Height-1);
        float sel = GetSelWeight(m_SelectionMask, m_Width, px, py, m_HasSelection);
        if (sel<1e-4f) continue;
        float rgba[4];
        layer.tileCache->GetPixelF(px, py, rgba);
        for(int c=0;c<4;++c) rgba[c]=rgba[c]*(1.f-blend*sel)+m_SmudgePickup[c]*blend*sel;
        layer.tileCache->SetPixelF(px, py, rgba);
    }
    pickupAt(x,y);
    layer.needsUpload=true;
    layer.filtersDirty=true;
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

        std::vector<float> region((size_t)rw * rh * 4, 0.f);
        for (int y = 0; y < rh; ++y) {
            for (int x = 0; x < rw; ++x) {
                float px[4];
                layer.tileCache->GetPixelF(rx0 + x, ry0 + y, px);
                size_t i = ((size_t)y * rw + x) * 4;
                region[i + 0] = px[0]; region[i + 1] = px[1];
                region[i + 2] = px[2]; region[i + 3] = px[3];
            }
        }

        layer_fx::ApplyPixelFilters(region, rw, rh, layer.filters, rx0, ry0);

        const int ox = x0 - rx0;
        const int oy = y0 - ry0;
        std::vector<float> tileF((size_t)tileW * tileH * 4);
        for (int y = 0; y < tileH; ++y) {
            for (int x = 0; x < tileW; ++x) {
                size_t si = ((size_t)(oy + y) * rw + (ox + x)) * 4;
                size_t di = ((size_t)y * tileW + x) * 4;
                tileF[di + 0] = region[si + 0];
                tileF[di + 1] = region[si + 1];
                tileF[di + 2] = region[si + 2];
                tileF[di + 3] = region[si + 3];
            }
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

void Canvas::EnsureFillLayerGpu(ID3D11Device* device, Layer& layer) {
    if (!device || !layer.IsFill()) return;

    // Interactive: FX bake only when Effects Preview is ON. Texture fill still needs full buffer.
    const bool fxActive = LayerFxPreviewActive(layer);
    const bool needsFull = fxActive || layer.fill.HasTexture();

    if (needsFull) {
        if (fxActive) {
            RebuildLayerPresentation(layer);
        } else {
            // Texture fill without FX (or FX preview off): bake full buffer for upload
            auto content = ResolveLayerContentF(layer);
            if (!layer.presentationCache)
                layer.presentationCache = std::make_unique<TileCache>();
            layer.presentationCache->Init(m_Width, m_Height, m_CanvasFormat);
            layer.presentationCache->ImportRGBA32F(content.data(), m_Width, m_Height);
            layer.presentationCache->MarkAllDirty();
            layer.needsUpload = true;
        }
        if (!layer.texture)
            RecreateLayerTexture(device, layer);
        // Ensure full-size texture (not leftover 1×1)
        if (layer.texture) {
            D3D11_TEXTURE2D_DESC desc{};
            layer.texture->GetDesc(&desc);
            if (desc.Width != (UINT)m_Width || desc.Height != (UINT)m_Height)
                RecreateLayerTexture(device, layer);
        }
        // presentationDirty cleared by RebuildLayerPresentation; solid+texture still needs upload path
        if (!fxActive)
            layer.presentationDirty = false;
        return;
    }

    // Cheap path: 1×1 solid color texture (sampled at any UV).
    // Multi-target Fill: pack enabled roles into current view map.
    float c[4] = {0,0,0,1};
    if (!m_ActiveSetMaps.empty()) {
        if (!layer.fill.ResolveForMap(m_ActiveSetMaps, m_ViewMapKind, c)) {
            // No contribution to this map — transparent (layer still filtered by workSpace)
            c[0] = c[1] = c[2] = 0.f; c[3] = 0.f;
        }
    } else {
        layer.fill.ResolveRgba(c);
    }
    if (layer.texture) {
        D3D11_TEXTURE2D_DESC desc{};
        layer.texture->GetDesc(&desc);
        if (desc.Width != 1 || desc.Height != 1) {
            layer.texture->Release(); layer.texture = nullptr;
            if (layer.srv) { layer.srv->Release(); layer.srv = nullptr; }
        }
    }
    if (!layer.texture) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 1; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = GetLayerDxgiFormat();
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &layer.texture))) return;
        device->CreateShaderResourceView(layer.texture, nullptr, &layer.srv);
    }
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (ctx) {
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
    // MUST clear all dirty flags — sticky presentationDirty forced full recomposite every frame
    // after enabling multi-map fill channels (lag only ended when the layer was deleted).
    layer.needsUpload = false;
    layer.presentationDirty = false;
    layer.stylesDirty = false;
    layer.filtersDirty = false;
}

bool Canvas::LoadFillTexture(int layerIdx, const std::string& filepath) {
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
    };

    if (filepath.empty()) {
        releasePrev();
        layer.fill.useTexture = false;
        layer.fill.texturePath.clear();
        layer.needsUpload = true;
        layer.presentationDirty = true;
        m_CompositeDirty = true;
        m_IsDocumentModified = true;
        return true;
    }

    // Shared decode via AssetStore — N fill layers with same path → 1 CPU blob.
    std::string key = assets::AssetStore::Get().AcquireFile(filepath);
    if (key.empty()) {
        Logger::Get().Error("LoadFillTexture failed: " + filepath);
        return false;
    }
    const assets::TextureAsset* tex = assets::AssetStore::Get().Get(key);
    if (!tex || tex->w <= 0 || tex->h <= 0) {
        assets::AssetStore::Get().Release(key);
        Logger::Get().Error("LoadFillTexture: empty asset " + filepath);
        return false;
    }

    releasePrev();
    layer.fill.useTexture = true;
    layer.fill.texturePath = filepath;
    layer.fill.textureAssetKey = key;
    layer.fill.textureW = tex->w;
    layer.fill.textureH = tex->h;
    // No private textureRgba copy — sample through AssetStore in FillSolidBuffer.
    layer.needsUpload = true;
    layer.presentationDirty = true;
    layer.presentationCache.reset();
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("Fill texture (AssetStore) " + std::to_string(tex->w) + "x" +
                       std::to_string(tex->h) + " key=" + key);
    return true;
}

bool Canvas::LoadOutlineTexture(int layerIdx, int styleIdx, const std::string& filepath) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size()) return false;
    Layer& layer = m_Layers[layerIdx];
    if (styleIdx < 0 || styleIdx >= (int)layer.styles.size()) return false;
    LayerStyle& st = layer.styles[styleIdx];
    if (filepath.empty()) {
        st.outlineTexturePath.clear();
        st.outlineTextureRgba.clear();
        st.outlineTextureW = st.outlineTextureH = 0;
        RequestPresentationRebuild(layerIdx);
        return true;
    }
    std::vector<uint8_t> px;
    int tw = 0, th = 0;
    if (!ImageManager::LoadImageFromFile(filepath, px, tw, th)) {
        Logger::Get().Error("LoadOutlineTexture failed: " + filepath);
        return false;
    }
    st.outlineTexturePath = filepath;
    st.outlineTextureRgba = std::move(px);
    st.outlineTextureW = tw;
    st.outlineTextureH = th;
    st.outlineFill = OutlineFillMode::Texture;
    RequestPresentationRebuild(layerIdx);
    m_IsDocumentModified = true;
    Logger::Get().Info("Outline texture loaded " + std::to_string(tw) + "x" + std::to_string(th));
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
    m_CompositeDirty = true;
    m_ChannelPreviewDirty = true;
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

    // Ensure children presentations/filters are ready
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        if (i == groupIdx || !IsLayerUnderGroup(i, groupIdx)) continue;
        Layer& ch = m_Layers[i];
        if (ch.isGroup) continue;
        if (ch.filtersDirty) RebuildFilteredPixels(ch);
        if (ch.HasEnabledStyles() && (ch.presentationDirty || ch.stylesDirty || !ch.presentationCache))
            RebuildLayerPresentation(ch, fullQuality);
    }

    // Flatten children in stack order (bottom → top)
    std::vector<float> acc((size_t)m_Width * m_Height * 4, 0.f);
    for (int i = 0; i < (int)m_Layers.size(); ++i) {
        Layer& L = m_Layers[i];
        if (L.isGroup || !L.visible || !IsLayerUnderGroup(i, groupIdx)) continue;

        std::vector<float> content;
        if (L.HasEnabledStyles() && L.presentationCache && !L.presentationCache->IsEmpty()) {
            content.resize((size_t)m_Width * m_Height * 4);
            L.presentationCache->ExportRGBA32F(content.data(), m_Width, m_Height);
        } else {
            content = ResolveLayerContentF(L);
            if (!L.filters.empty()) {
                if (L.filtersDirty) RebuildFilteredPixels(L);
                if (L.filteredCache && !L.filteredCache->IsEmpty()) {
                    content.resize((size_t)m_Width * m_Height * 4);
                    L.filteredCache->ExportRGBA32F(content.data(), m_Width, m_Height);
                }
            }
            layer_fx::PresentationParams pp;
            pp.fillOpacity = L.opacity;
            pp.bakeFillOpacity = true;
            pp.hasMask = L.hasMask && !L.mask.empty();
            pp.mask = pp.hasMask ? L.mask.data() : nullptr;
            pp.maskBytes = L.mask.size();
            pp.previewQuality = !fullQuality;
            content = layer_fx::BuildPresentation(content, m_Width, m_Height, {}, {}, pp);
        }
        layer_fx::CompositeOver(acc.data(), content.data(), m_Width * m_Height);
    }

    // Group filters + styles on the flattened sum
    layer_fx::PresentationParams gpp;
    gpp.fillOpacity = group.opacity;
    gpp.bakeFillOpacity = true; // styles independent: silhouette uses pre-fillOpacity path inside BuildPresentation
    gpp.hasMask = group.hasMask && !group.mask.empty();
    gpp.mask = gpp.hasMask ? group.mask.data() : nullptr;
    gpp.maskBytes = group.mask.size();
    gpp.previewQuality = !fullQuality;

    // If only opacity and no FX: still store flattened with opacity baked for simple draw
    if (!group.filters.empty() || group.HasEnabledStyles()) {
        // For group styles independence: pass fillOpacity correctly
        // BuildPresentation: silhouette without fill, content with fill
        gpp.fillOpacity = group.opacity;
        acc = layer_fx::BuildPresentation(acc, m_Width, m_Height, group.filters, group.styles, gpp);
    } else {
        for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i)
            acc[i * 4 + 3] = std::clamp(acc[i * 4 + 3] * group.opacity, 0.f, 1.f);
    }

    // Optional proxy bake for large docs when preview
    if (!fullQuality) {
        int bakeW = m_Width, bakeH = m_Height;
        ComputeCompositePreviewSize(m_Width, m_Height, bakeW, bakeH);
        if (bakeW != m_Width || bakeH != m_Height) {
            // RebuildGroup already did full flatten — downsample result for cheaper style already done
            // at full; for large docs, downsample final for GPU upload size still full texture...
            // Keep full for simplicity of upload path; cost was flatten. OK.
            (void)bakeW; (void)bakeH;
        }
    }

    if (!group.presentationCache)
        group.presentationCache = std::make_unique<TileCache>();
    group.presentationCache->Init(m_Width, m_Height, m_CanvasFormat);
    group.presentationCache->ImportRGBA32F(acc.data(), m_Width, m_Height);
    group.presentationCache->MarkAllDirty();
    group.stylesDirty = false;
    group.presentationDirty = false;
    group.filtersDirty = false;
    group.needsUpload = true;
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
    m_CompositeDirty = true;
}
void Canvas::RemoveLayerFromGroup(int layerIdx) {
    if (layerIdx>=0&&layerIdx<(int)m_Layers.size()) {
        m_Layers[layerIdx].parentGroupId=-1;
        m_CompositeDirty = true;
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
    m_CompositeDirty = true;
    return newIdx;
}

int Canvas::MoveLayerIntoGroup(int layerIdx, int groupIdx) {
    if (layerIdx < 0 || layerIdx >= (int)m_Layers.size() ||
        groupIdx < 0 || groupIdx >= (int)m_Layers.size()) return layerIdx;
    if (layerIdx == groupIdx || !m_Layers[groupIdx].isGroup) return layerIdx;
    if (m_Layers[layerIdx].isGroup) return layerIdx;

    int targetIdx = (layerIdx > groupIdx) ? groupIdx
                                         : ((groupIdx > 0) ? groupIdx - 1 : 0);
    int newLayerIdx = ReorderLayer(layerIdx, targetIdx);
    int newGroupIdx = MapLayerIndexAfterReorder(groupIdx, layerIdx, targetIdx);
    if (newGroupIdx >= 0 && newGroupIdx < (int)m_Layers.size() && m_Layers[newGroupIdx].isGroup)
        m_Layers[newLayerIdx].parentGroupId = newGroupIdx;
    else
        m_Layers[newLayerIdx].parentGroupId = -1;
    m_CompositeDirty = true;
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
    int minX = m_Width, minY = m_Height, maxX = -1, maxY = -1;
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            if (m_SelectionMask[(size_t)y * m_Width + x] > 0) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
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
            layer.filteredCache->ConvertFormat(target);
            layer.filteredCache->MarkAllDirty();
            layer.filtersDirty = true;
        }
        // Drop GPU textures — recreated on next compose with new DXGI format.
        if (layer.texture) { layer.texture->Release(); layer.texture = nullptr; }
        if (layer.srv)     { layer.srv->Release();     layer.srv = nullptr; }
        if (layer.thumbSRV) { layer.thumbSRV->Release(); layer.thumbSRV = nullptr; }
        if (layer.thumbTex) { layer.thumbTex->Release(); layer.thumbTex = nullptr; }
        layer.thumbDirty = true;
    }

    m_DocumentBitDepth = d;
    m_CanvasFormat = target;
    m_CompositeDirty = true;
    m_ChannelPreviewDirty = true;

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
    if (m_ChannelPreviewDirty || !m_ChannelPreviewSRV[(int)ch] ||
        m_ChannelPreviewW <= 0 || m_ChannelPreviewH <= 0) {
        RebuildChannelPreviews(device);
    }
    int idx = (int)ch;
    if (idx < 0 || idx > 3) return nullptr;
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

    if (layer.thumbSRV) { layer.thumbSRV->Release(); layer.thumbSRV = nullptr; }
    if (layer.thumbTex) { layer.thumbTex->Release(); layer.thumbTex = nullptr; }

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
// SVG Smart Object (minimal rasterizer: OpenCV-free; parse via simple image fallback)
// ---------------------------------------------------------------------------
bool Canvas::ImportSvgAsSmartObject(ID3D11Device* device, const std::string& filepath) {
#ifdef _WIN32
    std::ifstream in(PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(filepath)), std::ios::binary);
#else
    std::ifstream in(filepath, std::ios::binary);
#endif
    if (!in) {
        Logger::Get().Error("ImportSvgAsSmartObject: cannot open " + filepath);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;

    // Minimal SVG size parse: width/height attributes or viewBox
    int svgW = m_Width > 0 ? m_Width : 512;
    int svgH = m_Height > 0 ? m_Height : 512;
    std::string s(bytes.begin(), bytes.end());
    auto findAttr = [&](const char* name) -> float {
        auto p = s.find(name);
        if (p == std::string::npos) return -1.f;
        p = s.find('"', p);
        if (p == std::string::npos) return -1.f;
        return (float)std::atof(s.c_str() + p + 1);
    };
    float aw = findAttr("width="), ah = findAttr("height=");
    if (aw > 1.f && ah > 1.f) { svgW = (int)aw; svgH = (int)ah; }
    else {
        auto vb = s.find("viewBox");
        if (vb != std::string::npos) {
            auto q = s.find('"', vb);
            if (q != std::string::npos) {
                float minx, miny, vw, vh;
                if (sscanf(s.c_str() + q + 1, "%f %f %f %f", &minx, &miny, &vw, &vh) == 4 && vw > 1 && vh > 1) {
                    svgW = (int)vw; svgH = (int)vh;
                }
            }
        }
    }
    svgW = std::clamp(svgW, 1, 8192);
    svgH = std::clamp(svgH, 1, 8192);

    // Placeholder raster: checker + label (full SVG path raster needs nanosvg in later pass)
    std::vector<float> pixels((size_t)svgW * svgH * 4, 0.f);
    for (int y = 0; y < svgH; ++y) {
        for (int x = 0; x < svgW; ++x) {
            size_t i = ((size_t)y * svgW + x) * 4;
            bool c = ((x / 16) ^ (y / 16)) & 1;
            pixels[i + 0] = c ? 0.85f : 0.55f;
            pixels[i + 1] = c ? 0.85f : 0.55f;
            pixels[i + 2] = c ? 0.90f : 0.60f;
            pixels[i + 3] = 1.0f;
        }
    }

    // Try system conversion: if file can be loaded by STB after external convert — skip.
    // Prefer ImageMagick-less path: use OpenCV only for raster formats.

    bool isFirst = m_Layers.empty() ||
        (m_Layers.size() == 1 && m_Layers[0].name == "Background" &&
         (!m_Layers[0].tileCache || m_Layers[0].tileCache->IsEmpty()));
    if (isFirst) {
        m_Width = svgW;
        m_Height = svgH;
        m_CanvasFormat = CanvasPixelFormat::RGBA8;
        if (device) CreateCompositeResources(device);
        m_Layers.clear();
    }

    Layer layer;
    layer.name = std::filesystem::path(filepath).filename().string();
    layer.type = Layer::Type::VectorSvg;
    layer.smartSourceBytes = std::move(bytes);
    layer.smartSourcePath = filepath;
    layer.tileCache = std::make_unique<TileCache>();
    layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
    // Center placeholder into document
    int dx = (m_Width - svgW) / 2;
    int dy = (m_Height - svgH) / 2;
    if (svgW == m_Width && svgH == m_Height) {
        layer.tileCache->ImportRGBA32F(pixels.data(), svgW, svgH);
    } else {
        std::vector<float> full((size_t)m_Width * m_Height * 4, 0.f);
        for (int y = 0; y < svgH; ++y) {
            int yy = y + dy;
            if (yy < 0 || yy >= m_Height) continue;
            for (int x = 0; x < svgW; ++x) {
                int xx = x + dx;
                if (xx < 0 || xx >= m_Width) continue;
                size_t si = ((size_t)y * svgW + x) * 4;
                size_t di = ((size_t)yy * m_Width + xx) * 4;
                for (int c = 0; c < 4; ++c) full[di + c] = pixels[si + c];
            }
        }
        layer.tileCache->ImportRGBA32F(full.data(), m_Width, m_Height);
    }
    layer.needsUpload = true;
    if (device) RecreateLayerTexture(device, layer);
    m_Layers.push_back(std::move(layer));
    m_ActiveLayerIdx = (int)m_Layers.size() - 1;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("Imported SVG as VectorSvg smart object: " + filepath);
    Logger::Get().Warn("SVG raster is placeholder until nanosvg path; source bytes preserved for Rasterize/re-render.");
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

    layer.type = Layer::Type::Raster;
    layer.isGroup = false;
    layer.smartSourceBytes.clear();
    layer.smartSourcePath.clear();
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
