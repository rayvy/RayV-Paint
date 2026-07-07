# TASK 13 — Открытие больших текстур (4K/8K): OOM + Coordinate Mismatch

**Файлы:** `src/CanvasFileIO.cpp`, `src/CanvasPaint.cpp`, `src/CanvasRendererDX12.cpp`
**Приоритет:** 🔴 КРИТИЧЕСКИЙ
**Сложность:** Высокая

---

## Диагноз — 3 независимых проблемы

### BUG A: stbi_zlib_compress ограничение — int overflow при > 2GB

`SaveCanvasRayp()` строка 451:
```cpp
unsigned char* compData = stbi_zlib_compress(
    data,
    static_cast<int>(uncompressedSize),  // ← INT! 4K float = 1GB+, переполнение
    &compSize,
    8
);
```

4K × 4K × 4 float × 4 bytes = **256MB** per layer (RGBA32F).
`static_cast<int>` от 268MB = OK (int max ~2GB).
Но 8K: **1024MB** → приближается к int max.
16K: **4096MB** → OVERFLOW → stbi_zlib_compress вернёт nullptr → crash.

Также при загрузке 4K, `ExportLayerF` делает полный RGBA32F export = 256MB vector alloc.

### BUG B: Coordinate mismatch — кисть рисует не там

При изменении размера canvas через UI (ResizeCanvas), либо при открытии большой текстуры:
`CanvasRendererDX12` не сбрасывает `m_CompositeRTs` если размер совпадает с предыдущим документом.

Если открыть 4K документ, закрыть его, открыть другой 4K документ:
- `m_CompositeWidth == 4096, m_CompositeHeight == 4096` → ресурсы не пересоздаются
- Но `m_GpuLayers` сбрасывается (старый TileCache pointer невалиден)
- GPU читает старые или garbage tile данные → артефакты

Canvas координаты в shader вычисляются через `u_OffsetAndCanvasSize.zw` (canvas size).
При неправильном composite RT — UVs mapping рассинхронизирован → кисть рисует "не там".

### BUG C: ComposeVisibleLayers при Export — блокирует UI для 4K

`SaveCanvasStandard` → `ComposeVisibleLayers` → `ExportLayerF` для каждого слоя = 256MB alloc × N layers.
Для 4K + 3 слоя = 768MB RAM alloc, синхронно.

### BUG D: ResizeCanvas — нет GPU invalidation

`Canvas::ResizeCanvas()` меняет `m_Width, m_Height` и пересоздаёт tileCache, но не уведомляет renderer.
Renderer продолжает использовать старые tile GPU ресурсы (`m_GpuLayers` с устаревшими TileCache*).
→ crash при попытке рендера после resize.

---

## Решения

### Fix A: Тайловый save/load RAYP

Вместо полного `ExportLayerF` для сохранения — сохранять по тайлам:

```cpp
// В SaveCanvasRayp() — вместо:
std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);
// ...compress all at once

// СТАТЬ:
const int SAVE_CHUNK_HEIGHT = 256;  // Обрабатывать по 256 строк за раз
// Или: сохранять по тайлам (256×256), один тайл за раз
// Это требует изменения формата файла → bumped version = 2

// Альтернатива (быстрее реализовать): chunked processing
// Разбить на горизонтальные полосы по TILE_SIZE строк
```

**Ближайшее решение**: добавить проверку размера и предупреждение + для 4K сохранять 
синхронно но с progress callback.

Для `stbi_zlib_compress` — ограничение `int` реально только при > 2GB, для 4K (256MB) это OK.
Проблема не здесь — искать дальше.

### Fix B: Принудительная инвалидация GPU ресурсов при открытии нового документа

В `Canvas::LoadImageToLayer()` и `Canvas::LoadCanvasRayp()` — после очистки слоёв:
```cpp
// Canvas.h — добавить флаг:
bool m_RendererInvalidated = false;

// В LoadImageToLayer и LoadCanvasRayp — после m_Layers.clear():
m_RendererInvalidated = true;

// Getter:
bool IsRendererInvalidated() const { return m_RendererInvalidated; }
void ClearRendererInvalidated() { m_RendererInvalidated = false; }
```

В `CanvasRendererDX12::Render()` — в начале:
```cpp
if (canvas.IsRendererInvalidated()) {
    // Сбросить все GPU tile resources для удалённых layers
    GarbageCollectGpuLayers(canvas);
    // Принудительно пересоздать composite RT если размер изменился
    m_CompositeWidth = 0;  // Force recreation
    m_CompositeHeight = 0;
    canvas.ClearRendererInvalidated();
}
```

