# TASK 07 — Async Upload Queue (Copy Command List)

**Файлы:** `src/render/Dx12AsyncUploader.h` [NEW], `src/render/Dx12AsyncUploader.cpp` [NEW], `src/CanvasRendererDX12.cpp` [MODIFY]
**Приоритет:** СРЕДНИЙ (производительность при большом dirty area)
**Сложность:** Высокая
**Зависимость:** Выполнять ПОСЛЕ TASK_01 и TASK_04

---

## Проблема

Текущий upload dirty tiles происходит **синхронно** в главном render command list:
```cpp
// В Render() — блокирует рендер ImGui пока идёт upload
UploadDirtyTiles(cmdList, *activeCache, gpuRes, ...);
```

При большом dirty area (например, после import 8K image или Bucket Fill на весь canvas):
- Все тайлы помечены dirty
- 8K canvas = 32×32 = 1024 тайла по 256KB = 256MB upload
- Upload + barrier transitions = сотни миллисекунд stall в главном потоке

---

## Целевая архитектура

```
Main Thread (Direct queue):
  ├── ImGui render
  ├── Viewport checkerboard PSO
  ├── Layer composite PSO  ← использует уже загруженные тайлы
  └── Present

Upload Thread (Copy queue):
  └── Batch upload dirty tiles → signal fence when done
  
Sync:
  Main queue: WaitForFence(uploadFence) ТОЛЬКО для тайлов нужных в этом кадре
  Остальные тайлы могут догружаться в следующем кадре
```

### `src/render/Dx12AsyncUploader.h`

```cpp
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>

struct UploadRequest {
    // Данные для загрузки
    std::vector<uint8_t> data;    // копия данных тайла
    
    // Целевой ресурс
    ID3D12Resource* dstResource;  // raw ptr — lifetime гарантирован вызывающим
    DXGI_FORMAT format;
    uint32_t width;
    uint32_t height;
    
    // Callback после завершения upload — вызывается в upload thread
    std::function<void()> onComplete;
};

class Dx12AsyncUploader {
public:
    Dx12AsyncUploader() = default;
    ~Dx12AsyncUploader() { Shutdown(); }

    Dx12AsyncUploader(const Dx12AsyncUploader&) = delete;
    Dx12AsyncUploader& operator=(const Dx12AsyncUploader&) = delete;

    [[nodiscard]] bool Initialize(ID3D12Device* device, ID3D12CommandQueue* mainQueue);
    void Shutdown();

    // Поставить в очередь загрузку тайла. Thread-safe.
    // dstResource должен быть в D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE до вызова.
    // После onComplete() ресурс вернётся в это состояние.
    void EnqueueUpload(UploadRequest request);

    // Вызывать из main thread каждый кадр.
    // Ждёт завершения uploads нужных для текущего кадра (с timeout).
    // Возвращает количество завершённых uploads с прошлого кадра.
    uint32_t FlushCompleted(uint32_t timeoutMs = 0);

    // GPU fence value который сигнализирует завершение всех текущих uploads
    uint64_t GetCompletedFenceValue() const { 
        return m_UploadFence ? m_UploadFence->GetCompletedValue() : 0; 
    }

private:
    void UploadThreadFunc();
    void ExecuteUploadBatch(std::vector<UploadRequest>& batch);

    ID3D12Device* m_Device = nullptr;
    ID3D12CommandQueue* m_MainQueue = nullptr;

    // Copy command queue (приоритет ниже чем Direct)
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>       m_CopyQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>   m_CopyAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CopyList;  // D3D12_COMMAND_LIST_TYPE_COPY

    // Cross-queue fence для синхронизации
    Microsoft::WRL::ComPtr<ID3D12Fence> m_UploadFence;
    std::atomic<uint64_t> m_UploadFenceValue{0};
    HANDLE m_FenceEvent = nullptr;

    // Upload thread
    std::thread m_UploadThread;
    std::atomic<bool> m_Running{false};

    // Request queue
    std::mutex m_QueueMutex;
    std::condition_variable m_QueueCV;
    std::queue<UploadRequest> m_PendingRequests;

    // Completed callbacks (main thread collects these)
    std::mutex m_CompletedMutex;
    std::vector<std::function<void()>> m_CompletedCallbacks;
};
```

