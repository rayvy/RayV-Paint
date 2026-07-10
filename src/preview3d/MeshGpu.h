#pragma once

#include "../modio/ModTypes.h"

#include <d3d11.h>
#include <string>
#include <vector>

namespace preview3d {

// Unified interleaved vertex for RayV preview (roles decoded from BufferLayout).
// UV_Outline / UV_Backface are separate from UV0 — never assumed to be texture coords.
struct PreviewVertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float color[4];
    float uv0[2];          // AttrRole::UV0
    float uvLight[2];      // AttrRole::UV_LightMap
    float uvOutline[2];    // AttrRole::UV_Outline (packed outline, not sample UV)
    float uvBackface[2];   // AttrRole::UV_Backface
};

struct GpuMesh {
    std::string name;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    UINT vertexCount = 0;
    UINT indexCount = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
    bool valid = false;

    void Release();
};

// CPU-side decode using layouts (no D3D)
bool DecodeComponentVertices(const modio::ModComponent& comp,
                             std::vector<PreviewVertex>& outVerts,
                             std::string& err);

bool LoadIndexBufferFile(const std::string& path, std::vector<uint32_t>& outIndices,
                         std::string& err);

// Upload decoded mesh to GPU
bool CreateGpuMesh(ID3D11Device* device,
                   const std::vector<PreviewVertex>& verts,
                   const std::vector<uint32_t>& indices,
                   const std::string& name,
                   GpuMesh& out,
                   std::string& err);

// Build one part's mesh: full component VB + indices for that part's draws
bool BuildPartMesh(ID3D11Device* device,
                   const modio::ModComponent& comp,
                   const modio::ModPart& part,
                   GpuMesh& out,
                   std::string& err);

// Input layout for PreviewVertex (create once)
bool CreatePreviewInputLayout(ID3D11Device* device, ID3DBlob* vsBlob,
                              ID3D11InputLayout** outLayout);

} // namespace preview3d
