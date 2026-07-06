# Задачи: Реконструкция ядра RayV-Paint (16K support)

## Фаза 0 — Lazy Start
- [ ] `Canvas.cpp` / `Canvas.h`: убрать `CreateNewLayer` из `Initialize()`
- [ ] `main.cpp`: убрать `ResizeCanvas(nullptr, ...)` из стартового пути

## Фаза 1+2 — TileCache + CanvasPixelFormat
- [ ] `core/TileCache.h` [NEW]
- [ ] `core/TileCache.cpp` [NEW]
- [ ] `CMakeLists.txt`: добавить TileCache в сборку

## Фаза 3 — Canvas.h (Layer struct)
- [ ] `Canvas.h`: `Layer.pixels` → `unique_ptr<TileCache>`
- [ ] `Canvas.h`: `Layer.mask` → `vector<uint8_t>`, добавить `m_CanvasFormat`
- [ ] `Canvas.h`: `Layer.filteredPixels` → `unique_ptr<TileCache>`

## Фаза 4 — UndoRedoManager
- [ ] `UndoRedoManager.h`: `TileDelta.oldPixels/newPixels` → `vector<uint8_t>`
- [ ] `UndoRedoManager.h`: добавить memory limit

## Фаза 5 — PaintEngine
- [ ] `PaintEngine.h`: сменить сигнатуры `DrawStamp/DrawLine/DrawStrokeSegment`
- [ ] `PaintEngine.cpp`: работа через TileCache

## Фаза 6 — Canvas.cpp (main refactor)
- [ ] `RecreateLayerTexture()` — формат по `m_CanvasFormat`
- [ ] `CreateCompositeResources()` — формат по `m_CanvasFormat`
- [ ] `CreateNewLayer()` — init TileCache
- [ ] `ResizeCanvas()` — resize TileCaches
- [ ] `ComposeLayers()` — per-tile GPU upload
- [ ] `LoadImageToLayer()` — populate TileCache
- [ ] `BackupTile()` — snapshot из TileCache
- [ ] `PaintOnActiveLayer()` — lazy tile alloc
- [ ] Selection/mask operations — uint8_t masks
- [ ] `SaveCanvas()` — export из TileCache

## Фаза 7 — DdsHelper async
- [ ] `DdsHelper.h/.cpp`: ReadHeader() + streaming load into TileCache

## Фаза 8 — ImageManager
- [ ] `ImageManager.cpp`: streaming load into TileCache
