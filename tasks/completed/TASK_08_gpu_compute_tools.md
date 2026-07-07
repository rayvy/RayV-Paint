# TASK 08 — GPU Compute Tools: Magic Wand + Bucket Fill

**Файлы:** `src/shaders/FloodFill.hlsl` [NEW], `src/render/GpuComputeTools.h` [NEW], `src/render/GpuComputeTools.cpp` [NEW], `src/Canvas.cpp` [MODIFY]
**Приоритет:** СРЕДНИЙ (необходим для 8K производительности)
**Сложность:** Очень высокая
**Зависимость:** TASK_04 (Dx12Device class)

---

## Проблема

### Magic Wand — `ApplyMagicWandSelection`
Текущая реализация: CPU flood fill по flat pixel buffer.

При 8K canvas (8192×8192 = 67M пикселей):
- Worst case: весь canvas = 67M итераций
- ~500ms - 2000ms на слабых CPU
- Зависание UI во время выполнения

### Bucket Fill — `ApplyBucketFill`
Аналогично: CPU flood fill + fill loop.

---

## Стратегия

### GPU Compute Flood Fill — Multi-Pass BFS

**Идея:** Запускать compute shader многократно. Каждый pass распространяет flood fill на 4 соседа. Для 8K нужно ~8192 passes в худшем случае, но с оптимизацией (expand по 4 пикселя за pass) = ~2048 passes.

**Практично? Нет для реального GPU usage.** 2048 dispatch calls = overhead.

**Реальная стратегия — Jump Flooding Algorithm (JFA):**
- 1 pass = распространение на `2^k` пикселей
- log₂(8192) = 13 passes для 8K
- ~13 dispatch calls → практично

Но JFA не совсем подходит для irregular flood fill с tolerance.

**Гибридная стратегия (рекомендуемая):**
1. **CPU BFS с ThreadPool** — параллельный flood fill по тайлам
2. **GPU Upload результата** — финальная R8 mask → GPU
3. GPU compute только для `SmartSelect` (edge detection)

### Параллельный CPU BFS

```cpp
void Canvas::ApplyMagicWandSelection(int startX, int startY, float tolerance,
                                      bool add, bool subtract, bool contiguous) {
    if (!HasActiveLayer()) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.tileCache) return;
    
    // Запустить в background thread через ThreadPool
    m_SmartSelectInProgress.store(true);
    m_SmartSelectCancelled.store(false);
    
    ThreadPool::Get().Submit([this, startX, startY, tolerance, add, subtract, contiguous]() {
        auto newMask = std::vector<uint8_t>(m_Width * m_Height, 0);
        
        // Seed color
        float seedColor[4] = {};
        m_Layers[m_ActiveLayerIdx].tileCache->GetPixelF(startX, startY, seedColor);
        
        if (contiguous) {
            // BFS с tiled chunking
            MagicWandBFS_Tiled(newMask, startX, startY, seedColor, tolerance);
        } else {
            // Глобальный threshold pass — параллельно по тайлам
            MagicWandGlobalThreshold(newMask, seedColor, tolerance);
        }
        
        if (!m_SmartSelectCancelled.load()) {
            // Применить маску (thread-safe запись через mutex или atomic flag)
            std::lock_guard<std::mutex> lock(m_SelectionMutex);
            if (add) {
                for (size_t i = 0; i < newMask.size(); ++i)
                    m_SelectionMask[i] = std::max(m_SelectionMask[i], newMask[i]);
            } else if (subtract) {
                for (size_t i = 0; i < newMask.size(); ++i)
                    m_SelectionMask[i] = (newMask[i] > 0) ? 0 : m_SelectionMask[i];
            } else {
                m_SelectionMask = std::move(newMask);
            }
            m_HasSelection = true;
            m_SelectionMaskNeedsUpload = true;
        }
        
        m_SmartSelectInProgress.store(false);
    });
}
```

### Tiled BFS

