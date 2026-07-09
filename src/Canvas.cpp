#include "Canvas.h"
#include "core/TileCache.h"
#include "core/Logger.h"
#include "core/MemoryStats.h"
#include "core/ImageManager.h"
#include "core/IccProfiles.h"
#include "core/PathUtil.h"
// Prefer PathUtil for all disk paths (UTF-8 / wide on Windows).
#include <opencv2/imgproc.hpp>
#include "core/ConfigManager.h"
#include "core/TexconvHelper.h"
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
                             CanvasPixelFormat fmt = CanvasPixelFormat::RGBA8) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
    layer.tileCache->ImportRGBA32F(pixels.data(), w, h);
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    layer.filtersDirty = true;
}
static bool LayerHasPixels(const Layer& layer) {
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
static inline void BlendLayerPixelU8(uint8_t* dest, float sr, float sg, float sb, float sa, BlendMode mode) {
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
    dest[3] = (uint8_t)(std::clamp(sa + da * inv, 0.f, 1.f) * 255.f + 0.5f);
}

static bool LayerEffectivelyVisible(const std::vector<Layer>& layers, const Layer& layer) {
    if (layer.isGroup || !layer.visible) return false;
    if (layer.parentGroupId >= 0 && layer.parentGroupId < (int)layers.size()) {
        if (!layers[layer.parentGroupId].visible) return false;
    }
    return LayerHasPixels(layer);
}

static const TileCache* LayerExportCache(const Layer& layer) {
    if (!layer.filters.empty() && layer.filteredCache && !layer.filteredCache->IsEmpty()) {
        return layer.filteredCache.get();
    }
    return layer.tileCache.get();
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

    std::vector<const Layer*> vis;
    vis.reserve(layers.size());
    for (const auto& layer : layers) {
        if (LayerEffectivelyVisible(layers, layer)) vis.push_back(&layer);
    }
    if (vis.empty()) return true;

    // Fast path: one Normal layer, no FX/mask, full opacity.
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

            for (const Layer* layer : vis) {
                const TileCache* cache = LayerExportCache(*layer);
                if (!cache) continue;

                const bool useMask = layer->hasMask && layer->mask.size() == (size_t)w * h;
                const float opacity = layer->opacity;
                const BlendMode mode = layer->blendMode;
                const uint8_t* tile = (cache->GetFormat() == CanvasPixelFormat::RGBA8)
                    ? cache->GetTileData(tx, ty) : nullptr;

                for (int ly = 0; ly < th; ++ly) {
                    const int y = y0 + ly;
                    for (int lx = 0; lx < tw; ++lx) {
                        const int x = x0 + lx;
                        float sr, sg, sb, sa;
                        if (tile) {
                            const uint8_t* sp = tile + ((size_t)ly * TILE_SIZE + lx) * 4;
                            sr = sp[0] / 255.f; sg = sp[1] / 255.f; sb = sp[2] / 255.f;
                            sa = (sp[3] / 255.f) * opacity;
                        } else {
                            float rgba[4];
                            cache->GetPixelF(x, y, rgba);
                            sr = rgba[0]; sg = rgba[1]; sb = rgba[2];
                            sa = rgba[3] * opacity;
                        }
                        if (useMask) sa *= layer->mask[(size_t)y * w + x] / 255.f;
                        if (sa <= 0.f) continue;
                        uint8_t* dp = out.data() + ((size_t)y * w + x) * 4;
                        BlendLayerPixelU8(dp, sr, sg, sb, sa, mode);
                    }
                }
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

static void ComputeCompositePreviewSize(int canvasW, int canvasH, int& outW, int& outH) {
    constexpr int kProxyThreshold = 4096;
    constexpr int kProxyMaxDim = 2048;

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
    if (m_RasterizerState) { m_RasterizerState->Release(); m_RasterizerState = nullptr; }

    if (m_SelectionMaskTexture) { m_SelectionMaskTexture->Release(); m_SelectionMaskTexture = nullptr; }
    if (m_SelectionMaskSRV) { m_SelectionMaskSRV->Release(); m_SelectionMaskSRV = nullptr; }

    for (auto& layer : m_Layers) {
        if (layer.texture) layer.texture->Release();
        if (layer.srv) layer.srv->Release();
        if (layer.maskTexture) layer.maskTexture->Release();
        if (layer.maskSRV) layer.maskSRV->Release();
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

    // Photoshop default: white mask = fully reveal layer content.
    layer.mask.assign((size_t)m_Width * m_Height, 255);
    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    m_PaintTarget = PaintTarget::LayerMask;
    if (m_ActiveLayerIdx != index) m_ActiveLayerIdx = index;

    if (device) {
        UpdateLayerMaskTexture(device, index);
    }
    m_CompositeDirty = true;
    Logger::Get().InfoTag("io", "Created layer mask on '" + layer.name + "' (paint target = mask)");
}

void Canvas::CreateLayerMaskFromSelection(ID3D11Device* device, int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    
    layer.mask.assign((size_t)m_Width * m_Height, 0);
    if (m_HasSelection && m_SelectionMask.size() == (size_t)m_Width * m_Height) {
        std::copy(m_SelectionMask.begin(), m_SelectionMask.end(), layer.mask.begin());
    } else {
        layer.mask.assign((size_t)m_Width * m_Height, 255);
    }
    
    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    m_PaintTarget = PaintTarget::LayerMask;
    if (m_ActiveLayerIdx != index) m_ActiveLayerIdx = index;
    
    if (device) {
        UpdateLayerMaskTexture(device, index);
    }
    m_CompositeDirty = true;
    
    Logger::Get().Info("Created layer mask from selection for layer: " + layer.name);
}

void Canvas::DeleteLayerMask(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    
    if (layer.maskTexture) { layer.maskTexture->Release(); layer.maskTexture = nullptr; }
    if (layer.maskSRV) { layer.maskSRV->Release(); layer.maskSRV = nullptr; }
    layer.mask.clear();
    layer.hasMask = false;
    layer.maskNeedsUpload = false;
    if (m_PaintTarget == PaintTarget::LayerMask && m_ActiveLayerIdx == index) {
        m_PaintTarget = PaintTarget::LayerContent;
    }
    m_CompositeDirty = true;
    
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
    if (!layer.hasMask || layer.mask.size() != need) {
        layer.mask.assign(need, 255);
        layer.hasMask = true;
    }
}

void Canvas::PaintMaskStamp(float cx, float cy, const BrushSettings& brush) {
    EnsureActiveLayerMaskAllocated();
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.hasMask || layer.mask.empty()) return;

    const float radius = std::max(1.f, brush.radius);
    const float hardness = std::clamp(brush.hardness, 0.f, 1.f);
    const float opacity = std::clamp(brush.opacity, 0.f, 1.f);
    // Mask paint: brush color luminance → white paint reveals, black hides.
    // Eraser forces black (hide). Default white brush when painting mask.
    float paintVal = 1.f;
    if (brush.erase) {
        paintVal = 0.f;
    } else {
        // Use RGB average so any brush color maps to gray; pure white/black intentional.
        paintVal = std::clamp((brush.color[0] + brush.color[1] + brush.color[2]) / 3.f, 0.f, 1.f);
    }

    const int rCeil = (int)std::ceil(radius);
    const int x0 = std::max(0, (int)std::floor(cx) - rCeil);
    const int y0 = std::max(0, (int)std::floor(cy) - rCeil);
    const int x1 = std::min(m_Width - 1, (int)std::ceil(cx) + rCeil);
    const int y1 = std::min(m_Height - 1, (int)std::ceil(cy) + rCeil);
    const float softStart = radius * hardness; // hard core radius

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > radius) continue;
            float w = 1.f;
            if (dist > softStart && radius > softStart) {
                w = 1.f - (dist - softStart) / (radius - softStart);
                w = w * w * (3.f - 2.f * w); // smoothstep
            }
            w *= opacity;
            if (w <= 0.f) continue;
            size_t idx = (size_t)y * (size_t)m_Width + (size_t)x;
            float cur = layer.mask[idx] / 255.f;
            float out = cur * (1.f - w) + paintVal * w;
            layer.mask[idx] = (uint8_t)(std::clamp(out, 0.f, 1.f) * 255.f + 0.5f);
        }
    }
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
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float rgba[4];
            layer.tileCache->GetPixelF(x, y, rgba);
            rgba[3] *= SelU82F(layer.mask[(size_t)y * m_Width + x]);
            layer.tileCache->SetPixelF(x, y, rgba);
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
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    if (!layer.hasMask) return;
    
    if (layer.maskTexture) { layer.maskTexture->Release(); layer.maskTexture = nullptr; }
    if (layer.maskSRV) { layer.maskSRV->Release(); layer.maskSRV = nullptr; }
    
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
    initData.SysMemPitch = m_Width * sizeof(uint8_t);
    
    HRESULT hr = device->CreateTexture2D(&desc, &initData, &layer.maskTexture);
    if (SUCCEEDED(hr)) {
        device->CreateShaderResourceView(layer.maskTexture, nullptr, &layer.maskSRV);
    }
    layer.maskNeedsUpload = false;
    m_CompositeDirty = true;
}

