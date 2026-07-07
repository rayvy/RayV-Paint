#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <mutex>

struct FrameIndex {
    uint32_t value = 0;
    explicit operator uint32_t() const { return value; }
};

class Dx12Device {
public:
    Dx12Device() = default;
    ~Dx12Device() { Shutdown(); }

    Dx12Device(const Dx12Device&) = delete;
    Dx12Device& operator=(const Dx12Device&) = delete;

    static constexpr uint32_t kFrameCount = 2;

    // Initialize DXGI factory, adapter, device, command queue, fence
    // useNullDriver = true for headless/test mode
    [[nodiscard]] bool Initialize(HWND hwnd, bool useNullDriver = false);

    // Create/recreate swapchain back buffers and RTVs
    [[nodiscard]] bool CreateRenderTarget();
    void CleanupRenderTarget();

    // Resize swapchain
    void ResizeSwapchain(uint32_t width, uint32_t height);

    // Frame management
    [[nodiscard]] bool BeginFrame();  // returns false if minimized/zero size
    void EndFrame();                  // Present + Signal fence

    // GPU sync — wait for all in-flight frames
    void WaitForGpu();

    void Shutdown();

    // Accessors (raw pointers — valid until Shutdown())
    ID3D12Device*             GetDevice()       const { return m_Device.Get(); }
    ID3D12CommandQueue*       GetCommandQueue() const { return m_CommandQueue.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_CommandList.Get(); }
    ID3D12CommandAllocator*   GetCommandAllocator() const { return m_CommandAllocators[m_BackBufferIndex].Get(); }
    IDXGISwapChain3*          GetSwapChain()   const { return m_SwapChain.Get(); }
    ID3D12DescriptorHeap*     GetSrvHeap()     const { return m_SrvHeap.Get(); }

    uint32_t GetCurrentBackBufferIndex() const { return m_BackBufferIndex; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRtv() const;
    ID3D12Resource*             GetCurrentBackBuffer() const;

    // Descriptor allocation for SRV heap (used by ImGui and CanvasRenderer)
    [[nodiscard]] bool AllocateSrv(D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* outGpu);
    void FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

private:
    Microsoft::WRL::ComPtr<IDXGIFactory6>          m_Factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>          m_Adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>           m_Device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>     m_CommandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>        m_SwapChain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CommandAllocators[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence>            m_Fence;
    HANDLE                                          m_FenceEvent = nullptr;
    uint64_t                                        m_FenceValues[kFrameCount] = {};
    uint32_t                                        m_BackBufferIndex = 0;

    // Swapchain back buffers
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_BackBuffers[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_RtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE                    m_Rtvs[kFrameCount] = {};
    uint32_t                                        m_RtvDescriptorSize = 0;

    // SRV descriptor heap (shared between ImGui and CanvasRenderer)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>   m_SrvHeap;
    uint32_t                                        m_SrvDescriptorSize = 0;
    uint32_t                                        m_SrvHeapCapacity = 0;
    uint32_t                                        m_SrvHeapNextFree = 0;
    std::vector<uint32_t>                           m_SrvFreeList;
    std::mutex                                      m_SrvMutex;

    bool m_Initialized = false;
};