```cpp
// Private helper
void Canvas::MagicWandBFS_Tiled(std::vector<uint8_t>& outMask,
                                  int startX, int startY,
                                  const float seedColor[4], float tolerance) {
    // Std BFS с bitset для visited (не flat bool vector — cache friendly)
    // Chunk: обрабатывать по строкам тайла
    // При переходе в новый тайл — получать тайл через GetTileData() один раз
    
    struct Pos { int x, y; };
    std::queue<Pos> bfsQueue;
    std::vector<bool> visited(m_Width * m_Height, false);
    
    bfsQueue.push({startX, startY});
    visited[startY * m_Width + startX] = true;
    
    auto colorMatch = [&](int x, int y) -> bool {
        float px[4] = {};
        m_Layers[m_ActiveLayerIdx].tileCache->GetPixelF(x, y, px);
        float diff = 0;
        for (int c = 0; c < 3; ++c) diff += std::abs(px[c] - seedColor[c]);
        return diff / 3.0f <= tolerance;
    };
    
    const int dx[] = {1, -1, 0, 0};
    const int dy[] = {0, 0, 1, -1};
    
    size_t iterations = 0;
    while (!bfsQueue.empty() && !m_SmartSelectCancelled.load()) {
        auto [x, y] = bfsQueue.front();
        bfsQueue.pop();
        outMask[y * m_Width + x] = 255;
        
        for (int d = 0; d < 4; ++d) {
            int nx = x + dx[d];
            int ny = y + dy[d];
            if (nx < 0 || ny < 0 || nx >= m_Width || ny >= m_Height) continue;
            int idx = ny * m_Width + nx;
            if (visited[idx]) continue;
            visited[idx] = true;
            if (colorMatch(nx, ny)) {
                bfsQueue.push({nx, ny});
            }
        }
        
        // Проверять отмену каждые 10000 итераций
        if (++iterations % 10000 == 0 && m_SmartSelectCancelled.load()) break;
    }
}
```

**Оптимизация BFS:** получать весь тайл одним `GetTileData()` и читать пиксели напрямую из raw pointer — на порядок быстрее чем `GetPixelF()` per pixel.

---

## GPU Compute — SmartSelect (Edge Detection)

Для `ApplySmartSelectSelection` — это уже использует OpenCV (если подключён).
Оставить на CPU + OpenCV.

---

## GPU Compute Brush Stamping (Phase 8 preview)

Опционально: compute shader для brush stamp.

```hlsl
// ComputeBrush.hlsl
RWTexture2D<float4> gCanvas : register(u0);
Texture2D<float4>   gBrush  : register(t0);  // stamp mask

cbuffer BrushParams : register(b0) {
    float2 center;       // пиксельные координаты
    float  radius;
    float  hardness;
    float4 color;
    float  opacity;
    float  erase;
};

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float2 pos = float2(id.xy);
    float dist = length(pos - center);
    if (dist > radius) return;
    
    float falloff = hardness >= 1.0 ? 1.0 :
                    smoothstep(radius, radius * hardness, dist);
    
    float4 dst = gCanvas[id.xy];
    float4 src = color;
    src.a *= falloff * opacity;
    
    if (erase > 0.5) {
        dst.a = max(0, dst.a - src.a);
    } else {
        // Porter-Duff over
        float4 result;
        result.a = src.a + dst.a * (1 - src.a);
        result.rgb = (result.a > 0) ?
            (src.rgb * src.a + dst.rgb * dst.a * (1 - src.a)) / result.a :
            float3(0, 0, 0);
        dst = result;
    }
    gCanvas[id.xy] = dst;
}
```

Для GPU brush нужен `UAV` на tile texture. Это требует изменения `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` при создании tile resources.

---

## INPUT для агента

**Файлы для чтения:**
- `src/Canvas.h` — `ApplyMagicWandSelection`, `ApplyBucketFill` декларации
- `src/Canvas.cpp` (или `src/CanvasSelection.cpp` после TASK_05) — реализации
- `src/core/TileCache.h` — `GetPixelF`, `GetTileData`, `ForEachDirtyTile`
- `src/core/ThreadPool.h` — API

**Файлы для редактирования:**
- `src/Canvas.h` — добавить `std::mutex m_SelectionMutex`
- `src/Canvas.cpp` / `CanvasSelection.cpp` — переписать `ApplyMagicWandSelection`

**Файлы для создания:**
- `src/CanvasSelection.cpp` (если TASK_05 выполнен) — private helpers `MagicWandBFS_Tiled`, `MagicWandGlobalThreshold`

---

## OUTPUT / RETURN

**Минимальный (быстрый выигрыш):**
1. `ApplyMagicWandSelection` запускается в ThreadPool background
2. `m_SmartSelectInProgress` флаг корректно используется (UI показывает spinner)
3. BFS реализован с tiled cache-friendly доступом
4. `CancelSmartSelect()` работает корректно
5. Non-contiguous mode параллелен (по тайлам через ThreadPool)

**Расширенный (если время позволяет):**
- `ApplyBucketFill` тоже в background thread
- GPU compute brush stamping (требует UAV flag на tile resources)

---

## Стиль

- Background threads через `ThreadPool::Get().Submit()`
- Cancellation через `m_SmartSelectCancelled.store(true)`
- `std::mutex m_SelectionMutex` для thread-safe запись в `m_SelectionMask`
- BFS: `std::queue` для корректности; `std::deque` или ring buffer для производительности
- Проверять cancellation каждые 10K итераций
- После завершения background task → `MarkSelectionMaskDirty()`