DXGI_FORMAT Canvas::GetLayerDxgiFormat() const {
    return (m_CanvasFormat == CanvasPixelFormat::RGBA32F)
        ? DXGI_FORMAT_R32G32B32A32_FLOAT
        : DXGI_FORMAT_R8G8B8A8_UNORM;
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

    m_Layers.push_back(std::move(newLayer));
    m_ActiveLayerIdx = (int)m_Layers.size() - 1;
    m_CompositeDirty = true;
    Logger::Get().Info("Created new layer: " + name);
}

void Canvas::DeleteLayer(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    
    Logger::Get().Info("Deleted layer: " + m_Layers[index].name);

    if (m_Layers[index].texture) m_Layers[index].texture->Release();
    if (m_Layers[index].srv) m_Layers[index].srv->Release();
    if (m_Layers[index].maskTexture) m_Layers[index].maskTexture->Release();
    if (m_Layers[index].maskSRV) m_Layers[index].maskSRV->Release();
    
    // Adjust parentGroupId references for remaining layers
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
}

int Canvas::DuplicateLayer(ID3D11Device* device, int index) {
    if (index < 0 || index >= (int)m_Layers.size()) return -1;
    const Layer& src = m_Layers[index];

    Layer dup;
    dup.name = src.name + " copy";
    dup.visible = src.visible;
    dup.opacity = src.opacity;
    dup.blendMode = src.blendMode;
    dup.isGroup = src.isGroup;
    dup.type = src.type;
    dup.parentGroupId = src.parentGroupId;
    dup.groupExpanded = src.groupExpanded;
    dup.smartSourceBytes = src.smartSourceBytes;
    dup.smartSourcePath = src.smartSourcePath;
    dup.smartScale = src.smartScale;
    dup.filters = src.filters;
    dup.filtersDirty = true;

    if (!dup.isGroup) {
        dup.tileCache = std::make_unique<TileCache>();
        dup.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
        if (src.tileCache && !src.tileCache->IsEmpty()) {
            auto pixels = ExportLayerF(src, m_Width, m_Height);
            SetLayerPixelsF(dup, pixels, m_Width, m_Height, m_CanvasFormat);
        }
        if (src.hasMask && !src.mask.empty()) {
            dup.hasMask = true;
            dup.mask = src.mask;
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

    BrushSettings activeBrush = brush;
    activeBrush.writeR = m_ChannelR;
    activeBrush.writeG = m_ChannelG;
    activeBrush.writeB = m_ChannelB;
    activeBrush.writeA = m_ChannelA;
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
            // Default mask brush: white (reveal). Eraser paints black.
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
            // Space stamps along segment (same idea as stroke spacing).
            float dx = sx - m_LastDabX, dy = sy - m_LastDabY;
            float dist = std::sqrt(dx * dx + dy * dy);
            float step = std::max(1.f, activeBrush.radius * std::max(0.05f, activeBrush.spacing));
            while (m_StrokeDistanceAccumulator + dist >= step) {
                float t = (step - m_StrokeDistanceAccumulator) / std::max(dist, 1e-4f);
                float px = m_LastDabX + dx * t;
                float py = m_LastDabY + dy * t;
                PaintMaskStamp(px, py, activeBrush);
                if (m_MirrorHorizontal) PaintMaskStamp((float)m_Width - px, py, activeBrush);
                if (m_MirrorVertical) PaintMaskStamp(px, (float)m_Height - py, activeBrush);
                if (m_MirrorHorizontal && m_MirrorVertical)
                    PaintMaskStamp((float)m_Width - px, (float)m_Height - py, activeBrush);
                m_LastDabX = px; m_LastDabY = py;
                m_StrokeDistanceAccumulator = 0.f;
                dx = sx - m_LastDabX; dy = sy - m_LastDabY;
                dist = std::sqrt(dx * dx + dy * dy);
            }
            m_StrokeDistanceAccumulator += dist;
            m_LastDabX = sx; m_LastDabY = sy;
            m_PrevStabilizedX = sx; m_PrevStabilizedY = sy;
        } else if (phase == StrokePhase::End) {
            m_IsStrokeActive = false;
            m_IsDocumentModified = true;
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
        m_Layers[m_ActiveLayerIdx].filtersDirty = true;
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
        m_Layers[m_ActiveLayerIdx].filtersDirty = true;
    }
    else if (phase == StrokePhase::End) {
        m_IsStrokeActive = false;
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
            if (!layer.texture) {
                if (layer.needsUpload || (layer.tileCache && layer.tileCache->GetTileCount() > 0)) {
                    // Keep retrying texture creation for large docs if first attempt failed.
                    RecreateLayerTexture(device, layer);
                }
                if (!layer.texture) continue;
            }

            // Pick source cache (filtered or raw)
            TileCache* src = nullptr;
            bool layerNeedsUpload = layer.needsUpload;
            bool filtersWereDirty = !layer.filters.empty() && layer.filtersDirty;
            if (!layer.filters.empty()) {
                RebuildFilteredPixels(layer); // may rebuild filteredCache
                src = layer.filteredCache.get();
            }
            if (!src) src = layer.tileCache.get();
            if (!src) continue;

            bool layerHadUploads = false;
            size_t dirtyUploaded = 0;
            const bool hadPending = src->HasPendingGpuWork() || layerNeedsUpload;
            auto uploadStart = std::chrono::high_resolution_clock::now();
            // Upload dirty tiles + pending GPU clears (zero tiles after undo erase).
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
            if (layerHadUploads || filtersWereDirty || layerNeedsUpload || hadPending) {
                needsCompositeRebuild = true;
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

    context->OMSetBlendState(m_LayerBlendState, nullptr, 0xFFFFFFFF);

    // Draw visible layers bottom-to-top
    for (size_t i = 0; i < m_Layers.size(); ++i) {
        Layer& layer = m_Layers[i];
        if (layer.visible && layer.srv) {
            if (layer.hasMask && (layer.maskNeedsUpload || !layer.maskSRV)) {
                ID3D11Device* device = nullptr;
                context->GetDevice(&device);
                if (device) {
                    UpdateLayerMaskTexture(device, static_cast<int>(i));
                    device->Release();
                }
            }

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                LayerBuffer* lb = (LayerBuffer*)mapped.pData;
                float hasMaskVal = (layer.hasMask && layer.maskSRV) ? 1.0f : 0.0f;
                lb->layerParams = DirectX::XMFLOAT4(layer.opacity, hasMaskVal, 0.0f, 0.0f);
                lb->transformParams = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f); // default scale=1, rot=0, non-floating
                lb->centerParams = DirectX::XMFLOAT4(0.5f, 0.5f, (float)(uint32_t)layer.blendMode, 0.0f);
                context->Unmap(m_LayerConstantBuffer, 0);
            }
            context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
            context->PSSetShaderResources(0, 1, &layer.srv);
            if (layer.hasMask && layer.maskSRV) {
                context->PSSetShaderResources(1, 1, &layer.maskSRV);
            } else {
                ID3D11ShaderResourceView* nullSRV = nullptr;
                context->PSSetShaderResources(1, 1, &nullSRV);
            }
            // Blend modes need previous composite as t2. Never bind m_CompositeSRV
            // while m_CompositeRTV is the RT — copy to history texture first.
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

                D3D11_MAPPED_SUBRESOURCE fMapped;
                if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &fMapped))) {
                    LayerBuffer* lb = (LayerBuffer*)fMapped.pData;
                    float hasFMaskVal = m_FloatingMaskSRV ? 1.0f : 0.0f;
                    lb->layerParams = DirectX::XMFLOAT4(layer.opacity, hasFMaskVal, uOff, vOff);
                    lb->transformParams = DirectX::XMFLOAT4(m_FloatingScaleX, m_FloatingScaleY, m_FloatingRotation, 1.0f); // isFloating = 1.0f
                    lb->centerParams = DirectX::XMFLOAT4(centerX, centerY, 0.0f, 0.0f);
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
        m_CanvasFormat = CanvasPixelFormat::RGBA8;
        Logger::Get().Info("Canvas format: RGBA8");

        CreateCompositeResources(device);
        m_Layers.clear();
        m_ProjectType        = ProjectType::Simple;
        m_CurrentProjectFilePath = filepath;
        m_ExportPath         = filepath;

        if (ext == "dds") {
            if (loadedDdsFormat == DdsFormat::RGBA32_FLOAT) m_ExportFormat = "RGBA32_FLOAT";
            else if (loadedDdsFormat == DdsFormat::RGBA16_UNORM) m_ExportFormat = "RGBA16_UNORM";
            else if (loadedDdsFormat == DdsFormat::RGBA16_FLOAT) m_ExportFormat = "RGBA16_FLOAT";
            else if (loadedDdsFormat == DdsFormat::R8_UNORM)     m_ExportFormat = "R8_UNORM";
            else if (loadedDdsFormat == DdsFormat::R16_FLOAT)    m_ExportFormat = "R16_FLOAT";
            else if (loadedDdsFormat == DdsFormat::R32_FLOAT)    m_ExportFormat = "R32_FLOAT";
            else m_ExportFormat = "RGBA8_UNORM";
            Logger::Get().Info("Auto-configured DDS export format: " + m_ExportFormat);
        } else if (ext == "png") {
            ExtractAndSetICCProfile(filepath);
        }
    }

    // --- Build Layer with TileCache ---
    Layer imported;
    imported.name    = filepath.substr(filepath.find_last_of("\\/") + 1);
    imported.visible = true;
    imported.opacity = 1.0f;
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
    for (auto& layer : m_Layers) {
        if (layer.isGroup) continue;
        if (!layer.filters.empty()) {
            layer.filtersDirty = true;
            RebuildFilteredPixels(layer);
            Logger::Get().InfoTag("io",
                "Export: rebuilt filters on layer '" + layer.name +
                "' (count=" + std::to_string(layer.filters.size()) +
                ", blend=" + std::to_string((int)layer.blendMode) + ")");
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
    DdsFormat ddsFmt;
    bool isNative = false;
    if (formatStr == "R8G8B8A8_UNORM" || formatStr == "RGBA8_UNORM") { ddsFmt = DdsFormat::RGBA8_UNORM; isNative = true; }
    else if (formatStr == "R16G16B16A16_UNORM" || formatStr == "RGBA16_UNORM") { ddsFmt = DdsFormat::RGBA16_UNORM; isNative = true; }
    else if (formatStr == "R16G16B16A16_FLOAT" || formatStr == "RGBA16_FLOAT") { ddsFmt = DdsFormat::RGBA16_FLOAT; isNative = true; }
    else if (formatStr == "R32G32B32A32_FLOAT" || formatStr == "RGBA32_FLOAT") { ddsFmt = DdsFormat::RGBA32_FLOAT; isNative = true; }
    else if (formatStr == "R8_UNORM") { ddsFmt = DdsFormat::R8_UNORM; isNative = true; }
    else if (formatStr == "R16_FLOAT") { ddsFmt = DdsFormat::R16_FLOAT; isNative = true; }
    else if (formatStr == "R32_FLOAT") { ddsFmt = DdsFormat::R32_FLOAT; isNative = true; }

    if (isNative) {
        return SaveCanvas(filepath, ddsFmt);
    }

    std::string tempDir = ConfigManager::GetUserSubdirectory("temp");
    std::string tempFile = tempDir + "/temp_export_uncompressed.dds";

    struct FileCleanupGuard {
        std::wstring path;
        ~FileCleanupGuard() {
            if (!path.empty()) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    } guard;
#ifdef _WIN32
    guard.path = UTF8ToWString(tempFile);
#else
    guard.path = std::wstring(tempFile.begin(), tempFile.end());
#endif

    if (!SaveCanvas(tempFile, DdsFormat::RGBA8_UNORM)) {
        Logger::Get().Error("Failed to save temporary uncompressed DDS for texconv.");
        return false;
    }

    ExportSettings settings;
    settings.isDds = true;
    settings.ddsFormatStr = formatStr;
    settings.advancedMode = true;
    settings.compressionSpeed = speed;
    settings.generateMipMaps = generateMips;
    settings.mipFilter = mipFilter;
    settings.exportPath = filepath;

    return TexconvHelper::CompressDDS(tempFile, filepath, settings);
}

std::vector<float> Canvas::GetCompositePixels() const {
    if (m_Layers.empty()) {
        return std::vector<float>((size_t)m_Width * m_Height * 4, 0.0f);
    }
    return ComposeVisibleLayers(m_Layers, m_Width, m_Height);
}

void Canvas::SampleCompositePixel(int x, int y, float outColor[4]) const {
    outColor[0] = 0.0f;
    outColor[1] = 0.0f;
    outColor[2] = 0.0f;
    outColor[3] = 0.0f;

    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return;

    std::vector<const Layer*> vis;
    vis.reserve(m_Layers.size());
    for (const auto& layer : m_Layers) {
        if (LayerEffectivelyVisible(m_Layers, layer)) vis.push_back(&layer);
    }
    if (vis.empty()) return;

    uint8_t dp[4] = { 0, 0, 0, 0 };

    for (const Layer* layer : vis) {
        const TileCache* cache = layer->tileCache.get();
        if (!cache) continue;

        const bool useMask = layer->hasMask && layer->mask.size() == (size_t)m_Width * m_Height;
        const float opacity = layer->opacity;
        const BlendMode mode = layer->blendMode;

        float rgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cache->GetPixelF(x, y, rgba);

        float sr = rgba[0];
        float sg = rgba[1];
        float sb = rgba[2];
        float sa = rgba[3] * opacity;

        if (useMask) {
            sa *= layer->mask[(size_t)y * m_Width + x] / 255.f;
        }

        if (sa <= 0.f) continue;
        BlendLayerPixelU8(dp, sr, sg, sb, sa, mode);
    }

    outColor[0] = dp[0] / 255.f;
    outColor[1] = dp[1] / 255.f;
    outColor[2] = dp[2] / 255.f;
    outColor[3] = dp[3] / 255.f;
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
    newLayer.tileCache = std::make_unique<TileCache>();
    newLayer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);

    if (width == m_Width && height == m_Height) {
        newLayer.tileCache->ImportRGBA32F(pixels.data(), width, height);
    } else {
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
    Logger::Get().Info("Created new layer from clipboard/drop: " + name);
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
    default: return "raster";
    }
}
static Layer::Type LayerTypeFromString(const std::string& s) {
    if (s == "group") return Layer::Type::Group;
    if (s == "smart_object") return Layer::Type::SmartObject;
    if (s == "vector_svg") return Layer::Type::VectorSvg;
    return Layer::Type::Raster;
}

static json LayerToJson(const Layer& layer) {
    json j;
    j["name"] = layer.name;
    j["visible"] = layer.visible;
    j["opacity"] = layer.opacity;
    j["blend_mode"] = BlendModeToString(layer.blendMode);
    j["is_group"] = layer.isGroup;
    j["parent_group_id"] = layer.parentGroupId;
    j["group_expanded"] = layer.groupExpanded;
    j["layer_type"] = LayerTypeToString(layer.isGroup ? Layer::Type::Group : layer.type);
    j["smart_source_path"] = layer.smartSourcePath;
    j["smart_scale"] = layer.smartScale;
    j["has_smart_source"] = !layer.smartSourceBytes.empty();
    j["has_mask"] = layer.hasMask && !layer.mask.empty();

    json filters = json::array();
    for (const auto& f : layer.filters) {
        json fj;
        fj["type"] = FilterTypeToString(f.type);
        fj["enabled"] = f.enabled;
        fj["p"] = { f.p[0], f.p[1], f.p[2], f.p[3] };
        if (!f.lut.empty()) fj["lut"] = f.lut;
        filters.push_back(fj);
    }
    j["filters"] = filters;
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

    if (j.contains("is_group")) layer.isGroup = j["is_group"].get<bool>();
    if (j.contains("parent_group_id")) layer.parentGroupId = j["parent_group_id"].get<int>();
    if (j.contains("group_expanded")) layer.groupExpanded = j["group_expanded"].get<bool>();
    if (j.contains("layer_type")) layer.type = LayerTypeFromString(j["layer_type"].get<std::string>());
    else if (layer.isGroup) layer.type = Layer::Type::Group;
    if (j.contains("smart_source_path")) layer.smartSourcePath = j["smart_source_path"].get<std::string>();
    if (j.contains("smart_scale")) layer.smartScale = j["smart_scale"].get<float>();
    if (j.contains("has_mask")) layer.hasMask = j["has_mask"].get<bool>();

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
            if (fj.contains("lut") && fj["lut"].is_array()) {
                f.lut = fj["lut"].get<std::vector<float>>();
            }
            layer.filters.push_back(std::move(f));
        }
    }
    layer.filtersDirty = !layer.filters.empty();
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
        metadata["project_type"] = (m_ProjectType == ProjectType::Simple) ? "simple" : "advanced";
        metadata["format_features"] = "blend_filters_mask_groups";

        metadata["export_path"] = m_ExportPath;
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
        }
        m_Layers.clear();

        m_Width = metadata["width"].get<int>();
        m_Height = metadata["height"].get<int>();
        m_ActiveLayerIdx = metadata["active_layer"].get<int>();
        m_CurrentProjectFilePath = filepath;
        m_CanvasFormat = CanvasPixelFormat::RGBA8;

        if (metadata.contains("project_type")) {
            std::string pt = metadata["project_type"].get<std::string>();
            m_ProjectType = (pt == "simple") ? ProjectType::Simple : ProjectType::Advanced;
        } else {
            m_ProjectType = ProjectType::Advanced;
        }

        if (metadata.contains("export_path")) m_ExportPath = metadata["export_path"].get<std::string>();
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
        if (metadata.contains("brush_tip_id")) m_BrushTipId = metadata["brush_tip_id"].get<std::string>();
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
                RecreateLayerTexture(device, layer);
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
    std::string projectType = (m_ProjectType == ProjectType::Simple) ? "simple" : "advanced";

    json exportMeta;
    exportMeta["export_path"] = m_ExportPath;
    exportMeta["export_format"] = m_ExportFormat;
    exportMeta["export_advanced_mode"] = m_ExportAdvancedMode;
    exportMeta["export_compression_speed"] = m_ExportCompressionSpeed;
    exportMeta["export_generate_mip_maps"] = m_ExportGenerateMipMaps;
    exportMeta["export_mip_filter"] = m_ExportMipFilter;
    exportMeta["export_png_color_space"] = m_ExportPngColorSpace;
    exportMeta["export_icc_preset"] = IccPresetName(m_ExportIccPreset);
    exportMeta["brush_tip_id"] = m_BrushTipId;

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

