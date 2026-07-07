#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <unordered_map>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <directxmath.h>
#include "Canvas.h"

class CanvasRendererDX12 {
public:
    CanvasRendererDX12() = default;
    ~CanvasRendererDX12() { Shutdown(); }

    CanvasRendererDX12(const CanvasRendererDX12&) = delete;
    CanvasRendererDX12& operator=(const CanvasRendererDX12&) = delete;

    using AllocateSrvFn = std::function<bool(D3D12_CPU_DESCRIPTOR_HANDLE*, D3D12_GPU_DESCRIPTOR_HANDLE*)>;
    using FreeSrvFn = std::function<void(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)>;

    [[nodiscard]] bool Initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        AllocateSrvFn allocFn,
        FreeSrvFn freeFn
    );

    void Shutdown();

    // Composites active layers and renders the viewport checkerboard, albedo composite, and selection outline.
    [[nodiscard]] bool Render(
        ID3D12GraphicsCommandList* cmdList,
        Canvas& canvas,
        ID3D12Resource* viewportRT,
        D3D12_CPU_DESCRIPTOR_HANDLE viewportRtv,
        int viewportWidth,
        int viewportHeight
    );

private:
    struct GpuTile {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle = {};
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle = {};
        uint64_t lastAccess = 0;
    };

    struct LayerGpuResources {
        std::unordered_map<uint32_t, GpuTile> albedoTiles;
        std::unordered_map<uint32_t, GpuTile> maskTiles;
    };

    struct Vertex {
        DirectX::XMFLOAT2 pos;
        DirectX::XMFLOAT2 uv;
    };

    struct alignas(16) CanvasBufferData {
        DirectX::XMFLOAT4 viewportSizeAndZoom;   // xy: size, z: zoom, w: rotation
        DirectX::XMFLOAT4 offsetAndCanvasSize;   // xy: pan, zw: canvas size
        DirectX::XMFLOAT4 channelMasksAndFlags;  // x: R, y: G, z: B, w: A (1.0f/0.0f)
        DirectX::XMFLOAT4 viewportFlags;         // x: flipH, y: flipV, z: outlineTime, w: unused
    };

    struct alignas(16) LayerBufferData {
        DirectX::XMFLOAT4 layerParams;     // x: opacity, y: hasMask, zw: translation
        DirectX::XMFLOAT4 transformParams; // x: scaleX, y: scaleY, z: rotation, w: isFloating
        DirectX::XMFLOAT4 centerParams;    // x: centerX, y: centerY, z: blendMode, w: unused
    };

    struct alignas(16) TileParamsData {
        DirectX::XMFLOAT4 tileParams;      // x: tileX, y: tileY, z: canvasWidth, w: canvasHeight
    };

    void UploadDirtyTiles(
        ID3D12GraphicsCommandList* cmdList,
        const TileCache& tileCache,
        LayerGpuResources& gpuRes,
        bool isMask,
        const std::vector<uint8_t>& rawMaskData,
        int canvasWidth,
        int canvasHeight
    );

    [[nodiscard]] bool CreateRootSignatures();
    [[nodiscard]] bool CreatePipelineStates();
    [[nodiscard]] bool CreateConstantBuffers();
    [[nodiscard]] bool CreateDummyResources();
    [[nodiscard]] bool CreateQuadVertexBuffer();

    [[nodiscard]] bool CompileShaders(
        Microsoft::WRL::ComPtr<ID3DBlob>& vsMain,
        Microsoft::WRL::ComPtr<ID3DBlob>& psMain,
        Microsoft::WRL::ComPtr<ID3DBlob>& vsLayer,
        Microsoft::WRL::ComPtr<ID3DBlob>& psLayerBlend,
        Microsoft::WRL::ComPtr<ID3DBlob>& psSelectionOutline,
        Microsoft::WRL::ComPtr<ID3DBlob>& vsTile,
        Microsoft::WRL::ComPtr<ID3DBlob>& psTileBlend
    );

    void GarbageCollectGpuLayers(const Canvas& canvas);

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> CreateTextureResource(
        int width,
        int height,
        DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Resource> CreateStagingResource(size_t size);

    void TransitionResource(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES stateBefore,
        D3D12_RESOURCE_STATES stateAfter
    );

    uint32_t AllocateConstantBufferSpace(const void* data, uint32_t size);

    // Returns offset into the CB ring for the canvas global CBV; binds via slot 0 on cmdList.
    uint32_t UpdateCanvasBuffer(
        ID3D12GraphicsCommandList* cmdList,
        const Canvas& canvas,
        int viewportWidth,
        int viewportHeight
    );

    // Returns offset into the CB ring for the layer CBV; binds via slot 1 on cmdList.
    uint32_t UpdateLayerBuffer(
        ID3D12GraphicsCommandList* cmdList,
        float opacity,
        bool hasMask,
        float uOff, float vOff,
        float scaleX, float scaleY,
        float rotation,
        bool isFloating,
        float centerX, float centerY,
        BlendMode blendMode
    );

    std::string GetEmbeddedHLSL() const;
    std::string GetTileShadersHLSL() const;

    // Loads a precompiled .cso file. Returns empty vector on failure.
    static std::vector<uint8_t> LoadCsoFile(const std::string& path);

    ID3D12Device* m_Device = nullptr;
    ID3D12CommandQueue* m_CommandQueue = nullptr;

    AllocateSrvFn m_AllocFn;
    FreeSrvFn m_FreeFn;

    std::unordered_map<const TileCache*, LayerGpuResources> m_GpuLayers;

    // Monolithic selection mask
    Microsoft::WRL::ComPtr<ID3D12Resource> m_SelectionMaskTex;
    D3D12_CPU_DESCRIPTOR_HANDLE m_SelectionMaskSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_SelectionMaskSrvGpu = {};
    int m_SelectionMaskWidth = 0;
    int m_SelectionMaskHeight = 0;

    // Ping-pong composite render targets
    Microsoft::WRL::ComPtr<ID3D12Resource> m_CompositeRTs[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_CompositeRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_CompositeRtvs[2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_CompositeSrvCpus[2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_CompositeSrvGpus[2] = {};
    int m_CompositeWidth = 0;
    int m_CompositeHeight = 0;

    // Dummy textures
    Microsoft::WRL::ComPtr<ID3D12Resource> m_DummyWhiteTex;
    D3D12_CPU_DESCRIPTOR_HANDLE m_DummyWhiteSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_DummyWhiteSrvGpu = {};

    Microsoft::WRL::ComPtr<ID3D12Resource> m_DummyBlackTex;
    D3D12_CPU_DESCRIPTOR_HANDLE m_DummyBlackSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_DummyBlackSrvGpu = {};

    // Pipeline components
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PsoCheckerboard;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PsoLayerBlend;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PsoSelectionOutline;

    // Unified Ring Constant Buffer for CBVs
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ConstantBufferUpload;
    uint8_t* m_CbMappedData = nullptr;
    uint32_t m_CbOffset = 0;
    static constexpr uint32_t MAX_CB_SIZE = 1024 * 1024; // 1 MB ring buffer

    Microsoft::WRL::ComPtr<ID3D12Resource> m_QuadVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_QuadVertexBufferView = {};

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_StagingResources;
    uint64_t m_AccessCounter = 0;
};
