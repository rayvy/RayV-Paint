# RayV-Paint — Split Plan (Core vs UI Agent)

Дата: 2026-07-09  
Ветка: `DX11-upgrade`  
После ревью коммитов: `3fce3bb` (core handoff) + `8b1c200` (UI agent)

---

## Принцип разделения

| Part 1 — CORE (этот агент) | Part 2 — UI / tools agent (следующий) |
|---|---|
| Документ, пиксели, тайлы, undo, I/O, алгоритмы | ImGui, хоткеи, панели, гизмо-хиты, иконки, UX |
| API в `Canvas.h` / core modules | Только вызовы публичных API + layout |
| Без ImGui-зависимостей в core | Без переписывания TileCache / COW / export blend |

**Критические баги ядра идут в Part 1 даже если симптом «в UI».**  
Пример: groups DnD ломал `parentGroupId` после reorder — это integrity document model → Part 1 (hotfix уже в `EditorPanels`, но model rules остаются core).

---

# PART 1 — CORE (исполняет текущий агент)

Порядок = приоритет. Не начинать Part 2, пока не закрыты блоки **A–D**.

## A. Integrity / regression fixes (сначала)

### A1. Groups — document model + DnD (hotfix)
**Симптом:** drag слоя в группу не виден / parent ломается.  
**Причина:** `ReorderLayer` хранил `Layer*` и после `std::rotate` адреса невалидны; drop-target только на крошечной кнопке «G»; нет preview.  
**Статус:** fix в `EditorPanels.cpp` (stable identity remap + highlight + drop-into-group).  
**Остаток core:**
- [ ] `Canvas::MoveLayer` / `ReorderLayers` API с корректным remap `parentGroupId` (убрать дублирование логики из UI)
- [ ] Запрет циклов parent; опционально nested groups later
- [ ] Group composite: `m_GroupComposite*` зарезервирован, но `isGroup` сейчас `continue` в composite — дети рисуются плоско. Либо документировать «groups = folder only», либо реальный pass (opacity/blend group)

### A2. Operator undo (Blur / HSV / Curves / Noise / InvertAlpha…)
**Симптом:** Apply Blur → Undo не откатывает.  
**Причина:** `ApplyBlur` / `ApplyCurves` / … делают `ExportLayerF` → mutate → `SetLayerPixelsF` **без** `PushCommand` / tile snapshots.  
**Делать:**
- [ ] Общий helper `BeginLayerMutation()` → snapshot touched tiles (или full-layer tile deltas) → mutate → `EndLayerMutation("Blur")` → `PaintStrokeCommand`
- [ ] Все destructive operators через helper
- [ ] Не аллоцировать full-doc float на 16K без необходимости: предпочтительно tile-walk + dirty rect / selection bounds

### A3. Partial-tile undo after operators
**Симптом:** undo меняет только часть тайлов.  
**Причины-кандидаты:**
1. Snapshot только dirty/touched subset, а mutate пишет шире
2. `SetLayerPixelsF` / Import не помечает dirty на всех изменённых тайлах
3. Undo restore без `needsUpload` / composite dirty
4. COW: shared `TileData` restore не unique-copy

**Делать:**
- [ ] Аудит `PaintStrokeCommand::Undo/Redo` + `TileCache::RestoreSnapshot`
- [ ] Operators: delta list = **все** тайлы, пересекающие selection/bounds, до и после
- [ ] Тест: blur full canvas + selection rect + undo → pixel-identical

### A4. Transform commit → undo
**Симптом:** transform/move не в history.  
**Сейчас:** `CommitTransformation` есть; flips его зовут; floating move/transform path — нет полного snapshot path.  
**Делать:**
- [ ] `BeginMove/Transform`: snapshot source tiles + cut-out
- [ ] `CommitMovePixels` / accept transform: write back + `PushCommand("Transform")` с before/after tile deltas
- [ ] Cancel: restore snapshots, no command
- [ ] Не держать full `m_FloatingPixels` на 16K — bbox selection + proxy preview (см. A5)