bool Canvas::SaveProjectAuto() {
    if (m_CurrentProjectFilePath.empty()) {
        Logger::Get().Error("Cannot auto-save: current project file path is empty.");
        return false;
    }

    if (m_ProjectType == ProjectType::Simple) {
        std::string path = m_CurrentProjectFilePath;
        size_t dot = path.find_last_of('.');
        std::string ext = "";
        if (dot != std::string::npos) {
            ext = path.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }

        bool success = false;
        if (ext == "dds") {
            success = SaveCanvasCompressed(path, m_ExportFormat, m_ExportGenerateMipMaps, m_ExportMipFilter, m_ExportCompressionSpeed);
        } else {
            // Contract: PNG via IccPreset only (no free-text path)
            success = SaveCanvasStandard(path, GetExportIccPreset());
        }

        if (success) {
            m_IsDocumentModified = false;
            Logger::Get().Info("Simple project saved back to source image: " + path);
        } else {
            Logger::Get().Error("Failed to save simple project back to: " + path);
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
    if (!device) return;
    
    if (m_SelectionMaskTexture) {
        D3D11_TEXTURE2D_DESC desc = {};
        m_SelectionMaskTexture->GetDesc(&desc);
        if (desc.Width != m_Width || desc.Height != m_Height || desc.Format != DXGI_FORMAT_R8_UNORM) {
            m_SelectionMaskTexture->Release(); m_SelectionMaskTexture = nullptr;
            m_SelectionMaskSRV->Release(); m_SelectionMaskSRV = nullptr;
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
        if (SUCCEEDED(hr)) {
            device->CreateShaderResourceView(m_SelectionMaskTexture, nullptr, &m_SelectionMaskSRV);
        }
    }
    
    if (m_SelectionMaskTexture) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (context) {
            context->UpdateSubresource(m_SelectionMaskTexture, 0, nullptr, m_SelectionMask.data(), m_Width * sizeof(uint8_t), 0);
            context->Release();
        }
    }
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
    m_WandSourceRGB.clear();
    m_WandSourceW = m_WandSourceH = 0;
    m_WandSourceLayerIdx = -1;
}

void Canvas::EnsureWandSourceCache() {
    if (m_WandSourceW == m_Width && m_WandSourceH == m_Height &&
        m_WandSourceLayerIdx == m_ActiveLayerIdx && !m_WandSourceRGB.empty()) {
        return;
    }
    std::vector<float> srcPixels;
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size() &&
        !m_Layers[m_ActiveLayerIdx].isGroup) {
        srcPixels = ExportLayerF(m_Layers[m_ActiveLayerIdx], m_Width, m_Height);
    } else {
        srcPixels = GetCompositePixels();
    }
    m_WandSourceRGB.resize((size_t)m_Width * m_Height * 3);
    for (int i = 0; i < m_Width * m_Height; ++i) {
        m_WandSourceRGB[i * 3 + 0] = (uint8_t)(std::clamp(srcPixels[i * 4 + 0], 0.f, 1.f) * 255.f + 0.5f);
        m_WandSourceRGB[i * 3 + 1] = (uint8_t)(std::clamp(srcPixels[i * 4 + 1], 0.f, 1.f) * 255.f + 0.5f);
        m_WandSourceRGB[i * 3 + 2] = (uint8_t)(std::clamp(srcPixels[i * 4 + 2], 0.f, 1.f) * 255.f + 0.5f);
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
    cv::Mat mat(m_Height, m_Width, CV_8UC3, m_WandSourceRGB.data());
    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);

    const float tol = std::clamp(tolerance, 0.0f, 1.0f);
    if (contiguous) {
        // floodFill stops at color boundary — no full-scene scan beyond connected region.
        cv::Mat mask = cv::Mat::zeros(m_Height + 2, m_Width + 2, CV_8UC1);
        cv::Scalar loDiff(tol * 255.0f, tol * 255.0f, tol * 255.0f);
        cv::Scalar upDiff(tol * 255.0f, tol * 255.0f, tol * 255.0f);
        cv::floodFill(mat, mask, cv::Point(startX, startY), cv::Scalar(255), nullptr,
                      loDiff, upDiff, 4 | cv::FLOODFILL_MASK_ONLY | (255 << 8));
        temp = mask(cv::Range(1, m_Height + 1), cv::Range(1, m_Width + 1)).clone();
    } else {
        // Non-contiguous: inRange in RGB around seed (OpenCV-optimized).
        cv::Vec3b seed = mat.at<cv::Vec3b>(startY, startX);
        int d = std::max(0, std::min(255, (int)std::lround(tol * 255.0f * std::sqrt(3.0f))));
        cv::Scalar lo(std::max(0, seed[0] - d), std::max(0, seed[1] - d), std::max(0, seed[2] - d));
        cv::Scalar hi(std::min(255, seed[0] + d), std::min(255, seed[1] + d), std::min(255, seed[2] + d));
        cv::inRange(mat, lo, hi, temp);
    }

    cv::Mat current(m_Height, m_Width, CV_8UC1);
    if (!m_SelectionMask.empty() && (int)m_SelectionMask.size() == m_Width * m_Height) {
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
        combined = temp;
    }

    m_SelectionMask.resize((size_t)m_Width * m_Height);
    std::memcpy(m_SelectionMask.data(), combined.data, m_SelectionMask.size());
    m_HasSelection = false;
    for (uint8_t val : m_SelectionMask) {
        if (val > 0) { m_HasSelection = true; break; }
    }
    m_SelectionMaskNeedsUpload = true;
    if (device) UpdateSelectionMaskTexture(device);

    if (pushUndo) {
        m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
            "Magic Wand", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
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

    cv::Mat mat = ImageManager::PixelsToMat8UC3(layerPixels, m_Width, m_Height);
    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);

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

void Canvas::DrawMoveGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen) {
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
        // Corner handles
        drawHandle(p1); drawHandle(p2); drawHandle(p3); drawHandle(p4);
        // Edge handles
        drawEdgeHandle(mT); drawEdgeHandle(mR); drawEdgeHandle(mB); drawEdgeHandle(mL);
    }
}

