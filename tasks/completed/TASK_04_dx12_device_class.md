# TASK 04 — Dx12Device Class (Isolate Device/Swapchain from main.cpp)

**Файлы:** `src/render/Dx12Device.h` [NEW], `src/render/Dx12Device.cpp` [NEW], `src/main.cpp` [MODIFY]
**Приоритет:** ВЫСОКИЙ (архитектурный долг, безопасность RAII)
**Сложность:** Высокая

---

## Проблема

В `main.cpp` сейчас ~20 голых глобальных COM-указателей:
```cpp
static ID3D12Device*                g_pd3d12Device = nullptr;
static ID3D12CommandQueue*          g_pd3d12CommandQueue = nullptr;
static IDXGISwapChain3*             g_pSwapChain = nullptr;
static ID3D12Resource*              g_swapChainBackBuffers[kFrameCount] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE  g_mainRenderTargetViews[kFrameCount] = {};
static ID3D12DescriptorHeap*        g_swapChainRtvHeap = nullptr;
static ID3D12CommandAllocator*      g_pd3d12CommandAllocator = nullptr;
static ID3D12GraphicsCommandList*   g_pd3d12CommandList = nullptr;
static ID3D12Fence*                 g_pd3d12Fence = nullptr;
static HANDLE                       g_pd3d12FenceEvent = nullptr;
static UINT64                       g_pd3d12FenceValue = 0;
// ... и ещё canvas RT объекты
```

Нет RAII, нет инкапсуляции, функции `CreateDeviceD3D`, `CleanupDeviceD3D` — глобальные.

---

## Целевая архитектура

### `src/render/Dx12Device.h`

```cpp
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>

// Strongly-typed frame index
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

    bool m_Initialized = false;
};
```

---

## Изменения в main.cpp

После создания `Dx12Device`, в `main.cpp` остаётся только:
```cpp
static Dx12Device g_DX12;
```

Все вызовы заменяются на:
```cpp
g_DX12.GetDevice()          // вместо g_pd3d12Device
g_DX12.GetCommandQueue()    // вместо g_pd3d12CommandQueue
g_DX12.GetCommandList()     // вместо g_pd3d12CommandList
g_DX12.BeginFrame()         // вместо BeginMainRenderTarget()
g_DX12.EndFrame()           // вместо EndMainRenderTarget()
g_DX12.WaitForGpu()         // вместо WaitForGpu()
g_DX12.AllocateSrv(...)     // вместо AllocateSrvDescriptor(...)
g_DX12.FreeSrv(...)         // вместо FreeSrvDescriptor(...)
```

`CanvasRendererDX12::Initialize` получает:
```cpp
g_CanvasRenderer.Initialize(
    g_DX12.GetDevice(),
    g_DX12.GetCommandQueue(),
    [](auto* cpu, auto* gpu){ return g_DX12.AllocateSrv(cpu, gpu); },
    [](auto cpu, auto gpu){ g_DX12.FreeSrv(cpu, gpu); }
);
```

---

## CMakeLists.txt

Добавить новые файлы в `SOURCES`:
```cmake
src/render/Dx12Device.cpp
src/render/Dx12Device.h
```

---

## INPUT для агента

**Файлы для чтения:**
- `src/main.cpp` строки 98-165 (глобальные DX12 объекты + AllocateSrvDescriptor/FreeSrvDescriptor)
- `src/main.cpp` — поиск `CreateDeviceD3D`, `CleanupDeviceD3D`, `CreateRenderTarget`, `CleanupRenderTarget`, `BeginMainRenderTarget`, `EndMainRenderTarget`, `WaitForGpu` (найти все определения)
- `src/CanvasRendererDX12.h` строки 22-30 (Initialize сигнатура)

**Файлы для создания:**
- `src/render/Dx12Device.h`
- `src/render/Dx12Device.cpp`

**Файлы для редактирования:**
- `src/main.cpp` — заменить глобальные переменные на `g_DX12`
- `CMakeLists.txt` — добавить `src/render/Dx12Device.cpp`

---

## OUTPUT / RETURN

1. `src/render/Dx12Device.h` создан с публичным API как описан выше
2. `src/render/Dx12Device.cpp` содержит полную реализацию
3. `main.cpp` не содержит глобальных DX12 COM указателей (только `Dx12Device g_DX12`)
4. Функции `CreateDeviceD3D`, `CleanupDeviceD3D` удалены из main.cpp
5. `CanvasRendererDX12::Initialize` вызывается с `g_DX12` аксессорами
6. Проект компилируется

---

## Стиль и требования

- Все COM объекты через `Microsoft::WRL::ComPtr<>`
- Никаких `Release()` вручную
- Debug layer подключается в Debug builds: `D3D12GetDebugInterface` + `EnableDebugLayer()`
- `Logger::Get().Error()` при каждом FAILED()
- `[[nodiscard]]` на Initialize и AllocateSrv
- RAII: деструктор `~Dx12Device()` вызывает `Shutdown()`
- `Shutdown()` сначала `WaitForGpu()`, потом release

## Важно

`ID3D12Device` и `ID3D12CommandQueue` должны пережить `CanvasRendererDX12::Shutdown()`.
Порядок уничтожения в `main.cpp`:
1. `g_CanvasRenderer.Shutdown()` или `~CanvasRendererDX12()`
2. ImGui Shutdown
3. `g_DX12.Shutdown()`
