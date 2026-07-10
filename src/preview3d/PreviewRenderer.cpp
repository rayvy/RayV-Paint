#include "PreviewRenderer.h"
#include "../core/DdsHelper.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace DirectX;

namespace preview3d {
namespace {

struct CBData {
    XMFLOAT4X4 worldViewProj;
    XMFLOAT4X4 world;
    XMFLOAT4 lightDirIntensity;
    XMFLOAT4 ambientColor;
    XMFLOAT4 debugMode;
};

std::string FindShaderPath() {
    // Prefer next to exe
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();
    std::vector<std::filesystem::path> candidates = {
        exeDir / "Preview3D.hlsl",
        exeDir / "shaders" / "Preview3D.hlsl",
        exeDir.parent_path() / "shaders" / "Preview3D.hlsl",
        std::filesystem::path("src/shaders/Preview3D.hlsl"),
        std::filesystem::path("shaders/Preview3D.hlsl"),
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec))
            return PathUtil::WideToUtf8(c.wstring());
    }
    return "Preview3D.hlsl";
}

} // namespace

DirectX::XMMATRIX PreviewCamera::View() const {
    float cy = std::cos(yaw), sy = std::sin(yaw);
    float cp = std::cos(pitch), sp = std::sin(pitch);
    XMVECTOR eye = XMVectorSet(
        target[0] + distance * cy * cp,
        target[1] + distance * sp,
        target[2] + distance * sy * cp,
        1.f);
    XMVECTOR at = XMVectorSet(target[0], target[1], target[2], 1.f);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    return XMMatrixLookAtLH(eye, at, up);
}

DirectX::XMMATRIX PreviewCamera::Proj(float aspect) const {
    return XMMatrixPerspectiveFovLH(fovY, aspect > 0.01f ? aspect : 1.f, 0.05f, 500.f);
}

PreviewRenderer::~PreviewRenderer() { Shutdown(); }

void PreviewRenderer::ReleaseItems() {
    for (auto& it : m_Items) {
        it.mesh.Release();
        if (it.diffuseSRV) { it.diffuseSRV->Release(); it.diffuseSRV = nullptr; }
    }
    m_Items.clear();
}

void PreviewRenderer::Shutdown() {
    ReleaseItems();
    if (m_VS) { m_VS->Release(); m_VS = nullptr; }
    if (m_PS) { m_PS->Release(); m_PS = nullptr; }
    if (m_Layout) { m_Layout->Release(); m_Layout = nullptr; }
    if (m_CB) { m_CB->Release(); m_CB = nullptr; }
    if (m_Sampler) { m_Sampler->Release(); m_Sampler = nullptr; }
    if (m_RS) { m_RS->Release(); m_RS = nullptr; }
    if (m_DSS) { m_DSS->Release(); m_DSS = nullptr; }
    if (m_FallbackSRV) { m_FallbackSRV->Release(); m_FallbackSRV = nullptr; }
    if (m_FallbackTex) { m_FallbackTex->Release(); m_FallbackTex = nullptr; }
    m_Ready = false;
    m_Device = nullptr;
}

bool PreviewRenderer::CompileShaders(ID3D11Device* device) {
    std::string path = FindShaderPath();
    std::wstring wpath = PathUtil::Utf8ToWide(path);
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompileFromFile(wpath.c_str(), nullptr, nullptr, "VSMain", "vs_5_0",
                                    flags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            m_LastError = (const char*)errBlob->GetBufferPointer();
            errBlob->Release();
        } else {
            m_LastError = "VS compile failed: " + path;
        }
        return false;
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }

    hr = D3DCompileFromFile(wpath.c_str(), nullptr, nullptr, "PSMain", "ps_5_0",
                            flags, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            m_LastError = (const char*)errBlob->GetBufferPointer();
            errBlob->Release();
        } else m_LastError = "PS compile failed";
        vsBlob->Release();
        return false;
    }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_VS);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); m_LastError = "Create VS failed"; return false; }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_PS);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); m_LastError = "Create PS failed"; return false; }

    if (!CreatePreviewInputLayout(device, vsBlob, &m_Layout)) {
        vsBlob->Release(); psBlob->Release();
        m_LastError = "Create input layout failed";
        return false;
    }
    vsBlob->Release();
    psBlob->Release();
    return true;
}

bool PreviewRenderer::Initialize(ID3D11Device* device) {
    Shutdown();
    m_Device = device;
    if (!device) return false;

    if (!CompileShaders(device)) {
        Logger::Get().Error("PreviewRenderer: " + m_LastError);
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.ByteWidth = sizeof(CBData);
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &m_CB))) {
        m_LastError = "CB create failed";
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &m_Sampler);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &m_RS);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dd, &m_DSS);

    // 1x1 grey fallback texture
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint8_t px[4] = { 170, 170, 175, 255 };
    D3D11_SUBRESOURCE_DATA init{ px, 4, 0 };
    if (SUCCEEDED(device->CreateTexture2D(&td, &init, &m_FallbackTex)))
        device->CreateShaderResourceView(m_FallbackTex, nullptr, &m_FallbackSRV);

    m_Ready = true;
    m_Status = "Preview ready (no scene)";
    return true;
}