// ============================================================
//  Helpers
// ============================================================
#include <random>

static inline void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max({r,g,b}), mn = std::min({r,g,b});
    v = mx; float delta = mx - mn;
    s = (mx > 1e-6f) ? delta / mx : 0.f;
    if (delta < 1e-6f) { h = 0.f; return; }
    if      (mx == r) h = (g - b) / delta + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / delta + 2.f;
    else              h = (r - g) / delta + 4.f;
    h /= 6.f;
}
static inline void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b) {
    if (s < 1e-6f) { r = g = b = v; return; }
    float hh = h * 6.f; int i = (int)hh % 6; float f = hh - (int)hh;
    float p = v*(1.f-s), q = v*(1.f-s*f), t = v*(1.f-s*(1.f-f));
    switch(i) {
        case 0: r=v;g=t;b=p; break; case 1: r=q;g=v;b=p; break;
        case 2: r=p;g=v;b=t; break; case 3: r=p;g=q;b=v; break;
        case 4: r=t;g=p;b=v; break; default:r=v;g=p;b=q; break;
    }
}

static void BoxBlurH(std::vector<float>& px, int w, int h, int r) {
    std::vector<float> tmp(px.size());
    for (int y = 0; y < h; ++y) {
        float acc[4]={0,0,0,0}; int count=0;
        for (int kx=0; kx<=r; ++kx) { int cx=std::min(kx,w-1); for(int c=0;c<4;++c) acc[c]+=px[((size_t)y*w+cx)*4+c]; ++count; }
        for (int x = 0; x < w; ++x) {
            for(int c=0;c<4;++c) tmp[((size_t)y*w+x)*4+c]=acc[c]/(float)count;
            int add=x+r+1, rem=x-r;
            if (add<w) { for(int c=0;c<4;++c) acc[c]+=px[((size_t)y*w+add)*4+c]; ++count; }
            if (rem>=0) { for(int c=0;c<4;++c) acc[c]-=px[((size_t)y*w+rem)*4+c]; --count; }
        }
    }
    px=tmp;
}
static void BoxBlurV(std::vector<float>& px, int w, int h, int r) {
    std::vector<float> tmp(px.size());
    for (int x = 0; x < w; ++x) {
        float acc[4]={0,0,0,0}; int count=0;
        for (int ky=0; ky<=r; ++ky) { int cy=std::min(ky,h-1); for(int c=0;c<4;++c) acc[c]+=px[((size_t)cy*w+x)*4+c]; ++count; }
        for (int y = 0; y < h; ++y) {
            for(int c=0;c<4;++c) tmp[((size_t)y*w+x)*4+c]=acc[c]/(float)count;
            int add=y+r+1, rem=y-r;
            if (add<h) { for(int c=0;c<4;++c) acc[c]+=px[((size_t)add*w+x)*4+c]; ++count; }
            if (rem>=0) { for(int c=0;c<4;++c) acc[c]-=px[((size_t)rem*w+x)*4+c]; --count; }
        }
    }
    px=tmp;
}

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

