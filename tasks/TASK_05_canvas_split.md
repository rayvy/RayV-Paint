# TASK 05 — Canvas.cpp Split Into Submodules

**Файлы:** `src/Canvas.cpp` [MODIFY/SPLIT], новые файлы [NEW]
**Приоритет:** СРЕДНИЙ (maintainability, агент-friendly)
**Сложность:** Средняя (механический split, не рефактор логики)

---

## Проблема

`src/Canvas.cpp` = 110KB, один файл, все операции смешаны.
AI агенты не могут эффективно работать с файлом >2000 строк.
Каждое изменение требует читать весь файл.

---

## Целевая структура

```
src/
  Canvas.h           (без изменений)
  Canvas.cpp         (минимальный: только Canvas(), ~Canvas(), Initialize(), Shutdown(), Update())
  CanvasLayers.cpp   (Layer CRUD, mask operations, isolation, group management)
  CanvasSelection.cpp (все Selection операции: rect, ellipse, lasso, magic wand, smart select)
  CanvasPaint.cpp    (PaintOnActiveLayer, SmudgeOnActiveLayer, BucketFill, Gradient, MovePixels)
  CanvasAdjustments.cpp (ApplyBlur, ApplyHSV, ApplyCurves, ApplyNoise, InvertAlpha, filters)
  CanvasTransforms.cpp (FlipActiveLayerH/V, RotateCanvas, FlipCanvas, CommitTransformation)
  CanvasFileIO.cpp   (LoadImageToLayer, SaveCanvas*, SaveCanvasRayp*, LoadCanvasRayp, GetCompositePixels)
  CanvasUndo.cpp     (Undo, Redo, ClearUndoHistory, BackupTile, UndoRedo helpers)
```

---

## Правила split

1. **Каждый `.cpp` начинается с** `#include "Canvas.h"` + нужные std заголовки
2. **Приватные методы Canvas** остаются в том файле где они используются
   - `RebuildFilteredPixels` → `CanvasAdjustments.cpp`
   - `BackupTile` → `CanvasUndo.cpp`
   - `MarkCompositeResourcesDirty` → `Canvas.cpp` (вызывается везде)
   - `ExtractAndSetICCProfile` → `CanvasFileIO.cpp`
3. **Никаких новых заголовков** — все методы объявлены в `Canvas.h`, только `.cpp` разбиваются
4. **Порядок include в каждом файле:**
   ```cpp
   #include "Canvas.h"
   // --- STL ---
   #include <vector>
   #include <string>
   // --- Project ---
   #include "core/Logger.h"
   #include "core/TileCache.h"
   // ... etc
   ```

---

## Что идёт в каждый файл

### Canvas.cpp (core)
- `Canvas::Canvas()` — конструктор
- `Canvas::~Canvas()` — деструктор
- `Canvas::Initialize()`
- `Canvas::Shutdown()`
- `Canvas::Update()`
- `Canvas::ResetView()`
- `Canvas::ResizeCanvas()`
- `Canvas::MarkCompositeResourcesDirty()`
- `Canvas::GetComposedPixels()` (простая версия если есть)

### CanvasLayers.cpp
- `CreateNewLayer`
- `DeleteLayer`
- `SetActiveLayerIndex`
- `ToggleLayerIsolation`
- `IsLayerIsolated`
- `CreateLayerMask`
- `CreateLayerMaskFromSelection`
- `DeleteLayerMask`
- `ApplyLayerMask`
- `MarkLayerMaskDirty`
- `CreateLayerGroup`
- `AddLayerToGroup`
- `RemoveLayerFromGroup`
- `CreateLayerFromPixels`

### CanvasSelection.cpp
- `SelectAll`
- `ClearSelection`
- `InvertSelection`
- `SetSelectionMask`
- `MarkSelectionMaskDirty`
- `ApplyRectSelection`
- `ApplyEllipseSelection`
- `ApplyLassoSelection`
- `ApplyMagicWandSelection`
- `ApplySmartSelectSelection`

