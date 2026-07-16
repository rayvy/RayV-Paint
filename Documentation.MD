# RayV Paint — Documentation

**Status:** Work in Progress (not a public release).  
**Policy:** Format and API may change freely. No legacy compatibility for pre-standardization files (`.rvbrush`, bare preset `.json`, loose Core PNGs).

This document is the **canonical** terminology and file-system standard for RayV Paint.

---

## 1. Terminology

| Term | Meaning |
|------|---------|
| **Document / Project** | An open paint session, saved as `.rayp`. Holds layers, maps, and referenced **project** assets. |
| **Raster layer** | Pixel buffer on the canvas. **Not** an asset until converted/imported. |
| **Asset** | Addressable library resource with a stable **key** and ownership **category**. |
| **Package** | On-disk container (`RVPK`) used by RVPCF / RVPAF / RVPBF. |
| **Core** | Built-in read-only library next to the executable (`{exe}/assets/`). |
| **User / Library** | Writable library under `Documents/RayVPaint/`. |
| **Project / Session** | In-memory / packed-into-`.rayp` assets for the current document. |
| **External** | One-shot absolute path outside libraries (prefer promote to Project on save). |
| **Fill** | Parametric layer writing solid / textured color into maps; samples assets by key. |
| **SmartObject** | Non-destructive layer holding source identity + display cache (foundation; SVG deferred). |
| **Config package** | Settings / templates (export, shaders) — **RVPCF**. |
| **Asset package** | Media assets (textures, …) — **RVPAF**. |
| **Brush package** | Brush preset + tip resources — **RVPBF**. |

### 1.1 Asset categories (ownership)

| Category | Root | Writable | In `.rayp` |
|----------|------|----------|------------|
| **Core** | `{exe}/assets/` | No | No |
| **User** | `Documents/RayVPaint/assets/` | Yes | No |
| **Project** | Session table + blobs | Session | **Yes** (referenced only) |
| **External** | Absolute path | N/A | Promote → Project on save |

### 1.2 Stable asset keys

```
core:<relative path under exe assets>
user:<relative path under user assets>
proj:<uuid>
ext:<normalized absolute path>
```

Examples:

- `core:textures/checker.rvpaf`
- `user:textures/my_noise.rvpaf`
- `proj:a1b2c3d4-…`

### 1.3 Asset kinds (consumer filter)

| Kind | Package / extension | Typical consumers |
|------|---------------------|-------------------|
| `texture` | `.rvpaf` | Fill, Outline, future decals |
| `brush` | `.rvpbf` | Brush library / paint engine |
| `export_template` | `.rvpcf` (`kind=export_template`) | File Explorer export flow |
| `shader_preset` | `.rvpcf` (`kind=shader_preset`) | 3D Preview materials |
| `smart_source` | (future) | SmartObject sources |

Fill and Outline accept **`texture` only** (UI + API).

---

## 2. Package formats (standard)

All three product formats share one binary container:

### 2.1 Container: RVPK

```
offset  content
------  -------
0       u32 magic   = 'RVPK' (0x4B505652 little-endian)
4       u32 version = 1
8       u32 manifestSize
12      u8  manifestJson[manifestSize]   // UTF-8
…       u32 resourceCount
        repeated resourceCount times:
          u16 pathLen
          u8  path[pathLen]              // relative, '/' separators
          u32 dataLen
          u8  data[dataLen]
```

**Manifest** (JSON) always includes:

```json
{
  "format": "RVPCF" | "RVPAF" | "RVPBF",
  "version": 1,
  "id": "stable_id",
  "displayName": "Human name",
  "kind": "texture | brush | export_template | shader_preset | …"
}
```

Additional fields are format-specific.

---

### 2.2 RVPCF — RayV Painter Config File (`.rvpcf`)

**Purpose:** export templates, 3D shader / material presets (and future configs).

**Typical layout:**

| Resource path | Role |
|---------------|------|
| `config.json` | Primary settings (required) |
| `shaders/*.hlsl` | Optional override / helper shaders |

**Kinds:**

| `kind` | `config.json` content |
|--------|------------------------|
| `export_template` | Export naming, container, mip defaults |
| `shader_preset` | Channel graph + lighting knobs (MaterialConfig) |

**Example export template config:**

```json
{
  "namePattern": "{set}{suffix}",
  "exportAllSets": true,
  "container": "dds",
  "generateMipMaps": true
}
```

**Locations:**

- Core: `{exe}/assets/configs/`
- User: `Documents/RayVPaint/presets/` (shaders under `presets/shaders/`)

---

### 2.3 RVPAF — RayV Painter Asset File (`.rvpaf`)

**Purpose:** library textures and similar media. **Raster layers are not RVPAF** until import/convert.

**Typical layout:**

