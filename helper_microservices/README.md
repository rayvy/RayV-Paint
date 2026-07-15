# RayV Helper Microservices

Isolated micro-apps for Explorer / batch texture work. **Do not link into RayVPaint paint logic.**

## Binary

`RayVHelpers.exe` — single process, two tools:

| Mode | CLI | Context menu |
|------|-----|----------------|
| PNG → DDS converter | `--mode convert [files…]` | *Convert to DDS (RayVPaint)* on `.png` |
| Atlas creator | `--mode atlas [files…]` | *Create Texture Atlas (RayVPaint)* on `.png` |
| Register / unregister | `--register` / `--unregister` | — |

No args → hub UI (pick tool). Converter has **no** atlas options. Atlas may optionally export DDS after packing.

## Isolation rules

- Lives only under `helper_microservices/`.
- No OpenCV, no Python, no RayVPaint core sources.
- Stack: C++20, Win32, DX11, Dear ImGui (Win32 backend), WIC (PNG), `texconv.exe` (DDS).
- **Only** main-app hook: Help → Register helpers (writes shell verbs + same app icon).

## Settings

`%LOCALAPPDATA%\RayVPaint\helpers_settings.ini` — last PNG folder, DDS format, atlas options.

## Build

Root CMake target `RayVHelpers` → `build/<Config>/bin/RayVHelpers.exe` (next to Core + `texconv.exe`).
