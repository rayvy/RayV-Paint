# TASK 14 — Memory Info Status Bar (Footer HUD)

**Файлы:** `src/ui/EditorPanels.h`, `src/ui/EditorPanels.cpp` (или аналоги), `src/Canvas.h`, `src/Canvas.cpp`
**Приоритет:** 🟡 СРЕДНИЙ (необходимо для диагностики и профессионального вида)
**Сложность:** Средняя

---

## Цель

Отображать в нижней строке (footer/statusbar) следующую информацию в реальном времени:

```
[Canvas: 4096×4096 | Tiles: 128/1024 GPU, 256 CPU | VRAM: 512MB / 4GB | RAM: 1.2GB | HDD swap: 0 | Layers: 5]  FPS: 60 | 16.7ms
```

---

## Источники данных

### 1. Размер canvas, количество слоёв — из Canvas

```cpp
canvas.GetWidth(), canvas.GetHeight()
canvas.GetLayers().size()
```

### 2. Количество тайлов в VRAM и CPU RAM — нужен новый API

#### В `TileCache.h` добавить:

```cpp
// Количество реально аллоцированных тайлов (не пустых)
int GetAllocatedTileCount() const;

// Размер занятой CPU RAM в байтах (allocated tiles only)
size_t GetCpuRamBytes() const;  // = allocatedTiles * TILE_SIZE * TILE_SIZE * bpp

// Максимально возможных тайлов (по размеру canvas)
int GetMaxTileCount() const;  // = tilesX * tilesY
```

#### В `CanvasRendererDX12.h` добавить:

```cpp
// Статистика GPU тайлов
struct GpuStats {
    uint64_t gpuTileCount = 0;       // Количество GPU тайлов
    uint64_t gpuTileMaxCapacity = 0; // m_MaxGpuTiles
    size_t   vramEstimateBytes = 0;  // gpuTileCount * 256KB (approximation)
};
GpuStats GetGpuStats() const;
```

Реализация `GetGpuStats()`:
```cpp
CanvasRendererDX12::GpuStats CanvasRendererDX12::GetGpuStats() const {
    GpuStats stats;
    for (auto& [tc, gpuRes] : m_GpuLayers) {
        stats.gpuTileCount += gpuRes.albedoTiles.size();
        stats.gpuTileCount += gpuRes.maskTiles.size();
    }
    stats.gpuTileMaxCapacity = m_MaxGpuTiles;
    // Composite RTs
    if (m_CompositeWidth > 0 && m_CompositeHeight > 0) {
        stats.vramEstimateBytes += 2 * (size_t)m_CompositeWidth * m_CompositeHeight * 4;  // 2× ping-pong RGBA8
    }
    // Tile memory estimate (assuming RGBA8 256×256 tiles)
    stats.vramEstimateBytes += stats.gpuTileCount * 256 * 256 * 4;
    return stats;
}
```

### 3. VRAM usage из DXGI query

Добавить в `Dx12Device.h`:
```cpp
struct VramInfo {
    SIZE_T budgetBytes;   // OS-allocated budget для процесса
    SIZE_T usageBytes;    // Текущее использование
    SIZE_T availableBytes;
};
VramInfo QueryVramInfo() const;
```

Реализация в `Dx12Device.cpp`:
```cpp
Dx12Device::VramInfo Dx12Device::QueryVramInfo() const {
    VramInfo info = {};
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (m_Adapter && SUCCEEDED(m_Adapter.As(&adapter3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
        adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo);
        info.budgetBytes    = memInfo.Budget;
        info.usageBytes     = memInfo.CurrentUsage;
        info.availableBytes = memInfo.Budget > memInfo.CurrentUsage
                            ? memInfo.Budget - memInfo.CurrentUsage : 0;
    }
    return info;
}
```

### 4. CPU RAM usage

```cpp
// Windows API
PROCESS_MEMORY_COUNTERS pmc = {};
GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
size_t ramUsage = pmc.WorkingSetSize;  // байты физической памяти
```

### 5. HDD swap count — из TileCache

Если TileCache эвиктирует тайлы на диск (swapping), нужен счётчик:
```cpp
// В TileCache.h:
uint64_t GetHddSwapCount() const { return m_HddSwapCount; }

// В TileCache.cpp: инкрементировать при swap-to-disk
```

Если HDD swap не реализован (тайлы просто отбрасываются из RAM) — отображать "0 (no swap)".

---

## UI реализация

### Где рендерить statusbar

В `main.cpp` — после рендера всех panel'ей но до `ImGui::Render()`:

```cpp
// Statusbar (Bottom of screen)
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float barHeight = 24.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - barHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, barHeight));
    ImGui::SetNextWindowViewport(viewport->ID);
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoScrollWithMouse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoNav
                           | ImGuiWindowFlags_NoDocking;
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::Begin("##StatusBar", nullptr, flags);
    
    // Собрать статистику
    auto vramInfo  = g_DX12.QueryVramInfo();
    auto gpuStats  = g_CanvasRenderer.GetGpuStats();
    
    // CPU RAM (Windows)
    size_t ramUsageBytes = 0;
    {
        PROCESS_MEMORY_COUNTERS pmc = {};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            ramUsageBytes = pmc.WorkingSetSize;
    }
    
    // TileCache статистика (aggregate по слоям)
    int totalCpuTiles = 0;
    size_t totalCpuRamBytes = 0;
    for (const auto& layer : g_Canvas.GetLayers()) {
        if (layer.tileCache) {
            totalCpuTiles += layer.tileCache->GetAllocatedTileCount();
            totalCpuRamBytes += layer.tileCache->GetCpuRamBytes();
        }
    }
    
    auto formatBytes = [](size_t bytes) -> std::string {
        if (bytes >= 1024*1024*1024)
            return std::to_string(bytes / (1024*1024*1024)) + "." +
                   std::to_string((bytes % (1024*1024*1024)) / (1024*1024*100)) + "GB";
        if (bytes >= 1024*1024)
            return std::to_string(bytes / (1024*1024)) + "MB";
        if (bytes >= 1024)
            return std::to_string(bytes / 1024) + "KB";
        return std::to_string(bytes) + "B";
    };
    
    char buf[512];
    snprintf(buf, sizeof(buf),
        "Canvas: %d×%d  |  Tiles: %llu GPU / %d CPU  |  VRAM: %s / %s  |  RAM: %s  |  Layers: %d  |  FPS: %.0f",
        g_Canvas.GetWidth(), g_Canvas.GetHeight(),
        gpuStats.gpuTileCount, totalCpuTiles,
        formatBytes(vramInfo.usageBytes).c_str(), formatBytes(vramInfo.budgetBytes).c_str(),
        formatBytes(ramUsageBytes).c_str(),
        (int)g_Canvas.GetLayers().size(),
        uiState.fps
    );
    
    ImGui::TextUnformatted(buf);
    
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}
```

### Обновление статистики — не каждый кадр

DXGI VRAM query и GetProcessMemoryInfo — не free операции. Обновлять каждые 500ms:

```cpp
static float statUpdateTimer = 0.0f;
static Dx12Device::VramInfo cachedVramInfo = {};
static CanvasRendererDX12::GpuStats cachedGpuStats = {};
static size_t cachedRamBytes = 0;

statUpdateTimer += uiState.frameTimeMs;
if (statUpdateTimer >= 500.0f) {
    cachedVramInfo = g_DX12.QueryVramInfo();
    cachedGpuStats = g_CanvasRenderer.GetGpuStats();
    // ... GetProcessMemoryInfo
    statUpdateTimer = 0.0f;
}
// Использовать cached значения для рендера
```

---

## INPUT для агента

**Файлы для чтения:**
- `src/render/Dx12Device.h` — текущий API
- `src/CanvasRendererDX12.h` — private поля `m_GpuLayers`, `m_MaxGpuTiles`, `m_CompositeWidth/Height`
- `src/core/TileCache.h` — текущий API (проверить есть ли `GetAllocatedTileCount`)
- `src/main.cpp` строки 1505-1530 (render loop end) — место для statusbar

**Файлы для редактирования:**
- `src/render/Dx12Device.h` — добавить `VramInfo` + `QueryVramInfo()`
- `src/render/Dx12Device.cpp` — реализовать `QueryVramInfo()`
- `src/CanvasRendererDX12.h` — добавить `GpuStats` + `GetGpuStats()`
- `src/CanvasRendererDX12.cpp` — реализовать `GetGpuStats()`
- `src/core/TileCache.h` — добавить `GetAllocatedTileCount()`, `GetCpuRamBytes()`
- `src/core/TileCache.cpp` — реализовать
- `src/main.cpp` — добавить statusbar рендер

---

## OUTPUT / RETURN

1. `Dx12Device::QueryVramInfo()` возвращает real-time VRAM budget/usage от DXGI
2. `CanvasRendererDX12::GetGpuStats()` возвращает GPU tile count и VRAM estimate
3. `TileCache::GetAllocatedTileCount()` и `GetCpuRamBytes()` работают
4. Statusbar отображается в нижней части окна
5. Обновляется каждые 500ms (не каждый кадр)
6. Формат: `Canvas: WxH | Tiles: X GPU / Y CPU | VRAM: Xmb/Ymb | RAM: Zmb | Layers: N | FPS: XX`
7. Проект компилируется

---

## Стиль

- Statusbar — отдельный ImGui window без декорации
- `ImGuiWindowFlags_NoBringToFrontOnFocus` чтобы не перекрывать popup меню
- Cache interval = 500ms через static float timer
- Всегда видим (не прячем при отсутствии документа)
- При VRAM > 80% budget — показывать в красном цвете (`ImGui::TextColored`)
