# RayVPaint DX12 Hard Migration Plan

Цель: полностью убрать DX11 и D3D11On12 из RayVPaint. Все, что сейчас зависит от DX11 API, должно быть удалено или переписано на DX12. CPU-only код можно временно оставить как источник данных и поведения, если он не тянет DX11 в публичные API.

## Текущее состояние

Проект сейчас не является чистым DX12-приложением. DX12 используется для окна, swapchain и ImGui, но сам Canvas рендерится через D3D11On12:

- `src/main.cpp` создает `ID3D12Device`, `ID3D12CommandQueue`, `IDXGISwapChain3`, но параллельно создает `ID3D11On12Device`, `ID3D11Device`, `ID3D11DeviceContext`.
- `src/main.cpp` создает DX12 texture для canvas, оборачивает ее в D3D11 resource через `CreateWrappedResource`, рендерит Canvas через `ID3D11DeviceContext`, потом отдает SRV в ImGui DX12.
- `src/Canvas.h` публично зависит от `<d3d11.h>` и протаскивает `ID3D11Device*` / `ID3D11DeviceContext*` почти во все операции.
- `src/Canvas.cpp` содержит основную DX11 графику: textures, SRV, RTV, shaders, buffers, blend/rasterizer states, Map/UpdateSubresource/DrawIndexed.
- `src/ui/EditorPanels.h/.cpp` зависит от D3D11 только потому, что UI вызывает Canvas-операции с `ID3D11Device*`.
- `CMakeLists.txt` собирает `imgui_impl_dx11.cpp` и линкует `d3d11`.

Хорошая новость: `TileCache`, `PaintEngine`, undo/redo, clipboard, file IO и большая часть selection/adjustment логики могут пережить первую фазу как CPU модель документа.

## Нельзя Оставлять

Это должно исчезнуть из production-сборки:

- `#include <d3d11.h>` и `#include <d3d11on12.h>`.
- `ID3D11Device`, `ID3D11DeviceContext`, `ID3D11Resource`, `ID3D11Texture2D`, `ID3D11ShaderResourceView`, `ID3D11RenderTargetView`, `ID3D11Buffer`, `ID3D11*Shader`, `ID3D11BlendState`, `ID3D11RasterizerState`.
- `D3D11On12CreateDevice`, `CreateWrappedResource`, `AcquireWrappedResources`, `ReleaseWrappedResources`.
- `imgui_impl_dx11.cpp` from CMake.
- `d3d11` from target link libraries.
- Any Canvas/UI function whose signature takes a D3D11 device/context.

Temporary placeholder rule: if a DX11 feature is too large to port immediately, delete the DX11 path and leave a clear DX12 TODO stub that compiles and fails gracefully. Do not keep the D3D11 fallback.

## Можно Временно Оставить

Keep these CPU systems during the first hard migration:

- `TileCache` as CPU document/layer storage.
- `PaintEngine` brush stamping/smudge on CPU.
- CPU selection mask generation: rect, ellipse, lasso, magic wand, smart select.
- CPU destructive operations: blur, HSV, curves, noise, transforms, bucket fill, gradient.
- CPU export/import paths using `std::vector<float>` compatibility helpers, as long as they do not expose DX11.
- `DdsHelper` DXGI numeric constants are acceptable because DDS stores DXGI format identifiers. Do not confuse this with runtime DX11 usage.

These systems should later move to GPU compute/tiled upload, but they are not blockers for the first DX12 purge.

## Target Architecture

Create a small explicit DX12 backend instead of continuing with global raw pointers in `main.cpp`.

Recommended modules:

- `src/render/Dx12Device.h/.cpp`
  - DXGI factory, adapter selection, `ID3D12Device`.
  - debug layer and info queue in debug builds.
  - command queues.
  - fence and frame sync.

- `src/render/Dx12Swapchain.h/.cpp`
  - `IDXGISwapChain3`.
  - back buffers.
  - RTV descriptors.
  - resize handling.

