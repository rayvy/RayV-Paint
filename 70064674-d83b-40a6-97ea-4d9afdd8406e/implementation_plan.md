# Реконструкция ядра RayV-Paint — Поддержка 16K текстур

## Принятые архитектурные решения

| Вопрос | Решение | Статус |
|--------|---------|--------|
| Внутренний формат пикселей | **UINT8 RGBA** (4 байта/пикс) | ✅ Утверждено |
| Формат GPU текстур (слои) | **DXGI_FORMAT_R8G8B8A8_UNORM** | ✅ Утверждено |
| R16F vs R32F для composite RT | Позже | ⏳ Отложено |
| Размер тайла | **256×256** пикселей | ✅ Утверждено |
| Лимит Undo памяти | **256 МБ** (настраиваемо) | ✅ Утверждено |
| bcdec потокобезопасность | **Потокобезопасен** — все чистые функции, read-only таблицы | ✅ Проверено |
| Float DDS при импорте | Clamp/normalize в UINT8 (0–255). Экспорт в float = reconvert | ✅ Утверждено |

---

## Контекст и диагностика проблем

### Выявленные критические баги

| # | Симптом | Root Cause |
|---|---------|------------|
| 1 | Запуск 7 сек вместо 0.3 сек | `Canvas::Initialize()` → `CreateNewLayer()` → `RecreateLayerTexture()` синхронно создаёт R32G32B32A32_FLOAT текстуру полного размера. `g_Canvas.ResizeCanvas(nullptr, ...)` в main.cpp вызывается до DX11 — безвредно, но `CreateCompositeResources()` создаёт огромный composite target. |
| 2 | 16K DDS не открывается | `DdsHelper::LoadDDS` читает весь файл (≈4 ГБ для BC7) в `std::vector<uint8_t>` → heap allocation fail или OOM. После декомпрессии `std::vector<float>` с 16K×16K×4 float = **4 ГБ** RAM — гарантированный OOM. |
| 3 | 16K пустой холст — редактор замирает | `Layer.pixels` = `std::vector<float>(16384×16384×4)` = 4 ГБ. `ResizeCanvas` двойная аллокация (oldPixels + новый). `m_SelectionMask` = ещё 1 ГБ float. `filteredPixels` = +4 ГБ. Итого: 10+ ГБ аллокаций. |
| 4 | Кисть/операции невозможны | CPU pixel pipeline работает на `std::vector<float>` полного размера. При 16K один проход по всем пикселям = 268M итераций только для одного `memcpy`. |

---

## Архитектурная стратегия

Переход от **«весь холст в RAM как один float-буфер»** к **«тайловая GPU-native архитектура с on-demand CPU tiles»**.

```
БЫЛО:                              БУДЕТ:
Layer.pixels[W*H*4 float]          TileCache (256×256 тайлы, LRU eviction)
  → RecreateLayerTexture()         GPU Texture → R8G8B8A8_UNORM (4 байта/пикс!)
  → R32G32B32A32_FLOAT (16 б/п)   Dirty tile upload → UpdateSubresource per tile
  → ВСЁ в GPU VRAM                 Composite RT → R8G8B8A8_UNORM или R16G16B16A16

Память сравнение для 16K×16K:
  Старый float32:  16384² × 16 байт = 4 ГБ per layer (невозможно!)
  Новый uint8:     16384² × 4 байта = 1 ГБ per layer
  С тайлами+LRU:   только ГОРЯЧИЕ тайлы в RAM (типично 32–128 МБ в работе)
```

---

## Предлагаемые изменения

### Фаза 0 — Быстрый старт (критично для UX)

#### [MODIFY] [Canvas.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/Canvas.cpp) + [Canvas.h](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/Canvas.h)

**Проблема**: `Canvas::Initialize()` синхронно создаёт default layer с текстурой до того, как пользователь что-то сделал.

**Исправление**: убрать `CreateNewLayer(device, "Background")` из `Initialize()`. Вместо этого создавать холст lazy — только когда пользователь нажимает "New Canvas", открывает файл или начинает рисовать.

