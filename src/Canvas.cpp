#include "Canvas.h"
#include "core/TileCache.h"
#include "core/Logger.h"
#include "core/ImageManager.h"
#include <opencv2/imgproc.hpp>
#include "core/ConfigManager.h"
#include "core/TexconvHelper.h"
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
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
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
static std::vector<float> ComposeVisibleLayers(const std::vector<Layer>& layers, int w, int h) {
    std::vector<float> composite((size_t)w * h * 4, 0.f);
    int firstVisibleIdx = -1;
    for (int l = 0; l < (int)layers.size(); ++l) {
        if (layers[l].visible) { firstVisibleIdx = l; break; }
    }
    if (firstVisibleIdx == -1) return composite;

    const auto& baseLayer = layers[firstVisibleIdx];
    if (LayerHasPixels(baseLayer)) {
        auto basePx = ExportLayerF(baseLayer, w, h);
        std::memcpy(composite.data(), basePx.data(), basePx.size() * sizeof(float));
        if (baseLayer.opacity < 1.f) {
            for (size_t i = 0; i < (size_t)w * h; ++i) {
                composite[i * 4 + 3] *= baseLayer.opacity;
            }
        }
    }

    for (int l = firstVisibleIdx + 1; l < (int)layers.size(); ++l) {
        const auto& layer = layers[l];
        if (!layer.visible || !LayerHasPixels(layer)) continue;
        auto layerPx = ExportLayerF(layer, w, h);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            size_t base = i * 4;
            float srcR = layerPx[base + 0];
            float srcG = layerPx[base + 1];
            float srcB = layerPx[base + 2];
            float srcA = layerPx[base + 3] * layer.opacity;
            if (srcA <= 0.f) continue;
            float destR = composite[base + 0];
            float destG = composite[base + 1];
            float destB = composite[base + 2];
            float destA = composite[base + 3];
            float outA = srcA + destA * (1.f - srcA);
            if (outA > 0.f) {
                composite[base + 0] = (srcR * srcA + destR * destA * (1.f - srcA)) / outA;
                composite[base + 1] = (srcG * srcA + destG * destA * (1.f - srcA)) / outA;
                composite[base + 2] = (srcB * srcA + destB * destA * (1.f - srcA)) / outA;
                composite[base + 3] = outA;
            }
        }
    }
    return composite;
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
    }

    m_SelectionMask.assign((size_t)m_Width * m_Height, 0);
    m_HasSelection = false;
    m_CompositeDirty = true;
}

void Canvas::ReleaseCompositeResources() {
    if (m_CompositeTexture) { m_CompositeTexture->Release(); m_CompositeTexture = nullptr; }
    if (m_CompositeRTV) { m_CompositeRTV->Release(); m_CompositeRTV = nullptr; }
    if (m_CompositeSRV) { m_CompositeSRV->Release(); m_CompositeSRV = nullptr; }
    m_CompositeWidth = 0;
    m_CompositeHeight = 0;
    if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
    if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }
    if (m_FloatingMaskTexture) { m_FloatingMaskTexture->Release(); m_FloatingMaskTexture = nullptr; }
    if (m_FloatingMaskSRV) { m_FloatingMaskSRV->Release(); m_FloatingMaskSRV = nullptr; }
}

void Canvas::CreateLayerMask(ID3D11Device* device, int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    
    layer.mask.assign((size_t)m_Width * m_Height, 255);
    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    
    if (device) {
        UpdateLayerMaskTexture(device, index);
    }
    m_CompositeDirty = true;
    
    Logger::Get().Info("Created layer mask for layer: " + layer.name);
}

