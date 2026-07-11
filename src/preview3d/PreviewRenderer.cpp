#include "PreviewRenderer.h"
#include "../core/DdsHelper.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"
#include "../core/ConfigManager.h"

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

struct FrameCBData {
    XMFLOAT4X4 worldViewProj;
    XMFLOAT4X4 world;
    XMFLOAT4X4 worldInvTranspose;
    XMFLOAT4 lightDirIntensity;
    XMFLOAT4 ambientColor;
    XMFLOAT4 cameraPos;
    XMFLOAT4 frameDebug;
    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;
};

// Must match HLSL MaterialCB layout (float4 array)
struct MatCBData {
    XMFLOAT4 ch[10];
    XMFLOAT4 style0;
    XMFLOAT4 style1;
    XMFLOAT4 style2;
    XMFLOAT4 style3;
};

void GpuToMatCB(const GpuMaterialCB& g, MatCBData& m) {
    auto put = [](const GpuChannelPacked& p) {
        return XMFLOAT4(p.mapIdx, p.swizzle, p.scale, p.bias);
    };
    m.ch[0] = put(g.opacity);
    m.ch[1] = put(g.shadowMask);
    m.ch[2] = put(g.specular);
    m.ch[3] = put(g.metallic);
    m.ch[4] = put(g.roughness);
    m.ch[5] = put(g.ao);
    m.ch[6] = put(g.anisotropy);
    m.ch[7] = put(g.sssMask);
    m.ch[8] = put(g.glow);
    m.ch[9] = put(g.rimMask);
    m.style0 = XMFLOAT4(g.toonThreshold, g.toonSoftness, g.toonShadowTint, g.normalStrength);
    m.style1 = XMFLOAT4(g.rimStrength, g.rimPower, g.anisoStrength, g.anisoSharpness);
    m.style2 = XMFLOAT4(g.sssStrength, g.metalF0, g.glowStrength, g.alphaCutoff);
    m.style3 = XMFLOAT4(g.flags, g.debugMode, 0, 0);
}

std::string FindShaderPath() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();
    std::vector<std::filesystem::path> candidates = {
        exeDir / "shaders" / "Preview3D.hlsl",
        exeDir / "Preview3D.hlsl",
        std::filesystem::path("src/shaders/Preview3D.hlsl"),
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec))
            return PathUtil::WideToUtf8(c.wstring());
    }
    return "shaders/Preview3D.hlsl";
}

modio::MaterialSlot SlotForMapIndex(int i) {
    switch (i) {
    case 0: return modio::MaterialSlot::Diffuse;
    case 1: return modio::MaterialSlot::NormalMap;
    case 2: return modio::MaterialSlot::LightMap;
    case 3: return modio::MaterialSlot::MaterialMap;
    default: return modio::MaterialSlot::Unknown;
    }
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

void PreviewCamera::GetPosition(float out[3]) const {
    float cy = std::cos(yaw), sy = std::sin(yaw);
    float cp = std::cos(pitch), sp = std::sin(pitch);
    out[0] = target[0] + distance * cy * cp;
    out[1] = target[1] + distance * sp;
    out[2] = target[2] + distance * sy * cp;
}

void PreviewCamera::Reset(float dist) {
    yaw = 0.5f;
    pitch = 0.15f;
    distance = dist;
    target[0] = 0.f;
    target[1] = 0.9f;
    target[2] = 0.f;
}

void PreviewCamera::PanScreen(float dxPx, float dyPx, float viewportH) {
    if (viewportH < 1.f) viewportH = 1.f;
    // World units per pixel ~ based on distance / FOV
    float worldPerPx = (distance * 1.1f) / viewportH;
    float cy = std::cos(yaw), sy = std::sin(yaw);
    float cp = std::cos(pitch), sp = std::sin(pitch);
    // Camera forward (from target to eye is opposite of view dir in our orbit setup)
    // Right = cross(forward_from_eye, up) — eye looks toward target
    float fx = -cy * cp, fy = -sp, fz = -sy * cp; // from eye toward target
    // world up
    float ux = 0, uy = 1, uz = 0;
    // right = normalize(cross(forward, up))
    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rlen > 1e-6f) { rx /= rlen; ry /= rlen; rz /= rlen; }
    // cam up = cross(right, forward)
    float cux = ry * fz - rz * fy;
    float cuy = rz * fx - rx * fz;
    float cuz = rx * fy - ry * fx;
    target[0] += (-dxPx * worldPerPx) * rx + (dyPx * worldPerPx) * cux;
    target[1] += (-dxPx * worldPerPx) * ry + (dyPx * worldPerPx) * cuy;
    target[2] += (-dxPx * worldPerPx) * rz + (dyPx * worldPerPx) * cuz;
}