### A5. Transform performance (4K+)
**Симптом:** лаг на 4K+.  
**Делать:**
- [ ] Floating buffer = selection AABB (padded), не весь слой
- [ ] Preview: GPU texture transform (уже есть scale/rot в CB) — CPU resample только на Commit
- [ ] LOD proxy при interactive drag (½/¼ res), full quality on release
- [ ] Dirty только AABB tiles

### A6. UTF-8 paths (open/save/export/drop)
**Симптом:** кириллица/CJK в пути → fail.  
**Сейчас:** `UTF8ToWString` точечно в .rayp / части PNG; `filesystem::path` / STB / DDS / dialogs — дыры.  
**Делать:**
- [ ] Единый `PathUtil::OpenRead/OpenWrite` (Windows: `_wfopen` / `CreateFileW`)
- [ ] STB load via memory after wide open
- [ ] DDS/DirectXTex wide paths
- [ ] `std::filesystem::u8path` (или u8string) везде вместо implicit narrow
- [ ] Тест: `testfield/юникод/тест.png` open+save

### A7. PNG ICC — presets only
- [ ] Убрать free-text ICC path из export UX contract
- [ ] Core: enum `IccPreset { sRGB, DisplayP3, AdobeRGB, None }` + встроенные профили (или pack resources)
- [ ] `SaveCanvasStandard(path, IccPreset)` 

---

## B. Selection / tools algorithms (core)

### B1. Magic Wand — sticky sample + live tolerance
- [ ] State: `m_WandSeedX/Y`, `m_WandSeedValid`, seed color (Lab/RGB)
- [ ] Click sets seed; changing tolerance **re-runs** from same seed without new click
- [ ] API: `SetWandSeed`, `PreviewWandTolerance(tol)`, `CommitWandSelection`
- [ ] UI agent only binds slider → preview

### B2. Contiguous flood — early exit / correct domain
- [ ] OpenCV `floodFill` already stops at color boundary — if slow, cause is full-doc `ExportLayerF` + Mat copy every time
- [ ] Optimize: export **active layer tiles** only; optional ROI grow; reuse seed Mat while tolerance scrubbing
- [ ] Contiguous: floodFill mask only (already FLOODFILL_MASK_ONLY) — ensure no full-scene second pass
- [ ] Non-contiguous: vectorized / OpenCV `inRange` in Lab, tile parallel (ThreadPool)

### B3. Quick Selection Tool (non-AI)
Принцип пользователя: brush seeds → adaptive color/texture model → constrained region grow (Lab + texture + edge map Sobel/Scharr) → sticky edges → model update per stroke → light morpho smooth.  
**Core:**
- [ ] `QuickSelectSession`: seed mask, Lab mean/cov or histogram, edge map cache per layer revision
- [ ] `StrokeQuickSelect(points, radius, mode add/sub)`
- [ ] Grow limited to brush dilated region + margin (interactive)
- [ ] Output → `m_SelectionMask` + SelectionCommand undo
- [ ] Edge map: rebuild on layer content change, cache grayscale Sobel magnitude
**Не AI, не ONNX.**

### B4. Straight Lasso (Polygonal Lasso)
- [ ] Core: polyline points → fill polygon → selection mask (OpenCV `fillPoly`)
- [ ] Click-add vertex; double-click / Enter close; Esc cancel
- [ ] Shared with freehand lasso finalize path

### B5. Ctrl+Click layer / Ctrl+A
- [ ] `SelectOpaquePixels(layerIdx)` — alpha > threshold → mask
- [ ] `SelectAllCanvas()` / `SelectAllLayerBounds()`
- [ ] Undo via SelectionCommand

### B6. Crop operator (Ctrl+X on selection = crop canvas to selection bounds)
- [ ] `CropCanvasToSelection()` or `CropCanvas(rect)` 
- [ ] Equivalent to Extend-to-bounds but shrink: rewrite all layers tiles, selection, document size
- [ ] Full document undo command (not single-layer) — new `DocumentGeometryCommand` or snapshot meta+tiles
- [ ] Register in undo stack