void Canvas::CreateLayerMaskFromSelection(ID3D11Device* device, int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    
    layer.mask.assign((size_t)m_Width * m_Height, 0);
    if (m_HasSelection) {
        std::copy(m_SelectionMask.begin(), m_SelectionMask.end(), layer.mask.begin());
    } else {
        layer.mask.assign((size_t)m_Width * m_Height, 255);
    }
    
    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    
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
    m_CompositeDirty = true;
    
    Logger::Get().Info("Deleted layer mask for layer: " + layer.name);
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
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
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
        Logger::Get().Error("RecreateLayerTexture: CreateTexture2D failed");
        return;
    }
    device->CreateShaderResourceView(layer.texture, nullptr, &layer.srv);

    // Upload existing TileCache data if available
    if (layer.tileCache) {
        layer.tileCache->MarkAllDirty();
        layer.needsUpload = true;
    }
    layer.needsUpload = false; // will be handled by ComposeLayers dirty loop
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
    
    m_Layers.erase(m_Layers.begin() + index);

    if (m_Layers.empty()) {
        m_ActiveLayerIdx = -1;
    } else {
        m_ActiveLayerIdx = std::clamp(m_ActiveLayerIdx, 0, static_cast<int>(m_Layers.size()) - 1);
    }

    m_CompositeDirty = true;
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
    int key = tileY * numTilesX + tileX;

    if (m_ActiveStrokeDeltas.count(key)) return; // already backed up

    auto& layer = m_Layers[m_ActiveLayerIdx];
    TileDelta delta;
    delta.layerIdx  = m_ActiveLayerIdx;
    delta.tileX     = tileX;
    delta.tileY     = tileY;
    // Snapshot current tile state (empty vector if tile doesn't exist)
    delta.oldPixels = layer.tileCache ? layer.tileCache->SnapshotTile(tileX, tileY) : std::vector<uint8_t>{};

    m_ActiveStrokeDeltas[key] = std::move(delta);
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

        // Backup tiles covered by the first stamp (and its symmetries)
        backupSymmetricTiles(currRawX, currRawY, activeBrush.radius);
        if (m_MirrorHorizontal) {
            backupSymmetricTiles(static_cast<float>(m_Width) - currRawX, currRawY, activeBrush.radius);
        }
        if (m_MirrorVertical) {
            backupSymmetricTiles(currRawX, static_cast<float>(m_Height) - currRawY, activeBrush.radius);
        }
        if (m_MirrorHorizontal && m_MirrorVertical) {
            backupSymmetricTiles(static_cast<float>(m_Width) - currRawX, static_cast<float>(m_Height) - currRawY, activeBrush.radius);
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
            float minX = std::min(x0, x1) - activeBrush.radius;
            float maxX = std::max(x0, x1) + activeBrush.radius;
            float minY = std::min(y0, y1) - activeBrush.radius;
            float maxY = std::max(y0, y1) + activeBrush.radius;

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
                // Snapshot the tile AFTER the stroke (newPixels)
                delta.newPixels = layer.tileCache
                    ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                    : std::vector<uint8_t>{};
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

    // Resize selection mask (uint8_t, single-channel)
    {
        std::vector<uint8_t> oldSel = std::move(m_SelectionMask);
        m_SelectionMask.assign((size_t)m_Width * m_Height, 0);
        int copyW = std::min(oldW, m_Width);
        int copyH = std::min(oldH, m_Height);
        for (int y = 0; y < copyH; ++y) {
            for (int x = 0; x < copyW; ++x) {
                uint8_t v = oldSel.empty() ? 0 : oldSel[(size_t)y * oldW + x];
                m_SelectionMask[(size_t)y * m_Width + x] = v;
                if (v > 0) m_HasSelection = true;
            }
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
        for (size_t i = 0; i < m_Layers.size(); ++i) {
            auto& layer = m_Layers[i];
            if (layer.isGroup) continue;
            if (!layer.texture) continue;

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
            // Upload only dirty tiles
            src->ForEachDirtyTile([&](int tx, int ty, const uint8_t* data, int /*pitch*/) {
                layerHadUploads = true;
                D3D11_BOX box;
                box.left   = tx * TILE_SIZE;
                box.top    = ty * TILE_SIZE;
                box.front  = 0;
                box.right  = std::min(box.left + TILE_SIZE, (UINT)m_Width);
                box.bottom = std::min(box.top  + TILE_SIZE, (UINT)m_Height);
                box.back   = 1;
                UINT pitch = TILE_SIZE * (UINT)src->GetBytesPerPixel();
                context->UpdateSubresource(layer.texture, 0, &box, data, pitch, 0);
            });
            src->ClearAllDirty();
            layer.needsUpload = false;
            if (layerHadUploads || filtersWereDirty || layerNeedsUpload) {
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
            // Bind current composite as t2 for blend mode sampling
            // Must unbind RT first to avoid DX warning, then rebind after draw
            context->OMSetRenderTargets(0, nullptr, nullptr); // unbind RT temporarily
            context->PSSetShaderResources(2, 1, &m_CompositeSRV);
            context->OMSetRenderTargets(1, &m_CompositeRTV, nullptr); // rebind RT
            context->DrawIndexed(6, 0, 0);
            // Unbind composite SRV from t2
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
    std::ifstream file(UTF8ToWString(pngPath), std::ios::binary);
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
        std::ofstream outFile(UTF8ToWString(iccPath), std::ios::binary);
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

bool Canvas::LoadImageToLayer(ID3D11Device* device, const std::string& filepath) {
    std::string ext;
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    int imgWidth = 0, imgHeight = 0;
    std::vector<uint8_t> loadedU8;
    DdsFormat loadedDdsFormat = DdsFormat::RGBA8_UNORM;
    std::unique_ptr<TileCache> loadedTileCache;

    if (ext == "dds") {
        loadedTileCache = std::make_unique<TileCache>();
        if (!DdsHelper::LoadDDSToTileCache(filepath, *loadedTileCache, imgWidth, imgHeight, loadedDdsFormat)) {
            return false;
        }

        if (loadedDdsFormat == DdsFormat::R8_UNORM || loadedDdsFormat == DdsFormat::R16_FLOAT || loadedDdsFormat == DdsFormat::R32_FLOAT) {
            m_ChannelR = true; m_ChannelG = false; m_ChannelB = false; m_ChannelA = false;
            Logger::Get().Info("Single-channel DDS detected. Auto-configured R-only channels.");
        }
    } else {
        if (!ImageManager::LoadImageFromFile(filepath, loadedU8, imgWidth, imgHeight)) return false;
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

    RecreateLayerTexture(device, imported);
    m_Layers.push_back(std::move(imported));
    m_ActiveLayerIdx = (int)m_Layers.size() - 1;
    m_CompositeDirty = true;

    ResetView();
    Logger::Get().Info("Successfully imported layer from: " + filepath);
    return true;
}

bool Canvas::SaveCanvas(const std::string& filepath, DdsFormat ddsFormat) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save.");
        return false;
    }

    std::vector<float> composite = ComposeVisibleLayers(m_Layers, m_Width, m_Height);

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

    std::vector<float> composite = ComposeVisibleLayers(m_Layers, m_Width, m_Height);
    return ImageManager::SaveImageToFile(filepath, composite, m_Width, m_Height, iccProfilePath);
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
    }
    return res;
}

bool Canvas::Redo() {
    bool res = m_UndoRedoManager.Redo(this);
    if (res) {
        m_IsDocumentModified = true;
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

bool Canvas::SaveCanvasRayp(const std::string& filepath) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save in RAYP.");
        return false;
    }

    try {
        // 1. Create JSON metadata
        json metadata;
        metadata["width"] = m_Width;
        metadata["height"] = m_Height;
        metadata["active_layer"] = m_ActiveLayerIdx;
        metadata["project_type"] = (m_ProjectType == ProjectType::Simple) ? "simple" : "advanced";
        
        metadata["export_path"] = m_ExportPath;
        metadata["export_format"] = m_ExportFormat;
        metadata["export_advanced_mode"] = m_ExportAdvancedMode;
        metadata["export_compression_speed"] = m_ExportCompressionSpeed;
        metadata["export_generate_mip_maps"] = m_ExportGenerateMipMaps;
        metadata["export_mip_filter"] = m_ExportMipFilter;
        metadata["export_png_color_space"] = m_ExportPngColorSpace;

        json layersArray = json::array();
        for (const auto& layer : m_Layers) {
            json layerJson;
            layerJson["name"] = layer.name;
            layerJson["visible"] = layer.visible;
            layerJson["opacity"] = layer.opacity;
            layersArray.push_back(layerJson);
        }
        metadata["layers"] = layersArray;

        std::string metadataStr = metadata.dump();

        // 2. Open binary file for writing
#ifdef _WIN32
        std::ofstream out(UTF8ToWString(filepath), std::ios::binary);
#else
        std::ofstream out(filepath, std::ios::binary);
#endif
        if (!out.is_open()) {
            Logger::Get().Error("Could not open file for saving RAYP: " + filepath);
            return false;
        }

        // Write Magic header
        out.write("RAYP", 4);
        
        // Write format version
        uint32_t version = 1;
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));

        // Write Metadata size and content
        uint64_t metadataSize = metadataStr.size();
        out.write(reinterpret_cast<const char*>(&metadataSize), sizeof(metadataSize));
        out.write(metadataStr.data(), metadataStr.size());

        // 3. Compress and write pixel data for each layer
        for (auto& layer : m_Layers) {
            std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);
            uint64_t uncompressedSize = layerPixels.size() * sizeof(float);
            
            int compSize = 0;
            unsigned char* compData = stbi_zlib_compress(
                reinterpret_cast<unsigned char*>(layerPixels.data()),
                static_cast<int>(uncompressedSize),
                &compSize,
                8 // good compression level
            );

            if (!compData) {
                Logger::Get().Error("Failed to compress layer data for " + layer.name);
                return false;
            }

            uint64_t compressedSize = compSize;
            out.write(reinterpret_cast<const char*>(&uncompressedSize), sizeof(uncompressedSize));
            out.write(reinterpret_cast<const char*>(&compressedSize), sizeof(compressedSize));
            out.write(reinterpret_cast<const char*>(compData), compressedSize);

            free(compData);
        }

        m_IsDocumentModified = false;
        m_CurrentProjectFilePath = filepath;
        Logger::Get().Info("Successfully saved project to " + filepath);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Exception saving RAYP: " + std::string(e.what()));
        return false;
    }
}