const char* ModelOrientation::UpAxisName(ModelUpAxis a) {
    switch (a) {
    case ModelUpAxis::PlusY:  return "+Y";
    case ModelUpAxis::MinusY: return "-Y";
    case ModelUpAxis::PlusZ:  return "+Z";
    case ModelUpAxis::MinusZ: return "-Z";
    case ModelUpAxis::PlusX:  return "+X";
    case ModelUpAxis::MinusX: return "-X";
    default: return "+Y";
    }
}

DirectX::XMMATRIX ModelOrientation::Matrix() const {
    const float kPi = 3.14159265f;
    // Map chosen model axis → world +Y (viewer up)
    XMMATRIX R = XMMatrixIdentity();
    switch (upAxis) {
    case ModelUpAxis::PlusY:
        R = XMMatrixIdentity();
        break;
    case ModelUpAxis::MinusY:
        R = XMMatrixRotationZ(kPi);
        break;
    case ModelUpAxis::PlusZ:
        // (x,y,z) → (x,z,-y) : old +Z becomes +Y
        R = XMMatrixRotationX(-kPi * 0.5f);
        break;
    case ModelUpAxis::MinusZ:
        R = XMMatrixRotationX(kPi * 0.5f);
        break;
    case ModelUpAxis::PlusX:
        // (x,y,z) → (-y,x,z) roughly: +X → +Y
        R = XMMatrixRotationZ(kPi * 0.5f);
        break;
    case ModelUpAxis::MinusX:
        R = XMMatrixRotationZ(-kPi * 0.5f);
        break;
    }
    XMMATRIX S = XMMatrixScaling(flipX ? -1.f : 1.f, flipY ? -1.f : 1.f, flipZ ? -1.f : 1.f);
    XMMATRIX Yaw = XMMatrixRotationY(yawOffsetDeg * (kPi / 180.f));
    // Scale in model space, then basis fix, then yaw around world Y
    return S * R * Yaw;
}

PreviewRenderer::~PreviewRenderer() { Shutdown(); }

void PreviewRenderer::ReleaseItems() {
    for (auto& it : m_Items) {
        it.mesh.Release();
        // SRVs owned by cache
        for (int i = 0; i < 4; ++i) it.srvs[i] = nullptr;
    }
    m_Items.clear();
    for (auto& kv : m_TexCache) {
        if (kv.second) kv.second->Release();
    }
    m_TexCache.clear();
}

void PreviewRenderer::Shutdown() {
    ReleaseItems();
    if (m_VS) { m_VS->Release(); m_VS = nullptr; }
    if (m_PS) { m_PS->Release(); m_PS = nullptr; }
    if (m_Layout) { m_Layout->Release(); m_Layout = nullptr; }
    if (m_OutlineVS) { m_OutlineVS->Release(); m_OutlineVS = nullptr; }
    if (m_OutlinePS) { m_OutlinePS->Release(); m_OutlinePS = nullptr; }
    if (m_OutlineLayout) { m_OutlineLayout->Release(); m_OutlineLayout = nullptr; }
    if (m_FrameCB) { m_FrameCB->Release(); m_FrameCB = nullptr; }
    if (m_MatCB) { m_MatCB->Release(); m_MatCB = nullptr; }
    if (m_OutlineCB) { m_OutlineCB->Release(); m_OutlineCB = nullptr; }
    if (m_Sampler) { m_Sampler->Release(); m_Sampler = nullptr; }
    if (m_RSCullNone) { m_RSCullNone->Release(); m_RSCullNone = nullptr; }
    if (m_RSCullFront) { m_RSCullFront->Release(); m_RSCullFront = nullptr; }
    if (m_DSS) { m_DSS->Release(); m_DSS = nullptr; }
    if (m_DSSOutline) { m_DSSOutline->Release(); m_DSSOutline = nullptr; }
    if (m_FallbackSRV) { m_FallbackSRV->Release(); m_FallbackSRV = nullptr; }
    if (m_FallbackTex) { m_FallbackTex->Release(); m_FallbackTex = nullptr; }
    m_Ready = false;
    m_Device = nullptr;
}

