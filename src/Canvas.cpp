#include "Canvas.h"
#include "core/Logger.h"
#include "core/ImageManager.h"
#include <d3dcompiler.h>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <stb_image.h>
#include <stb_image_write.h>
#include <nlohmann/json.hpp>
#include <thread>

// Explicitly declare stbi_zlib_compress which is defined in ImageManager.cpp (via stb_image_write implementation)
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

using json = nlohmann::json;

// Helper to compile shaders from file at runtime
static HRESULT CompileShaderFromFile(const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut) {
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
    return S_OK;
}

// Get the directory containing the running executable
static std::wstring GetExecutableDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : path.substr(0, pos);
}

Canvas::Canvas()
    : m_Width(1024)
    , m_Height(1024)
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

    // 9. Create Composite targets
    CreateCompositeResources(device);

    // 10. Start with one default layer
    CreateNewLayer(device, "Background");

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
    if (m_InputLayout) { m_InputLayout->Release(); m_InputLayout = nullptr; }
    if (m_SamplerState) { m_SamplerState->Release(); m_SamplerState = nullptr; }
    if (m_LayerBlendState) { m_LayerBlendState->Release(); m_LayerBlendState = nullptr; }

    for (auto& layer : m_Layers) {
        if (layer.texture) layer.texture->Release();
        if (layer.srv) layer.srv->Release();
    }
    m_Layers.clear();
}

void Canvas::CreateCompositeResources(ID3D11Device* device) {
    ReleaseCompositeResources();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_Width;
    desc.Height = m_Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_CompositeTexture);
    if (SUCCEEDED(hr)) {
        device->CreateRenderTargetView(m_CompositeTexture, nullptr, &m_CompositeRTV);
        device->CreateShaderResourceView(m_CompositeTexture, nullptr, &m_CompositeSRV);
    }
}

void Canvas::ReleaseCompositeResources() {
    if (m_CompositeTexture) { m_CompositeTexture->Release(); m_CompositeTexture = nullptr; }
    if (m_CompositeRTV) { m_CompositeRTV->Release(); m_CompositeRTV = nullptr; }
    if (m_CompositeSRV) { m_CompositeSRV->Release(); m_CompositeSRV = nullptr; }
}

void Canvas::RecreateLayerTexture(ID3D11Device* device, Layer& layer) {
    if (layer.texture) { layer.texture->Release(); layer.texture = nullptr; }
    if (layer.srv) { layer.srv->Release(); layer.srv = nullptr; }

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
    initData.pSysMem = layer.pixels.data();
    initData.SysMemPitch = m_Width * sizeof(float) * 4;

    HRESULT hr = device->CreateTexture2D(&desc, &initData, &layer.texture);
    if (SUCCEEDED(hr)) {
        device->CreateShaderResourceView(layer.texture, nullptr, &layer.srv);
    }
    layer.needsUpload = false;
}

void Canvas::CreateNewLayer(ID3D11Device* device, const std::string& name) {
    Layer newLayer;
    newLayer.name = name;
    newLayer.visible = true;
    newLayer.opacity = 1.0f;
    newLayer.pixels.resize((size_t)m_Width * m_Height * 4, 0.0f);
    newLayer.needsUpload = true;

    // Background starts white or transparent? Let's make it transparent.
    RecreateLayerTexture(device, newLayer);

    m_Layers.push_back(newLayer);
    m_ActiveLayerIdx = static_cast<int>(m_Layers.size()) - 1;
    Logger::Get().Info("Created new layer: " + name);
}

