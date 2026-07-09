# RayV-Paint Core Architecture Handoff (for UI / product agent)

**Audience:** another AI agent implementing UI/UX only.  
**Owner of this document:** core agent. Do not reinvent tile storage, export, or blend math — call these APIs.

---

## What the core already guarantees

| Area | Status |
|------|--------|
| Sparse tiles (`TileCache`, 256², COW shared `TileData`) | Done |
| 16K open (DDS stream decode) | Done |
| Streamed-ish PNG export (RGBA8 composite, filters + blend modes) | Done |
| `.rayp` v2 (blend, filters, mask, groups meta) | Done |
| `.rayp` open routing (`OpenDocument`) | Done |
| Undo tile COW + global unique budget | Done |
| **Viewport blend modes** (history texture, no RT/SRV conflict) | Done (core) |
| Paint target: layer content vs **layer mask** | Core API ready |
| Load **progress callback** | Core API ready |
| Layer groups (create/parent, save/load) | Partial core |
| Layer / channel thumbnails | **Not in core yet** — see “cheap thumbs” |
| float16/32 document mode | Partial (format enum exists; not full pipeline) |
| Smart objects / vectors / 3D | Design only — do not implement in UI without core design |

---

## Critical APIs for UI

### Document open (use this, not raw image load)
```cpp
bool Canvas::OpenDocument(ID3D11Device* device, const std::string& path,
                          LoadProgressFn progress = nullptr);
// progress(float 0..1, const char* stage);
// stages: "start","open","decode_dds","decoded","metadata","layer","gpu_upload","finalize","done","error"
```

### Project
```cpp
bool SaveCanvasRayp(path);           // v2
bool LoadCanvasRayp(path, device, progress);
bool SaveCanvasStandard(path, icc);  // PNG/JPG with filters + blend modes
```

### Layers / groups
```cpp
CreateNewLayer / DeleteLayer / SetActiveLayerIndex
CreateLayerGroup / AddLayerToGroup / RemoveLayerFromGroup
Layer::blendMode, opacity, visible, filters, hasMask, isGroup, parentGroupId
MarkCompositeDirty() after any property change that affects view
```

### Masks (Photoshop model)
```cpp
CreateLayerMask(device, layerIndex);           // white mask, switches paint target to mask
CreateLayerMaskFromSelection(...)
DeleteLayerMask / ApplyLayerMask
SetPaintTarget(PaintTarget::LayerContent | LayerMask)
GetPaintTarget() / IsEditingLayerMask() / ActiveLayerHasMask()
PaintOnActiveLayer(...)  // already routes to mask when target=Mask
```
- White = reveal, black = hide.
- Eraser on mask paints black.
- Default mask brush when painting mask is forced white in core.

### Paint
```cpp
PaintOnActiveLayer(x, y, StrokePhase, BrushSettings)
// When mask target: do not expect color channels; core writes grayscale mask.
```

---

## UI work expected from the other agent (do not reimplement core)

### 1) Fast-start illusion
- Show main window **immediately** after D3D/ImGui init.
- Defer `OpenDocument` / drop / recovery to a short idle frame **or** background with progress.
- Modal or status **progress bar** bound to `LoadProgressFn`.
- Never block first paint for texture decode.

### 2) Layer panel: thumbnails (only if cheap)
**Allowed (cheap):**
- On layer dirty flag, render **64×64** (or 48×48) via:
  - `CopySubresourceRegion` / downsample from existing layer GPU texture, **or**
  - one-time CPU downsample from a few tiles (not full 16K).
- Cache thumbnails; regenerate only when `needsUpload` / mask / filter dirty.
- Cap: max N thumbs per frame (e.g. 2–4).

**Forbidden:**
- Full-res readback every frame.
- Second full composite at document resolution for thumbs.

If unsure cost — **skip thumbs**, show solid color / icon.

### 3) Channel previews
- Reuse existing channel solo flags (`Get/SetChannelR/G/B/A`).
- Optional small preview strip: sample **composite proxy** (already ≤2048), not 16K document.

### 4) Mask UI
- Layer row: thumbnail | mask thumbnail | visibility.
- Click content thumbnail → `SetPaintTarget(LayerContent)`.
- Click mask thumbnail → `SetPaintTarget(LayerMask)` (create mask if missing via core).
- When editing mask, optional viewport overlay: grayscale mask (sample `maskSRV`) — **UI only**, core already modulates composite via mask in GPU path.

### 5) Groups UI
- Tree indentation by `parentGroupId`.
- Create group button → `CreateLayerGroup`.
- Drag reparent → `AddLayerToGroup` / `RemoveLayerFromGroup`.
- Collapse: `groupExpanded` (display only).

### 6) Do not touch unless core asks
- Tile COW, swap, export math, blend HLSL (except if core adds new modes).
- Full float16/32 pipeline.
- Smart objects / vectors / 3D meshes.

---

## Architecture notes (for future, not now)

### Float16 / Float32 documents
- `CanvasPixelFormat::RGBA32F` exists; DDS can load float.
- Full float documents will need: GPU format, export path, mask tiling, higher VRAM.
- **Do not** force float for every project; keep 8-bit default.

### Smart objects / vectors (future Substance-like)
- Treat as **external document reference** + transform + optional raster cache tile set.
- Same as “smart object”: source identity + affine + dirty → re-rasterize into tiles.
- Vectors: path data + style → rasterize to tiles at needed LOD.
- 3D: later, mesh + material graph; paint still lands in 2D tile maps (UDIM-like).

### Performance invariants (do not break)
1. Never allocate full float `W×H×4` for large docs.
2. Composite RT is **proxy-sized** (≤2048), not 16K.
3. Dirty tile upload only.
4. Undo shares `TileData` (COW).
5. Blend modes sample **history texture**, not the live RT.

---

## Known gaps left for core later
- Tiled masks (still flat `vector<u8>` — expensive on 16K).
- Mask undo history (mask paint not yet tile-undo).
- Group isolated composite path for nested blend.
- True strip PNG encoder (peak RAM still ~1 GiB @ 16K RGBA8).
- Layer thumbnail generation helper in core (optional).

---

## Verify after UI work
```bat
# project with blur + overlay
RayVPaint.exe --headless --script testfield/_export_project.py
# expect: rebuilt filters, blend=3, PNG written

# open rayp
RayVPaint.exe --headless --script testfield/_load_rayp.py
```

Manual: open project.rayp → Overlay layer must **not** be black in viewport → export matches view.
