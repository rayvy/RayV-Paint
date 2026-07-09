# Prompt for UI / Tools Agent (Part 2)

Copy below the line after **Part 1 core is merged**.

---

You are the **UI/Tools agent** for RayV-Paint (C++ / ImGui / DX11).

## Authority
1. **`plans/CORE_UI_CONTRACT.md`** — frozen API table (read first)  
2. `plans/SPLIT_PLAN.md` — Part 2 section + item mapping  
3. `plans/PART1_STATUS.md` / `plans/core_architecture_handoff.md`  
4. `src/Canvas.h` — **only** call public APIs  
5. `src/ui/EditorPanels.*`, `src/main.cpp`, `src/core/KeymapManager.*`

## You must NOT
- Rewrite `TileCache`, COW undo budget, export composite/HLSL blend parity  
- Reimplement flood-fill / quick-select / transform math in UI  
- Full-document float readbacks per frame  
- Free-text ICC file paths (presets only)

## Goals (order)

### 1. Tools & hotkeys
- Lasso tool hotkey **L** via KeymapManager  
- Register **Straight Lasso** (polygonal): click vertices, Enter/double-click close, Esc cancel — core fills polygon  
- Register **Quick Selection** tool: brush stroke → `StrokeQuickSelect` (or final Part 1 name)  
- Magic Wand: if core exposes seed + `PreviewWandTolerance`, wire tolerance slider to live preview; draw seed crosshair  
- **Ctrl+Click** layer → `SelectOpaquePixels(layerIdx)`  
- **Ctrl+A** → core select-all API  
- **Ctrl+X** with active selection → `CropCanvasToSelection` (not “cut pixels to clipboard” unless product says otherwise — follow core)

### 2. Transform gizmo UX
- Working handles: move / scale / rotate  
- Rotate when dragging **outside** the gizmo bounds but gesture is rotate (not when missed handle starts wrong op)  
- Commit/cancel must call core (undo already in core)  
- Do not keep 16K CPU buffers in UI

### 3. Panels
- **Toolbar:** size constraints work when **docked** and floating; icon size from dock width/height; **preserve aspect**  
- **Curves:** mouse on curve editor moves points, **not** the window; title-bar drag only; add **Alpha** channel UI → core Alpha LUT  
- **Canvas Edit** (renamed from Canvas Size): modes Extend | Resize; algorithm Nearest / Bilinear / Lanczos  
- **PNG save dialog:** ICC preset combo only  

### 4. Icons
- Sources: `testfield/svg/` placeholders OK  
- Theme tint: dark UI → white icons; light UI → black; **ignore** native SVG colors  
- Use for toolbar tools where assets exist  

### 5. Smart Object / SVG UX
- Badge on smart/vector layers  
- Context: Rasterize → core `RasterizeLayer`  
- Drop `.svg` already creates smart layer in core — show name/type in list  

### 6. Groups
- Prefer core `MoveLayerIntoGroup` / `ReorderLayer` if present; keep visual drop highlight  
- Do not reintroduce `Layer*` pointer remap after rotate  

## Done when
- Tools reachable; hotkeys work  
- Gizmo usable; transform undoes (core)  
- Curves editable + Alpha  
- Toolbar OK in dock; icons themed  
- Canvas Edit + ICC presets UX complete  
- `testfield/project.rayp` opens; no obvious core regressions  

## Out of scope
Brush dab math, tile COW, flood-fill internals, SVG rasterizer implementation, document geometry undo internals.