static std::string FindOutlineShaderPath() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();
    std::vector<std::filesystem::path> candidates = {
        exeDir / "shaders" / "Preview3D_Outline.hlsl",
        exeDir / "Preview3D_Outline.hlsl",
        std::filesystem::path("src/shaders/Preview3D_Outline.hlsl"),
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec))
            return PathUtil::WideToUtf8(c.wstring());
    }
    return "shaders/Preview3D_Outline.hlsl";
}

bool PreviewRenderer::CompileShaders(ID3D11Device* device) {
    std::string path = FindShaderPath();
    std::wstring wpath = PathUtil::Utf8ToWide(path);
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;

    HRESULT hr = D3DCompileFromFile(wpath.c_str(), nullptr, nullptr, "VSMain", "vs_5_0",
                                    flags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        m_LastError = errBlob ? (const char*)errBlob->GetBufferPointer()
                              : ("VS compile failed: " + path);
        if (errBlob) errBlob->Release();
        return false;
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }

    hr = D3DCompileFromFile(wpath.c_str(), nullptr, nullptr, "PSMain", "ps_5_0",
                            flags, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        m_LastError = errBlob ? (const char*)errBlob->GetBufferPointer() : "PS compile failed";
        if (errBlob) errBlob->Release();
        vsBlob->Release();
        return false;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_VS);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_PS);
    if (!CreatePreviewInputLayout(device, vsBlob, &m_Layout)) {
        m_LastError = "Input layout failed";
        vsBlob->Release(); psBlob->Release();
        return false;
    }
    vsBlob->Release();
    psBlob->Release();
    Logger::Get().Info("Preview3D main uber compiled: " + path);

    // --- ZZZ Outline pass ---
    std::string opath = FindOutlineShaderPath();
    std::wstring wopath = PathUtil::Utf8ToWide(opath);
    ID3DBlob* ovs = nullptr;
    ID3DBlob* ops = nullptr;
    hr = D3DCompileFromFile(wopath.c_str(), nullptr, nullptr, "VSOutlineZZZ", "vs_5_0",
                            flags, 0, &ovs, &errBlob);
    if (FAILED(hr)) {
        m_LastError = errBlob ? (const char*)errBlob->GetBufferPointer()
                              : ("Outline VS failed: " + opath);
        if (errBlob) errBlob->Release();
        return false;
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }
    hr = D3DCompileFromFile(wopath.c_str(), nullptr, nullptr, "PSOutlineZZZ", "ps_5_0",
                            flags, 0, &ops, &errBlob);
    if (FAILED(hr)) {
        m_LastError = errBlob ? (const char*)errBlob->GetBufferPointer() : "Outline PS failed";
        if (errBlob) errBlob->Release();
        ovs->Release();
        return false;
    }
    device->CreateVertexShader(ovs->GetBufferPointer(), ovs->GetBufferSize(), nullptr, &m_OutlineVS);
    device->CreatePixelShader(ops->GetBufferPointer(), ops->GetBufferSize(), nullptr, &m_OutlinePS);
    if (!CreatePreviewInputLayout(device, ovs, &m_OutlineLayout)) {
        m_LastError = "Outline input layout failed";
        ovs->Release(); ops->Release();
        return false;
    }
    ovs->Release();
    ops->Release();
    Logger::Get().Info("Preview3D ZZZ outline compiled: " + opath);
    return true;
}

