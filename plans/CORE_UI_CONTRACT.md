# Core ↔ UI Contract (frozen for UI agent)

**Read with:** `src/Canvas.h`, `plans/PART1_STATUS.md`, `plans/PART2_UI_AGENT_PROMPT.md`  
**Do not reimplement** TileCache COW, export blend, geometry undo internals.

---

## 1. Tools / behaviour

### Magic Wand
| Call | When | Undo |
|------|------|------|
| `ApplyMagicWandSelection(dev, x, y, tol, add, sub, contig)` | Click | **yes** (pushUndo) |
| `PreviewWandFromSeed(dev, tol, add, sub, contig)` | Tolerance slider | **no** |
| `HasWandSeed` / `GetWandSeed` / `ClearWandSeed` | Seed marker | — |

Click path (core already): `InvalidateWandSourceCache` → `EnsureWandSourceCache` → `RunMagicWand(..., pushUndo=true)`.

### Transform / Move
| API | Notes |
|-----|--------|
| `StartMovePixels` / `UpdateMovePixels` / `CommitMovePixels` / `CancelMovePixels` | Commit → undo `"Transform"`; Cancel restores tiles |
| `GetFloatingBBox(x,y,w,h)` | Selection AABB for gizmo |
| `GetFloatingOffsetX/Y`, `Get/SetFloatingScale*`, `Get/SetFloatingRotation` | Gizmo state |

### Operators (full-tile undo)
`InvertColors`, `InvertAlpha`, `ApplyBlur`, `ApplyHSV`, `ApplyCurves(lutRGB, lutAlpha={})`, `ApplyNoise`  
Pattern: BackupAll → mutate → CommitActiveLayerMutation.

### Crop / Canvas Edit
| API | Undo |
|-----|------|
| `CropCanvasToSelection(dev)` / `CropCanvasToRect(dev,x,y,w,h)` | `DocumentGeometryCommand` |
| `EditCanvas(dev, Extend\|Resize, w, h, Nearest\|Bilinear\|Lanczos, anchorX, anchorY)` | same |

### Quick Select / Smart Select
| API | Notes |
|-----|--------|
| `BeginQuickSelectStroke` / `StrokeQuickSelect(pts, radius, sub)` | Working mask only |
| `EndQuickSelectStroke(dev, add, sub)` | Commits selection + undo |
| `CancelQuickSelectStroke()` | Drop stroke, **no** selection/undo change |
| `IsQuickSelectStrokeActive()` | UI cancel button |
| `ApplySmartSelectSelection` / `CancelSmartSelect` / `IsSmartSelectInProgress` | Existing async path |

### Selection helpers
`SelectAll`, `SelectOpaquePixels(layerIdx=-1)`, `ApplyPolygonalLassoSelection`

### Groups
`CreateLayerGroup`, `AddLayerToGroup`, `RemoveLayerFromGroup`, `ReorderLayer`, `MoveLayerIntoGroup`

### Brush tips
`BrushSettings::tip` → `&BrushPresets::SoftRound()` etc.  
Project prefs: `Set/GetBrushTipId`, `Set/GetCustomBrushTip`  
Ids: `procedural` \| `soft_round` \| `hard_round` \| `pencil` \| `airbrush` \| `custom`

### SVG / layer types
`ImportSvgAsSmartObject`, `RasterizeLayer`  
`Layer::type` + `smartSourceBytes` / `smartSourcePath` (persisted in .rayp)

### ICC / export
```cpp
enum IccPreset { None, sRGB, DisplayP3, AdobeRGB };
SetExportIccPreset / GetExportIccPreset / IccPresetName / IccPresetFromName
SaveCanvasStandard(path, IccPreset)  // embeds profile bytes; no free-text ICC path
GetExportPath/Format + setters      // Quick Export / Project Output
```

### Channels preview
`GetChannelPreviewSRV(device, ChannelPreview::{R,G,B,A})`  
Proxy-sized grayscale thumbs from composite (not full 16K).

---

## 2. .rayp metadata (v2+)

| Key | Type | UI use |
|-----|------|--------|
| `export_path` | string | Project Output path |
| `export_format` | string | DDS preset |
| `export_advanced_mode` | bool | |
| `export_compression_speed` | string | |
| `export_generate_mip_maps` | bool | |
| `export_mip_filter` | string | |
| `export_png_color_space` | string | legacy name |
| **`export_icc_preset`** | string name | PNG ICC combo |
| **`brush_tip_id`** | string | tip combo |
| **`brush_tip_custom_size`** | int | custom tip |
| **`brush_tip_custom_pixels`** | u8 array | custom tip |
| layer `layer_type` | string | badge |
| layer `has_smart_source` | bool | + zlib blob after mask |
| layer `smart_source_path` | string | |

---

## 3. I/O rules

- **UTF-8 paths**: Windows wide via `PathUtil` / existing `UTF8ToWString` on open/save/drop.
- **ICC**: preset → built-in bytes (`IccProfiles`) → PNG iCCP. No user-typed profile path.
- **SVG drop**: `OpenDocument` → `ImportSvgAsSmartObject`.

---

## 4. Explicitly deferred (optional / later core)

| Item | Status |
|------|--------|
| Tiled layer masks (16K) | Not yet — full mask buffer may be large |
| Real group composite pass | Folders only (`isGroup` skips pixels) |
| Full nanosvg raster | Placeholder checker + source bytes |

---

## 5. UI agent must not break

1. Call `PreviewWandFromSeed` on tol scrub — never re-`ApplyMagicWandSelection` every frame.
2. Transform: `CommitMovePixels` on accept, `CancelMovePixels` on Esc.
3. Quick Select: `CancelQuickSelectStroke` on tool switch / Esc mid-stroke.
4. PNG save: only `SaveCanvasStandard(path, GetExportIccPreset())`.
5. After geometry ops, expect composite recreate (core handles).
6. Prefer `MoveLayerIntoGroup` / `ReorderLayer` over inventing `Layer*` remap.
