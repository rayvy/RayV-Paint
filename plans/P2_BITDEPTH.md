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

## Next (P2.2 / P2.3)

- Color picker / brush values outside 0–1 when depth ≠ U8 (UI)
- Optional height mono brush helper
- Minimal Document → 8/16f/32f menu
- Undo for bit-depth convert
