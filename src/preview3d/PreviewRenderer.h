#pragma once

#include "MeshGpu.h"
#include "../modio/ModTypes.h"

#include <d3d11.h>
#include <directxmath.h>
#include <string>
#include <vector>

namespace preview3d {

struct PreviewCamera {
    float yaw = 0.4f;
    float pitch = 0.3f;
    float distance = 2.5f;
    float target[3] = { 0, 1.0f, 0 };
    float fovY = 45.f * 3.14159265f / 180.f;

    DirectX::XMMATRIX View() const;
    DirectX::XMMATRIX Proj(float aspect) const;
};

struct PartDrawItem {
    std::string componentName;
    std::string partName;
    GpuMesh mesh;
    bool visible = true;
    // first diffuse path if any
    std::string diffusePath;
    ID3D11ShaderResourceView* diffuseSRV = nullptr; // owned by cache later
};

class PreviewRenderer {
public:
    PreviewRenderer() = default;
    ~PreviewRenderer();

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Rebuild GPU meshes from parsed ModScene (uses layouts/roles)
    bool LoadScene(ID3D11Device* device, const modio::ModScene& scene);

    void SetDebugMode(int mode) { m_DebugMode = mode; } // 0 shaded, 1 uv0, 2 nrm, 3 color, 4 outline
    int GetDebugMode() const { return m_DebugMode; }

    PreviewCamera& Camera() { return m_Camera; }
    const std::vector<PartDrawItem>& Items() const { return m_Items; }
    std::vector<PartDrawItem>& Items() { return m_Items; }

    // Render into existing RTV (viewport already set by caller)
    void Render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv, float aspect,
                float clearR = 0.12f, float clearG = 0.12f, float clearB = 0.14f);

    bool IsReady() const { return m_Ready && !m_Items.empty(); }
    const std::string& LastError() const { return m_LastError; }
    const std::string& Status() const { return m_Status; }

private:
    ID3D11Device* m_Device = nullptr; // not owned
    ID3D11VertexShader* m_VS = nullptr;
    ID3D11PixelShader* m_PS = nullptr;
    ID3D11InputLayout* m_Layout = nullptr;
    ID3D11Buffer* m_CB = nullptr;
    ID3D11SamplerState* m_Sampler = nullptr;
    ID3D11RasterizerState* m_RS = nullptr;
    ID3D11DepthStencilState* m_DSS = nullptr;
    ID3D11ShaderResourceView* m_FallbackSRV = nullptr;
    ID3D11Texture2D* m_FallbackTex = nullptr;

    std::vector<PartDrawItem> m_Items;
    PreviewCamera m_Camera;
    int m_DebugMode = 0;
    bool m_Ready = false;
    std::string m_LastError;
    std::string m_Status;

    bool CompileShaders(ID3D11Device* device);
    void ReleaseItems();
    bool LoadDiffuseSRV(ID3D11Device* device, const std::string& path, ID3D11ShaderResourceView** outSRV);
    void FitCameraToMeshes();
};

} // namespace preview3d