bool Canvas::LoadCanvasRayp(const std::string& filepath, ID3D11Device* device) {
    try {
        // 1. Open binary file for reading
#ifdef _WIN32
        std::ifstream in(UTF8ToWString(filepath), std::ios::binary);
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

        // Read format version
        uint32_t version = 0;
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (version != 1) {
            Logger::Get().Error("Unsupported RAYP version: " + std::to_string(version));
            return false;
        }

        // Read Metadata size and content
        uint64_t metadataSize = 0;
        in.read(reinterpret_cast<char*>(&metadataSize), sizeof(metadataSize));
        
        std::string metadataStr;
        metadataStr.resize(metadataSize);
        in.read(&metadataStr[0], metadataSize);

        // Parse JSON metadata
        json metadata = json::parse(metadataStr);

        // Release old resources
        for (auto& layer : m_Layers) {
            if (layer.texture) layer.texture->Release();
            if (layer.srv) layer.srv->Release();
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

        // Recreate composition resources
        CreateCompositeResources(device);

        auto layersArray = metadata["layers"];
        for (size_t idx = 0; idx < layersArray.size(); ++idx) {
            auto layerJson = layersArray[idx];
            Layer layer;
            layer.name = layerJson["name"].get<std::string>();
            layer.visible = layerJson["visible"].get<bool>();
            layer.opacity = layerJson["opacity"].get<float>();
            
            // Read uncompressed and compressed size
            uint64_t uncompressedSize = 0;
            uint64_t compressedSize = 0;
            in.read(reinterpret_cast<char*>(&uncompressedSize), sizeof(uncompressedSize));
            in.read(reinterpret_cast<char*>(&compressedSize), sizeof(compressedSize));

            std::vector<uint8_t> compressedBytes(compressedSize);
            in.read(reinterpret_cast<char*>(compressedBytes.data()), compressedSize);

            // Decompress using stbi_zlib_decode_malloc
            int decompSize = 0;
            char* decompData = stbi_zlib_decode_malloc(
                reinterpret_cast<const char*>(compressedBytes.data()),
                static_cast<int>(compressedSize),
                &decompSize
            );

            if (!decompData || static_cast<size_t>(decompSize) != uncompressedSize) {
                Logger::Get().Error("Failed to decompress layer data for " + layer.name);
                if (decompData) free(decompData);
                return false;
            }

            std::vector<float> layerPixels(uncompressedSize / sizeof(float));
            std::memcpy(layerPixels.data(), decompData, uncompressedSize);
            free(decompData);

            layer.tileCache = std::make_unique<TileCache>();
            layer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);
            layer.tileCache->ImportRGBA32F(layerPixels.data(), m_Width, m_Height);
            layer.tileCache->MarkAllDirty();
            layer.needsUpload = true;
            RecreateLayerTexture(device, layer);

            m_Layers.push_back(std::move(layer));
        }
        m_CompositeDirty = true;

        m_UndoRedoManager.Clear();
        m_IsDocumentModified = false;
        m_CurrentProjectFilePath = filepath;
        Logger::Get().Info("Successfully loaded project from " + filepath);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Exception loading RAYP: " + std::string(e.what()));
        return false;
    }
}