### CanvasPaint.cpp
- `PaintOnActiveLayer`
- `SmudgeOnActiveLayer`
- `ApplyBucketFill`
- `ApplyGradient`
- `StartMovePixels`
- `UpdateMovePixels`
- `CommitMovePixels`
- `CancelMovePixels`
- `DrawMoveGizmo`

### CanvasAdjustments.cpp
- `ApplyBlur`
- `ApplyHSV`
- `ApplyCurves`
- `ApplyNoise`
- `InvertAlpha`
- `RebuildFilteredPixels` (приватный)

### CanvasTransforms.cpp
- `FlipActiveLayerHorizontal`
- `FlipActiveLayerVertical`
- `RotateCanvas90`
- `FlipCanvasHorizontal`
- `FlipCanvasVertical`
- `CommitTransformation`

### CanvasFileIO.cpp
- `LoadImageToLayer`
- `SaveCanvas`
- `SaveCanvasStandard`
- `SaveCanvasCompressed`
- `GetCompositePixels`
- `SaveCanvasRayp`
- `SaveCanvasRaypAsync`
- `LoadCanvasRayp`
- `SaveProjectAuto`
- `ExtractAndSetICCProfile` (приватный)

### CanvasUndo.cpp
- `Undo`
- `Redo`
- `CanUndo`
- `CanRedo`
- `GetUndoName`
- `GetRedoName`
- `ClearUndoHistory`
- `BackupTile` (приватный)

---

## CMakeLists.txt изменения

```cmake
set(SOURCES
    src/main.cpp
    src/Canvas.cpp
    src/Canvas.h
    src/CanvasLayers.cpp       # NEW
    src/CanvasSelection.cpp    # NEW
    src/CanvasPaint.cpp        # NEW
    src/CanvasAdjustments.cpp  # NEW
    src/CanvasTransforms.cpp   # NEW
    src/CanvasFileIO.cpp       # NEW
    src/CanvasUndo.cpp         # NEW
    src/CanvasRendererDX12.cpp
    src/CanvasRendererDX12.h
    # ... остальное без изменений
)
```

---

## INPUT для агента

**Файлы для чтения:**
- `src/Canvas.h` (полностью — чтобы знать все методы)
- `src/Canvas.cpp` (полностью — искать функции по имени из списков выше)
- `CMakeLists.txt` строки 78-108 (SOURCES list)

**Файлы для создания:**
- `src/CanvasLayers.cpp`
- `src/CanvasSelection.cpp`
- `src/CanvasPaint.cpp`
- `src/CanvasAdjustments.cpp`
- `src/CanvasTransforms.cpp`
- `src/CanvasFileIO.cpp`
- `src/CanvasUndo.cpp`

**Файлы для редактирования:**
- `src/Canvas.cpp` — удалить перенесённые функции, оставить только core
- `CMakeLists.txt` — добавить новые .cpp в SOURCES

---

## OUTPUT / RETURN

1. Все 7 новых `.cpp` файлов созданы
2. Каждый `.cpp` содержит только методы из соответствующего списка выше
3. `Canvas.cpp` содержит только core методы
4. `CMakeLists.txt` обновлён
5. Проект компилируется без ошибок (нет duplicate symbol, нет missing symbol)

---

## Важные предупреждения для агента

- Не изменять `Canvas.h` — только `.cpp` файлы
- Не создавать новых классов или функций
- Не переименовывать методы
- Это **механический split** — логика не меняется
- Если метод `A` вызывает приватный метод `B`, и они в разных `.cpp` → `B` остаётся в том `.cpp` где вызывается. Если `B` вызывается из нескольких файлов → оставить в `Canvas.cpp`
- `MarkCompositeResourcesDirty` вызывается из многих мест → остаётся в `Canvas.cpp`

---

## Стиль

- Нет `using namespace std` в `.cpp` файлах
- Нет глобальных переменных вне Canvas
- Комментарии не трогать (сохранять существующие)