bool PreviewRenderer::LoadDiffuseSRV(ID3D11Device* device, const std::string& path,
                                     ID3D11ShaderResourceView** outSRV) {
    *outSRV = nullptr;
    if (path.empty() || !device) return false;
    DdsImage img;
    if (!DdsHelper::LoadDDS(path, img) || img.width <= 0 || img.height <= 0 || img.pixels.empty())
        return false;

    // Upload as RGBA8 (preview only)
    std::vector<uint8_t> rgba((size_t)img.width * img.height * 4);
    for (int i = 0; i < img.width * img.height; ++i) {
        rgba[i * 4 + 0] = (uint8_t)std::clamp(img.pixels[i * 4 + 0] * 255.f, 0.f, 255.f);
        rgba[i * 4 + 1] = (uint8_t)std::clamp(img.pixels[i * 4 + 1] * 255.f, 0.f, 255.f);
        rgba[i * 4 + 2] = (uint8_t)std::clamp(img.pixels[i * 4 + 2] * 255.f, 0.f, 255.f);
        rgba[i * 4 + 3] = (uint8_t)std::clamp(img.pixels[i * 4 + 3] * 255.f, 0.f, 255.f);
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = img.width;
    td.Height = img.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba.data();
    init.SysMemPitch = (UINT)img.width * 4;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&td, &init, &tex)))
        return false;
    HRESULT hr = device->CreateShaderResourceView(tex, nullptr, outSRV);
    tex->Release();
    return SUCCEEDED(hr);
}

bool PreviewRenderer::LoadScene(ID3D11Device* device, const modio::ModScene& scene) {
    if (!m_Ready && !Initialize(device))
        return false;
    ReleaseItems();
    m_LastError.clear();

    int okCount = 0, failCount = 0;
    for (const auto& c : scene.components) {
        if (!c.visible) continue;
        for (const auto& p : c.parts) {
            if (!p.visible || !p.hasGeometry) continue;
            PartDrawItem item;
            item.componentName = c.name;
            item.partName = p.name;
            item.visible = true;
            for (const auto& t : p.textures) {
                if (t.slot == modio::MaterialSlot::Diffuse && t.exists) {
                    item.diffusePath = t.absolutePath;
                    break;
                }
            }
            std::string err;
            if (!BuildPartMesh(device, c, p, item.mesh, err)) {
                Logger::Get().Warn("Preview mesh skip " + c.name + "/" + p.name + ": " + err);
                ++failCount;
                continue;
            }
            if (!item.diffusePath.empty())
                LoadDiffuseSRV(device, item.diffusePath, &item.diffuseSRV);
            m_Items.push_back(std::move(item));
            ++okCount;
        }
    }

    if (m_Items.empty()) {
        m_Status = "No geometry parts loaded";
        if (failCount > 0)
            m_LastError = "All mesh builds failed (" + std::to_string(failCount) + ")";
        else
            m_LastError = "Scene has no drawable parts (texture-only mod?)";
        return false;
    }

    FitCameraToMeshes();
    m_Status = "Loaded " + std::to_string(okCount) + " part(s)"
        + (failCount ? (", " + std::to_string(failCount) + " failed") : "");
    Logger::Get().Info("PreviewRenderer: " + m_Status);
    return true;
}

void PreviewRenderer::FitCameraToMeshes() {
    float minB[3] = { 1e9f, 1e9f, 1e9f };
    float maxB[3] = { -1e9f, -1e9f, -1e9f };
    bool any = false;
    // Approximate from first mesh — full fit would need CPU verts retained
    // Use default framing for character
    m_Camera.target[0] = 0.f;
    m_Camera.target[1] = 0.9f;
    m_Camera.target[2] = 0.f;
    m_Camera.distance = 2.8f;
    m_Camera.yaw = 0.5f;
    m_Camera.pitch = 0.15f;
    (void)minB; (void)maxB; (void)any;
}

void PreviewRenderer::Render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                             ID3D11DepthStencilView* dsv, float aspect,
                             float clearR, float clearG, float clearB) {
    if (!ctx || !rtv || !m_Ready) return;

    float clear[4] = { clearR, clearG, clearB, 1.f };
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    ctx->ClearRenderTargetView(rtv, clear);
    if (dsv) ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.f, 0);

    if (m_Items.empty()) return;

    ctx->IASetInputLayout(m_Layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_VS, nullptr, 0);
    ctx->PSSetShader(m_PS, nullptr, 0);
    ctx->RSSetState(m_RS);
    ctx->OMSetDepthStencilState(m_DSS, 0);
    ctx->PSSetSamplers(0, 1, &m_Sampler);

    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX view = m_Camera.View();
    XMMATRIX proj = m_Camera.Proj(aspect);
    XMMATRIX wvp = world * view * proj;

    CBData cb{};
    XMStoreFloat4x4(&cb.worldViewProj, XMMatrixTranspose(wvp));
    XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
    cb.lightDirIntensity = XMFLOAT4(0.35f, -0.85f, 0.35f, 0.85f);
    cb.ambientColor = XMFLOAT4(0.25f, 0.25f, 0.28f, 1.f);
    cb.debugMode = XMFLOAT4((float)m_DebugMode, 0, 0, 0);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(m_CB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(m_CB, 0);
    }
    ctx->VSSetConstantBuffers(0, 1, &m_CB);
    ctx->PSSetConstantBuffers(0, 1, &m_CB);

    for (auto& item : m_Items) {
        if (!item.visible || !item.mesh.valid) continue;
        UINT stride = sizeof(PreviewVertex);
        UINT offset = 0;
        ctx->IASetVertexBuffers(0, 1, &item.mesh.vb, &stride, &offset);
        ctx->IASetIndexBuffer(item.mesh.ib, item.mesh.indexFormat, 0);
        ID3D11ShaderResourceView* srv = item.diffuseSRV ? item.diffuseSRV : m_FallbackSRV;
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->DrawIndexed(item.mesh.indexCount, 0, 0);
    }

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
}

} // namespace preview3d