static bool SaveCanvasRaypInternal(const std::string& filepath, int width, int height, int activeLayerIdx,
                                  const std::vector<std::string>& layerNames,
                                  const std::vector<bool>& layerVisibles,
                                  const std::vector<float>& layerOpacities,
                                  const std::vector<std::vector<float>>& layerPixels,
                                  const std::string& exportPath,
                                  const std::string& exportFormat,
                                  bool exportAdvancedMode,
                                  const std::string& exportCompressionSpeed,
                                  bool exportGenerateMipMaps,
                                  const std::string& exportMipFilter,
                                  const std::string& exportPngColorSpace) {
    try {
        json metadata;
        metadata["width"] = width;
        metadata["height"] = height;
        metadata["active_layer"] = activeLayerIdx;

        metadata["export_path"] = exportPath;
        metadata["export_format"] = exportFormat;
        metadata["export_advanced_mode"] = exportAdvancedMode;
        metadata["export_compression_speed"] = exportCompressionSpeed;
        metadata["export_generate_mip_maps"] = exportGenerateMipMaps;
        metadata["export_mip_filter"] = exportMipFilter;
        metadata["export_png_color_space"] = exportPngColorSpace;

        json layersArray = json::array();
        for (size_t i = 0; i < layerNames.size(); ++i) {
            json layerJson;
            layerJson["name"] = layerNames[i];
            layerJson["visible"] = layerVisibles[i];
            layerJson["opacity"] = layerOpacities[i];
            layersArray.push_back(layerJson);
        }
        metadata["layers"] = layersArray;

        std::string metadataStr = metadata.dump();

#ifdef _WIN32
        std::ofstream out(UTF8ToWString(filepath), std::ios::binary);
#else
        std::ofstream out(filepath, std::ios::binary);
#endif
        if (!out.is_open()) {
            return false;
        }

        out.write("RAYP", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));

        uint64_t metadataSize = metadataStr.size();
        out.write(reinterpret_cast<const char*>(&metadataSize), sizeof(metadataSize));
        out.write(metadataStr.data(), metadataStr.size());

        for (size_t i = 0; i < layerPixels.size(); ++i) {
            const auto& pixels = layerPixels[i];
            uint64_t uncompressedSize = pixels.size() * sizeof(float);
            
            int compSize = 0;
            unsigned char* compData = stbi_zlib_compress(
                reinterpret_cast<unsigned char*>(const_cast<float*>(pixels.data())),
                static_cast<int>(uncompressedSize),
                &compSize,
                8
            );

            if (!compData) {
                return false;
            }

            uint64_t compressedSize = compSize;
            out.write(reinterpret_cast<const char*>(&uncompressedSize), sizeof(uncompressedSize));
            out.write(reinterpret_cast<const char*>(&compressedSize), sizeof(compressedSize));
            out.write(reinterpret_cast<const char*>(compData), compressedSize);

            free(compData);
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

void Canvas::SaveCanvasRaypAsync(const std::string& filepath, std::function<void(bool)> callback) {
    int width = m_Width;
    int height = m_Height;
    int activeLayer = m_ActiveLayerIdx;
    
    std::vector<std::string> names;
    std::vector<bool> visibles;
    std::vector<float> opacities;
    std::vector<std::vector<float>> pixels;
    
    names.reserve(m_Layers.size());
    visibles.reserve(m_Layers.size());
    opacities.reserve(m_Layers.size());
    pixels.reserve(m_Layers.size());
    
    for (size_t i = 0; i < m_Layers.size(); ++i) {
        const auto& layer = m_Layers[i];
        names.push_back(layer.name);
        visibles.push_back(layer.visible);
        opacities.push_back(layer.opacity);
        pixels.push_back(ExportLayerF(layer, width, height));
    }

    std::string expPath = m_ExportPath;
    std::string expFormat = m_ExportFormat;
    bool expAdv = m_ExportAdvancedMode;
    std::string expSpeed = m_ExportCompressionSpeed;
    bool expMips = m_ExportGenerateMipMaps;
    std::string expMipF = m_ExportMipFilter;
    std::string expPngCS = m_ExportPngColorSpace;
    
    std::thread([=, pixels = std::move(pixels)]() {
        bool success = SaveCanvasRaypInternal(filepath, width, height, activeLayer, names, visibles, opacities, pixels,
                                             expPath, expFormat, expAdv, expSpeed, expMips, expMipF, expPngCS);
        if (callback) {
            callback(success);
        }
    }).detach();
}

std::vector<float> Canvas::GetComposedPixels() {
    return ComposeVisibleLayers(m_Layers, m_Width, m_Height);
}

void Canvas::CommitTransformation(const std::string& actionName) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    std::vector<TileDelta> deltas;
    deltas.reserve(m_ActiveStrokeDeltas.size());

    for (auto& pair : m_ActiveStrokeDeltas) {
        auto& delta = pair.second;
        delta.newPixels = layer.tileCache
            ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
            : std::vector<uint8_t>{};
        deltas.push_back(std::move(delta));
    }

    auto cmd = std::make_shared<PaintStrokeCommand>(
        layer.name + " " + actionName, m_ActiveLayerIdx, std::move(deltas)
    );
    m_UndoRedoManager.PushCommand(cmd);
    m_ActiveStrokeDeltas.clear();
    m_IsDocumentModified = true;
    layer.needsUpload = true;
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
            std::string icc = m_ExportPngColorSpace;
            success = SaveCanvasStandard(path, icc == "sRGB" ? "" : icc);
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
    if (!m_HasSelection) return;
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    m_SelectionMask.assign(m_Width * m_Height, 0);
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

void Canvas::ApplyMagicWandSelection(ID3D11Device* device, int startX, int startY, float tolerance, bool add, bool subtract, bool contiguous) {
    if (startX < 0 || startX >= m_Width || startY < 0 || startY >= m_Height) return;

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    std::vector<float> srcPixels;
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
        srcPixels = ExportLayerF(m_Layers[m_ActiveLayerIdx], m_Width, m_Height);
    } else {
        srcPixels = GetCompositePixels();
    }

    cv::Mat mat = ImageManager::PixelsToMat8UC3(srcPixels, m_Width, m_Height);
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
                cv::Vec3b color = mat.at<cv::Vec3b>(y, x);
                float diff = std::sqrt(
                    std::pow(static_cast<float>(color[0]) - seedColor[0], 2) +
                    std::pow(static_cast<float>(color[1]) - seedColor[1], 2) +
                    std::pow(static_cast<float>(color[2]) - seedColor[2], 2)
                ) / 255.0f;
                temp.at<uint8_t>(y, x) = (diff <= tolerance * std::sqrt(3.0f)) ? 255 : 0;
            }
        }
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

    UpdateSelectionMaskTexture(device);

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
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
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
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
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
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
    
    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;
    for (int ty = 0; ty < numTilesY; ++ty) {
        for (int tx = 0; tx < numTilesX; ++tx) {
            BackupTile(tx, ty);
        }
    }
    
    m_IsMovingPixels = true;
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_FloatingScaleX = 1.0f;
    m_FloatingScaleY = 1.0f;
    m_FloatingRotation = 0.0f;
    m_StartActiveLayerIdx = m_ActiveLayerIdx;
    
    m_OriginalSelectionMask.assign(m_Width * m_Height, 255);
    if (m_HasSelection) {
        std::copy(m_SelectionMask.begin(), m_SelectionMask.end(), m_OriginalSelectionMask.begin());
    }
    
    Layer& layer = m_Layers[m_ActiveLayerIdx];
    m_FloatingPixels.assign(m_Width * m_Height * 4, 0.0f);
    
    std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);
    
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            size_t maskIdx = y * m_Width + x;
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
    
    if (m_StartActiveLayerIdx >= 0 && m_StartActiveLayerIdx < m_Layers.size()) {
        Layer& layer = m_Layers[m_StartActiveLayerIdx];
        std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                size_t maskIdx = y * m_Width + x;
                float weight = SelU82F(m_OriginalSelectionMask[maskIdx]);
                if (weight > 0.0f) {
                    size_t pixelIdx = maskIdx * 4;
                    layerPixels[pixelIdx + 0] = m_FloatingPixels[pixelIdx + 0];
                    layerPixels[pixelIdx + 1] = m_FloatingPixels[pixelIdx + 1];
                    layerPixels[pixelIdx + 2] = m_FloatingPixels[pixelIdx + 2];
                    layerPixels[pixelIdx + 3] = m_FloatingPixels[pixelIdx + 3];
                }
            }
        }
        SetLayerPixelsF(layer, layerPixels, m_Width, m_Height, m_CanvasFormat);
    }
    
    if (m_FloatingTexture) { m_FloatingTexture->Release(); m_FloatingTexture = nullptr; }
    if (m_FloatingSRV) { m_FloatingSRV->Release(); m_FloatingSRV = nullptr; }
    if (m_FloatingMaskTexture) { m_FloatingMaskTexture->Release(); m_FloatingMaskTexture = nullptr; }
    if (m_FloatingMaskSRV) { m_FloatingMaskSRV->Release(); m_FloatingMaskSRV = nullptr; }
    m_FloatingPixels.clear();
    m_OriginalSelectionMask.clear();
    
    m_IsMovingPixels = false;
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
    m_SelectionMask.assign((size_t)m_Width * m_Height, 255);
    m_HasSelection = true;
    m_SelectionMaskNeedsUpload = true;
    Logger::Get().Info("SelectAll");
}