bool PreviewRenderer::Initialize(ID3D11Device* device) {
    Shutdown();
    m_Device = device;
    if (!device) return false;

    ShaderPresetLibrary::Get().LoadBuiltins();
    // User + install preset dirs
    std::string userPresets = ConfigManager::GetUserSubdirectory("presets/shaders");
    ShaderPresetLibrary::Get().LoadDirectory(userPresets);

    if (!CompileShaders(device)) {
        Logger::Get().Error("PreviewRenderer: " + m_LastError);
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = (sizeof(FrameCBData) + 15) & ~15;
    device->CreateBuffer(&cbd, nullptr, &m_FrameCB);
    cbd.ByteWidth = (sizeof(MatCBData) + 15) & ~15;
    device->CreateBuffer(&cbd, nullptr, &m_MatCB);
    struct OutlineCBData { XMFLOAT4 params; XMFLOAT4 tint; };
    cbd.ByteWidth = (sizeof(OutlineCBData) + 15) & ~15;
    device->CreateBuffer(&cbd, nullptr, &m_OutlineCB);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &m_Sampler);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &m_RSCullNone);
    rd.CullMode = D3D11_CULL_FRONT; // inverted hull outline
    device->CreateRasterizerState(&rd, &m_RSCullFront);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dd, &m_DSS);
    // Outline: write depth so main mesh can occlude interior of hull correctly
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device->CreateDepthStencilState(&dd, &m_DSSOutline);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint8_t px[4] = { 180, 180, 185, 255 };
    D3D11_SUBRESOURCE_DATA init{ px, 4, 0 };
    if (SUCCEEDED(device->CreateTexture2D(&td, &init, &m_FallbackTex)))
        device->CreateShaderResourceView(m_FallbackTex, nullptr, &m_FallbackSRV);

    m_Ready = true;
    m_Status = "Preview ready (no scene)";
    return true;
}

ID3D11ShaderResourceView* PreviewRenderer::GetOrLoadSRV(ID3D11Device* device, const std::string& path) {
    if (path.empty()) return m_FallbackSRV;
    // Normalize cache key so the same file never gets two entries / wrong hit
    const std::string key = PathUtil::NormalizeToUtf8Path(path);
    auto it = m_TexCache.find(key);
    if (it != m_TexCache.end()) return it->second;

    DdsImage img;
    if (!DdsHelper::LoadDDS(key, img) || img.width <= 0 || img.pixels.empty()) {
        Logger::Get().Warn("Preview DDS load failed: " + key);
        m_TexCache[key] = m_FallbackSRV;
        if (m_FallbackSRV) m_FallbackSRV->AddRef();
        return m_FallbackSRV;
    }

    // Cap preview upload for speed (max 2048)
    int w = img.width, h = img.height;
    int step = 1;
    while (w / step > 2048 || h / step > 2048) step *= 2;
    int ow = w / step, oh = h / step;
    std::vector<uint8_t> rgba((size_t)ow * oh * 4);
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            int sx = x * step, sy = y * step;
            int si = (sy * w + sx) * 4;
            int di = (y * ow + x) * 4;
            rgba[di + 0] = (uint8_t)std::clamp(img.pixels[si + 0] * 255.f, 0.f, 255.f);
            rgba[di + 1] = (uint8_t)std::clamp(img.pixels[si + 1] * 255.f, 0.f, 255.f);
            rgba[di + 2] = (uint8_t)std::clamp(img.pixels[si + 2] * 255.f, 0.f, 255.f);
            rgba[di + 3] = (uint8_t)std::clamp(img.pixels[si + 3] * 255.f, 0.f, 255.f);
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = ow; td.Height = oh;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sinit{};
    sinit.pSysMem = rgba.data();
    sinit.SysMemPitch = (UINT)ow * 4;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device->CreateTexture2D(&td, &sinit, &tex))) {
        m_TexCache[key] = m_FallbackSRV;
        if (m_FallbackSRV) m_FallbackSRV->AddRef();
        return m_FallbackSRV;
    }
    device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    m_TexCache[key] = srv;
    Logger::Get().Info("Preview DDS loaded " + key + " "
        + std::to_string(ow) + "x" + std::to_string(oh)
        + " avgRGB=" + std::to_string((rgba[0] + rgba[1] + rgba[2]) / 3));
    return srv;
}