| Resource path | Role |
|---------------|------|
| `image.png` or `image.dds` | Full asset (required) |
| `thumbnail.png` | 32×32 browser grid (recommended) |
| `thumbnail_h.png` | 128×128 hover preview (optional) |

**Manifest extras:**

```json
{
  "format": "RVPAF",
  "kind": "texture",
  "width": 256,
  "height": 256,
  "image": "image.png",
  "thumbnail": "thumbnail.png",
  "thumbnail_h": "thumbnail_h.png"
}
```

**Import policy:**

- User import of PNG/JPG/DDS → **always written as `.rvpaf`** into User library.
- Fill/Outline file pick → Project session (`proj:`) for portable `.rayp`.

**Locations:**

- Core: `{exe}/assets/textures/*.rvpaf`
- User: `Documents/RayVPaint/assets/textures/*.rvpaf`

---

### 2.4 RVPBF — RayV Painter Brush File (`.rvpbf`)

**Purpose:** brush presets. Self-contained for sharing.

**Typical layout:**

| Resource path | Role |
|---------------|------|
| `config.json` | Brush parameters + tip type |
| `tip.raw8` | Optional grayscale tip (`size×size` bytes) |

**config.json tip types:**

- `none` — procedural round
- `builtin` — engine tip id (`soft_round`, …)
- `embedded` — pixels in `tip.raw8`

**Location:**

- User: `Documents/RayVPaint/brushes/*.rvpbf`

Built-in brushes remain in-engine (not disk packages).

---

## 3. Document format (`.rayp`)

Unchanged at high level: `RAYP` magic + JSON metadata + zlib blobs.

**Assets:**

- Layers store **keys** (`texture_asset_key`, `outline_texture_asset_key`, `smart_asset_key`).
- On save, referenced **`proj:`** assets are packed as `project_assets[]` + zlib blobs **before** layer pixels.
- **`ext:`** keys are promoted to **`proj:`** on save when payload/path is available.
- **`core:` / `user:`** are not packed (machine/app must provide library).

---

## 4. Runtime systems

| Component | Role |
|-----------|------|
| `rvp::PackageIO` | Read/write RVPK |
| `assets::AssetManager` | Façade: list, import, load, kind gate |
| `assets::AssetStore` | Decode/cache payloads (async) |
| `assets::AssetLibraryIndex` | Scan Core/User catalogs |
| `assets::AssetThumbCache` | 32/128 thumbs (from package or generate) |
| `BrushLibrary` | Builtins + `*.rvpbf` disk presets |
| `preview3d::ShaderPresetLibrary` | Builtins + `*.rvpcf` shader presets |

**Async contract:** UI never blocks on full decode. Missing/pending assets → solid fallback (Fill) or skip sample (Outline bake).

**Fill texture GPU path:** pattern uploaded at source resolution; composite samples with wrap UV (no full-document CPU bake unless FX requires it).

---

## 5. Directory map

```
{exe}/
  assets/                          # Core
    textures/*.rvpaf
    configs/*.rvpcf                # export templates etc.
    configs/shaders/*.rvpcf        # optional shipped shaders
  shaders/                         # app HLSL (engine)
  …

Documents/RayVPaint/
  user/config.json                 # app prefs (not RVPCF)
  assets/textures/*.rvpaf          # User library
  brushes/*.rvpbf
  presets/shaders/*.rvpcf
  temp/
```

---

## 6.1 Related plans

| Plan | Topic |
|------|--------|
| `plans/API_STANDARTIZATION.md` | Operators, AppContext, keymap, Python/document API — **foundation in code:** `src/core/ops/` (ActionCatalog + AppContext + TryConsumeAction) |
| `plans/Standartization(Krita/Photoshop).MD` | Lossy PSD/KRA/ABR interop (**after** API plan) |

## 6. What we deliberately do **not** support

- Opening old `.rvbrush` JSON brushes  
- Loading bare MaterialConfig `.json` as presets (use `.rvpcf`)  
- Treating loose Core PNGs as library assets (use `.rvpaf`)  
- SVG SmartObject rasterization (disabled until a real vector pipeline)

---

## 7. Extending the system

1. New media type → prefer **RVPAF** `kind` + resource paths.  
2. New settings pack → **RVPCF** `kind` + `config.json` schema.  
3. New tool consumer → call `AssetManager` with `AssetFilter.kind`; never re-decode paths in UI.  
4. Update this document when adding a `kind` or resource path convention.

---

## 8. Source map (implementation)

| Area | Path |
|------|------|
| Package I/O | `src/package/PackageIO.*`, `PackageTypes.h` |
| Asset core | `src/assets/*` |
| Brushes | `src/core/BrushLibrary.*` |
| Shader presets | `src/preview3d/MaterialConfig.*` |
| Fill / outline bind | `src/Canvas.*`, `src/layer/LayerStyles.*` |
| Browser UI | `src/ui/panels/AssetBrowserPanel.*`, `src/ui/widgets/UiAsset*` |
