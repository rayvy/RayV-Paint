# TASK 09 — VRAM Overflow Strategy + Device Lost Recovery

**Файлы:** `src/CanvasRendererDX12.cpp`, `src/CanvasRendererDX12.h`, `src/main.cpp`
**Приоритет:** СРЕДНИЙ (критично для продакшна, не для разработки)
**Сложность:** Высокая
**Зависимость:** TASK_04 (Dx12Device class)

---

## Проблема 1: VRAM Overflow

Текущее состояние:
- `TileCache` (CPU) имеет LRU eviction: `m_MaxTilesInRAM = 512` (~128MB RGBA8)
- GPU tile textures (`m_GpuLayers`) никогда не эвиктируются — растут до исчерпания VRAM

При 8K canvas × 5 слоёв × RGBA8:
- 32×32 тайлов × 256KB = 8MB per layer per format
- 5 layers × 8MB albedo + 5 × 8MB mask = 80MB только для тайлов
- + Composite ping-pong: 2 × 8K × 8K × 4B = 512MB
- + Selection mask: 8K × 8K × 1B = 64MB
- **Total: ~660MB VRAM** для базового случая

На GPU с 4GB VRAM — управляемо. На 2GB GPU — критично.

---

## Стратегия VRAM Management

### Часть 1: GPU Tile Eviction

Добавить LRU eviction для GPU tiles:

```cpp
// В CanvasRendererDX12.h
static constexpr uint64_t kMaxGpuTiles = 2048;  // ~512MB RGBA8

// В GarbageCollectGpuLayers() — добавить GPU LRU trim
void CanvasRendererDX12::TrimGpuTileCache() {
    // Собрать все GPU tiles отсортированные по lastAccess
    struct TileRef {
        TileCache* cache;
        uint32_t tileKey;
        uint64_t lastAccess;
        bool isAlbedo;
    };
    
    std::vector<TileRef> allTiles;
    for (auto& [tc, gpuRes] : m_GpuLayers) {
        for (auto& [key, tile] : gpuRes.albedoTiles)
            allTiles.push_back({const_cast<TileCache*>(tc), key, tile.lastAccess, true});
        for (auto& [key, tile] : gpuRes.maskTiles)
            allTiles.push_back({const_cast<TileCache*>(tc), key, tile.lastAccess, false});
    }
    
    if (allTiles.size() <= kMaxGpuTiles) return;  // Не переполнено
    
    // Сортировать по lastAccess — старые первые
    std::sort(allTiles.begin(), allTiles.end(),
        [](const TileRef& a, const TileRef& b) { return a.lastAccess < b.lastAccess; });
    
    // Эвиктировать старые тайлы
    size_t toEvict = allTiles.size() - kMaxGpuTiles;
    for (size_t i = 0; i < toEvict; ++i) {
        auto& ref = allTiles[i];
        auto it = m_GpuLayers.find(ref.cache);
        if (it == m_GpuLayers.end()) continue;
        
        auto& tiles = ref.isAlbedo ? it->second.albedoTiles : it->second.maskTiles;
        auto tileIt = tiles.find(ref.tileKey);
        if (tileIt == tiles.end()) continue;
        
        // Free GPU resources
        if (tileIt->second.srvCpuHandle.ptr != 0) {
            m_FreeFn(tileIt->second.srvCpuHandle, tileIt->second.srvGpuHandle);
        }
        tiles.erase(tileIt);
        
        // Помечаем соответствующий CPU tile как dirty 
        // чтобы при следующем обращении он перегрузился
        int tx = ref.tileKey & 0xFFFF;
        int ty = ref.tileKey >> 16;
        ref.cache->MarkDirty(tx, ty);
    }
}
```

Вызывать `TrimGpuTileCache()` в начале `Render()` после GarbageCollect.

### Часть 2: DXGI_ADAPTER_DESC для VRAM query

```cpp
// В Dx12Device::Initialize() — запросить размер VRAM и сохранить
SIZE_T vramBytes = 0;
Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
if (SUCCEEDED(m_Adapter.As(&adapter3))) {
    DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo);
    vramBytes = memInfo.Budget;  // Текущий бюджет
}
Logger::Get().Info("VRAM Budget: " + std::to_string(vramBytes / (1024*1024)) + " MB");

// Установить kMaxGpuTiles на основе VRAM:
// 256KB per RGBA8 tile, оставить 50% VRAM для других нужд
uint64_t maxTilesFromVram = (vramBytes / 2) / (256 * 1024);
m_MaxGpuTiles = std::min(maxTilesFromVram, static_cast<uint64_t>(4096));
```

---

## Проблема 2: Device Lost Recovery