### Fix C: Async / chunked composition для Export

`GetCompositePixels()` и `ComposeVisibleLayers` для Export:
- Добавить проверку размера: если > 2000×2000 → показать прогресс-бар в ImGui
- Запустить в thread pool, UI не блокировать

Для быстрого fix: добавить progress callback в `SaveCanvasStandard`:
```cpp
bool Canvas::SaveCanvasStandard(const std::string& path, const std::string& icc,
                                 std::function<void(float)> progress = nullptr);
```

### Fix D: GPU invalidation при ResizeCanvas

В `Canvas::ResizeCanvas()` — добавить `m_RendererInvalidated = true`.

---

## Fix для coordinate mapping при 4K

Проверить расчёт в `main.cpp` строки 818-843:
```cpp
float screenOriginX = std::floor(g_Canvas.GetPan().x + static_cast<float>(viewportWidth) * 0.5f);
float screenOriginY = std::floor(g_Canvas.GetPan().y + static_cast<float>(viewportHeight) * 0.5f);
float rotatedX = (localMouseX - screenOriginX) / g_Canvas.GetZoom();
float rotatedY = (localMouseY - screenOriginY) / g_Canvas.GetZoom();
```

Это должно совпадать с вершинным шейдером `Canvas.hlsl` строки 64-65:
```hlsl
float2 screenOrigin = floor(panOffset + viewportSize * 0.5f);
float2 screenPixelPos = floor(rotatedPixelPos * zoom) + screenOrigin;
```

**Потенциальный mismatch**: CPU использует `floor()` для screenOrigin, shader тоже.
Но CPU не применяет `floor()` к `rotatedPixelPos * zoom` — только к screenOrigin.
Shader применяет `floor()` к обоим. → **1 пиксель смещение** при некоторых zoom levels.

Исправить CPU side:
```cpp
float screenPixelX = std::floor(rotatedPixelPos.x * g_Canvas.GetZoom()) + screenOriginX;
// Обратное преобразование:
float canvasX = (localMouseX - screenOriginX) / g_Canvas.GetZoom();
// Точнее:
// localMouseX ≈ floor(canvasX * zoom) + screenOriginX
// canvasX = (localMouseX - screenOriginX) / zoom
// (без floor — floor уже применён пикселем, float division даёт правильный результат)
```

На практике для 4K: при zoom = 0.25 (4K в небольшом окне):
- 1 экранный пиксель = 4 canvas пикселя
- Погрешность ±1 screen pixel = ±4 canvas pixels → заметный сдвиг кисти

---

## INPUT для агента

**Файлы для чтения:**
- `src/Canvas.h` строки 83-280 (публичные методы Canvas)
- `src/CanvasFileIO.cpp` строки 204-302 (`LoadImageToLayer`)
- `src/CanvasFileIO.cpp` строки 482-601 (`LoadCanvasRayp`)
- `src/CanvasRendererDX12.cpp` — начало `Render()` (поиск `GarbageCollectGpuLayers`, `m_CompositeWidth`)
- `src/main.cpp` строки 818-843 (canvas coordinate mapping)

**Файлы для редактирования:**
- `src/Canvas.h` — добавить `m_RendererInvalidated`
- `src/CanvasFileIO.cpp` — `LoadImageToLayer`, `LoadCanvasRayp` → установить флаг
- `src/Canvas.cpp` — `ResizeCanvas()` → установить флаг
- `src/CanvasRendererDX12.cpp` — проверять флаг в начале Render
- `src/main.cpp` — проверить coordinate math (строки 818-843)

---

## OUTPUT / RETURN

1. `Canvas::IsRendererInvalidated()` → `true` после `LoadImageToLayer`, `LoadCanvasRayp`, `ResizeCanvas`
2. `CanvasRendererDX12::Render()` при `IsRendererInvalidated()` → GC + reset composite RT size
3. После fix: открыть 4K, закрыть, открыть другой 4K → нет артефактов
4. Coordinate mapping верифицирован — кисть рисует точно там где курсор
5. Нет крашей при открытии 4K PNG или DDS

---

## Тест (запуск скрипта)

```powershell
# Тест 1: открыть 4K текстуру
./RayVPaint.exe "C:/path/to/test_4096x4096.png"
# Ожидание: открывается без краша, без freeze

# Тест 2: изменить размер окна
# Resize несколько раз → без краша

# Тест 3: кисть точность
# Zoom 25% → провести линию → проверить координаты в пикселях
```
