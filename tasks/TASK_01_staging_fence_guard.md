# TASK 01 — Staging Resource Lifetime / Fence Guard

**Файл:** `src/CanvasRendererDX12.cpp` + `src/CanvasRendererDX12.h`
**Приоритет:** КРИТИЧЕСКИЙ (возможный GPU crash / device lost)
**Сложность:** Средняя

---

## Проблема

В `CanvasRendererDX12::Render()` есть строка:
```cpp
m_StagingResources.clear(); // Free resources uploaded in previous frame
```

Эта строка стоит в самом начале `Render()`. Она освобождает staging буферы **предыдущего кадра**.

**Но GPU ещё может читать их.** В DX12 нет автоматической синхронизации при уничтожении ресурса.
Если GPU использует эти буферы (например, при `CopyTextureRegion`) а CPU уже их освободил — **device lost**.

Вторая проблема: `CreateStagingResource` аллоцирует новый ресурс каждый frame без pooling.

---

## Архитектура решения

### Схема per-frame staging pools

```
Frame N:    [upload tiles] -> staging pool A -> [GPU copy] -> fence(N)
Frame N+1:  [upload tiles] -> staging pool B -> [GPU copy] -> fence(N+1)
CPU: clear staging pool A ТОЛЬКО после fence(N) сигнализирован
```

### Структура в .h

```cpp
// В private секции CanvasRendererDX12:
struct FrameStagingPool {
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
    uint64_t fenceValue = 0;  // fence value когда эти ресурсы безопасно освобождать
};

static constexpr int kMaxFramesInFlight = 2;
FrameStagingPool m_StagingPools[kMaxFramesInFlight];
int m_CurrentFrameIdx = 0;

// Fence для sync
Microsoft::WRL::ComPtr<ID3D12Fence> m_UploadFence;
HANDLE m_UploadFenceEvent = nullptr;
uint64_t m_UploadFenceValue = 0;
```

Убрать:
```cpp
// УДАЛИТЬ:
std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_StagingResources;
```

### Изменения в Initialize()

```cpp
// После CreateQuadVertexBuffer():
if (FAILED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_UploadFence))))
    return false;
m_UploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
if (!m_UploadFenceEvent) return false;
```

### Изменения в Shutdown()

```cpp
// Перед освобождением ресурсов — дождаться всех frame
if (m_UploadFence) {
    m_UploadFenceValue++;
    m_CommandQueue->Signal(m_UploadFence.Get(), m_UploadFenceValue);
    if (m_UploadFence->GetCompletedValue() < m_UploadFenceValue) {
        m_UploadFence->SetEventOnCompletion(m_UploadFenceValue, m_UploadFenceEvent);
        WaitForSingleObject(m_UploadFenceEvent, INFINITE);
    }
}
for (auto& pool : m_StagingPools) pool.resources.clear();
if (m_UploadFenceEvent) { CloseHandle(m_UploadFenceEvent); m_UploadFenceEvent = nullptr; }
```

### Изменения в Render() — начало

```cpp
// Вместо m_StagingResources.clear():
m_CurrentFrameIdx = (m_CurrentFrameIdx + 1) % kMaxFramesInFlight;
auto& currentPool = m_StagingPools[m_CurrentFrameIdx];

// Дождаться fence предыдущего использования этого слота
if (currentPool.fenceValue > 0 &&
    m_UploadFence->GetCompletedValue() < currentPool.fenceValue) {
    m_UploadFence->SetEventOnCompletion(currentPool.fenceValue, m_UploadFenceEvent);
    WaitForSingleObject(m_UploadFenceEvent, INFINITE);
}
currentPool.resources.clear();  // Теперь безопасно освобождать
```

### В конце Render() — после всех CopyTextureRegion

```cpp
// Сигнализировать fence
m_UploadFenceValue++;
m_CommandQueue->Signal(m_UploadFence.Get(), m_UploadFenceValue);
currentPool.fenceValue = m_UploadFenceValue;
```

### Изменения в CreateStagingResource и processUpload

Везде где `m_StagingResources.push_back(staging)` — заменить на:
```cpp
currentPool.resources.push_back(staging);
```

Добавить параметр `FrameStagingPool& pool` в `UploadDirtyTiles`.

---

## INPUT для агента

**Файлы для чтения:**
- `src/CanvasRendererDX12.h` (полностью)
- `src/CanvasRendererDX12.cpp` строки 1-434 (Render + UploadDirtyTiles)

**Файлы для редактирования:**
- `src/CanvasRendererDX12.h`
- `src/CanvasRendererDX12.cpp`

---

## OUTPUT / RETURN

После завершения агент должен убедиться что:
1. `m_StagingResources` поле удалено из `.h`
2. `FrameStagingPool[2]` добавлен в `.h`
3. `m_UploadFence` + `m_UploadFenceEvent` в `.h`
4. В `Render()` нет `m_StagingResources.clear()` — есть fence wait + pool selection
5. В `Render()` в конце `m_CommandQueue->Signal(m_UploadFence, ...)`
6. В `Shutdown()` есть WaitForGpu перед release
7. Проект компилируется без ошибок

---

## Стиль кода

- `Microsoft::WRL::ComPtr<>` для всех COM объектов
- `[[nodiscard]]` для функций возвращающих bool
- Никаких raw COM Release() вызовов вручную
- Логирование через `Logger::Get().Error(...)` при FAILED()
