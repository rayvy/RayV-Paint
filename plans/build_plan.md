# RayVPaint Build Roadmap - Early Stages

This document tracks the initial design and iteration steps of RayVPaint from scratch to a working DirectX 11 / ImGui tech-art paint editor.

## Build 1: Foundations
* **Window & Swapchain Setup:** GLFW with Win32 platform integration, DirectX 11 graphics context, and swapchain initialization.
* **UI Layout:** ImGui Integration with docking enabled, persistent header, and footer workspace configuration.
* **Canvas Engine:** Orthographic rendering camera, Zoom and Pan controls, and checkered transparent background rendering.
* **Configuration:** JSON configuration loader (`config.json`) managing default canvas width/height.
* **Command Line & Automation:** 
  - Subsystem controls (GUI by default, Console via CLI arguments like `--console`).
  - Headless/hidden-window modes (`--test`, `--headless`) with embedded Python 3 environment (`pybind11`).
  - Batch file for MSVC compiler setup (`build.bat`) and test runner (`run_tests.bat`).

## Build 2: Painting Tools & Native Format Polishing
* **Viewport Fixes:**
  - dynamic swapchain resizing (`ResizeBuffers`) on window size change.
  - Pixel-perfect mouse cursor transformation mapping to eliminate drawing offset drift.
  - Visual brush circle rendering over the canvas; OS cursor hiding.
  - Integer-based viewport mapping to prevent float sub-pixel rendering artifacts.
* **Native DDS Support:**
  - Block decompression on-the-fly (`bcdec.h`) supporting BC1-BC5 and BC7 formats.
  - Multi-format DDS saving support: 8-bit, 16-bit (UNORM/float), and 32-bit float (SDR, HDR, Grayscale options).
* **PNG Color Spaces:**
  - PNG ICC profile embedding by compressing profile binaries into `iCCP` chunks.
* **Layers & Rendering:**
  - Multi-layer list management with visibility and opacity.
  - Custom pixel shader (`PSLayerBlend`) for layer-blending composition.
  - Color channel visualization modes (RGBA, RGB-only, Alpha-only, Alpha overlay mask).
