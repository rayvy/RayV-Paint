#include "Dx12Device.h"
#include "../core/Logger.h"
#include <iostream>

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::GetCurrentRtv() const {
    if (!m_Initialized) return {};
    return m_Rtvs[m_BackBufferIndex];
}

ID3D12Resource* Dx12Device::GetCurrentBackBuffer() const {
    if (!m_Initialized) return nullptr;
    return m_BackBuffers[m_BackBufferIndex].Get();
}

bool Dx12Device::Initialize(HWND hwnd, bool useNullDriver) {
    if (m_Initialized) return true;

#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }
#endif

    HRESULT hr = S_OK;

    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_Factory));
    if (FAILED(hr)) {
        Logger::Get().Error("Failed to create DXGI Factory");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
    m_Factory.As(&factory6);

    if (useNullDriver) {
        if (FAILED(m_Factory->EnumWarpAdapter(IID_PPV_ARGS(&m_Adapter)))) {
            Logger::Get().Error("Failed to enumerate Warp Adapter");
            return false;
        }
        hr = D3D12CreateDevice(m_Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device));
    } else {
        // Enumerate GPU preference
        if (factory6) {
            for (UINT i = 0;; ++i) {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                HRESULT enumHr = factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
                if (enumHr == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(enumHr) || !adapter) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc = {};
                adapter->GetDesc1(&desc);
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    continue;
                }
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device)))) {
                    m_Adapter = adapter;
                    break;
                }
            }
        }
        if (!m_Device) {
            hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device));
        }
    }

    if (FAILED(hr) || !m_Device) {
        Logger::Get().Error("Failed to create D3D12 Device");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue));
    if (FAILED(hr)) {
        Logger::Get().Error("Failed to create Command Queue");
        return false;
    }

    m_RtvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_SrvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Create RTV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_RtvHeap));
    if (FAILED(hr)) {
        Logger::Get().Error("Failed to create RTV Descriptor Heap");
        return false;
    }

    // Create SRV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 64;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = m_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvHeap));
    if (FAILED(hr)) {
        Logger::Get().Error("Failed to create SRV Descriptor Heap");
        return false;
    }
    m_SrvHeapCapacity = srvHeapDesc.NumDescriptors;
    m_SrvHeapNextFree = 0;
    m_SrvFreeList.clear();

    // Create Command Allocators per frame
    for (UINT i = 0; i < kFrameCount; ++i) {
        hr = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocators[i]));
        if (FAILED(hr)) {
            Logger::Get().Error("Failed to create Command Allocator " + std::to_string(i));
            return false;
        }
    }

    // Create Command List (linked to frame 0 allocator)
    hr = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList));
    if (FAILED(hr)) {
        Logger::Get().Error("Failed to create Command List");
        return false;
    }
    m_CommandList->Close();

    // Create Fence and event
    hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
    if (FAILED(hr)) {
        Logger::Get().Error("Failed to create Fence");
        return false;
    }

    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_FenceEvent) {
        Logger::Get().Error("Failed to create Fence Event");
        return false;
    }

    // Create Swapchain
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrameCount;
    sd.Width = 0;
    sd.Height = 0;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Stereo = FALSE;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    IDXGIFactory2* swapFactory = nullptr;
    if (factory6) {
        swapFactory = factory6.Get();
    } else {
        swapFactory = m_Factory.Get();
    }
    hr = swapFactory->CreateSwapChainForHwnd(m_CommandQueue.Get(), hwnd, &sd, nullptr, nullptr, &swapChain1);
    if (SUCCEEDED(hr)) {
        swapFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        hr = swapChain1.As(&m_SwapChain);
    }

    if (FAILED(hr) || !m_SwapChain) {
        Logger::Get().Error("Failed to create Swap Chain");
        return false;
    }

    m_Initialized = true;

    // Create render targets initially
    if (!CreateRenderTarget()) {
        return false;
    }

    return true;
}

bool Dx12Device::CreateRenderTarget() {
    if (!m_Initialized || !m_SwapChain || !m_Device || !m_RtvHeap) return false;

    CleanupRenderTarget();

    m_BackBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        HRESULT hr = m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_BackBuffers[i]));
        if (FAILED(hr) || !m_BackBuffers[i]) {
            Logger::Get().Error("Failed to get swap chain buffer " + std::to_string(i));
            continue;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE currentHandle = rtvHandle;
        currentHandle.ptr += static_cast<SIZE_T>(i) * m_RtvDescriptorSize;
        m_Rtvs[i] = currentHandle;
        m_Device->CreateRenderTargetView(m_BackBuffers[i].Get(), nullptr, currentHandle);
    }

    return true;
}

void Dx12Device::CleanupRenderTarget() {
    for (UINT i = 0; i < kFrameCount; ++i) {
        m_BackBuffers[i].Reset();
        m_Rtvs[i].ptr = 0;
    }
}