При VRAM исчерпании, TDR (Timeout Detection and Recovery) или driver crash:
- `Present()` или `ExecuteCommandLists()` возвращает `DXGI_ERROR_DEVICE_REMOVED`
- Текущий код: нет обработки → crash

### Device Lost Detection

В `main.cpp` после `Present` или `ExecuteCommandLists`:
```cpp
HRESULT hr = g_pSwapChain->Present(1, 0);
if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    HandleDeviceLost();
    return;  // Пропустить этот кадр
}
```

### HandleDeviceLost() strategy

```cpp
void HandleDeviceLost() {
    Logger::Get().Error("GPU Device Lost detected. Attempting recovery...");
    
    // 1. Сохранить документ на диск (emergency autosave)
    if (g_Canvas.IsDocumentModified()) {
        std::string emergencyPath = ConfigManager::Get().GetBackupDir() + "/emergency_autosave.rayp";
        Logger::Get().Info("Emergency autosave to: " + emergencyPath);
        g_Canvas.SaveCanvasRayp(emergencyPath);  // sync, blocking
    }
    
    // 2. Показать пользователю сообщение через ImGui modal
    // (устанавливаем флаг, показываем в следующем кадре после recovery)
    g_DeviceLostOccurred = true;
    
    // 3. Уничтожить все GPU ресурсы
    g_CanvasRenderer.Shutdown();
    ImGui_ImplDX12_Shutdown();
    // ... cleanup swapchain, heaps
    
    // 4. Попытаться пересоздать device (раз в N секунд)
    // Если hardware не восстановился — завершить приложение gracefully
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (TryRecreateDevice()) {
            Logger::Get().Info("Device recovered successfully.");
            return;
        }
    }
    
    Logger::Get().Error("Device recovery failed. Exiting.");
    PostQuitMessage(0);
}
```

### TryRecreateDevice()

```cpp
bool TryRecreateDevice() {
    // Проверить причину потери устройства
    HRESULT reason = g_pd3d12Device->GetDeviceRemovedReason();
    Logger::Get().Error("Device removed reason: 0x" + /* hex string */ ...);
    
    // Попытаться пересоздать (через Dx12Device class после TASK_04)
    return g_DX12.Initialize(hWnd, false);
}
```

---

## Composite RT размер — ограничение для очень больших canvas

Для canvas > GPU_MAX_TEXTURE_DIM (обычно 16384 для DX12 hardware):
- Не создавать composite RT на весь canvas
- Использовать viewport-clipped rendering: рендерить только видимую область

Добавить проверку в `CanvasRendererDX12::Render()`:
```cpp
// Ограничить composite RT размер до аппаратного максимума
D3D12_FEATURE_DATA_FORMAT_SUPPORT formatData = {};
UINT maxTexSize = 16384;  // DX12 Feature Level 11.0 minimum
if (canvasWidth > (int)maxTexSize || canvasHeight > (int)maxTexSize) {
    Logger::Get().Error("Canvas exceeds max texture size — viewport clipping not yet implemented.");
    // TODO: viewport-clipped rendering
    return false;
}
```

---

## INPUT для агента

**Файлы для чтения:**
- `src/CanvasRendererDX12.h` — `GpuTile` struct, `LayerGpuResources`
- `src/CanvasRendererDX12.cpp` — `GarbageCollectGpuLayers()` (строки 436-469)
- `src/CanvasRendererDX12.cpp` — начало `Render()` (строки 80-145)
- `src/main.cpp` — поиск `Present` и `ExecuteCommandLists` вызовов

**Файлы для редактирования:**
- `src/CanvasRendererDX12.h` — добавить `kMaxGpuTiles`, `m_MaxGpuTiles`, `TrimGpuTileCache()`
- `src/CanvasRendererDX12.cpp` — добавить `TrimGpuTileCache()`, вызов в `Render()`
- `src/main.cpp` — добавить device lost check после Present, `HandleDeviceLost()`

---

## OUTPUT / RETURN

**Минимальный:**
1. `TrimGpuTileCache()` реализован и вызывается из `Render()`
2. GPU tile count ограничен `kMaxGpuTiles = 2048`
3. Эвиктированные GPU tiles помечают соответствующий CPU tile как dirty

**Полный:**
4. Device lost detection в main loop (после Present)
5. Emergency autosave при device lost
6. `TryRecreateDevice()` попытка recovery

---

## Стиль

- `kMaxGpuTiles` — constexpr или runtime из VRAM query
- GPU tile evict → `m_FreeFn(cpu, gpu)` + `MarkDirty` на CPU
- Device lost → Logger::Error + emergency save + graceful exit
- Никаких `assert` в production при device lost — полная обработка
