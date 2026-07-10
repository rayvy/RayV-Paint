#include "MeshGpu.h"
#include "../core/PathUtil.h"
#include "../core/Logger.h"
#include "../modio/VertexLayout.h"

#include <d3dcompiler.h>
#include <fstream>
#include <cstring>
#include <algorithm>
#ifndef _countof
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
#endif

namespace preview3d {
namespace {

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    const std::string p = PathUtil::NormalizeToUtf8Path(path);
#ifdef _WIN32
    std::ifstream in(PathUtil::FromUtf8(p), std::ios::binary);
#else
    std::ifstream in(p, std::ios::binary);
#endif
    if (!in) return false;
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz <= 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    in.read(reinterpret_cast<char*>(out.data()), sz);
    return (bool)in || in.eof();
}

void ApplyRole(PreviewVertex& v, modio::AttrRole role, const float f[4]) {
    using modio::AttrRole;
    switch (role) {
    case AttrRole::Position:
        v.position[0] = f[0]; v.position[1] = f[1]; v.position[2] = f[2];
        break;
    case AttrRole::Normal:
        v.normal[0] = f[0]; v.normal[1] = f[1]; v.normal[2] = f[2];
        break;
    case AttrRole::Tangent:
        v.tangent[0] = f[0]; v.tangent[1] = f[1]; v.tangent[2] = f[2]; v.tangent[3] = f[3];
        break;
    case AttrRole::VertexColor:
        v.color[0] = f[0]; v.color[1] = f[1]; v.color[2] = f[2]; v.color[3] = f[3];
        break;
    case AttrRole::UV0:
        v.uv0[0] = f[0]; v.uv0[1] = f[1];
        break;
    case AttrRole::UV_LightMap:
        v.uvLight[0] = f[0]; v.uvLight[1] = f[1];
        break;
    case AttrRole::UV_Outline:
        v.uvOutline[0] = f[0]; v.uvOutline[1] = f[1];
        break;
    case AttrRole::UV_Backface:
        v.uvBackface[0] = f[0]; v.uvBackface[1] = f[1];
        break;
    case AttrRole::UV_Extra0:
        // stash in backface if empty — or leave for later
        if (v.uvBackface[0] == 0.f && v.uvBackface[1] == 0.f) {
            v.uvBackface[0] = f[0]; v.uvBackface[1] = f[1];
        }
        break;
    case AttrRole::None:
    case AttrRole::BlendWeight:
    case AttrRole::BlendIndex:
    case AttrRole::Custom:
    default:
        break;
    }
}

} // namespace

void GpuMesh::Release() {
    if (vb) { vb->Release(); vb = nullptr; }
    if (ib) { ib->Release(); ib = nullptr; }
    vertexCount = indexCount = 0;
    valid = false;
}

bool DecodeComponentVertices(const modio::ModComponent& comp,
                             std::vector<PreviewVertex>& outVerts,
                             std::string& err) {
    outVerts.clear();
    if (comp.positionPath.empty()) {
        err = "No position buffer path for " + comp.name;
        return false;
    }

    std::vector<uint8_t> posBytes, texBytes;
    if (!ReadFileBytes(comp.positionPath, posBytes)) {
        err = "Cannot read position: " + comp.positionPath;
        return false;
    }

    const modio::BufferLayout& posLayout = comp.positionLayout.valid
        ? comp.positionLayout
        : modio::Preset_ZZZ_Position40();
    int posStride = posLayout.stride > 0 ? posLayout.stride : comp.positionStride;
    if (posStride <= 0) posStride = 40;

    if (posBytes.size() % (size_t)posStride != 0) {
        err = "Position buffer size " + std::to_string(posBytes.size()) +
              " not divisible by stride " + std::to_string(posStride);
        // soft: floor
    }
    size_t vertCount = posBytes.size() / (size_t)posStride;
    if (vertCount == 0) {
        err = "Empty position buffer";
        return false;
    }

    bool hasTex = !comp.texcoordPath.empty() && ReadFileBytes(comp.texcoordPath, texBytes);
    const modio::BufferLayout& texLayout = comp.texcoordLayout.valid
        ? comp.texcoordLayout
        : modio::PresetForTexcoordStride(comp.texcoordStride > 0 ? comp.texcoordStride : 20);
    int texStride = texLayout.stride > 0 ? texLayout.stride : comp.texcoordStride;
    if (texStride <= 0) texStride = 20;

    if (hasTex) {
        size_t texVerts = texBytes.size() / (size_t)texStride;
        if (texVerts != vertCount) {
            Logger::Get().Warn("Texcoord vertex count " + std::to_string(texVerts) +
                               " != position " + std::to_string(vertCount) +
                               " (" + comp.name + ") — clamping to min");
            vertCount = std::min(vertCount, texVerts);
        }
    }

    outVerts.resize(vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
        PreviewVertex& v = outVerts[i];
        std::memset(&v, 0, sizeof(v));
        v.normal[1] = 1.f;
        v.tangent[0] = 1.f;
        v.tangent[3] = 1.f;
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.f;

        const uint8_t* pBase = posBytes.data() + i * (size_t)posStride;
        for (const auto& el : posLayout.elements) {
            if (el.role == modio::AttrRole::None) continue;
            float f[4];
            if (modio::DecodeAttrToFloat4(pBase, posStride, el, f))
                ApplyRole(v, el.role, f);
        }

        if (hasTex) {
            const uint8_t* tBase = texBytes.data() + i * (size_t)texStride;
            for (const auto& el : texLayout.elements) {
                if (el.role == modio::AttrRole::None) continue;
                float f[4];
                if (modio::DecodeAttrToFloat4(tBase, texStride, el, f))
                    ApplyRole(v, el.role, f);
            }
        }
    }

    err.clear();
    return true;
}