void Dx12Device::ResizeSwapchain(uint32_t width, uint32_t height) {
    if (!m_Initialized || !m_SwapChain) return;

    WaitForGpu();
    CleanupRenderTarget();

    HRESULT hr = m_SwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        Logger::Get().Error("Failed to resize Swap Chain buffers to " + std::to_string(width) + "x" + std::to_string(height));
    }

    (void)CreateRenderTarget();
}

bool Dx12Device::BeginFrame() {
    if (!m_Initialized) return false;

    m_CommandAllocators[m_BackBufferIndex]->Reset();
    m_CommandList->Reset(m_CommandAllocators[m_BackBufferIndex].Get(), nullptr);

    // Transition backbuffer to RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_BackBuffers[m_BackBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CommandList->ResourceBarrier(1, &barrier);

    return true;
}

void Dx12Device::EndFrame() {
    if (!m_Initialized) return;

    // Transition backbuffer to PRESENT
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_BackBuffers[m_BackBufferIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CommandList->ResourceBarrier(1, &barrier);

    m_CommandList->Close();

    ID3D12CommandList* commandLists[] = { m_CommandList.Get() };
    m_CommandQueue->ExecuteCommandLists(1, commandLists);

    if (m_SwapChain) {
        HRESULT hr = m_SwapChain->Present(1, 0);
        if (FAILED(hr)) {
            Logger::Get().Error("Failed to Present swap chain");
        }
    }

    // Signal fence for the completed frame
    const uint64_t fenceValueToSignal = ++m_FenceValues[m_BackBufferIndex];
    m_CommandQueue->Signal(m_Fence.Get(), fenceValueToSignal);

    // Update back buffer index
    if (m_SwapChain) {
        m_BackBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
    } else {
        m_BackBufferIndex = (m_BackBufferIndex + 1) % kFrameCount;
    }

    // Wait for the next back buffer to be ready
    if (m_Fence->GetCompletedValue() < m_FenceValues[m_BackBufferIndex]) {
        m_Fence->SetEventOnCompletion(m_FenceValues[m_BackBufferIndex], m_FenceEvent);
        WaitForSingleObject(m_FenceEvent, INFINITE);
    }
}

void Dx12Device::WaitForGpu() {
    if (!m_Initialized || !m_CommandQueue || !m_Fence || !m_FenceEvent) return;

    const uint64_t fenceValue = ++m_FenceValues[m_BackBufferIndex];
    m_CommandQueue->Signal(m_Fence.Get(), fenceValue);
    m_Fence->SetEventOnCompletion(fenceValue, m_FenceEvent);
    WaitForSingleObject(m_FenceEvent, INFINITE);
}

void Dx12Device::Shutdown() {
    if (!m_Initialized) return;

    WaitForGpu();
    CleanupRenderTarget();

    if (m_FenceEvent) {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }

    m_Fence.Reset();
    m_CommandList.Reset();
    for (UINT i = 0; i < kFrameCount; ++i) {
        m_CommandAllocators[i].Reset();
    }
    m_SrvHeap.Reset();
    m_RtvHeap.Reset();
    m_SwapChain.Reset();
    m_CommandQueue.Reset();
    m_Device.Reset();
    m_Adapter.Reset();
    m_Factory.Reset();

    m_SrvFreeList.clear();
    m_SrvHeapCapacity = 0;
    m_SrvHeapNextFree = 0;

    m_Initialized = false;
}

bool Dx12Device::AllocateSrv(D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                             D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    if (!m_Initialized || !m_SrvHeap || m_SrvDescriptorSize == 0) return false;

    std::lock_guard<std::mutex> lock(m_SrvMutex);

    uint32_t index = 0;
    if (!m_SrvFreeList.empty()) {
        index = m_SrvFreeList.back();
        m_SrvFreeList.pop_back();
    } else {
        if (m_SrvHeapNextFree >= m_SrvHeapCapacity) {
            Logger::Get().Error("DX12 SRV Descriptor Heap exhausted");
            return false;
        }
        index = m_SrvHeapNextFree++;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    outCpu->ptr = cpuStart.ptr + static_cast<SIZE_T>(index) * m_SrvDescriptorSize;
    outGpu->ptr = gpuStart.ptr + static_cast<UINT64>(index) * m_SrvDescriptorSize;
    return true;
}

void Dx12Device::FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    (void)gpu;
    if (!m_Initialized || !m_SrvHeap || m_SrvDescriptorSize == 0) return;

    std::lock_guard<std::mutex> lock(m_SrvMutex);

    const SIZE_T base = m_SrvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr < base) return;

    const SIZE_T offset = cpu.ptr - base;
    if (offset % m_SrvDescriptorSize != 0) return;

    const uint32_t index = static_cast<uint32_t>(offset / m_SrvDescriptorSize);
    if (index < m_SrvHeapCapacity) {
        m_SrvFreeList.push_back(index);
    }
}