---

## Интеграция в CanvasRendererDX12

### В CanvasRendererDX12.h добавить

```cpp
#include "render/Dx12AsyncUploader.h"

// В private:
Dx12AsyncUploader m_AsyncUploader;
bool m_AsyncUploaderReady = false;
```

### В CanvasRendererDX12::Initialize()

```cpp
if (m_AsyncUploader.Initialize(device, queue)) {
    m_AsyncUploaderReady = true;
    Logger::Get().Info("Async upload queue initialized.");
} else {
    Logger::Get().Info("Async upload unavailable — using sync path.");
}
```

### В UploadDirtyTiles — разделить на sync/async path

```cpp
void CanvasRendererDX12::UploadDirtyTiles(...) {
    auto processUpload = [&](int tx, int ty, const uint8_t* srcData) {
        // ... создать tile resource если нет
        
        if (m_AsyncUploaderReady) {
            // Async path: копировать данные в vector и поставить в очередь
            int totalBytes = TILE_SIZE * TILE_SIZE * bytesPerPixel;
            UploadRequest req;
            req.data.assign(srcData, srcData + totalBytes);
            req.dstResource = tile.resource.Get();
            req.format = format;
            req.width = TILE_SIZE;
            req.height = TILE_SIZE;
            req.onComplete = [&tile, this]() {
                // tile доступен для рендера
                tile.lastAccess = m_AccessCounter;
            };
            m_AsyncUploader.EnqueueUpload(std::move(req));
        } else {
            // Sync path (старая логика)
            // ... CopyTextureRegion через staging
        }
    };
    // ...
}
```

### В начале Render()

```cpp
// Собрать завершённые upload callbacks
if (m_AsyncUploaderReady) {
    m_AsyncUploader.FlushCompleted(0); // non-blocking
}
```

---

## Упрощённый первый шаг (если сложность высокая)

Если полный async thread слишком сложен — начать с **отдельного copy command list без thread**:

1. Создать `m_CopyCommandList` типа `D3D12_COMMAND_LIST_TYPE_COPY`
2. Batch все dirty tile uploads в `m_CopyCommandList`  
3. Execute copy list через main command queue (или dedicated copy queue)
4. Signal fence
5. В следующем кадре Wait на fence перед рендером

Это даёт:
- Меньше barrier transitions в main command list
- Возможность параллелизовать copy и draw в GPU
- Без сложного multi-threading

---

## INPUT для агента

**Файлы для чтения:**
- `src/CanvasRendererDX12.h` (полностью)
- `src/CanvasRendererDX12.cpp` — `UploadDirtyTiles` функция (строки 339-433)
- `src/CanvasRendererDX12.cpp` — начало `Render()` (строки 80-145)
- TASK_01 (для понимания fence pattern)

**Файлы для создания:**
- `src/render/Dx12AsyncUploader.h`
- `src/render/Dx12AsyncUploader.cpp`

**Файлы для редактирования:**
- `src/CanvasRendererDX12.h` — добавить `Dx12AsyncUploader m_AsyncUploader`
- `src/CanvasRendererDX12.cpp` — добавить async/sync switch в UploadDirtyTiles
- `CMakeLists.txt` — добавить `src/render/Dx12AsyncUploader.cpp`

---

## OUTPUT / RETURN

1. `Dx12AsyncUploader` создан с Initialize/Shutdown/EnqueueUpload/FlushCompleted
2. `CanvasRendererDX12` использует async uploader если доступен
3. Sync path остаётся как fallback
4. Thread-safe: никаких data races на tile resources
5. Проект компилируется

---

## Стиль

- `std::thread` для upload thread (не Win32 threads)
- `std::mutex` + `std::condition_variable` для queue sync
- `std::atomic<uint64_t>` для fence value (lockless read)
- Все COM через `Microsoft::WRL::ComPtr<>`
- Логирование ошибок через `Logger::Get()`
- Никаких блокирующих операций в main thread кроме `FlushCompleted(0)` (non-blocking)
