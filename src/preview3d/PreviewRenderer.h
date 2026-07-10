#pragma once

#include "MeshGpu.h"
#include "MaterialConfig.h"
#include "RenderPasses.h"
#include "../modio/ModTypes.h"

#include <d3d11.h>
#include <directxmath.h>
#include <string>
#include <unordered_map>
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
    void GetPosition(float out[3]) const;
    void Reset(float dist = 2.8f);
    void PanScreen(float dxPx, float dyPx, float viewportH);
};

enum class ModelUpAxis : uint8_t {
    PlusY = 0,
    MinusY,
    PlusZ,
    MinusZ,
    PlusX,
    MinusX
};

struct ModelOrientation {
    ModelUpAxis upAxis = ModelUpAxis::PlusZ;
    bool flipX = false;
    bool flipY = false;
    bool flipZ = false;
    float yawOffsetDeg = 0.f;

    DirectX::XMMATRIX Matrix() const;
    static const char* UpAxisName(ModelUpAxis a);
};

struct PartDrawItem {
    std::string componentName;
    std::string partName;
    GpuMesh mesh;
    bool visible = true;

    std::string paths[4];
    std::string resNames[4]; // Resource* names for diagnostics
    ID3D11ShaderResourceView* srvs[4] = {};

    MaterialConfig material;
    std::string presetId;
};

class PreviewRenderer {
public:
    PreviewRenderer() = default;
    ~PreviewRenderer();

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    bool LoadScene(ID3D11Device* device, const modio::ModScene& scene);

    void SetDebugMode(int mode) { m_DebugMode = mode; }
    int GetDebugMode() const { return m_DebugMode; }

    PreviewCamera& Camera() { return m_Camera; }
    PreviewLighting& Lighting() { return m_Lighting; }
    ModelOrientation& Orientation() { return m_Orientation; }
    const ModelOrientation& Orientation() const { return m_Orientation; }
    PassConfig& Passes() { return m_Passes; }
    const PassConfig& Passes() const { return m_Passes; }

    std::vector<PartDrawItem>& Items() { return m_Items; }
    const std::vector<PartDrawItem>& Items() const { return m_Items; }

    void ResetView() { m_Camera.Reset(); FitCameraToMeshes(); }

    void ApplyPresetToPart(int partIndex, const MaterialConfig& cfg);
    void ApplyPresetToAll(const MaterialConfig& cfg);

    // Multi-pass: Main (+ optional Glow later) then OutlineZZZ
    void Render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv, float aspect,
                float clearR = 0.12f, float clearG = 0.12f, float clearB = 0.14f);

    bool IsReady() const { return m_Ready && !m_Items.empty(); }
    const std::string& LastError() const { return m_LastError; }
    const std::string& Status() const { return m_Status; }

private:
    ID3D11Device* m_Device = nullptr;

    // Main pass
    ID3D11VertexShader* m_VS = nullptr;
    ID3D11PixelShader* m_PS = nullptr;
    ID3D11InputLayout* m_Layout = nullptr;

    // Outline ZZZ pass
    ID3D11VertexShader* m_OutlineVS = nullptr;
    ID3D11PixelShader* m_OutlinePS = nullptr;
    ID3D11InputLayout* m_OutlineLayout = nullptr;

    ID3D11Buffer* m_FrameCB = nullptr;
    ID3D11Buffer* m_MatCB = nullptr;
    ID3D11Buffer* m_OutlineCB = nullptr;
    ID3D11SamplerState* m_Sampler = nullptr;
    ID3D11RasterizerState* m_RSCullNone = nullptr;
    ID3D11RasterizerState* m_RSCullFront = nullptr; // inverted hull
    ID3D11DepthStencilState* m_DSS = nullptr;
    ID3D11DepthStencilState* m_DSSOutline = nullptr;
    ID3D11ShaderResourceView* m_FallbackSRV = nullptr;
    ID3D11Texture2D* m_FallbackTex = nullptr;

    std::unordered_map<std::string, ID3D11ShaderResourceView*> m_TexCache;

    std::vector<PartDrawItem> m_Items;
    PreviewCamera m_Camera;
    PreviewLighting m_Lighting;
    ModelOrientation m_Orientation;
    PassConfig m_Passes;
    int m_DebugMode = 0;
    bool m_Ready = false;
    std::string m_LastError;
    std::string m_Status;

    bool CompileShaders(ID3D11Device* device);
    void ReleaseItems();
    ID3D11ShaderResourceView* GetOrLoadSRV(ID3D11Device* device, const std::string& path);
    void FitCameraToMeshes();
    static MaterialConfig GuessPresetForPart(const std::string& component, const std::string& part);

    void DrawMainPass(ID3D11DeviceContext* ctx);
    void DrawOutlineZZZPass(ID3D11DeviceContext* ctx);
};

} // namespace preview3d