void Canvas::ApplyBlur(float radius) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    int r=std::max(1,(int)radius);
    std::vector<float> blurred=pixels;
    for (int pass=0;pass<3;++pass){BoxBlurH(blurred,m_Width,m_Height,r);BoxBlurV(blurred,m_Width,m_Height,r);}
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        for(int c=0;c<4;++c) pixels[idx+c]=pixels[idx+c]*(1.f-sel)+blurred[idx+c]*sel;
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Blur");
    Logger::Get().Info("ApplyBlur r="+std::to_string(r));
}

void Canvas::ApplyHSV(float dH, float dS, float dV) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        float rr=pixels[idx],gg=pixels[idx+1],bb=pixels[idx+2];
        float h,s,v; RGBtoHSV(rr,gg,bb,h,s,v);
        h=fmodf(h+dH+1.f,1.f); s=std::clamp(s+dS,0.f,1.f); v=std::clamp(v+dV,0.f,1.f);
        float nr,ng,nb; HSVtoRGB(h,s,v,nr,ng,nb);
        pixels[idx]  =pixels[idx]  *(1.f-sel)+nr*sel;
        pixels[idx+1]=pixels[idx+1]*(1.f-sel)+ng*sel;
        pixels[idx+2]=pixels[idx+2]*(1.f-sel)+nb*sel;
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("HSV");
    Logger::Get().Info("ApplyHSV");
}

