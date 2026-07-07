#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <functional>
#include <directxmath.h>

class GpuComputeTools {
public:
    GpuComputeTools() = default;
    ~GpuComputeTools() { Shutdown(); }

    GpuComputeTools(const GpuComputeTools&) = delete;
    GpuComputeTools& operator=(const GpuComputeTools&) = delete;

    using AllocateSrvFn = std::function<bool(D3D12_CPU_DESCRIPTOR_HANDLE*, D3D12_GPU_DESCRIPTOR_HANDLE*)>;
    using FreeSrvFn = std::function<void(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)>;

    [[nodiscard]] bool Initialize(
        ID3D12Device* device,
        AllocateSrvFn allocFn,
        FreeSrvFn freeFn
    );

    void Shutdown();

    // Runs GPU compute flood fill on the selection mask using input albedo texture.
    // Returns true on success.
    [[nodiscard]] bool RunFloodFill(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* albedoRes,
        DXGI_FORMAT albedoFormat,
        ID3D12Resource* maskRes,
        int width,
        int height,
        int startX,
        int startY,
        const float seedColor[4],
        float tolerance,
        bool contiguous
    );

private:
    struct alignas(16) FloodFillParams {
        DirectX::XMFLOAT4 seedColor;
        float tolerance;
        DirectX::XMINT2 seedPos;
        DirectX::XMINT2 canvasSize;
        int passIndex;
        float padding[3];
    };

    [[nodiscard]] bool CreateRootSignature();
    [[nodiscard]] bool CreatePipelineStates();
    [[nodiscard]] bool CompileShaders(
        Microsoft::WRL::ComPtr<ID3DBlob>& csInit,
        Microsoft::WRL::ComPtr<ID3DBlob>& csMain,
        Microsoft::WRL::ComPtr<ID3DBlob>& csGlobalThreshold
    );

    void TransitionResource(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES stateBefore,
        D3D12_RESOURCE_STATES stateAfter
    );

    std::string GetEmbeddedFloodFillHLSL() const;
    static std::vector<uint8_t> LoadCsoFile(const std::string& path);

    ID3D12Device* m_Device = nullptr;
    AllocateSrvFn m_AllocFn;
    FreeSrvFn m_FreeFn;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PsoInit;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PsoMain;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PsoGlobalThreshold;
};