void Canvas::InvertSelection() {
    if (!m_HasSelection) { SelectAll(); return; }
    for (auto& v : m_SelectionMask) v = 255 - v;
    m_SelectionMaskNeedsUpload = true;
    Logger::Get().Info("InvertSelection");
}

void Canvas::InvertAlpha() {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<0.5f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        pixels[idx+3]=1.f-pixels[idx+3];
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    layer.filtersDirty=true;
    Logger::Get().Info("InvertAlpha");
}

void Canvas::ApplyBlur(float radius) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
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
    layer.filtersDirty=true;
    Logger::Get().Info("ApplyBlur r="+std::to_string(r));
}

void Canvas::ApplyHSV(float dH, float dS, float dV) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
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
    layer.filtersDirty=true;
    Logger::Get().Info("ApplyHSV");
}

void Canvas::ApplyCurves(const std::vector<float>& lut256) {
    if ((int)lut256.size()<256||m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    auto sample=[&](float v)->float{
        float fi=v*255.f; int i=std::clamp((int)fi,0,254); float t=fi-i;
        return lut256[i]*(1.f-t)+lut256[i+1]*t;
    };
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        for(int c=0;c<3;++c) pixels[idx+c]=pixels[idx+c]*(1.f-sel)+sample(pixels[idx+c])*sel;
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    layer.filtersDirty=true;
    Logger::Get().Info("ApplyCurves");
}

void Canvas::ApplyNoise(float strength, bool colorNoise) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
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
    layer.filtersDirty=true;
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
    Layer grp; grp.name=name; grp.isGroup=true; grp.visible=true; grp.opacity=1.f; grp.blendMode=BlendMode::Normal;
    m_Layers.push_back(std::move(grp));
    m_ActiveLayerIdx=(int)m_Layers.size()-1;
    m_CompositeDirty = true;
    Logger::Get().Info("Created layer group: "+name);
}
void Canvas::AddLayerToGroup(int layerIdx, int groupLayerIdx) {
    if (layerIdx<0||layerIdx>=(int)m_Layers.size()||groupLayerIdx<0||groupLayerIdx>=(int)m_Layers.size()) return;
    if (!m_Layers[groupLayerIdx].isGroup) return;
    m_Layers[layerIdx].parentGroupId=groupLayerIdx;
    m_CompositeDirty = true;
}
void Canvas::RemoveLayerFromGroup(int layerIdx) {
    if (layerIdx>=0&&layerIdx<(int)m_Layers.size()) {
        m_Layers[layerIdx].parentGroupId=-1;
        m_CompositeDirty = true;
    }
}