void Canvas::ApplyCurves(const std::vector<float>& lutRGB, const std::vector<float>& lutAlpha) {
    if ((int)lutRGB.size()<256||m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    auto sample=[&](const std::vector<float>& lut, float v)->float{
        float fi=v*255.f; int i=std::clamp((int)fi,0,254); float t=fi-i;
        return lut[i]*(1.f-t)+lut[i+1]*t;
    };
    const bool hasA = (int)lutAlpha.size() >= 256;
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        for(int c=0;c<3;++c) pixels[idx+c]=pixels[idx+c]*(1.f-sel)+sample(lutRGB, pixels[idx+c])*sel;
        if (hasA) pixels[idx+3]=pixels[idx+3]*(1.f-sel)+sample(lutAlpha, pixels[idx+3])*sel;
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    CommitActiveLayerMutation("Curves");
    Logger::Get().Info("ApplyCurves");
}

void Canvas::ApplyNoise(float strength, bool colorNoise) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    if (layer.isGroup) return;
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    BackupAllActiveLayerTiles();
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-1.f,1.f);
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        if (colorNoise) { for(int c=0;c<3;++c) pixels[idx+c]=std::clamp(pixels[idx+c]+dist(rng)*strength*sel,0.f,1.f); }
        else { float n=dist(rng)*strength*sel; for(int c=0;c<3;++c) pixels[idx+c]=std::clamp(pixels[idx+c]+n,0.f,1.f); }
    }
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
//  Non-destructive Filters
// ============================================================