MaterialConfig PreviewRenderer::GuessPresetForPart(const std::string& component, const std::string& part) {
    auto& lib = ShaderPresetLibrary::Get();
    std::string c = component, p = part;
    std::transform(c.begin(), c.end(), c.begin(), ::tolower);
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
    if (c.find("hair") != std::string::npos || p.find("hair") != std::string::npos) {
        if (const auto* m = lib.Find("ZZZ-Hair")) return *m;
    }
    if (c.find("face") != std::string::npos || p.find("face") != std::string::npos ||
        c.find("head") != std::string::npos) {
        if (const auto* m = lib.Find("ZZZ-Face")) return *m;
    }
    if (c.find("body") != std::string::npos || c.find("skin") != std::string::npos ||
        c.find("leg") != std::string::npos) {
        if (const auto* m = lib.Find("ZZZ-Skin")) return *m;
    }
    if (const auto* m = lib.Find("ZZZ-Cloth")) return *m;
    return MaterialConfig::MakeDefaultZZZ_Skin();
}

bool PreviewRenderer::LoadScene(ID3D11Device* device, const modio::ModScene& scene) {
    if (!m_Ready && !Initialize(device))
        return false;
    // Refresh builtin channel maps (ZZZ LightMap/Material/Normal community layout)
    ShaderPresetLibrary::Get().LoadBuiltins();
    ShaderPresetLibrary::Get().LoadDirectory(
        ConfigManager::GetUserSubdirectory("presets/shaders"));
    ReleaseItems();
    m_LastError.clear();

    int okCount = 0, failCount = 0;
    for (const auto& c : scene.components) {
        if (!c.visible) continue;
        for (const auto& p : c.parts) {
            if (!p.visible || !p.hasGeometry) continue;
            // One GPU draw item per texture batch (multi-texture within same IB section)
            for (const auto& bat : p.batches) {
                if (!bat.visible) continue;
                PartDrawItem item;
                item.componentName = c.name;
                item.partName = p.name + "/" + bat.name;
                item.visible = true;
                item.material = GuessPresetForPart(c.name, p.name + " " + bat.name);
                item.presetId = item.material.id;

                // Resolve binds: last matching slot wins; do not require exists flag
                // (Exists can fail on path quirks while file is still loadable.)
                for (int mi = 0; mi < 4; ++mi) {
                    item.paths[mi].clear();
                    item.resNames[mi].clear();
                    item.srvs[mi] = nullptr;
                    modio::MaterialSlot slot = SlotForMapIndex(mi);
                    for (const auto& t : bat.textures) {
                        if (t.slot != slot) continue;
                        if (t.absolutePath.empty() && t.resourceName.empty()) continue;
                        item.paths[mi] = t.absolutePath;
                        item.resNames[mi] = t.resourceName;
                    }
                }

                std::string err;
                if (!BuildBatchMesh(device, c, p, bat, item.mesh, err)) {
                    Logger::Get().Warn("Preview mesh skip " + item.partName + ": " + err);
                    ++failCount;
                    continue;
                }
                for (int mi = 0; mi < 4; ++mi) {
                    item.srvs[mi] = GetOrLoadSRV(device, item.paths[mi]);
                    // Log bind table so we can catch path/name mismatches
                    Logger::Get().Info("  bind " + item.partName + " [" + std::to_string(mi) + "] "
                        + item.resNames[mi] + " => " +
                        (item.paths[mi].empty() ? std::string("(empty)") : item.paths[mi]));
                }

                m_Items.push_back(std::move(item));
                ++okCount;
            }
        }
    }

    if (m_Items.empty()) {
        m_Status = "No geometry parts loaded";
        m_LastError = failCount ? "Mesh builds failed" : "No drawable parts";
        return false;
    }

    FitCameraToMeshes();
    m_Status = "Loaded " + std::to_string(okCount) + " part(s) · multipass Main+OutlineZZZ";
    Logger::Get().Info("PreviewRenderer: " + m_Status);
    return true;
}

void PreviewRenderer::ApplyPresetToPart(int partIndex, const MaterialConfig& cfg) {
    if (partIndex < 0 || partIndex >= (int)m_Items.size()) return;
    m_Items[partIndex].material = cfg;
    m_Items[partIndex].presetId = cfg.id;
}