**Изменение в `main.cpp`**: убрать `g_Canvas.ResizeCanvas(nullptr, ...)` из стартового пути. Канвас создаётся пустым; composite resources создаются только при первом реальном использовании.

**Ожидаемый эффект**: Старт ≈ 0.1 с (только DX11 + ImGui + шейдеры из кэша .cso).

---

### Фаза 1 — GPU-native формат текстур

#### [MODIFY] [Canvas.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/Canvas.cpp) — `RecreateLayerTexture()` и `CreateCompositeResources()`

**Переход на UINT8**: слои хранятся как `DXGI_FORMAT_R8G8B8A8_UNORM` (4 байта/пиксель). Это прямое соответствие внутреннему `TileCache` формату — никакой конвертации при GPU upload.

**Изменение composite RT**: пока оставить `DXGI_FORMAT_R8G8B8A8_UNORM` для composite target. При многослойном blending могут появиться артефакты precision — можно при необходимости повысить до R16 позже.

**Shader `Canvas.hlsl`**: сэмплер уже совместим с UNORM форматом, изменений минимум.

Мемори-эффект: слой 16K×16K = **1 ГБ** (было 4 ГБ). GPU VRAM — аналогично.

---

### Фаза 2 — Тайловая CPU-память (TileCache)

#### [NEW] [TileCache.h](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/core/TileCache.h) + [TileCache.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/core/TileCache.cpp)

Замена `std::vector<float> pixels` в `Layer` на **тайловый кэш**:

```cpp
// Размер тайла: 256×256 пикселей × 4 байта RGBA UINT8 = 256 КБ per tile
// При 16K: (16384/256)² = 4096 тайлов × 256 КБ = 1 ГБ max (все тайлы заполнены)
// LRU eviction: держим в RAM только N горячих тайлов (~32-128 МБ в работе)

class TileCache {
    struct Tile {
        std::array<uint8_t, 256*256*4> data; // 256 КБ per tile — UINT8 RGBA
        bool dirty = false;
        uint64_t lastAccess = 0;
    };
    
    std::unordered_map<uint32_t, std::unique_ptr<Tile>> m_Tiles; // key = tileY*tilesX+tileX
    size_t m_MaxTilesInRAM = 512; // ~128 МБ per layer default
    int m_TilesX, m_TilesY;
    
    Tile& GetOrCreate(int tileX, int tileY); // lazy alloc
    void EvictLRU();                          // free cold tiles
    bool HasTile(int tileX, int tileY) const;
    
    // Read/write pixel through tile (uint8, 0-255)
    void GetPixel(int x, int y, uint8_t rgba[4]) const;
    void SetPixel(int x, int y, const uint8_t rgba[4]);
    
    // Float helpers (конвертация на лету для PaintEngine и blend-операций)
    void GetPixelF(int x, int y, float rgba[4]) const; // rgba[i] = data[i] / 255.0f
    void SetPixelF(int x, int y, const float rgba[4]); // data[i] = clamp(v*255+0.5f)
    
    // Bulk operations
    void FillRect(int x0, int y0, int x1, int y1, const uint8_t rgba[4]);
    void CopyFrom(const TileCache& src, int srcX, int srcY, int dstX, int dstY, int w, int h);
    
    // Direct tile data ptr для GPU upload
    const uint8_t* GetTileData(int tileX, int tileY) const;
};
```

**Изменение в `Layer`** (`Canvas.h`):
```cpp
struct Layer {
    // БЫЛО:
    // std::vector<float> pixels;       // float32 RGBA — УДАЛЯЕМ
    // std::vector<float> filteredPixels;
    // bool needsUpload = false;
    
    // БУДЕТ:
    std::unique_ptr<TileCache> tileCache;  // CPU-side pixel store (UINT8 RGBA)
    std::vector<bool> gpuTileDirty;        // per-tile dirty flags для GPU upload
    ID3D11Texture2D* texture = nullptr;    // GPU master texture (R8G8B8A8_UNORM)
    ID3D11ShaderResourceView* srv = nullptr;
    // needsUpload — заменяется на gpuTileDirty
};
```