### B7. Canvas Edit (rename from Canvas Size)
- [ ] Mode **Extend** (current): pad/crop edges, content unscaled
- [ ] Mode **Resize**: scale all layer pixels to new W×H
- [ ] Filters: Nearest, Bilinear, Lanczos (OpenCV `INTER_NEAREST/LINEAR/LANCZOS4`; optional sharp bilinear)
- [ ] Undo: geometry command
- [ ] API: `EditCanvas(mode, w, h, anchor, resample)`

---

## C. Brush system (core)

### C1. Krita-like brush optimization
- [ ] Stamp only dirty tiles; spacing by effective radius
- [ ] Dabs in tile-local space; avoid full-layer locks
- [ ] Soft brush: LUT falloff, fewer per-pixel branches
- [ ] Optional stamp atlas / cached soft kernel per size bucket
- [ ] Smudge: smaller neighborhood, tile-aware

### C2. Custom brush tips (presets, no regression)
- [ ] `BrushTip` resource: grayscale stamp, spacing, hardness, flow, size jitter hooks
- [ ] Built-in presets: Soft Round, Hard Round, Pencil, Airbrush (2–4 max v1)
- [ ] Paint path: if tip==null → current procedural circle (bit-identical path preserved)
- [ ] Serialize tip id in stroke settings only; not break existing .rayp

---

## D. Layer types foundation (SVG / smart object) — core half

### D1. Layer type enum
- [ ] `LayerType { Raster, Group, SmartObject, VectorSvg }` (Group already `isGroup`)
- [ ] Raster path 100% unchanged when type==Raster

### D2. SVG import as Smart Object
- [ ] Parse/render SVG → raster cache at document PPI (nanosvg or existing dep)
- [ ] Store source bytes + transform in layer meta (.rayp v3 field)
- [ ] Display via rasterized tileCache; edit = re-raster on transform scale
- [ ] `RasterizeLayer(idx)` → type Raster, bake pixels, drop source

### D3. Drop .svg
- [ ] `OpenDocument` / drop: extension `.svg` → create SmartObject layer (not flat accidental decode)

**UI icons theming / toolbar** — Part 2, but core may expose `LoadSvgIcon` helper if shared.

---

## E. Explicitly NOT in Part 1 (leave for Part 2)

- Toolbar dock min/max constraints (ImGui docking)
- Toolbar icon aspect fit
- Curves panel click-steal (ImGui item flags / NoMove)
- Curves Alpha channel UI + wiring (core LUT may need A support in ApplyCurves — small core note below)
- Hotkey L = Lasso, tool registration for Straight Lasso / Quick Select buttons
- SVG icon theming (white/black by theme)
- Transform **gizmo hit-testing** and «drag outside gizmo but in viewport rotates» feel
- PNG export dialog preset combo UI
- Channels panel polish

**Core note for Curves Alpha:** `ApplyCurves` currently samples RGB only — Part 1 should add optional alpha LUT channel in the operator so Part 2 can expose it.

---

# PART 2 — UI / Tools Agent (после Part 1)

Скопировать в промпт UI-агента. **Читать:** `Canvas.h`, этот файл, `plans/core_architecture_handoff.md`.

## P2-0. Constraints
- Не трогать TileCache COW, export blend HLSL parity, undo budget logic.
- Только публичные API из Part 1.
- Groups DnD: если core API `MoveLayerIntoGroup` появится — переключить UI на него.

## P2-1. Tools / hotkeys
1. Lasso → hotkey **L** (KeymapManager)
2. **Straight Lasso** tool entry + cursor; click-to-add, Enter/dblclick close (core polyline API)
3. **Quick Selection** tool button + brush size UI; call `StrokeQuickSelect`
4. Wand: tolerance slider live-calls `PreviewWandTolerance` when seed valid; show seed marker in viewport
5. Transform gizmo:
   - Hit handles (corners scale, edge scale, outside=rotate, inside=move)
   - Fix «drag outside gizmo rotates» only when gesture started correctly (no accidental rotate)
   - On commit call core `CommitTransform`
6. Ctrl+Click layer thumbnail → `SelectOpaquePixels`
7. Ctrl+A → select all / opaque per product decision from core API

