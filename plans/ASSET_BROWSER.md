# Asset Browser

**Status:** Core + UI + Fill + project packing + SmartObject foundation implemented (2026-07).

## Categories (ownership, not file types)

| Category | Root | Mutability | Ship in `.rayp` |
|----------|------|------------|-----------------|
| **Core** (`BuiltIn`) | `{exe}/assets/` | Read-only | No |
| **User** | `Documents/RayVPaint/assets/` | User r/w | No |
| **Project** | Session memory | Session | **Yes** — referenced blobs |
| **External** | Absolute path | Migration | Prefer promote to Project |

## Asset kinds (consumer filter)

`Texture`, `SmartSource`, `BrushTip`, `ExportTemplate` (.rayexpt hook), `Preview3dTemplate` (.ray3dt hook), `Unknown`.

Fill accepts **Texture only** (UI + `BindFillTextureAsset`).

## Architecture

| Component | Path | Role |
|-----------|------|------|
| `AssetManager` | `src/assets/AssetManager.*` | Public façade for tools/UI |
| `AssetStore` | `src/assets/AssetStore.*` | Payload cache, async load, refcount |
| `AssetLibraryIndex` | `src/assets/AssetLibraryIndex.*` | Core/User/Project catalog |
| `AssetThumbCache` | `src/assets/AssetThumbCache.*` | 32 / 128 thumbs + GPU LRU |
| Browser panel | `src/ui/panels/AssetBrowserPanel.*` | Dock UI |
| Picker / grid | `src/ui/widgets/UiAssetPicker.*`, `UiAssetGrid.*` | Modal pick + grid |

## Keys

- `core:<rel>` · `user:<rel>` · `proj:<uuid>` · `ext:<abs>`
- Legacy `builtin:` accepted as Core

## Thumb sidecars (disk)

```
asset.ext
asset.thumbnail.png      # 32×32
asset.thumbnail_h.png    # 128×128 (hover)
```

Written on User import / first browse generate. Core is not written (read-only).

## Async contract

- `RequestLoad` / `Poll` — never block UI on full decode
- Fill samples `GetPayload` (shared_ptr); missing → solid color fallback
- Thumbs load async via ThreadPool

## Fill

- Bind via `Canvas::BindFillTextureAsset` / Asset Browser / picker
- File import → **Project** asset (`ImportFileToProject`) then bind
- No private multi-MB `textureRgba` when store Ready
- Solid fill still 1×1 GPU path

## `.rayp` packing

Metadata `project_assets[]` + zlib blobs **before** layer pixels.  
Only **referenced** `proj:` keys (fill + smartAssetKey).

## SmartObject foundation

- `Layer::smartAssetKey`
- `CanPaintContent()` false for SmartObject / VectorSvg
- `ConvertLayerToSmartObject` / `ReplaceSmartObjectSource`
- Rasterize clears smart key/bytes

## Conceptual rules

1. Raster layer ≠ asset until convert/import  
2. Using an asset does not force layer rasterization  
3. Brushes may pick assets later; `.rvbrush` still embeds tip bytes  
4. Templates `.rayexpt` / `.ray3dt` — kind hooks only (not full format yet)

## Checklist

1. `AssetStore` + path resolution — ✅  
2. Async load + payload pin — ✅  
3. Library index Core/User — ✅  
4. Thumbs 32/128 + disk sidecar — ✅  
5. UI browser + picker — ✅  
6. Fill wiring (no path-only identity) — ✅  
7. Project packing in `.rayp` — ✅  
8. SmartObject foundation — ✅  
9. Export/3D template formats — deferred (hooks only)  
10. GPU textured-fill sample (no full-doc bake) — future  
