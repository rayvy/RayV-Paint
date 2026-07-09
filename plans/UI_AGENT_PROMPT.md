# Prompt for UI / ImGui Agent

Copy everything below the line into the UI agent.

---

You are the **UI/UX agent** for RayV-Paint (C++ / ImGui / DX11).  
The **core agent** owns `TileCache`, `Canvas` document I/O, export, blend math, undo, masks paint routing.  
**You must not reimplement those.** Wire UI to existing core APIs.

## Read first
1. `plans/core_architecture_handoff.md` — authoritative API + constraints  
2. `src/Canvas.h` — public methods  
3. `src/ui/EditorPanels.cpp` / `EditorPanels.h` — current UI  
4. `src/main.cpp` — frame loop, drop, open, progress hook points  

## Goals (in order)
1. **Instant shell + progress on load**  
   - Main window/ImGui visible ASAP.  
   - Any open (CLI path, drop, menu, recovery) uses `Canvas::OpenDocument(device, path, progressFn)`.  
   - Show modal or footer progress bar from `progressFn(float, stage)`.  
2. **Viewport blend modes already fixed in core** — do not “fix Overlay” in UI; only verify.  
3. **Layer panel**  
   - Groups: tree by `parentGroupId`, create via `CreateLayerGroup`, reparent APIs.  
   - Mask UI: click content vs mask → `SetPaintTarget(LayerContent|LayerMask)`.  
   - Create mask → `CreateLayerMask`.  
   - Optional cheap 48–64px thumbs only if dirty-driven and frame-budgeted (see handoff). If costly, skip.  
4. **Channel UI** — already have R/G/B/A flags; optional small preview from **composite proxy**, never full doc.  
5. **Do not implement:** float16/32 pipeline, smart objects, vectors, 3D, full Substance features. Document only if needed.

## Constraints
- No full-document float buffers.  
- No per-frame 16K readbacks.  
- Call `MarkCompositeDirty()` after layer property changes (opacity, blend, visibility).  
- Paint still goes through `PaintOnActiveLayer` — core routes to mask when target is mask.  
- Save projects with `SaveCanvasRayp` (v2). Export with `SaveCanvasStandard`.  

## Test assets
- `testfield/project.rayp` — 2 layers, blur + Overlay on layer 2.  
- `testfield/_export_project.py` — headless load+export.  
- `testfield/16Ktest.dds` — large open/export stress.  

## Done when
- App feels instant; open shows progress.  
- project.rayp Overlay looks correct in viewport (core fix) and export matches.  
- User can create mask, switch paint target, paint black/white on mask, see composite clipped.  
- Groups appear in layer tree and save/load via .rayp.  
- No new core regressions (`run_tests.bat` smoke if available).  

## Out of scope for you
Rewriting `TileCache`, COW undo, export composite math, HLSL blend modes (unless core explicitly asks for a shader wire-up).