**Совместимость с mask/selection**: `layer.mask` и `m_SelectionMask` тоже переводим из `vector<float>` → `vector<uint8_t>` (0 = не выбрано, 255 = полностью выбрано). GPU mask texture → `R8_UNORM`.

> [!NOTE]
> Тайлы создаются **lazy**: пустой слой не аллоцирует ни одного тайла. При рисовании кистью аллоцируются только затронутые тайлы.

---

### Фаза 3 — Async File Loading (DDS / PNG Streaming)

#### [MODIFY] [DdsHelper.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/core/DdsHelper.cpp)

**Проблема**: `LoadDDS` читает весь файл в память, потом декомпрессирует в огромный `float` буфер — блокирует UI thread.

**Исправление**:

```cpp
// Новый API:
class DdsHelper {
public:
    // Sync header-only read (для получения размеров без загрузки пикселей)
    static bool ReadHeader(const std::string& filename, int& outWidth, int& outHeight, DdsFormat& outFormat);
    
    // Streaming async load: callback вызывается по одной строке тайлов
    // После каждого callback слой становится частично рендерабельным
    static void LoadDDSAsync(const std::string& filename, TileCache& outCache,
                             std::function<void(int progressPct)> onProgress,
                             std::function<void(bool success)> onComplete,
                             std::atomic<bool>& cancelToken);
    
    // Legacy sync (маленькие файлы < 512×512)
    static bool LoadDDS(const std::string& filename, DdsImage& outImage);
};
```

**Реализация**: читаем файл чанками по 16 тайловых строк (16×256 = 4096 строк), декомпрессируем BC7 на ThreadPool (bcdec потокобезопасен ✅), заполняем TileCache с `uint8_t` данными. Float DDS (R32F, R16F) — нормализуем в 0–255 при декодировании:
```cpp
// Конвертация float → uint8 при импорте
uint8_t FloatToU8(float v) { return (uint8_t)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
// «Фейковый» float32 при экспорте
float U8ToFloat(uint8_t v) { return v / 255.0f; }
```
Текстуру в GPU обновляем инкрементально через `UpdateSubresource` per tile.

#### [MODIFY] [ImageManager.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/core/ImageManager.cpp)

Аналогично: добавить `LoadImageFromFileAsync()` со streaming через stb_image в тайловом режиме.

---

### Фаза 4 — Тайловый GPU Upload (Incremental UpdateSubresource)

#### [MODIFY] [Canvas.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/Canvas.cpp) — `ComposeLayers()`

**Было**: `context->UpdateSubresource(layer.texture, 0, nullptr, srcPixels.data(), ...)` — загружает **всю** текстуру целиком каждый раз.

**Будет**: только изменённые тайлы:

```cpp
// В ComposeLayers():
for (auto& layer : m_Layers) {
    if (!layer.tileCache) continue;
    
    // Находим dirty тайлы и делаем точечный UpdateSubresource
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (!layer.gpuTileDirty[ty * tilesX + tx]) continue;
            if (!layer.tileCache->HasTile(tx, ty)) {
                layer.gpuTileDirty[ty * tilesX + tx] = false;
                continue; // empty tile = transparent, already cleared
            }
            
            D3D11_BOX box = { tx*256, ty*256, 0, (tx+1)*256, (ty+1)*256, 1 };
            context->UpdateSubresource(layer.texture, 0, &box,
                layer.tileCache->GetTileData(tx, ty),
                256 * 4, // UINT8 RGBA pitch = 256 * 4 bytes (не float!)
                0);
            layer.gpuTileDirty[ty * tilesX + tx] = false;
        }
    }
}
```

---

### Фаза 5 — Undo/Redo с ограничением памяти

#### [MODIFY] [UndoRedoManager.h/.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/core/UndoRedoManager.cpp)

**Проблема**: `TileDelta.oldPixels` / `newPixels` — 256KB на тайл. При 16K холсте одна операция (например Blur All) может занять **4 ГБ** undo history.