void Canvas::DeleteLayer(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    
    Logger::Get().Info("Deleted layer: " + m_Layers[index].name);

    if (m_Layers[index].texture) m_Layers[index].texture->Release();
    if (m_Layers[index].srv) m_Layers[index].srv->Release();
    
    m_Layers.erase(m_Layers.begin() + index);

    if (m_Layers.empty()) {
        m_ActiveLayerIdx = -1;
    } else {
        m_ActiveLayerIdx = std::clamp(m_ActiveLayerIdx, 0, static_cast<int>(m_Layers.size()) - 1);
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
}

void Canvas::BackupTile(int tileX, int tileY) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    int numTilesX = (m_Width + 255) / 256;
    int key = tileY * numTilesX + tileX;
    
    if (m_ActiveStrokeDeltas.find(key) != m_ActiveStrokeDeltas.end()) {
        return; // Already backed up this tile
    }

    auto& layer = m_Layers[m_ActiveLayerIdx];
    TileDelta delta;
    delta.layerIdx = m_ActiveLayerIdx;
    delta.tileX = tileX;
    delta.tileY = tileY;
    delta.oldPixels.resize(256 * 256 * 4, 0.0f);

    int startX = tileX * 256;
    int startY = tileY * 256;
    for (int y = 0; y < 256; ++y) {
        int canvasY = startY + y;
        if (canvasY >= m_Height) break;
        for (int x = 0; x < 256; ++x) {
            int canvasX = startX + x;
            if (canvasX >= m_Width) break;
            
            int tileOffset = (y * 256 + x) * 4;
            int canvasOffset = (canvasY * m_Width + canvasX) * 4;
            
            delta.oldPixels[tileOffset + 0] = layer.pixels[canvasOffset + 0];
            delta.oldPixels[tileOffset + 1] = layer.pixels[canvasOffset + 1];
            delta.oldPixels[tileOffset + 2] = layer.pixels[canvasOffset + 2];
            delta.oldPixels[tileOffset + 3] = layer.pixels[canvasOffset + 3];
        }
    }
    m_ActiveStrokeDeltas[key] = std::move(delta);
}

extern float g_PenPressure;

