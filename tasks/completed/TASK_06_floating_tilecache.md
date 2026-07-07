# TASK 06 — Floating Pixels: std::vector<float> → TileCache

**Файлы:** `src/Canvas.h`, `src/Canvas.cpp` (CanvasPaint.cpp после split)
**Приоритет:** ВЫСОКИЙ (OOM при 8K, зависание при Move Pixels)
**Сложность:** Высокая

---

## Проблема

В `Canvas.h`:
```cpp
// Move Pixels State
bool m_IsMovingPixels = false;
std::vector<float> m_FloatingPixels;      // ← ПРОБЛЕМА
std::vector<uint8_t> m_OriginalSelectionMask;
int m_FloatingOffsetX = 0;
int m_FloatingOffsetY = 0;
```

**Расчёт памяти:**
- 8K × 8K canvas = 8192 × 8192 × 4 float = **1 Гигабайт** per floating layer
- 16K × 16K = **4 Гигабайта** → OOM guaranteed

При `StartMovePixels()` код делает:
```cpp
// Предположительно что-то вроде:
m_FloatingPixels = ExportLayerF(activeLayer);  // Весь слой в RAM как float
```

---

## Целевая архитектура

Заменить `m_FloatingPixels: std::vector<float>` на `m_FloatingTileCache: std::unique_ptr<TileCache>`.

### Изменения в Canvas.h

```cpp
// УБРАТЬ:
std::vector<float> m_FloatingPixels;

// ДОБАВИТЬ:
std::unique_ptr<TileCache> m_FloatingTileCache;
```

### Изменения в StartMovePixels()

**Было (примерно):**
```cpp
void Canvas::StartMovePixels() {
    if (m_ActiveLayerIdx < 0) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    
    // Экспортировать в flat float vector
    m_FloatingPixels.resize(m_Width * m_Height * 4);
    // ... копирование пикселей
    
    // Очистить слой в зоне selection
    // ...
}
```

**Должно стать:**
```cpp
void Canvas::StartMovePixels() {
    if (m_ActiveLayerIdx < 0) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.tileCache) return;
    
    m_StartActiveLayerIdx = m_ActiveLayerIdx;
    
    // Создать floating TileCache с тем же форматом
    m_FloatingTileCache = std::make_unique<TileCache>();
    m_FloatingTileCache->Init(m_Width, m_Height, layer.tileCache->GetFormat());
    
    // Если есть selection — копировать только выделенные пиксели
    if (m_HasSelection) {
        // Итерировать по тайлам, копировать только где маска > 0
        int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
        int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
        
        for (int ty = 0; ty < tilesY; ++ty) {
            for (int tx = 0; tx < tilesX; ++tx) {
                const uint8_t* srcTile = layer.tileCache->GetTileData(tx, ty);
                if (!srcTile) continue;
                
                // Проверить есть ли selection в этом тайле
                bool hasMaskInTile = false;
                for (int py = 0; py < TILE_SIZE && !hasMaskInTile; ++py) {
                    int canvasY = ty * TILE_SIZE + py;
                    if (canvasY >= m_Height) break;
                    for (int px = 0; px < TILE_SIZE; ++px) {
                        int canvasX = tx * TILE_SIZE + px;
                        if (canvasX >= m_Width) break;
                        if (m_SelectionMask[canvasY * m_Width + canvasX] > 0) {
                            hasMaskInTile = true;
                            break;
                        }
                    }
                }
                if (!hasMaskInTile) continue;
                
                // Копировать тайл в floating cache
                uint8_t* dstTile = m_FloatingTileCache->LockTile(tx, ty);
                std::memcpy(dstTile, srcTile, TILE_SIZE * TILE_SIZE * layer.tileCache->GetBytesPerPixel());
                
                // Очистить источник в зоне selection (cut, не copy)
                uint8_t* srcMutable = layer.tileCache->LockTile(tx, ty);
                for (int py = 0; py < TILE_SIZE; ++py) {
                    int canvasY = ty * TILE_SIZE + py;
                    if (canvasY >= m_Height) break;
                    for (int px = 0; px < TILE_SIZE; ++px) {
                        int canvasX = tx * TILE_SIZE + px;
                        if (canvasX >= m_Width) break;
                        if (m_SelectionMask[canvasY * m_Width + canvasX] > 0) {
                            int bpp = layer.tileCache->GetBytesPerPixel();
                            int pixOff = (py * TILE_SIZE + px) * bpp;
                            std::memset(srcMutable + pixOff, 0, bpp);
                        }
                    }
                }
                layer.tileCache->MarkDirty(tx, ty);
            }
        }
    } else {
        // Нет selection — floating = весь слой (только занятые тайлы)
        // CopyFrom только копирует существующие тайлы — sparse!
        m_FloatingTileCache->CopyFrom(*layer.tileCache, 0, 0, 0, 0, m_Width, m_Height);
        layer.tileCache->Clear();
    }
    
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_IsMovingPixels = true;
    
    // Сохранить маску
    m_OriginalSelectionMask = m_SelectionMask;
}
```