- `src/render/Dx12Descriptors.h/.cpp`
  - RTV/DSV heaps.
  - shader-visible CBV/SRV/UAV heap.
  - descriptor allocation/free with generation-safe handles.
  - reserved ImGui descriptor range.

- `src/render/Dx12Upload.h/.cpp`
  - upload ring buffer.
  - texture upload helpers.
  - row pitch alignment.
  - transition barriers.

- `src/render/CanvasRendererDX12.h/.cpp`
  - owns canvas render target.
  - owns layer/mask/floating textures.
  - owns PSOs/root signatures.
  - uploads dirty tiles from `TileCache`.
  - composites layers into a DX12 render target.
  - renders viewport checkerboard, pan/zoom/rotation/channel masks/selection outline.

- `src/render/Dx12Types.h`
  - RAII aliases/wrappers around `Microsoft::WRL::ComPtr`.
  - small strongly typed handles for texture IDs/descriptors.

Main principle: `Canvas` should become document/model logic. `CanvasRendererDX12` should become GPU presentation/composition logic.

## Phase 0 - Build Safety and Baseline

1. Add a migration branch and keep the current app runnable as baseline.
2. Add a CI/local check that fails if runtime DX11 tokens remain after the purge:
   - `rg "d3d11|D3D11|ID3D11|D3D11On12|imgui_impl_dx11" src CMakeLists.txt`
3. Build current project once and record:
   - startup path.
   - open/import image.
   - paint stroke.
   - layer visibility/opacity.
   - selection outline.
   - save/export.
4. Decide placeholder policy:
   - If a feature requires old DX11 renderer, keep CPU state mutation but disable GPU preview until DX12 renderer catches up.

Deliverable: baseline known, no functional guessing.

## Phase 1 - Cut the DX11 Bridge

Primary goal: remove D3D11On12 from `main.cpp`.

Tasks:

1. Delete `#include <d3d11.h>` and `#include <d3d11on12.h>` from `main.cpp`.
2. Delete globals:
   - `g_pd3d11On12Device`
   - `g_pd3dDevice`
   - `g_pd3dDeviceContext`
   - `g_canvasTexture11`
   - `g_canvasRTV`
   - `g_canvasTextureAcquired`
3. Rewrite `CreateDeviceD3D` into pure DX12 creation:
   - keep DXGI factory/adapter selection.
   - keep `D3D12CreateDevice`.
   - keep command queue.
   - remove `D3D11On12CreateDevice`.
4. Rewrite canvas render target creation as native DX12 only:
   - create `ID3D12Resource` with render-target and shader-resource states.
   - create RTV and SRV directly.
   - no wrapped resources.
5. Replace `RenderCanvasToTexture` with a temporary DX12 placeholder:
   - clear the canvas texture via DX12 command list.
   - expose SRV to ImGui.
   - no DX11 render path.

Acceptance:

- App opens with ImGui through DX12.
- Canvas panel shows a DX12-cleared placeholder texture.
- No D3D11On12 objects exist.

## Phase 2 - Remove DX11 From Build System

Tasks:

1. Remove `${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp` from `CMakeLists.txt`.
2. Remove `d3d11` from `target_link_libraries`.
3. Keep:
   - `d3d12`
   - `dxgi`
   - `d3dcompiler` temporarily, unless switching shader compilation now.
4. Ensure `imgui_impl_dx12.cpp` remains the only ImGui DirectX backend.
5. Run the DX11 token check.

Acceptance:

- Project links without `d3d11.lib`.
- `imgui_impl_dx11` is gone.

## Phase 3 - Split Canvas Model From Renderer

Primary goal: `Canvas.h` must stop including `d3d11.h`.

Tasks:

1. Remove D3D11 resources from `Layer`.
   - Replace `texture`, `srv`, `maskTexture`, `maskSRV` with renderer-owned handles.
   - Layer should only contain document state: name, visibility, opacity, blend mode, tile cache, mask data, filters, grouping metadata.
2. Remove D3D11 resources from `Canvas`.
   - Vertex/index/constant buffers.
   - shaders.
   - input layout.
   - sampler/blend/rasterizer state.
   - composite/group/floating/selection GPU textures.
3. Change Canvas public API:
   - `Initialize()` no longer takes GPU device.
   - document operations no longer take `ID3D11Device*`.
   - `Render(...)` is removed from Canvas.
   - `ComposeLayers(...)` GPU path is removed from Canvas.
4. Introduce dirty events/flags for renderer:
   - layer created/deleted/resized.
   - layer tile dirty.
   - mask dirty.
   - selection mask dirty.
   - floating pixels dirty.
   - composite dirty.
5. Update `EditorPanels` so it no longer takes device/context.

Acceptance:

- `src/Canvas.h` has zero Direct3D includes.
- `UI::RenderAll` has no graphics API parameters.
- Canvas compiles as CPU document model.

## Phase 4 - Native DX12 Canvas Renderer MVP

Primary goal: restore visible canvas rendering without DX11.

Minimum DX12 implementation:

1. Root signature:
   - descriptor table for SRVs.
   - CBV/root constants for viewport/layer parameters.
   - static sampler or descriptor sampler.
2. Pipeline states:
   - composite/layer blend PSO.
   - viewport/checkerboard PSO.
   - selection outline PSO.
3. Resources:
   - canvas viewport render target.
   - composite render target.
   - per-layer texture resources.
   - per-mask R8 resources.
   - optional floating texture/mask resources.
4. Upload:
   - dirty tile upload from `TileCache` through upload heap.
   - full upload fallback for import/load/resize.
   - texture barriers tracked explicitly.
5. Rendering:
   - clear composite RT.
   - draw visible layers in order.
   - draw floating pixels if active.
   - draw checkerboard + composite into canvas texture.
   - draw selection outline.
   - return ImGui-compatible SRV GPU handle.

Acceptance:

- Import image appears.
- Painting updates visible texture.
- Layer opacity/visibility works.
- Selection outline works or is replaced by a compiling placeholder with no DX11.

## Phase 5 - Shader Modernization

Tasks:

1. Keep `src/shaders/Canvas.hlsl` initially, but compile for DX12-compatible profiles.
2. Prefer precompiled shader bytecode:
   - compile at build time to `.cso`, or
   - move to DXC later for SM 6.x.
3. Remove runtime ad-hoc shader cache from `Canvas.cpp`.
4. Move shader loading into `CanvasRendererDX12`.
5. Define shared CPU/GPU constant structs with explicit alignment.

Acceptance:

- No shader creation in Canvas model.
- Shader compilation/load errors are renderer errors.

## Phase 6 - CPU Compatibility Cleanup

Tasks:

1. Keep `ExportLayerF` / `SetLayerPixelsF` only as temporary compatibility helpers.
2. Mark high-cost full-canvas operations as migration debt:
   - `GetCompositePixels`
   - full layer export/import loops.
   - CPU blur/HSG/curves/noise.
   - move pixels transform using full `std::vector<float>`.
3. Replace repeated full-canvas export paths with tile iterators where practical.
4. Make clipboard/file export explicitly request a CPU composite from Canvas, not from renderer.
5. Ensure every CPU mutation marks the correct dirty tile/mask/selection flags.

Acceptance:

- CPU features still work.
- Renderer sees dirty state without API-specific calls from Canvas/UI.

## Phase 7 - C++20 / Code Quality Purge

Targets:

1. Replace raw COM pointers with `Microsoft::WRL::ComPtr` or a local RAII wrapper.
2. Remove manual repeated `Release()` blocks.
3. Replace global renderer state in `main.cpp` with owned objects.
4. Move huge unrelated code blocks out of `main.cpp`.
5. Move Win32 dialog helpers out of `EditorPanels.cpp`.
6. Fix mojibake comments/text in UI files or remove broken comments.
7. Prefer `enum class`, `std::span`, `std::optional`, `std::filesystem::path`, narrow ownership.
8. Reduce `Canvas.cpp` size by splitting:
   - `CanvasDocument.cpp`
   - `CanvasSelection.cpp`
   - `CanvasLayers.cpp`
   - `CanvasFileIO.cpp`
   - `CanvasAdjustments.cpp`
   - `CanvasTransforms.cpp`
9. Add `[[nodiscard]]` for operations returning failure.
10. Centralize logging and error handling around renderer init/resource creation.

Acceptance:

- `main.cpp` is app orchestration, not renderer implementation.
- Renderer lifetime is RAII.
- Canvas model has no GPU API coupling.

## Phase 8 - Optional GPU-Driven Follow-Up

Only after the hard DX11 purge:

1. Move brush stamping to compute shader.
2. Move selection mask generation to compute/render passes.
3. Move blur/HSV/curves/noise to compute passes.
4. Use tiled GPU resources/atlases or sparse residency strategy for very large documents.
5. Add async copy queue for uploads/readbacks.
6. Add GPU timestamp profiling.
7. Add persistent descriptor indexing/bindless-like material table if needed.

This phase is deliberately not required for initial DX12 migration.

## File-Level Hit List

Immediate delete/rewrite:

- `src/main.cpp`
  - remove D3D11 includes/globals/device creation.
  - remove wrapped canvas texture path.
  - isolate DX12 device/swapchain/descriptors into renderer modules.

- `src/Canvas.h`
  - remove `<d3d11.h>`.
  - remove all D3D11 members from `Layer` and `Canvas`.
  - remove D3D11 parameters from public methods.

- `src/Canvas.cpp`
  - delete DX11 initialization/render/composite/resource code.
  - keep CPU document operations.
  - move GPU code into `CanvasRendererDX12`.

- `src/ui/EditorPanels.h/.cpp`
  - remove D3D11 include and function parameters.
  - call Canvas CPU operations without graphics device.

- `CMakeLists.txt`
  - remove ImGui DX11 backend.
  - remove `d3d11` link.
  - add new `src/render/*.cpp` files.

Keep but revisit:

- `src/core/TileCache.*`
- `src/core/PaintEngine.*`
- `src/core/UndoRedoManager.*`
- `src/core/DdsHelper.*`
- `src/core/ImageManager.*`
- `src/core/ClipboardHelper.*`

## Hard Gates

Gate 1:

```powershell
rg "d3d11|D3D11|ID3D11|D3D11On12|imgui_impl_dx11" src CMakeLists.txt
```

Allowed result: no runtime DX11 hits. `DdsHelper` numeric DDS comments/constants may be reviewed separately.

Gate 2:

```powershell
rg "ID3D12|D3D12|DXGI" src/Canvas.h src/ui/EditorPanels.h
```

Allowed result: ideally none. UI and Canvas model should not expose graphics API types.

Gate 3:

Manual smoke:

- launch app.
- create/open document.
- import image.
- paint stroke.
- toggle layer visibility/opacity.
- use selection.
- save/export.
- resize window.
- close without device leak/assert.

## Suggested Execution Order

1. Phase 1 + Phase 2 in one hard commit: make app pure DX12 shell with placeholder canvas.
2. Phase 3 in second commit: split Canvas model from GPU ownership.
3. Phase 4 in third/fourth commits: restore DX12 canvas rendering feature by feature.
4. Phase 5 after first DX12 canvas is visible.
5. Phase 6/7 as cleanup commits once behavior is restored.
6. Phase 8 only after the codebase is no longer dragging DX11 behind it.

The brutal but clean rule: after Phase 2, DX11 is not allowed back into the tree, even temporarily.