void Canvas::RebuildFilteredPixels(Layer& layer) {
    if (!layer.filtersDirty) return;
    if (layer.filters.empty() || !LayerHasPixels(layer)) {
        layer.filteredCache.reset();
        layer.filtersDirty=false;
        return;
    }
    std::vector<float> tmp = ExportLayerF(layer, m_Width, m_Height);
    int w=m_Width,h=m_Height;
    for (auto& f : layer.filters) {
        if (!f.enabled) continue;
        switch (f.type) {
        case FilterType::Blur: { int rr=std::max(1,(int)f.p[0]); for(int p=0;p<3;++p){BoxBlurH(tmp,w,h,rr);BoxBlurV(tmp,w,h,rr);} } break;
        case FilterType::HSV: {
            for(int i=0;i<w*h;++i){
                size_t idx=(size_t)i*4; float hr,hs,hv;
                RGBtoHSV(tmp[idx],tmp[idx+1],tmp[idx+2],hr,hs,hv);
                hr=fmodf(hr+f.p[0]+1.f,1.f); hs=std::clamp(hs+f.p[1],0.f,1.f); hv=std::clamp(hv+f.p[2],0.f,1.f);
                float r2,g2,b2; HSVtoRGB(hr,hs,hv,r2,g2,b2); tmp[idx]=r2;tmp[idx+1]=g2;tmp[idx+2]=b2;
            }
        } break;
        case FilterType::Curves: {
            if ((int)f.lut.size()==256) {
                auto sam=[&](float v)->float{ float fi=v*255.f; int ii=std::clamp((int)fi,0,254); float t=fi-ii; return f.lut[ii]*(1.f-t)+f.lut[ii+1]*t; };
                for(int i=0;i<w*h;++i){ size_t idx=(size_t)i*4; for(int c=0;c<3;++c) tmp[idx+c]=sam(tmp[idx+c]); }
            }
        } break;
        case FilterType::AlphaInvert: for(int i=0;i<w*h;++i) tmp[(size_t)i*4+3]=1.f-tmp[(size_t)i*4+3]; break;
        case FilterType::Noise: {
            std::mt19937 rng2(1337); std::uniform_real_distribution<float> dist2(-1.f,1.f);
            bool col=(f.p[1]>0.5f);
            for(int i=0;i<w*h;++i){ size_t idx=(size_t)i*4;
                if(col){for(int c=0;c<3;++c) tmp[idx+c]=std::clamp(tmp[idx+c]+dist2(rng2)*f.p[0],0.f,1.f);}
                else { float n=dist2(rng2)*f.p[0]; for(int c=0;c<3;++c) tmp[idx+c]=std::clamp(tmp[idx+c]+n,0.f,1.f); }
            }
        } break;
        }
    }
    if (!layer.filteredCache) {
        layer.filteredCache = std::make_unique<TileCache>();
    }
    layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
    layer.filteredCache->ImportRGBA32F(tmp.data(), m_Width, m_Height);
    layer.filteredCache->MarkAllDirty();
    layer.filtersDirty=false;
}

// ============================================================
//  Layer Groups
// ============================================================

void Canvas::CreateGroupCompositeResources(ID3D11Device* device) {
    ReleaseGroupCompositeResources();
    D3D11_TEXTURE2D_DESC desc={};
    desc.Width=m_Width; desc.Height=m_Height; desc.MipLevels=1; desc.ArraySize=1;
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
    m_QuickSelectMask.assign((size_t)m_Width * m_Height, 0);
    m_QuickSelectSampleCount = 0;
    m_QuickSelectLabMean[0] = m_QuickSelectLabMean[1] = m_QuickSelectLabMean[2] = 0.f;
    if (!m_QuickSelectEdgeValid) {
        // Build edge map from active layer / composite once per session revision
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
        m_QuickSelectEdge.assign((size_t)m_Width * m_Height, 0);
        float inv = (maxV > 1e-6) ? (1.0f / (float)maxV) : 1.0f;
        for (int i = 0; i < m_Width * m_Height; ++i) {
            float e = mag.at<float>(i / m_Width, i % m_Width) * inv;
            m_QuickSelectEdge[i] = (uint8_t)(std::clamp(e, 0.f, 1.f) * 255.f + 0.5f);
        }
        m_QuickSelectEdgeValid = true;
    }
}

