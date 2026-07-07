# TASK 03 — Mask Upload Dirty Tracking

**Файл:** `src/CanvasRendererDX12.cpp`, `src/Canvas.h`
**Приоритет:** ВЫСОКИЙ (лишние GPU uploads каждый frame)
**Сложность:** Средняя

---

## Проблема

В `CanvasRendererDX12::Render()`:
```cpp
if (layer.hasMask && !layer.mask.empty()) {
    UploadDirtyTiles(cmdList, *activeCache, gpuRes, true, layer.mask, canvasWidth, canvasHeight);
}
layer.maskNeedsUpload = false;
```

В `UploadDirtyTiles` для `isMask == true`:
```cpp
// Итерирует по ВСЕМ albedo tiles и перезаписывает mask tiles каждый кадр
for (auto& [tileKey, albedoTile] : gpuRes.albedoTiles) {
    // ... processUpload для каждого тайла
}
```

**Проблема:** Нет dirty tracking для маски. Каждый кадр все mask tiles перезаписываются заново, даже если маска не изменилась.

---

## Архитектура решения

### Часть 1: Dirty tracking для маски

В `Canvas.h`, в структуре `Layer`:
```cpp
bool maskNeedsUpload = false;  // уже есть — ИСПОЛЬЗОВАТЬ корректно
```

Текущая проблема: `maskNeedsUpload` сбрасывается но UploadDirtyTiles для маски игнорирует его.

### Изменение в Render():

```cpp
if (layer.hasMask && !layer.mask.empty()) {
    if (layer.maskNeedsUpload) {  // ← добавить эту проверку
        UploadDirtyTiles(cmdList, *activeCache, gpuRes, true, layer.mask, canvasWidth, canvasHeight);
        layer.maskNeedsUpload = false;  // сбросить ПОСЛЕ upload
    }
}
// Убрать layer.maskNeedsUpload = false; из общего места
```

### Часть 2: Tiled dirty tracking для маски

Текущая маска = `std::vector<uint8_t> mask` (flat). Нет per-tile dirty info.

Добавить в `Layer`:
```cpp
std::vector<bool> maskDirtyTiles;  // [tileY * tilesX + tileX] = dirty
```

При `MarkLayerMaskDirty(index)` в Canvas.cpp — установить все `maskDirtyTiles[i] = true`.

При операциях, затрагивающих конкретную область (selection из rect) — установить только dirty для affected tile range.

В `UploadDirtyTiles` для `isMask == true`:
```cpp
for (auto& [tileKey, albedoTile] : gpuRes.albedoTiles) {
    int tx = tileKey & 0xFFFF;
    int ty = tileKey >> 16;
    
    // Проверить dirty flag
    int tilesX = (canvasWidth + TILE_SIZE - 1) / TILE_SIZE;
    int tileIdx = ty * tilesX + tx;
    
    bool isDirty = layer.maskDirtyTiles.empty() ||  // первый upload
                   (tileIdx < (int)layer.maskDirtyTiles.size() && layer.maskDirtyTiles[tileIdx]);
    
    if (!isDirty) continue;  // ← skip если не dirty
    
    // ... processUpload
}

// После upload — сбросить dirty flags
std::fill(layer.maskDirtyTiles.begin(), layer.maskDirtyTiles.end(), false);
```

### Где устанавливать maskDirtyTiles

В `Canvas.cpp`, функции работающие с масками:
- `CreateLayerMask(index)` → установить все тайлы dirty
- `CreateLayerMaskFromSelection(index)` → установить тайлы dirty в range selection
- `ApplyLayerMask(index)` → установить layer dirty (albedo изменился)
- `MarkLayerMaskDirty(index)` → установить все тайлы dirty

Пример для `MarkLayerMaskDirty`:
```cpp
void Canvas::MarkLayerMaskDirty(int index) {
    if (index < 0 || index >= (int)m_Layers.size()) return;
    auto& layer = m_Layers[index];
    layer.maskNeedsUpload = true;
    int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    layer.maskDirtyTiles.assign(tilesX * tilesY, true);
}
```

---

## Важно: Forward reference

`UploadDirtyTiles` принимает `layer.mask` и `layer.maskDirtyTiles` как параметры.
Сигнатура изменится:

```cpp
void UploadDirtyTiles(
    ID3D12GraphicsCommandList* cmdList,
    const TileCache& tileCache,
    LayerGpuResources& gpuRes,
    bool isMask,
    const std::vector<uint8_t>& rawMaskData,
    const std::vector<bool>& maskDirtyTiles,  // ← ДОБАВИТЬ
    int canvasWidth,
    int canvasHeight
);
```

В `.h` обновить декларацию. В `.cpp` обновить определение и все вызовы.

---

## INPUT для агента

**Файлы для чтения:**
- `src/Canvas.h` — структура `Layer` (строки 44-72)
- `src/CanvasRendererDX12.h` — `UploadDirtyTiles` сигнатура (строка 79-87)
- `src/CanvasRendererDX12.cpp` — `UploadDirtyTiles` реализация (строки 339-433)
- `src/CanvasRendererDX12.cpp` — `Render()` секция про слои (строки 164-246)
- `src/Canvas.cpp` — функции `MarkLayerMaskDirty`, `CreateLayerMask`, `CreateLayerMaskFromSelection`

**Файлы для редактирования:**
- `src/Canvas.h` — добавить `maskDirtyTiles` в `Layer`
- `src/Canvas.cpp` — обновить mask-related функции
- `src/CanvasRendererDX12.h` — обновить `UploadDirtyTiles` сигнатуру
- `src/CanvasRendererDX12.cpp` — добавить dirty check в mask upload path

---

## OUTPUT / RETURN

1. `Layer` содержит `std::vector<bool> maskDirtyTiles`
2. `UploadDirtyTiles` принимает `const std::vector<bool>& maskDirtyTiles`
3. Mask tiles upload происходит только если `maskNeedsUpload == true`
4. Mask tiles upload пропускает не-dirty тайлы
5. `MarkLayerMaskDirty` устанавливает all tiles dirty
6. Проект компилируется

---

## Стиль

- `std::vector<bool>` допустим для bitfield (memory-efficient)
- Размер `maskDirtyTiles` = `tilesX * tilesY` при инициализации маски
- Пустой вектор = "все dirty" (first upload)