## P2-2. Panels / chrome
1. **Toolbar dock:** `SetNextWindowSizeConstraints` must apply when docked too (use dock node size / `ImGuiWindowFlags`; adaptive `btnSize` from dock width/height; icons keep aspect)
2. **Curves panel:** `ImGui::InvisibleButton` / child with `NoMove` so curve handles capture mouse; window drag only from title bar; add **Alpha** channel curve → pass A LUT to core
3. **Canvas Edit** dialog: rename from Canvas Size; mode Extend | Resize; algorithm combo; call core API
4. **PNG save:** ICC preset dropdown only (no path textbox)
5. Progress bar polish if needed (already exists)

## P2-3. Icons / SVG chrome
1. Load icons from `testfield/svg/` (and later `assets/icons/`)
2. Theme: dark theme → white icons; light → black (tint in shader or CPU multiply; **ignore native SVG fill colors**)
3. Placeholders OK if missing asset
4. Toolbar / tool buttons use SVG not text where available

## P2-4. Smart object / SVG UX
1. Drop .svg → layer shows smart-object badge
2. Context menu: Rasterize
3. Double-click behavior: optional later (edit source) — stub OK

## P2-5. Done criteria (Part 2)
- [ ] All tools reachable from toolbar + hotkeys
- [ ] Gizmo usable; transform undoes
- [ ] Curves draggable; Alpha channel present
- [ ] Toolbar resizes in dock mode; icons proportional
- [ ] Icons theme-correct
- [ ] Canvas Edit Extend/Resize UX complete
- [ ] No core regressions (`run_tests.bat` / open project.rayp / export)

---

# Mapping user items 1–19

| # | Item | Part |
|---|------|------|
| 1 | Brush optimization (Krita-like) | **1 C1** |
| 2 | Custom brushes + presets | **1 C2** (+ UI picker Part 2) |
| 3 | Transform undo | **1 A4** |
| 3.1 | Gizmo broken | **2 P2-1** |
| 3.2 | Transform lag 4K+ | **1 A5** |
| 3.3 | Outside gizmo rotates | **2 P2-1** (+ core gesture flags if needed) |
| 4 | Wand sticky sample + live tol | **1 B1** + UI bind Part 2 |
| 4.1 | Contiguous faster | **1 B2** |
| 5 | Quick Selection | **1 B3** + UI Part 2 |
| 6 | Lasso → L | **2** |
| 6.1 | Straight Lasso | **1 B4** + UI Part 2 |
| 7 | Vector/SVG smart objects | **1 D** + UI Part 2 |
| 8 | Ctrl+Click layer select | **1 B5** + UI Part 2 |
| 9 | Ctrl+A | **1 B5** + UI Part 2 |
| 10 | SVG icons | **2 P2-3** |
| 11 | Icon theme tint | **2 P2-3** |
| 12 | Canvas Edit Extend/Resize | **1 B7** + dialog Part 2 |
| 13 | Crop operator Ctrl+X | **1 B6** + keybind Part 2 |
| 14 | Toolbar dock min/max + icon aspect | **2 P2-2** |
| 15 | Curves drag + Alpha | **2** (+ core Alpha LUT **1**) |
| 16 | Operators not in undo | **1 A2** |
| 17 | Partial tile undo | **1 A3** |
| 18 | PNG ICC presets | **1 A7** + dialog Part 2 |
| 19 | UTF-8 paths | **1 A6** |
| — | Groups DnD | **1 A1** (hotfix done; API cleanup remains) |

---

# Suggested execution order (Part 1 only)

```
A2/A3 operator undo integrity  →  A4/A5 transform undo+perf
    → A6 UTF-8  → A7 ICC presets
    → B1/B2 wand  → B5 select helpers  → B6 crop  → B7 canvas edit
    → B4 straight lasso  → B3 quick select
    → C1 brush perf  → C2 brush tips
    → D layer types + SVG smart object
```

После merge Part 1: отдать UI-агенту секцию **PART 2** + актуальный `Canvas.h`.
