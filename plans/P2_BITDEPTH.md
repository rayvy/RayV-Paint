# P2 Document Bit Depth — Status

Date: 2026-07-10

## Model (locked)

- **Global** working depth: `U8 | F16 | F32` (all channels same).
- **Export packing** free (BC7, R32, RGB10A2, …) — not per-channel document modes.
- Default / BC7 / depth-view / R8G8 → **U8**.
- R16F / RGBA16F / R11 → **F16** storage.
- R32F / RGBA32F → **F32** storage.

## Done (P2.1 foundation)

- `CanvasPixelFormat::RGBA16F` (8 B/px)
- `HalfFloat.h` shared encode/decode
- TileCache get/set/import/export/fill + `ConvertFormat` tile-wise
- PaintEngine float intermediate; U8 clamps; F16/F32 preserve HDR values
- GPU layer: `R8G8B8A8` / `R16G16B16A16_FLOAT` / `R32G32B32A32_FLOAT`
- `SetDocumentBitDepth` converts all layers + drops GPU textures for re-upload
- Open policy promotes only explicit float/half sources
- .rayp `document_bit_depth` maps to real F16 format

## Done (P2.2 / P2.3 UI)

- **Image → Document Bit Depth** menu (U8 / F16 / F32)
- **Properties** combo + B/px readout
- Status bar: `U8|F16|F32 (NB/px)`
- **Colors** panel when float doc: DragFloat4 (no clamp), Mono R→RGB, R-only paint
- HSV/swatches still 0..1 display (clamped preview)

## Tools float support (2026-07-10)

| Tool | Float F16/F32 |
|------|----------------|
| Brush / eraser | Yes (PaintEngine raw float) |
| Smudge | Yes (GetPixelF/SetPixelF) |
| Gradient | Yes (float lerp + SetLayerPixelsF) |
| Bucket | Yes (float flood-fill, no OpenCV 8-bit) |
| Pipette sample | Yes — active layer raw; HUD shows float |
| Status bar | Brush RGBA as float when float doc |

**Depth dump note:** D32 opens as **U8 view** (by design) → pipette 0..255 is expected. Convert to F32 or open true R32 height for float readout.

## Next (optional)

- Undo for bit-depth convert
- Tone-map HDR viewport preview
- Height-specific tool preset
- Optional: open depth dumps as F32 storage for full Z range