void PreviewRenderer::ApplyPresetToAll(const MaterialConfig& cfg) {
    for (auto& it : m_Items) {
        it.material = cfg;
        it.presetId = cfg.id;
    }
}

void PreviewRenderer::FitCameraToMeshes() {
    m_Camera.target[0] = 0.f;
    m_Camera.target[1] = 0.9f;
    m_Camera.target[2] = 0.f;
    m_Camera.distance = 2.8f;
    m_Camera.yaw = 0.5f;
    m_Camera.pitch = 0.15f;
}

void PreviewRenderer::DrawMainPass(ID3D11DeviceContext* ctx) {
    ctx->IASetInputLayout(m_Layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_VS, nullptr, 0);
    ctx->PSSetShader(m_PS, nullptr, 0);
    ctx->RSSetState(m_RSCullNone);
    ctx->OMSetDepthStencilState(m_DSS, 0);
    ctx->PSSetSamplers(0, 1, &m_Sampler);
    ctx->VSSetConstantBuffers(0, 1, &m_FrameCB);
    ctx->PSSetConstantBuffers(0, 1, &m_FrameCB);

    for (auto& item : m_Items) {
        if (!item.visible || !item.mesh.valid) continue;
        if (!item.material.outlineEnable && false) { /* keep material flags free */ }

        GpuMaterialCB gmat{};
        MaterialConfigToGpu(item.material, m_DebugMode, gmat);
        MatCBData mcb{};
        GpuToMatCB(gmat, mcb);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_MatCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &mcb, sizeof(mcb));
            ctx->Unmap(m_MatCB, 0);
        }
        ctx->PSSetConstantBuffers(1, 1, &m_MatCB);

        ID3D11ShaderResourceView* srvs[4];
        for (int i = 0; i < 4; ++i)
            srvs[i] = item.srvs[i] ? item.srvs[i] : m_FallbackSRV;
        ctx->PSSetShaderResources(0, 4, srvs);

        UINT stride = sizeof(PreviewVertex);
        UINT offset = 0;
        ctx->IASetVertexBuffers(0, 1, &item.mesh.vb, &stride, &offset);
        ctx->IASetIndexBuffer(item.mesh.ib, item.mesh.indexFormat, 0);
        ctx->DrawIndexed(item.mesh.indexCount, 0, 0);
    }
}