void Canvas::PaintOnActiveLayer(float currRawX, float currRawY, StrokePhase phase, const BrushSettings& brush) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;

    BrushSettings activeBrush = brush;
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

    if (phase == StrokePhase::Begin) {
        m_IsStrokeActive = true;
        m_StrokeDistanceAccumulator = 0.0f;
        m_LastDabX = currRawX;
        m_LastDabY = currRawY;
        m_PrevStabilizedX = currRawX;
        m_PrevStabilizedY = currRawY;
        m_ActiveStrokeDeltas.clear();

        // Backup tiles covered by the first stamp
        float minX = currRawX - activeBrush.radius;
        float maxX = currRawX + activeBrush.radius;
        float minY = currRawY - activeBrush.radius;
        float maxY = currRawY + activeBrush.radius;
        
        int minTileX = std::max(0, static_cast<int>(minX) / 256);
        int maxTileX = std::max(0, static_cast<int>(maxX) / 256);
        int minTileY = std::max(0, static_cast<int>(minY) / 256);
        int maxTileY = std::max(0, static_cast<int>(maxY) / 256);

        for (int ty = minTileY; ty <= std::min(maxTileY, numTilesY - 1); ++ty) {
            for (int tx = minTileX; tx <= std::min(maxTileX, numTilesX - 1); ++tx) {
                BackupTile(tx, ty);
            }
        }

        // Place the very first stamp immediately
        PaintEngine::DrawStamp(m_Layers[m_ActiveLayerIdx].pixels, m_Width, m_Height, 
                               currRawX, currRawY, activeBrush);
        m_Layers[m_ActiveLayerIdx].needsUpload = true;
    }
    else if (phase == StrokePhase::Update && m_IsStrokeActive) {
        // Apply stabilization
        float weight = 1.0f / static_cast<float>(std::max(1, activeBrush.stabilization));
        float stabilizedX = m_PrevStabilizedX + weight * (currRawX - m_PrevStabilizedX);
        float stabilizedY = m_PrevStabilizedY + weight * (currRawY - m_PrevStabilizedY);

        // Backup tiles covered by the stroke segment
        float minX = std::min(m_PrevStabilizedX, stabilizedX) - activeBrush.radius;
        float maxX = std::max(m_PrevStabilizedX, stabilizedX) + activeBrush.radius;
        float minY = std::min(m_PrevStabilizedY, stabilizedY) - activeBrush.radius;
        float maxY = std::max(m_PrevStabilizedY, stabilizedY) + activeBrush.radius;

        int minTileX = std::max(0, static_cast<int>(minX) / 256);
        int maxTileX = std::max(0, static_cast<int>(maxX) / 256);
        int minTileY = std::max(0, static_cast<int>(minY) / 256);
        int maxTileY = std::max(0, static_cast<int>(maxY) / 256);

        for (int ty = minTileY; ty <= std::min(maxTileY, numTilesY - 1); ++ty) {
            for (int tx = minTileX; tx <= std::min(maxTileX, numTilesX - 1); ++tx) {
                BackupTile(tx, ty);
            }
        }

        // Draw segment from previous stabilized position to current stabilized position
        PaintEngine::DrawStrokeSegment(m_Layers[m_ActiveLayerIdx].pixels, m_Width, m_Height,
                                       m_PrevStabilizedX, m_PrevStabilizedY,
                                       stabilizedX, stabilizedY,
                                       activeBrush, m_StrokeDistanceAccumulator,
                                       m_LastDabX, m_LastDabY);

        m_PrevStabilizedX = stabilizedX;
        m_PrevStabilizedY = stabilizedY;
        m_Layers[m_ActiveLayerIdx].needsUpload = true;
    }
    else if (phase == StrokePhase::End) {
        m_IsStrokeActive = false;
        if (!m_ActiveStrokeDeltas.empty()) {
            auto& layer = m_Layers[m_ActiveLayerIdx];
            std::vector<TileDelta> deltas;
            deltas.reserve(m_ActiveStrokeDeltas.size());

            for (auto& pair : m_ActiveStrokeDeltas) {
                auto& delta = pair.second;
                delta.newPixels.resize(256 * 256 * 4, 0.0f);

                int startX = delta.tileX * 256;
                int startY = delta.tileY * 256;
                for (int y = 0; y < 256; ++y) {
                    int canvasY = startY + y;
                    if (canvasY >= m_Height) break;
                    for (int x = 0; x < 256; ++x) {
                        int canvasX = startX + x;
                        if (canvasX >= m_Width) break;
                        
                        int tileOffset = (y * 256 + x) * 4;
                        int canvasOffset = (canvasY * m_Width + canvasX) * 4;
                        
                        delta.newPixels[tileOffset + 0] = layer.pixels[canvasOffset + 0];
                        delta.newPixels[tileOffset + 1] = layer.pixels[canvasOffset + 1];
                        delta.newPixels[tileOffset + 2] = layer.pixels[canvasOffset + 2];
                        delta.newPixels[tileOffset + 3] = layer.pixels[canvasOffset + 3];
                    }
                }
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

    // Recreate composition texture
    CreateCompositeResources(device);

    // Resize each layer's pixel buffers
    for (auto& layer : m_Layers) {
        std::vector<float> oldPixels = layer.pixels;
        layer.pixels.assign((size_t)m_Width * m_Height * 4, 0.0f);

        // Copy old contents top-left aligned
        int copyW = std::min(oldW, m_Width);
        int copyH = std::min(oldH, m_Height);

        for (int y = 0; y < copyH; ++y) {
            for (int x = 0; x < copyW; ++x) {
                size_t oldIdx = ((size_t)y * oldW + x) * 4;
                size_t newIdx = ((size_t)y * m_Width + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    layer.pixels[newIdx + c] = oldPixels[oldIdx + c];
                }
            }
        }

        RecreateLayerTexture(device, layer);
    }
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
    // 1. Upload layers with dirty CPU data
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (device) {
        for (auto& layer : m_Layers) {
            if (layer.needsUpload && layer.texture) {
                context->UpdateSubresource(layer.texture, 0, nullptr, layer.pixels.data(), m_Width * sizeof(float) * 4, 0);
                layer.needsUpload = false;
            }
        }
        device->Release();
    }

    if (!m_CompositeRTV) return;

    // 2. Clear composite target
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // Clear to transparent!
    context->ClearRenderTargetView(m_CompositeRTV, clearColor);

    // Save previous targets & viewport
    ID3D11RenderTargetView* prevRTV = nullptr;
    ID3D11DepthStencilView* prevDSV = nullptr;
    context->OMGetRenderTargets(1, &prevRTV, &prevDSV);

    UINT numViewports = 1;
    D3D11_VIEWPORT prevViewport = {};
    context->RSGetViewports(&numViewports, &prevViewport);

    // Set viewport to exact canvas size
    D3D11_VIEWPORT compViewport = {};
    compViewport.Width = static_cast<float>(m_Width);
    compViewport.Height = static_cast<float>(m_Height);
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
    for (const auto& layer : m_Layers) {
        if (layer.visible && layer.srv) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(m_LayerConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                LayerBuffer* lb = (LayerBuffer*)mapped.pData;
                lb->layerParams = DirectX::XMFLOAT4(layer.opacity, 0.0f, 0.0f, 0.0f);
                context->Unmap(m_LayerConstantBuffer, 0);
            }
            context->PSSetConstantBuffers(1, 1, &m_LayerConstantBuffer);
            context->PSSetShaderResources(0, 1, &layer.srv);
            context->DrawIndexed(6, 0, 0);
        }
    }

    // Restore previous target
    context->OMSetRenderTargets(1, &prevRTV, prevDSV);
    if (prevRTV) prevRTV->Release();
    if (prevDSV) prevDSV->Release();

    context->RSSetViewports(1, &prevViewport);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

void Canvas::Render(ID3D11DeviceContext* context, float viewportWidth, float viewportHeight) {
    // 1. Update constant buffer first so ComposeLayers has access to up-to-date visMode
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = context->Map(m_ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        CanvasBuffer* cb = (CanvasBuffer*)mappedResource.pData;
        cb->viewportSizeAndZoom = DirectX::XMFLOAT4(viewportWidth, viewportHeight, m_Zoom, 0.0f);
        cb->offsetAndCanvasSize = DirectX::XMFLOAT4(m_Pan.x, m_Pan.y, (float)m_Width, (float)m_Height);
        cb->visModeAndMaskColor = DirectX::XMFLOAT4((float)m_VisMode, m_AlphaMaskColor[0], m_AlphaMaskColor[1], m_AlphaMaskColor[2]);
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

    context->DrawIndexed(6, 0, 0);

    // Clean slot
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
}

bool Canvas::LoadImageToLayer(ID3D11Device* device, const std::string& filepath) {
    std::string ext = "";
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    int imgWidth = 0;
    int imgHeight = 0;
    std::vector<float> loadedPixels;

    if (ext == "dds") {
        DdsImage dds;
        if (!DdsHelper::LoadDDS(filepath, dds)) {
            return false;
        }
        imgWidth = dds.width;
        imgHeight = dds.height;
        loadedPixels = std::move(dds.pixels);
    } 
    else {
        if (!ImageManager::LoadImageFromFile(filepath, loadedPixels, imgWidth, imgHeight)) {
            return false;
        }
    }

    // Auto resize canvas size to match the imported image if it's the first layer
    // or if the dimensions differ significantly and user wants to scale
    if (m_Layers.empty() || (m_Layers.size() == 1 && m_Width == 1024 && m_Height == 1024 && m_Layers[0].name == "Background" && m_Layers[0].pixels == std::vector<float>((size_t)1024*1024*4, 0.0f))) {
        m_Width = imgWidth;
        m_Height = imgHeight;
        CreateCompositeResources(device);
        m_Layers.clear();
    }

    // Allocate layer
    Layer imported;
    imported.name = filepath.substr(filepath.find_last_of("\\/") + 1);
    imported.visible = true;
    imported.opacity = 1.0f;
    imported.pixels.assign((size_t)m_Width * m_Height * 4, 0.0f);

    // Copy loaded pixels centering them or fitting top-left
    int copyW = std::min(imgWidth, m_Width);
    int copyH = std::min(imgHeight, m_Height);
    for (int y = 0; y < copyH; ++y) {
        for (int x = 0; x < copyW; ++x) {
            size_t oldIdx = ((size_t)y * imgWidth + x) * 4;
            size_t newIdx = ((size_t)y * m_Width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                imported.pixels[newIdx + c] = loadedPixels[oldIdx + c];
            }
        }
    }

    RecreateLayerTexture(device, imported);
    m_Layers.push_back(imported);
    m_ActiveLayerIdx = static_cast<int>(m_Layers.size()) - 1;

    ResetView();
    Logger::Get().Info("Successfully imported layer from: " + filepath);
    return true;
}

bool Canvas::SaveCanvas(const std::string& filepath, DdsFormat ddsFormat) {
    if (m_Layers.empty()) {
        Logger::Get().Error("No layers to save.");
        return false;
    }

    // Composite all layers into a single CPU buffer for export
    std::vector<float> composite((size_t)m_Width * m_Height * 4, 0.0f);

    for (const auto& layer : m_Layers) {
        if (!layer.visible) continue;
        
        for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i) {
            size_t base = i * 4;
            float srcR = layer.pixels[base + 0];
            float srcG = layer.pixels[base + 1];
            float srcB = layer.pixels[base + 2];
            float srcA = layer.pixels[base + 3] * layer.opacity;

            if (srcA <= 0.0f) continue;

            float destR = composite[base + 0];
            float destG = composite[base + 1];
            float destB = composite[base + 2];
            float destA = composite[base + 3];

            float outA = srcA + destA * (1.0f - srcA);
            if (outA > 0.0f) {
                composite[base + 0] = (srcR * srcA + destR * destA * (1.0f - srcA)) / outA;
                composite[base + 1] = (srcG * srcA + destG * destA * (1.0f - srcA)) / outA;
                composite[base + 2] = (srcB * srcA + destB * destA * (1.0f - srcA)) / outA;
                composite[base + 3] = outA;
            }
        }
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

    // Composite layers CPU side
    std::vector<float> composite((size_t)m_Width * m_Height * 4, 0.0f);

    for (const auto& layer : m_Layers) {
        if (!layer.visible) continue;

        for (size_t i = 0; i < (size_t)m_Width * m_Height; ++i) {
            size_t base = i * 4;
            float srcR = layer.pixels[base + 0];
            float srcG = layer.pixels[base + 1];
            float srcB = layer.pixels[base + 2];
            float srcA = layer.pixels[base + 3] * layer.opacity;

            if (srcA <= 0.0f) continue;

            float destR = composite[base + 0];
            float destG = composite[base + 1];
            float destB = composite[base + 2];
            float destA = composite[base + 3];

            float outA = srcA + destA * (1.0f - srcA);
            if (outA > 0.0f) {
                composite[base + 0] = (srcR * srcA + destR * destA * (1.0f - srcA)) / outA;
                composite[base + 1] = (srcG * srcA + destG * destA * (1.0f - srcA)) / outA;
                composite[base + 2] = (srcB * srcA + destB * destA * (1.0f - srcA)) / outA;
                composite[base + 3] = outA;
            }
        }
    }

    return ImageManager::SaveImageToFile(filepath, composite, m_Width, m_Height, iccProfilePath);
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
        std::ofstream out(filepath, std::ios::binary);
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
        for (const auto& layer : m_Layers) {
            uint64_t uncompressedSize = layer.pixels.size() * sizeof(float);
            
            int compSize = 0;
            unsigned char* compData = stbi_zlib_compress(
                reinterpret_cast<unsigned char*>(const_cast<float*>(layer.pixels.data())),
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
        std::ifstream in(filepath, std::ios::binary);
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

            layer.pixels.resize(uncompressedSize / sizeof(float));
            std::memcpy(layer.pixels.data(), decompData, uncompressedSize);
            free(decompData);

            layer.needsUpload = true;
            RecreateLayerTexture(device, layer);

            m_Layers.push_back(std::move(layer));
        }

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
                                  const std::vector<std::vector<float>>& layerPixels) {
    try {
        json metadata;
        metadata["width"] = width;
        metadata["height"] = height;
        metadata["active_layer"] = activeLayerIdx;

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

        std::ofstream out(filepath, std::ios::binary);
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
    
    for (const auto& layer : m_Layers) {
        names.push_back(layer.name);
        visibles.push_back(layer.visible);
        opacities.push_back(layer.opacity);
        pixels.push_back(layer.pixels);
    }
    
    std::thread([=, pixels = std::move(pixels)]() {
        bool success = SaveCanvasRaypInternal(filepath, width, height, activeLayer, names, visibles, opacities, pixels);
        if (callback) {
            callback(success);
        }
    }).detach();
}