**Исправление**:
- Лимит **256 МБ** (настраивается через `config.json`)
- `TileDelta.oldPixels` / `newPixels` — теперь `vector<uint8_t>` вместо `vector<float>` (256KB per tile instead of 1MB — уже 4x экономия от формата!)
- `EnforceLimits()` выталкивает oldest операции при превышении лимита по байтам
- Будущая оптимизация (отдельная задача): diff-encoding или LZ4, чтобы Undo не зависело от размера текстуры

```cpp
// TileDelta — обновлённый
struct TileDelta {
    int layerIdx;
    int tileX;
    int tileY;
    std::vector<uint8_t> oldPixels; // 256×256×4 = 256 КБ (было 1 МБ float)
    std::vector<uint8_t> newPixels;
};

class UndoRedoManager {
    static constexpr size_t DEFAULT_MAX_MEMORY = 256 * 1024 * 1024; // 256 МБ
    size_t m_MaxMemoryBytes = DEFAULT_MAX_MEMORY;
    size_t m_CurrentMemoryBytes = 0;
    
    void EnforceLimits(); // выбрасывает oldest до fit в лимит
};
```

---

### Фаза 6 — CPU Operations SIMD

#### [MODIFY] [PaintEngine.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/core/PaintEngine.cpp)

**Проблема**: `DrawStamp` итерирует пиксели scalar. При 16K кисть с radius=500 проходит ~785K пикселей поштучно.

**Исправление**: 
- Ограничить рабочую область `DrawStamp` только теми тайлами TileCache, которые пересекаются с bounding box кисти
- Параллельная обработка тайлов через `ThreadPool::Enqueue` (bcdec-style: каждый тайл независим)
- Внутри тайла: SIMD для hot path (uint8 × 16 через SSE2, доступно везде)
- `PaintEngine` работает через `TileCache::SetPixelF()` / `GetPixelF()` — float-интерфейс поверх UINT8 без изменения алгоритмики кисти

---

### Фаза 7 — Texture Streaming UI (Progress HUD)

#### [MODIFY] [EditorPanels.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/ui/EditorPanels.cpp) / [Canvas.cpp](file:///c:/Users/Rayvy/Documents/GitHub/RayV-Paint/src/Canvas.cpp)

Добавить:
- `Canvas::IsLoadingAsync() const` → возвращает `true` если идёт фоновая загрузка
- `Canvas::GetLoadingProgress() const` → 0.0...1.0
- В `EditorPanels`: прогресс-бар в viewport overlay пока `IsLoadingAsync() == true`
- Холст отображается по мере загрузки (частично — с уже загруженными тайлами)

---

## Приоритет реализации

| Фаза | Критичность | Эффект | Сложность |
|------|-------------|--------|-----------|
| Фаза 0: Lazy Start | 🔴 Critical | Старт 7с → 0.1с | Низкая |
| Фаза 1: R16F формат | 🔴 Critical | VRAM ×2 экономия | Низкая |
| Фаза 3: Async DDS Load | 🔴 Critical | 16K DDS открывается | Средняя |
| Фаза 2: TileCache | 🟠 High | RAM ×10 экономия | Высокая |
| Фаза 4: Tile GPU Upload | 🟠 High | Smooth редактирование | Средняя |
| Фаза 5: Undo limits | 🟡 Medium | Стабильность | Низкая |
| Фаза 6: SIMD Brush | 🟡 Medium | Brushing performance | Высокая |
| Фаза 7: Progress HUD | 🟢 Low | UX polish | Низкая |

---

## Открытые вопросы

> [!NOTE]
> **Q1: Composite RT формат** (отложено)
> Пока используем `R8G8B8A8_UNORM` для composite target. При работе со многими слоями возможны precision артефакты на blending. Решим по факту — если заметим бандинг, повысим до R16G16B16A16_UNORM для intermediate compositing.

## План проверки

- Unit тест: открыть 16K DDS BC7 файл — должен открыться без OOM
- Unit тест: старт приложения замерить `std::chrono` — должен быть < 0.5с
- Smoke test: создать пустой холст 16K × 16K, рисовать кистью — не должно фризить
- Regression тест: все существующие операции (Blur, HSV, Curves, Select) на 4K холсте — должны работать без изменений