### UpdateMovePixels() — без изменений по логике

Только обновляет `m_FloatingOffsetX/Y`.

### CommitMovePixels()

**Было (примерно):** итерировать по `m_FloatingPixels` float array

**Должно стать:** использовать `TileCache::CopyFrom` со смещением:

```cpp
void Canvas::CommitMovePixels() {
    if (!m_IsMovingPixels || !m_FloatingTileCache) return;
    
    auto& layer = m_Layers[m_StartActiveLayerIdx];
    if (!layer.tileCache) return;
    
    // CopyFrom поддерживает dst offset
    layer.tileCache->CopyFrom(
        *m_FloatingTileCache,
        0, 0,                          // srcX, srcY
        m_FloatingOffsetX,             // dstX
        m_FloatingOffsetY,             // dstY
        m_Width, m_Height
    );
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    
    m_FloatingTileCache.reset();
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_IsMovingPixels = false;
    ClearSelection();
    MarkCompositeDirty();
}
```

### CancelMovePixels()

```cpp
void Canvas::CancelMovePixels() {
    if (!m_IsMovingPixels || !m_FloatingTileCache) return;
    
    auto& layer = m_Layers[m_StartActiveLayerIdx];
    if (layer.tileCache) {
        // Restore original pixels from floating (without offset)
        layer.tileCache->CopyFrom(*m_FloatingTileCache, 0, 0, 0, 0, m_Width, m_Height);
        layer.tileCache->MarkAllDirty();
        layer.needsUpload = true;
    }
    
    m_FloatingTileCache.reset();
    m_IsMovingPixels = false;
    m_SelectionMask = m_OriginalSelectionMask;
    MarkCompositeDirty();
}
```

---

## Rendering floating pixels

В `CanvasRendererDX12::Render()` нужно рендерить floating тайлы со смещением.

Текущий код проверяет `isFloating` в `LayerBufferData`. Нужно добавить:
- Floating TileCache в `CanvasRendererDX12` как отдельный GPU layer
- При рендере если `canvas.IsMovingPixels()` → загрузить `m_FloatingTileCache` тайлы
- Применить `m_FloatingOffsetX/Y` как translation в LayerBuffer

Для этого нужно добавить в Canvas публичный accessor:
```cpp
const TileCache* GetFloatingTileCache() const { return m_FloatingTileCache.get(); }
int GetFloatingOffsetX() const { return m_FloatingOffsetX; }
int GetFloatingOffsetY() const { return m_FloatingOffsetY; }
```

---

## INPUT для агента

**Файлы для чтения:**
- `src/Canvas.h` — структура `Layer`, приватные поля Move Pixels (строки 337-346)
- `src/Canvas.cpp` — функции `StartMovePixels`, `UpdateMovePixels`, `CommitMovePixels`, `CancelMovePixels` (найти по имени)
- `src/core/TileCache.h` — методы `CopyFrom`, `LockTile`, `GetTileData`, `MarkAllDirty` (строки 83-106)
- `src/CanvasRendererDX12.cpp` — секция рендера floating pixels (поиск по `isFloating`)

**Файлы для редактирования:**
- `src/Canvas.h`
- `src/Canvas.cpp` (или `src/CanvasPaint.cpp` после TASK_05)
- `src/CanvasRendererDX12.cpp` — обновить floating render path

---

## OUTPUT / RETURN

1. `m_FloatingPixels: std::vector<float>` удалён из Canvas.h
2. `m_FloatingTileCache: std::unique_ptr<TileCache>` добавлен в Canvas.h
3. `StartMovePixels()` создаёт TileCache вместо vector
4. `CommitMovePixels()` использует `TileCache::CopyFrom` с offset
5. `CancelMovePixels()` восстанавливает через `TileCache::CopyFrom` без offset
6. Публичные accessors `GetFloatingTileCache()`, `GetFloatingOffsetX/Y()` добавлены
7. `CanvasRendererDX12` рендерит floating через GPU tile upload
8. Проект компилируется

---

## Стиль

- Никаких плоских `std::vector<float>` для пиксельных данных
- Все tile операции через TileCache API
- `m_FloatingTileCache.reset()` в Commit/Cancel перед return
- Sparse: только занятые тайлы хранятся в TileCache