bool LoadIndexBufferFile(const std::string& path, std::vector<uint32_t>& outIndices,
                         std::string& err) {
    outIndices.clear();
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) {
        err = "Cannot read IB: " + path;
        return false;
    }
    if (bytes.size() % 4 != 0) {
        err = "IB size not multiple of 4: " + path;
        return false;
    }
    size_t n = bytes.size() / 4;
    outIndices.resize(n);
    std::memcpy(outIndices.data(), bytes.data(), bytes.size());
    return true;
}

bool CreateGpuMesh(ID3D11Device* device,
                   const std::vector<PreviewVertex>& verts,
                   const std::vector<uint32_t>& indices,
                   const std::string& name,
                   GpuMesh& out,
                   std::string& err) {
    out.Release();
    out.name = name;
    if (!device || verts.empty() || indices.empty()) {
        err = "CreateGpuMesh: empty input";
        return false;
    }

    D3D11_BUFFER_DESC vbd{};
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(PreviewVertex));
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit{};
    vinit.pSysMem = verts.data();
    HRESULT hr = device->CreateBuffer(&vbd, &vinit, &out.vb);
    if (FAILED(hr)) {
        err = "CreateBuffer VB failed";
        return false;
    }

    D3D11_BUFFER_DESC ibd{};
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.ByteWidth = (UINT)(indices.size() * sizeof(uint32_t));
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iinit{};
    iinit.pSysMem = indices.data();
    hr = device->CreateBuffer(&ibd, &iinit, &out.ib);
    if (FAILED(hr)) {
        out.Release();
        err = "CreateBuffer IB failed";
        return false;
    }

    out.vertexCount = (UINT)verts.size();
    out.indexCount = (UINT)indices.size();
    out.indexFormat = DXGI_FORMAT_R32_UINT;
    out.valid = true;
    return true;
}

static bool CollectDrawIndices(const std::vector<modio::DrawIndexed>& draws,
                               const std::vector<uint32_t>& fullIb,
                               std::vector<uint32_t>& used) {
    used.clear();
    for (const auto& d : draws) {
        if (!d.visible || d.indexCount <= 0) continue;
        int start = d.indexStart;
        int count = d.indexCount;
        if (start < 0) start = 0;
        if (start + count > (int)fullIb.size())
            count = std::max(0, (int)fullIb.size() - start);
        for (int i = 0; i < count; ++i)
            used.push_back(fullIb[(size_t)start + (size_t)i]);
    }
    return !used.empty();
}

bool BuildBatchMesh(ID3D11Device* device,
                    const modio::ModComponent& comp,
                    const modio::ModPart& part,
                    const modio::DrawBatch& batch,
                    GpuMesh& out,
                    std::string& err) {
    if (!batch.visible) {
        err = "batch hidden";
        return false;
    }
    std::vector<PreviewVertex> verts;
    if (!DecodeComponentVertices(comp, verts, err))
        return false;

    std::vector<uint32_t> fullIb;
    if (part.ibAbsolutePath.empty() || !LoadIndexBufferFile(part.ibAbsolutePath, fullIb, err))
        return false;

    std::vector<uint32_t> used;
    if (!CollectDrawIndices(batch.draws, fullIb, used)) {
        err = "no visible draws in batch " + batch.name;
        return false;
    }
    std::string meshName = comp.name + "/" + part.name + "/" + batch.name;
    return CreateGpuMesh(device, verts, used, meshName, out, err);
}

bool BuildPartMesh(ID3D11Device* device,
                   const modio::ModComponent& comp,
                   const modio::ModPart& part,
                   GpuMesh& out,
                   std::string& err) {
    // Merge all visible batch draws (single mesh — loses multi-texture)
    std::vector<PreviewVertex> verts;
    if (!DecodeComponentVertices(comp, verts, err))
        return false;
    std::vector<uint32_t> fullIb;
    if (part.ibAbsolutePath.empty() || !LoadIndexBufferFile(part.ibAbsolutePath, fullIb, err))
        return false;
    std::vector<uint32_t> used;
    for (const auto& b : part.batches) {
        if (!b.visible) continue;
        std::vector<uint32_t> tmp;
        CollectDrawIndices(b.draws, fullIb, tmp);
        used.insert(used.end(), tmp.begin(), tmp.end());
    }
    if (used.empty()) {
        err = "no visible draws";
        return false;
    }
    return CreateGpuMesh(device, verts, used, comp.name + "/" + part.name, out, err);
}

bool CreatePreviewInputLayout(ID3D11Device* device, ID3DBlob* vsBlob,
                              ID3D11InputLayout** outLayout) {
    if (!device || !vsBlob || !outLayout) return false;
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,       0, 72, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT,       0, 80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    HRESULT hr = device->CreateInputLayout(layout, _countof(layout),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), outLayout);
    return SUCCEEDED(hr);
}

} // namespace preview3d