void Canvas::StrokeQuickSelect(const std::vector<std::pair<int, int>>& points, float radius, bool subtract) {
    if (points.empty() || m_Width <= 0 || m_Height <= 0) return;
    if (m_QuickSelectMask.size() != (size_t)m_Width * m_Height)
        BeginQuickSelectStroke();

    EnsureWandSourceCache();
    const int r = std::max(1, (int)std::lround(radius));
    const int r2 = r * r;

    // Update color model from brush seeds
    for (const auto& pt : points) {
        int cx = pt.first, cy = pt.second;
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r2) continue;
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) continue;
                size_t idx = (size_t)y * m_Width + x;
                if (subtract) {
                    m_QuickSelectMask[idx] = 0;
                    continue;
                }
                // Seed pixel
                m_QuickSelectMask[idx] = 255;
                float rf = m_WandSourceRGB[idx * 3 + 0] / 255.f;
                float gf = m_WandSourceRGB[idx * 3 + 1] / 255.f;
                float bf = m_WandSourceRGB[idx * 3 + 2] / 255.f;
                float L, a, b;
                RgbToLabApprox(rf, gf, bf, L, a, b);
                int n = m_QuickSelectSampleCount;
                m_QuickSelectLabMean[0] = (m_QuickSelectLabMean[0] * n + L) / (n + 1);
                m_QuickSelectLabMean[1] = (m_QuickSelectLabMean[1] * n + a) / (n + 1);
                m_QuickSelectLabMean[2] = (m_QuickSelectLabMean[2] * n + b) / (n + 1);
                m_QuickSelectSampleCount = n + 1;
            }
        }
    }
    if (subtract || m_QuickSelectSampleCount == 0) return;

    // Constrained region grow from seeds within dilated brush band
    const float colorThresh = 0.18f;
    const float edgeStop = 0.45f; // sticky edge
    std::vector<std::pair<int, int>> q;
    q.reserve(1024);
    for (const auto& pt : points) {
        int cx = pt.first, cy = pt.second;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r2) continue;
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) continue;
                if (m_QuickSelectMask[(size_t)y * m_Width + x]) q.push_back({x, y});
            }
    }
    // Grow margin around stroke
    const int margin = r * 3;
    int minX = m_Width, minY = m_Height, maxX = 0, maxY = 0;
    for (const auto& pt : points) {
        minX = std::min(minX, pt.first - margin);
        maxX = std::max(maxX, pt.first + margin);
        minY = std::min(minY, pt.second - margin);
        maxY = std::max(maxY, pt.second + margin);
    }
    minX = std::max(0, minX); maxX = std::min(m_Width - 1, maxX);
    minY = std::max(0, minY); maxY = std::min(m_Height - 1, maxY);

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
            if (edge >= edgeStop) continue; // sticky edge
            float rf = m_WandSourceRGB[nidx * 3 + 0] / 255.f;
            float gf = m_WandSourceRGB[nidx * 3 + 1] / 255.f;
            float bf = m_WandSourceRGB[nidx * 3 + 2] / 255.f;
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

void Canvas::CancelQuickSelectStroke() {
    m_QuickSelectMask.clear();
    m_QuickSelectSampleCount = 0;
    m_QuickSelectLabMean[0] = m_QuickSelectLabMean[1] = m_QuickSelectLabMean[2] = 0.f;
    // Keep edge cache (layer unchanged).
}

void Canvas::EndQuickSelectStroke(ID3D11Device* device, bool add, bool subtract) {
    if (m_QuickSelectMask.size() != (size_t)m_Width * m_Height) return;

    // Light morpho smooth
    cv::Mat m(m_Height, m_Width, CV_8UC1, m_QuickSelectMask.data());
    cv::Mat smoothed;
    cv::morphologyEx(m, smoothed, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));
    cv::morphologyEx(smoothed, smoothed, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHas = m_HasSelection;
    cv::Mat current(m_Height, m_Width, CV_8UC1);
    if (!m_SelectionMask.empty() && (int)m_SelectionMask.size() == m_Width * m_Height)
        std::memcpy(current.data, m_SelectionMask.data(), m_SelectionMask.size());
    else
        current = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);

    cv::Mat combined;
    if (add) cv::bitwise_or(current, smoothed, combined);
    else if (subtract) cv::bitwise_and(current, ~smoothed, combined);
    else combined = smoothed;

    m_SelectionMask.resize((size_t)m_Width * m_Height);
    std::memcpy(m_SelectionMask.data(), combined.data, m_SelectionMask.size());
    m_HasSelection = false;
    for (uint8_t v : m_SelectionMask) { if (v) { m_HasSelection = true; break; } }
    m_SelectionMaskNeedsUpload = true;
    if (device) UpdateSelectionMaskTexture(device);
    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>(
        "Quick Select", std::move(oldMask), oldHas, m_SelectionMask, m_HasSelection));
    m_QuickSelectMask.clear();
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

    // Document-level undo: push a command that re-extends is incomplete without full snapshot.
    // Record as layer mutations for active layer only is insufficient.
    // Store a PaintStrokeCommand per layer by re-snapshotting is too late (already cropped).
    // Mark modified; geometry undo uses a simple "restore size + note" via Clear for v1 integrity:
    // We'll push one command that captures NOTHING but name — BAD.
    // Better: build Multi-layer command before mutation — redo this properly next.

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
// Channel preview thumbs (proxy composite → grayscale R/G/B/A)
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
    if (!device || m_CompositeWidth <= 0 || m_CompositeHeight <= 0 || !m_CompositeTexture)
        return;

    // Read back composite proxy (small — not full doc)
    D3D11_TEXTURE2D_DESC desc = {};
    m_CompositeTexture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging)
        return;

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) { staging->Release(); return; }
    ctx->CopyResource(staging, m_CompositeTexture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        ctx->Release();
        return;
    }

    const int w = (int)desc.Width;
    const int h = (int)desc.Height;
    std::vector<uint8_t> ch[4];
    for (int c = 0; c < 4; ++c) ch[c].assign((size_t)w * h, 0);

    // Assume RGBA8 composite
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (int x = 0; x < w; ++x) {
            size_t di = (size_t)y * w + x;
            ch[0][di] = row[x * 4 + 0];
            ch[1][di] = row[x * 4 + 1];
            ch[2][di] = row[x * 4 + 2];
            ch[3][di] = row[x * 4 + 3];
        }
    }
    ctx->Unmap(staging, 0);
    staging->Release();
    ctx->Release();

    ReleaseChannelPreviewResources();
    m_ChannelPreviewW = w;
    m_ChannelPreviewH = h;

    // RGBA8 grayscale (R=G=B=channel, A=255) so ImGui/DX11 samples as visible gray,
    // not R8 which returns (r,0,0,1) and looks black/near-black when tinted.
    for (int c = 0; c < 4; ++c) {
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        for (int i = 0; i < w * h; ++i) {
            uint8_t v = ch[c][(size_t)i];
            rgba[(size_t)i * 4 + 0] = v;
            rgba[(size_t)i * 4 + 1] = v;
            rgba[(size_t)i * 4 + 2] = v;
            rgba[(size_t)i * 4 + 3] = 255;
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
        m_ChannelPreviewW != m_CompositeWidth || m_ChannelPreviewH != m_CompositeHeight) {
        RebuildChannelPreviews(device);
    }
    int idx = (int)ch;
    if (idx < 0 || idx > 3) return nullptr;
    return m_ChannelPreviewSRV[idx];
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
    if (layer.type == Layer::Type::Raster && !layer.isGroup) return true;
    if (layer.isGroup) return false;
    layer.type = Layer::Type::Raster;
    layer.smartSourceBytes.clear();
    layer.smartSourcePath.clear();
    layer.needsUpload = true;
    m_CompositeDirty = true;
    m_IsDocumentModified = true;
    Logger::Get().Info("RasterizeLayer " + layer.name);
    return true;
}
