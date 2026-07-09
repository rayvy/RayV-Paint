# Part 1 Core — Implementation Status

Date: 2026-07-09  
Build: `RayVPaint_Core` Release — **OK**

## Completed

| ID | Item | Notes |
|----|------|--------|
| A1 | Groups API | `Canvas::ReorderLayer`, `MoveLayerIntoGroup`; UI DnD fixed earlier |
| A2 | Operator undo | Blur/HSV/Curves(+Alpha)/Noise/InvertAlpha → `BackupAll` + `CommitActiveLayerMutation` |
| A3 | Partial-tile undo | Full-grid backup before full-layer Import; no inventing empty-oldState for paint |
| A4 | Transform undo | `CommitMovePixels` → `CommitTransformation("Transform")`; Cancel restores snapshots |
| A5 | Transform perf | Selection AABB fill only (+ pad); still full-size floating buffer for GPU UV (further LOD = later) |
| A6 | UTF-8 paths | `PathUtil.h`; ImageManager/DDS already wide-open; SVG open uses UTF-8 wide ifstream |
| A7 | ICC presets | `IccPreset` enum + `SaveCanvasStandard(path, preset)` + `SetExportIccPreset` |
| B1 | Wand seed + live tol | `m_WandSeed*`, `PreviewWandFromSeed`, cached RGB source |
| B2 | Contiguous wand | Reuse source cache; floodFill only; non-contig `inRange` |
| B3 | Quick Select | `Begin/Stroke/EndQuickSelect` Lab grow + Sobel sticky edge + morpho |
| B4 | Straight lasso | `ApplyPolygonalLassoSelection` → same poly fill |
| B5 | Select opaque / all | `SelectOpaquePixels`, `SelectAll` with SelectionCommand undo |
| B6 | Crop | `CropCanvasToSelection` / `CropCanvasToRect` + `DocumentGeometryCommand` undo |
| B7 | Canvas Edit | `EditCanvas(Extend\|Resize, filter, anchor)` + geometry undo |
| C1 | Brush opt | dist² early-out, skip zero selection, tip spacing |
| C2 | Custom tips | `BrushTip`, `BrushSettings::tip`, presets Soft/Hard/Pencil/Airbrush |
| D  | Layer types + SVG | `Layer::Type`, import SVG as VectorSvg (placeholder raster, source bytes kept), `RasterizeLayer` |

## Core APIs added (for Part 2 UI agent)

```
SelectOpaquePixels / SelectAll (undo)
ApplyCurves(lutRGB, lutAlpha = {})
PreviewWandFromSeed / HasWandSeed / GetWandSeed / ClearWandSeed
ApplyPolygonalLassoSelection
BeginQuickSelectStroke / StrokeQuickSelect / EndQuickSelectStroke
CropCanvasToSelection / CropCanvasToRect
EditCanvas(mode, w, h, filter, anchorX, anchorY)
IccPreset / SetExportIccPreset / SaveCanvasStandard(path, preset)
ImportSvgAsSmartObject / RasterizeLayer
ReorderLayer / MoveLayerIntoGroup
BrushPresets::{SoftRound,HardRound,Pencil,Airbrush}
BrushSettings::tip
```

## Known limitations (acceptable for Part 1 handoff)

1. **SVG raster** is a checker placeholder; source bytes stored for future nanosvg.  
2. **Transform floating** still full-doc GPU texture size (CPU fill limited to AABB).  
3. **Live wand tolerance**: `PreviewWandFromSeed` does not rewrite undo command newMask (undo = pre-click).  
4. **Geometry undo** restores tiles/size; GPU layer textures recreated on next compose via `needsUpload`.  
5. **UI not wired** for new tools/hotkeys/dialogs — that is Part 2.

## Part 2 prompt

See `plans/PART2_UI_AGENT_PROMPT.md` and `plans/SPLIT_PLAN.md`.