void PreviewRenderer::DrawOutlineZZZPass(ID3D11DeviceContext* ctx) {
    if (!m_OutlineVS || !m_OutlinePS) return;

    // Per-part thickness can use material.outlineThickness as scale on global
    struct OutlineCBData { XMFLOAT4 params; XMFLOAT4 tint; };

    ctx->IASetInputLayout(m_OutlineLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_OutlineVS, nullptr, 0);
    ctx->PSSetShader(m_OutlinePS, nullptr, 0);
    ctx->RSSetState(m_RSCullFront); // inverted hull
    ctx->OMSetDepthStencilState(m_DSSOutline, 0);
    ctx->PSSetSamplers(0, 1, &m_Sampler);
    ctx->VSSetConstantBuffers(0, 1, &m_FrameCB);
    ctx->PSSetConstantBuffers(0, 1, &m_FrameCB);

    for (auto& item : m_Items) {
        if (!item.visible || !item.mesh.valid) continue;
        if (!item.material.outlineEnable) continue;

        // Global pass scale; material can multiply if it set a non-default thickness
        float thick = m_Passes.outlineThickness;
        if (item.material.outlineThickness > 0.f &&
            std::abs(item.material.outlineThickness - 1.0f) > 0.01f)
            thick *= item.material.outlineThickness;
        float mul = m_Passes.outlineAlbedoMul;
        if (item.material.outlineColorMul > 0.f &&
            std::abs(item.material.outlineColorMul - 0.42f) > 0.01f)
            mul = item.material.outlineColorMul;

        OutlineCBData ocb{};
        // x = view-space thickness scale (UI ~0.5–3), y = albedo darken (~0.4 game-like)
        ocb.params = XMFLOAT4(
            thick,
            mul,
            m_Passes.outlineUseVertexColor ? 1.f : 0.f,
            0.35f); // min COLOR.r scale
        ocb.tint = XMFLOAT4(
            m_Passes.outlineTint[0], m_Passes.outlineTint[1], m_Passes.outlineTint[2],
            m_Passes.outlineUseFixedTint ? 1.f : 0.f);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_OutlineCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &ocb, sizeof(ocb));
            ctx->Unmap(m_OutlineCB, 0);
        }
        ctx->VSSetConstantBuffers(1, 1, &m_OutlineCB);
        ctx->PSSetConstantBuffers(1, 1, &m_OutlineCB);

        // t0 Diffuse + t2 LightMap (outline colour / ramp)
        ID3D11ShaderResourceView* srvs[3] = {
            item.srvs[0] ? item.srvs[0] : m_FallbackSRV,
            nullptr,
            item.srvs[2] ? item.srvs[2] : m_FallbackSRV,
        };
        ctx->PSSetShaderResources(0, 3, srvs);

        UINT stride = sizeof(PreviewVertex);
        UINT offset = 0;
        ctx->IASetVertexBuffers(0, 1, &item.mesh.vb, &stride, &offset);
        ctx->IASetIndexBuffer(item.mesh.ib, item.mesh.indexFormat, 0);
        ctx->DrawIndexed(item.mesh.indexCount, 0, 0);
    }
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

    XMMATRIX world = m_Orientation.Matrix();
    XMMATRIX view = m_Camera.View();
    XMMATRIX proj = m_Camera.Proj(aspect);
    XMMATRIX wvp = world * view * proj;
    XMMATRIX wit = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

    float ldir[3];
    if (m_Lighting.followCamera) {
        float cam[3];
        m_Camera.GetPosition(cam);
        ldir[0] = cam[0] - m_Camera.target[0];
        ldir[1] = cam[1] - m_Camera.target[1];
        ldir[2] = cam[2] - m_Camera.target[2];
        float len = std::sqrt(ldir[0]*ldir[0]+ldir[1]*ldir[1]+ldir[2]*ldir[2]);
        if (len > 1e-6f) { ldir[0]/=len; ldir[1]/=len; ldir[2]/=len; }
    } else {
        m_Lighting.GetDirection(ldir);
    }

    float camPos[3];
    m_Camera.GetPosition(camPos);

    FrameCBData fcb{};
    XMStoreFloat4x4(&fcb.worldViewProj, XMMatrixTranspose(wvp));
    XMStoreFloat4x4(&fcb.world, XMMatrixTranspose(world));
    XMStoreFloat4x4(&fcb.worldInvTranspose, XMMatrixTranspose(wit));
    XMStoreFloat4x4(&fcb.view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&fcb.proj, XMMatrixTranspose(proj));
    fcb.lightDirIntensity = XMFLOAT4(ldir[0], ldir[1], ldir[2], m_Lighting.intensity);
    fcb.ambientColor = XMFLOAT4(
        m_Lighting.ambientTint[0] * m_Lighting.ambient,
        m_Lighting.ambientTint[1] * m_Lighting.ambient,
        m_Lighting.ambientTint[2] * m_Lighting.ambient, 1.f);
    fcb.cameraPos = XMFLOAT4(camPos[0], camPos[1], camPos[2], 1.f);
    fcb.frameDebug = XMFLOAT4((float)m_DebugMode, 0, 0, 0);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(m_FrameCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &fcb, sizeof(fcb));
        ctx->Unmap(m_FrameCB, 0);
    }

    // ---- Pass graph (ZZZ): Outline first (backfaces) → Main → Glow(later) ----
    // Drawing outline before main is classic inverted-hull: silhouette stays behind body.
    if (m_Passes.enableOutline &&
        m_Passes.outlineMode == PassConfig::OutlineMode::ZZZ &&
        m_DebugMode == 0) {
        DrawOutlineZZZPass(ctx);
    }

    if (m_Passes.enableMain)
        DrawMainPass(ctx);

    // Glow / Bloom reserved — enableGlow / enableBloom

    ID3D11ShaderResourceView* nulls[4] = {};
    ctx->PSSetShaderResources(0, 4, nulls);
}

} // namespace preview3d

