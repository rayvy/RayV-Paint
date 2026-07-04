# Paint.NET / Photoshop Parity Roadmap (Build 10 Target)

This document outlines the architectural blueprints and build-by-build milestones required to upgrade RayVPaint to a highly optimized, professional-grade desktop image editor.

---

## Part 1: Architectural Foundations

### 1. High-Performance Undo/Redo System (16K Canvas Ready)
Storing full 16K floating-point layers (16,384 x 16,384 x 16 bytes = 4.29 GB per state) in memory is impossible. We must use a **Tiled/Chunked Command Pattern**:
* **Grid Partitioning:** Divide the canvas into 256x256 pixel tiles.
* **Dirty Tile Tracking:** When painting, only tiles that intersect the brush stroke are updated on the GPU and copied back to CPU memory.
* **Delta Undo Commands:** The Undo system stores only the previous states of these specific dirty tiles rather than the entire layer.
* **History Compression & Limits:** Implement a configurable RAM budget (e.g., max 2 GB) or step limit (e.g., 50 actions). When exceeded, the oldest tiles are serialized to disk in a fast compressed temp file or discarded.

```
       Canvas (16K x 16K)
+---+---+---+---+---+---+
|   |   |   |   |   |   |   Dirty Tiles (Modified by stroke):
+---+---+---+---+---+---+   - Only tiles A & B are backed up
|   | A*| B*|   |   |   |   - Memory usage: 2 tiles * 256KB = 512KB
+---+---+---+---+---+---+     instead of 4.29GB!
|   |   |   |   |   |   |
+---+---+---+---+---+---+
```

### 2. Extensible Brush Engine & Stroke Stabilization
* **Distance-Based Spacing (Dabbing):** Currently, stamps are placed based on frame rate or mouse moves. We must switch to a parameter-based distance system:
  $$\text{Next Stamp Position} = \text{Prev Position} + \text{Direction} \times (\text{Brush Diameter} \times \text{Spacing Percentage})$$
* **Stroke Stabilization:** Implement a configurable stabilization queue (Running Average or exponential smoothing filter). The brush cursor lags slightly behind the mouse cursor, pulling it along a smoothed path (Bezier/B-Spline interpolation) to create clean, smooth lines.
* **Windows Ink / Tablet Support:** Integrate WM_POINTER / Wacom WinTab APIs to capture pen pressure, tilt, and hover parameters dynamically adjusting radius and opacity.

### 3. Layer Tree & Advanced Compositing (Channel-Level Blending)
* **Layer Groups:** Redefine the flat layer list as a tree structure (nodes containing layers, folders, or adjustments).
* **GPU Blend Modes:** Custom pixel shader blending for standard modes: Multiply, Screen, Overlay, Color Dodge, Linear Burn, etc.
* **Channel-Level Blending (Killer Feature):** Provide a routing matrix for each layer:
  - Configure which source channels (R, G, B, A) map to which target channels.
  - Enable mask layers to isolate blending to specific color channels (e.g., blending normal maps exclusively on RG channels while preserving heights on B).

### 4. Selection & Masking Engine
* **Selection Buffer:** Maintain a single-channel 8-bit mask texture on the GPU representing selection opacity (0.0 to 1.0) for antialiased/feathered edges.
* **Interactive Tooling:** 
  - **Rectangular/Elliptical:** Primitive shapes drawn to mask.
  - **Lasso/Polygonal Lasso:** Freehand polygons rendered to the mask.
  - **Magic Wand:** Flood-fill algorithm executed on a GPU compute shader or fast CPU queue (BFS) comparing color tolerance.
* **Marching Ants Visualizer:** A dynamic shader displaying animated dashed borders around the selection boundary using a moving texture coordinate offset.

---

## Part 2: Build-by-Build Milestones

### Build 3: Brush Physics & Color GUI
* **Brush Spacing Fix:** Implement distance-based stamp dabbing.
* **Stroke Stabilization:** Add smoothed running-average stroke filter.
* **ImGui Color Wheel:** Clean HSL/HSV color picker panel with configurable color swatches (palettes).

### Build 4: Tiled Undo/Redo Engine
* **Tile Managers:** Refactor layer storage to 256x256 tiles.
* **Command History:** Implement undo/redo stack recording tile deltas.
* **Pipette Tool:** Eyedropper tool to select color from composed canvas viewport under cursor.

### Build 5: Selections & Magic Wand
* **Selection State:** Implement the mask buffer and selection states (Add, Subtract, Intersect).
* **Marching Ants:** Render animated dash selection outline.
* **Magic Wand / Lasso:** CPU/GPU flood fill tolerance algorithms.

### Build 6: Layer Blending & Bucket Fill
* **Blend Modes:** Implement advanced Photoshop/Paint.NET blend modes in HLSL.
* **Channel Mapping:** UI Matrix for channel-level blend configuration.
* **Bucket Fill Tool:** Flood-fill color within boundaries on active layer.

### Build 7: Transformations & Canvas Resizing
* **Image Resize:** nearest-neighbor, bilinear, and bicubic resampling.
* **Canvas Resize:** Resizing canvas dimensions with anchor point presets (Top-Left, Center, Bottom-Right, etc.).
* **Pixel Move Tool:** Translate, scale, and rotate selected pixels.

### Build 8: Color Adjustments & Smart Objects
* **Adjustments:** Curves, HSL sliders, Levels adjustments running non-destructively as shader passes.
* **Smart Objects:** File-referenced raster layers that preserve source pixel resolution when scaled.

### Build 9: Clone Stamp & Native Project Files (`.rayp`)
* **Stamp Tool:** Clone stamp tool copying pixels from a defined source offset.
* **`.rayp` Format:** Multi-layer projects serialized to a ZIP-packaged binary containing layers, JSON metadata, and adjustment parameters.

### Build 10: Performance Profiling & Optimization
* **SIMD/Multithreading:** Optimizing tile copy-backs and flood fill algorithms.
* **Final QA:** Stability tests, memory leak checks with large 8K/16K canvases, and UI polish.
