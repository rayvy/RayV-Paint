# Standardization (Krita / Photoshop)

**Status:** Plan only — not an implementation ticket yet.  
**Audience:** agents and humans preparing RayV Paint for industry-adjacent interchange.  
**Canonical product formats:** `Documentation.MD` (`.rayp`, RVPAF / RVPCF / RVPBF).  
**Krita perf reference in-repo:** `krita-reference/analyse.md` (tiles/swap/async — **not** full layer taxonomy).

---

## 0. Executive summary

| Goal | Reality today | Strategy |
|------|---------------|----------|
| Import **Krita `.kra`** | No reader | Long-term; ZIP+XML + tiled pixels |
| Import **Photoshop `.psd`/`.psb`** | No reader | Prefer **PSD as primary foreign document format** (widest ecosystem) |
| Import **ABR / Krita brush** | No reader | Python converter → **`.rvpbf`** |
| Export layered foreign formats | Only flat PNG/DDS + `.rayp` | Phase later; fidelity will stay **lossy** for years |
| Python as converter hub | `rayv` module is smoke-only | **Blocked on** `plans/API_STANDARTIZATION.md` (operators + document API + Python surface) |

**Hard truth (user-aligned):** the app is **not ready** for lossless Krita/Photoshop interchange. Any converter must be framed as **best-effort import** with explicit **data-loss tables**, not “open PSD like native”.

**Prerequisite (separate plan):** do **not** expand interop until operator/context/API foundation exists — see **`plans/API_STANDARTIZATION.md`**. That plan owns Blender-like operators, AppContext, keymap, footer Context debug, and the Python/document surface formerly called “Phase A” here.

---

## 1. What we standardize (scope of this plan)

### 1.1 In scope

1. **Document / layer stack model** — map between RayV internal model and Krita/PS concepts.  
2. **Brush preset model** — map between RayV paint params + tip and Krita/PS brush subsets.  
3. **Converter architecture** — Python middleware + C++ document/brush API.  
4. **Fidelity policy** — what is preserved, baked, dropped, or deferred.

### 1.2 Out of scope (this plan)

- Becoming a full Krita brush engine or full PS style engine.  
- Cloud PSDC, proprietary smart filters, full vector tools.  
- Perfect round-trip (RayV → PSD → RayV with zero loss).  
- Replacing `.rayp` as the **authoritative** native document.

### 1.3 Success criteria (when phases complete)

| Milestone | Definition of done |
|-----------|-------------------|
| **M0 (external)** | `API_STANDARTIZATION` Phase **O4** done: operators + Python can open/save `.rayp`, layers, pixels, masks, simple brush ops |
| **M1 Brush import** | Import common **ABR** (subset) and/or **Krita `.kpp`** (subset) → `.rvpbf` + tips |
| **M2 PSD import (lossy)** | Open multi-layer PSD as RayV document: raster + groups + opacity/blend/mask where mappable; rest baked or dropped with report |
| **M3 KRA import (lossy)** | Same for `.kra` via ZIP+XML + paint device streams |
| **M4 Report UX** | Import produces human-readable loss report (log + optional UI panel) |

---

## 2. Current RayV inventory (baseline)

### 2.1 Layers & effects

**Types** (`Layer::Type`): Raster, Group, Fill, SmartObject, VectorSvg.

| Feature | Status | Notes for converters |
|---------|--------|----------------------|
| Raster pixels | Production | `TileCache` 256×256, COW-ish |
| Groups | Partial | Hierarchy via `parentGroupId`; composite isolation incomplete vs PS pass-through |
| Fill layers | Production | Multi-map RGBA + texture asset keys — **game/texture-set native**, not PS solid/pattern fill |
| SmartObject | Foundation | Asset key + display cache; no linked SO, no smart filters |
| VectorSvg | Stub | Import **disabled** |
| Blend modes | 10 modes | Normal, Multiply, Screen, Overlay, Add, Subtract, Darken, Lighten, HardLight, SoftLight |
| Opacity | Yes | Single opacity (no separate **Fill %** vs **Opacity %**) |
| Layer mask | Yes | Single mask; no invert/disable/link flags in model |
| ND filters | Limited | Blur, HSV, Curves, AlphaInvert, Noise — on layer, not adj-layer type |
| Layer styles | Limited | **Drop Shadow** + **Outline** only (outline ≈ stroke subset) |
| Clipping masks | No | |
| Adjustment layers | No | Filters live on raster instead |
| Text / shape layers | No | |
| Color spaces per layer | No | Doc bit-depth only (U8 / F16 / F32) + export ICC |

**Serialization:** `.rayp` v2 — JSON layer tree + zlib float RGBA tiles + optional mask/smart bytes + `project_assets`.

### 2.2 Brushes

**Live paint params** (`BrushSettings` + `PaintEngine`):

- Radius, hardness, opacity, spacing, stabilization  
- Channel write R/G/B/A, erase, rgbMorphOnly  
- Pressure → radius / hardness / opacity; rotation used; scatter / angle jitter implemented  
- Grayscale tip stamp or procedural circle  
- Brush blend mode (same enum as layers), clone stamp offsets  

**Presets:** `.rvpbf` (RVPK + `config.json` + optional `tip.raw8`). FG color not stored.

**Not implemented (placeholders / gaps):** full PS dual brush, texture depth, separate **flow**, tilt/barrel curves, color dynamics, multi-engine paintops (Krita).

### 2.3 Python today

`ScriptingEngine` → module `rayv`:

- log_*, config size, zoom/pan, `load_image`, `save_image` / `save_dds`, canvas size, tile/memory diagnostics  

**Missing for converters:** `.rayp` I/O, layer graph, pixels, masks, styles, brushes, assets.

### 2.4 File open surface

| Format | Open |
|--------|------|
| `.rayp` | Yes |
| PNG/JPG/BMP/TGA | Yes (flat → layer) |
| DDS | Yes (strong) |
| SVG | Explicitly **disabled** |
| PSD / PSB / KRA / ORA / ABR / KPP | **No** |

---

## 3. External reference models

### 3.1 Photoshop document (PSD/PSB)

Authoritative structure (Adobe File Formats Spec, 2019+):

1. File header (`8BPS`, channels, size, depth, color mode)  
2. Color mode data  
3. Image resources  
4. **Layer and mask information** (layer records, channel image data, blend mode keys, opacity, clipping, masks, tagged blocks for effects)  
5. Merged image data  

**Layer record concepts relevant to mapping:**

- Bounds rectangle (layer may be smaller than canvas)  
- Per-channel data (R/G/B + transparency −1 + user mask −2)  
- Blend mode 4-char key (`norm`, `mul `, `scrn`, `over`, … dozens)  
- Opacity 0–255, clipping flag, visibility flags  
- Layer mask block, blending ranges  
- Additional layer info: effects (`lrFX` / `lfx2`), smart objects, solid fills, etc.

**Smart Objects (product model, not full binary decode here):**

- Embedded document inside host  
- Linked variants, rasterize / edit contents  
- Smart filters as non-destructive stack on SO  

**RayV mapping implication:** SO → `SmartObject` + `smartAssetKey` **or bake to Raster**; smart filters → bake or RayV ND filters when 1:1.

**Layer styles (PS):** Drop Shadow, Inner Shadow, Outer/Inner Glow, Bevel & Emboss, Satin, Color/Gradient/Pattern Overlay, Stroke, Contour, global light.  
**RayV:** only Shadow + Outline → import must **bake or drop** most FX.

### 3.2 Photoshop brushes (ABR)

- Proprietary binary resource collections (brush tips + dynamics).  
- Community parsers exist (e.g. ABR readers in Python/JS); **not** an official simple JSON schema.  
- Practical import: tip grayscale + size + spacing + a few dynamics → RayV subset.

### 3.3 Krita document (`.kra`)

From Krita docs + architecture:

- `.kra` ≈ **ZIP** (ODF-like packaging); can inspect as zip.  
- Contains structure XML + per-layer pixel stores + `mergedimage.png` (preview / interchange).  
- `.krz` = compressed kra without merged preview.

**Runtime model (from `krita-reference/` + analyse.md):**

```
KisImage
  └── KisGroupLayer (root)
        └── KisNode tree (paint layers, groups, masks as nodes)
              └── KisPaintDevice → 64×64 sparse COW tiles
```

**Composition:** dirty patches (512×512), async merger, `KoCompositeOp` blend registry.

**Not in `krita-reference` tree (must use full Krita sources / impex):**

- Full layer type catalog  
- Mask types  
- Composite op table  
- `plugins/impex/kra` and `plugins/impex/psd`  
- Brush engine / `.kpp` paintops  

### 3.4 Krita brushes

- Multi-engine: pixel, color smudge, spray, hatch, deform, filter, …  
- Presets often `.kpp` (XML + resources) and shared resource system  
- **RayV non-goal:** full paintop port (`plans/BRUSH_ENGINE_EXPANSION.md`)  
- **Goal:** import **pixel-brush subset** → `.rvpbf`

---

## 4. Capability matrix (honest)

### 4.1 Layers

| Concept | PS | Krita | RayV | Converter action |
|---------|----|-------|------|------------------|
| Raster layer | ✓ | ✓ | ✓ | Map 1:1 (with size/offset) |
| Group | ✓ | ✓ | Partial | Map hierarchy; pass-through may bake |
| Opacity | ✓ | ✓ | ✓ | Map |
| Fill opacity (separate) | ✓ | — | ✗ | Fold into opacity or drop |
| Blend modes (full set) | ✓ | ✓ | Subset | Map known; rest → Normal + warn |
| Layer mask | ✓ | ✓ (multi) | 1 mask | First mask only; rest drop/bake |
| Vector mask | ✓ | ✓ | ✗ | Rasterize or drop |
| Clipping | ✓ | ✓ | ✗ | Bake clip group or drop |
| Drop shadow | ✓ | limited | ✓ | Map approximate |
| Stroke / outline | ✓ | limited | Outline | Approximate |
| Other layer FX | ✓ | limited | ✗ | **Bake to pixels** or drop |
| Adjustment layers | ✓ | filter layers | ✗ | Bake or convert to ND filter if mappable |
| Fill / solid color | ✓ | fill layer | Fill (maps) | Approximate as Fill or Raster |
| Smart Object | ✓ | file layers-ish | Foundation | Embed as SO or rasterize |
| Text | ✓ | ✓ | ✗ | Rasterize |
| Texture set / multi-map | — | — | **Native strength** | Export side only |

### 4.2 Brushes

| Concept | PS | Krita | RayV | Converter action |
|---------|----|-------|------|------------------|
| Tip stamp | ✓ | ✓ | ✓ | Map |
| Size / opacity / spacing | ✓ | ✓ | ✓ | Map |
| Hardness (procedural) | ✓ | ✓ | ✓ | Map |
| Flow | ✓ | ✓ | ✗ | Map → opacity approx + warn |
| Pressure size/opac | ✓ | ✓ | ✓ | Map |
| Scatter / angle | ✓ | ✓ | Basic | Map subset |
| Dual brush / texture | ✓ | ✓ | ✗ | Drop or flatten tip |
| Color dynamics | ✓ | ✓ | ✗ | Drop |
| Smudge engines | ✓ | ✓ | Separate tool | Don’t map as brush preset |
| ABR / KPP file | ✓ | ✓ | ✗ → RVPBF | Python import |

---

## 5. Architecture: converter sandwich

```
┌─────────────────────────────────────────────────────────────┐
│  UI / File Explorer: Open → detect format → “Import foreign” │
└────────────────────────────┬────────────────────────────────┘
                             │
         ┌───────────────────▼───────────────────┐
         │  C++ host (from API_STANDARTIZATION)   │
         │  OperatorRegistry + AppContext         │
         │  rayv.ops / document / layers API      │
         │  (Phase O4 — not defined in this file) │
         └───────────────────┬───────────────────┘
                             │ pybind11
         ┌───────────────────▼───────────────────┐
         │  Python package: rayv_interop          │
         │  ┌─────────────┐  ┌─────────────────┐  │
         │  │ psd_import  │  │ kra_import      │  │
         │  │ (psd-tools/ │  │ (zip+xml+tiles) │  │
         │  │  custom)    │  │                 │  │
         │  └──────┬──────┘  └────────┬────────┘  │
         │         │                  │           │
         │  ┌──────▼──────────────────▼────────┐  │
         │  │ Intermediate IR (JSON + arrays)  │  │
         │  │ DocumentIR / BrushIR             │  │
         │  └──────────────────┬───────────────┘  │
         │                     │ map + loss report│
         │  ┌──────────────────▼───────────────┐  │
         │  │ ops.invoke / document builders   │  │
         │  └──────────────────────────────────┘  │
         └───────────────────────────────────────┘
```

### 5.1 Why Python here

| Task | Python advantage |
|------|------------------|
| ZIP + XML (`.kra`) | stdlib + lxml |
| PSD parse | `psd-tools`, `pytoshop`, etc. (verify license) |
| ABR parse | community ABR readers |
| PNG/decode intermediates | Pillow |
| Prototyping mappers | Fast iteration |

| Task | Stay in C++ |
|------|-------------|
| Authoritative `.rayp` write | Existing `SaveCanvasRayp` |
| Tile upload / GPU | Canvas |
| Hot paint path | PaintEngine |
| Package formats RVP* | PackageIO |

### 5.2 Intermediate IR (DocumentIR)

JSON-friendly, **converter-owned**, not user-facing:

```json
{
  "schema": "rayv.document_ir/1",
  "width": 1024,
  "height": 1024,
  "bit_depth": "u8",
  "color_mode": "rgb",
  "layers": [
    {
      "id": "uuid",
      "name": "Layer 1",
      "type": "raster|group|fill|smart|rasterized_foreign",
      "parent_id": null,
      "visible": true,
      "opacity": 1.0,
      "blend": "normal|multiply|…|unknown",
      "blend_original": "div ",
      "bounds": [0, 0, 1024, 1024],
      "pixels_ref": "blobs/layer0.rgba8",
      "mask_ref": null,
      "styles": [],
      "filters": [],
      "warnings": ["blend mode 'div ' mapped to normal"]
    }
  ],
  "import_report": {
    "source_format": "psd",
    "dropped": ["layer_fx:bevel", "adjustment:curves_layer"],
    "baked": ["smart_object:Logo"]
  }
}
```

**BrushIR** similarly → maps into RVPBF config + tip.raw8.

### 5.3 Fidelity classes

| Class | Meaning | User-facing |
|-------|---------|-------------|
| **A Preserve** | Semantic + visual match within RayV | No warning |
| **B Approximate** | Same intent, different math | Soft warning |
| **C Bake** | Flattened to raster | Warning + baked name suffix |
| **D Drop** | Unsupported | Error/warning list |

Default import mode: **B+C**, never silent D without report.

---

## 6. Prerequisites before any PSD/KRA open button

These are **blockers**, not optional polish.

### 6.1 P0 — Editor API foundation (external plan)

**Owned entirely by `plans/API_STANDARTIZATION.md`:**

- Operator registry + `AppContext` + poll (fixes Backspace/text conflicts)  
- Keymap → operator ids  
- Footer **Context** debug panel  
- Python: `rayv.ops.invoke`, document/layers/pixels/rayp (**Phase O4**)  

**Do not re-specify that work here.** Interop starts only after O4 exit criteria.

### 6.2 P0 — Python interop package (after O4)

1. `rayv_interop` under `src/scripting/python/rayv_interop/`.  
2. Optional extras: `[psd]`, `[kra]` with fail-soft if missing.  
3. Logging through `rayv.log_*` / ops only.  
4. **DocumentIR / BrushIR** schemas live under `plans/ir/` when B/C start (not in API plan).

### 6.3 P1 — RayV model gaps that reduce import pain

| Gap | Why converters need it | Phase |
|-----|------------------------|-------|
| Expand blend mode enum (at least Difference, Exclusion, Hue, Saturation, Color, Luminosity, ColorBurn/Dodge if math available) | Fewer D-class drops | Before M2 |
| Layer **offset / bounds** independent of full canvas write | PSD/KRA layers rarely full-frame | Before M2 |
| Group composite parity | Nested stacks | M2 quality |
| Fill % vs Opacity (optional field) | PS fidelity | Optional |
| Clipping mask flag (even if composite later) | Store intent | Optional |
| Serialize more style fields / bake FX helper | PSD effects | M2 |
| Flow + dynamics curves in brush schema | ABR subset | M1 |

### 6.4 P1 — Explicit “lossy import” UX

- Dialog: “Import as RayV document (lossy)”  
- Checklist of preserved features  
- Post-import report panel / log file next to document  

---

## 7. Workstreams & phases

> **Phase A removed.** Foundation API / operators / Python document surface →  
> **`plans/API_STANDARTIZATION.md`** (phases O0–O5).

### Phase B — Brushes (easier, high user value)  
**Depends on:** API plan O4 (brush apply + rvpbf) at minimum.

| ID | Work | Notes |
|----|------|-------|
| B1 | BrushIR schema | Subset of RVPBF |
| B2 | ABR tip extract (Python) | Size, spacing, tip pixels; ignore dual/texture initially |
| B3 | Optional Krita `.kpp` pixel-brush subset | Parse tip + basic params only |
| B4 | Write `.rvpbf` via PackageIO / rayv.brushes | |
| B5 | UI: Import brushes… → library folder | |
| B6 | Manual fidelity suite | 10 ABR fixtures |

**Exit:** User can import a folder of ABR tips into RayV brush library with report.

### Phase C — PSD import (lossy document)

| ID | Work | Notes |
|----|------|-------|
| C1 | Choose library (`psd-tools` vs custom) + license check | Prefer maintained pure Py |
| C2 | PSD → DocumentIR: raster + groups + opacity + blend map | |
| C3 | Channel assembly (RGB + alpha) → RGBA8 | Downscale 16/32-bit to RayV bit depth policy |
| C4 | Layer mask → RayV mask | |
| C5 | Layer FX: map DropShadow/Stroke approx; bake rest | |
| C6 | Smart Object: rasterize composite of SO content | |
| C7 | Adjustment layers: bake or drop | |
| C8 | Wire File Explorer / OpenDocument route for `.psd`/`.psb` | |
| C9 | Large doc policy (tile streaming, max dim, RAM budget) | Align with 16K lessons from Krita analyse |

**Exit:** “Open PSD” yields usable RayV doc for typical illustration PSDs; report lists losses.

### Phase D — KRA import (lossy)

| ID | Work | Notes |
|----|------|-------|
| D1 | Unzip + parse maindoc XML (layer tree) | Full Krita impex as reference |
| D2 | Decode paint device tile streams where possible | May start with `mergedimage.png` + layer previews only |
| D3 | Map blend/opacity/visibility/groups | |
| D4 | Prefer path: if hard, **Krita export PSD then C** as interim | Pragmatic |

**Exit:** Usable import for simple Krita files; complex animation/filter stacks reported.

### Phase E — Export (optional, later)

| ID | Work |
|----|------|
| E1 | Export flat PSD (merged + optional single layer) |
| E2 | Export multi-layer PSD (raster only, no FX) |
| E3 | Export ORA (simpler open stack) as alternative |
| E4 | Never claim full KRA export until model parity |

---

## 8. Blend mode mapping (initial)

| RayV | PS key (examples) | Krita composite (examples) |
|------|-------------------|----------------------------|
| Normal | `norm` | normal |
| Multiply | `mul ` | multiply |
| Screen | `scrn` | screen |
| Overlay | `over` | overlay |
| Darken | `dark` | darken |
| Lighten | `lite` | lighten |
| HardLight | `hLit` | hard_light |
| SoftLight | `sLit` | soft_light |
| Add | `lddg` / linear dodge-ish | addition |
| Subtract | `fsub` | subtract |

**Unmapped foreign modes →** Normal + **B/D warning** until RayV expands enum/math in `Canvas`/`LayerStyles` blend shaders.

---

## 9. Math / engine notes (converter-relevant)

### 9.1 Layer composite

- RayV GPU path: `PSLayerBlend` + CPU fallbacks for export (`ComposeVisibleLayersRGBA8`).  
- PS blend keys are **not** identical math to all engines; even “Multiply” can differ with gamma / color space.  
- Import policy: treat as **intent**, not pixel-perfect PS.

### 9.2 Shadow / outline

- RayV shadow: distance, angle, offset, spread, size blur, color, opacity.  
- PS Drop Shadow: more (contour, noise, layer knocks out, global light).  
- Map subset; ignore contour/noise initially (**B**).

### 9.3 Brush dabs

- RayV: spacing accumulator, hardness falloff (procedural), tip grayscale multiply.  
- PS/Krita: more transfer curves and dual tips.  
- Import tip **bitmap** + size/spacing/opacity/pressure flags first (**A/B**).

### 9.4 Color depth

| Source | RayV policy (proposal) |
|--------|------------------------|
| PSD 8-bit RGB | Direct U8 |
| PSD 16/32-bit | Convert to doc bit depth with clamp + warn |
| Krita float | Same |
| CMYK / Lab PSD | Convert via library → RGB + strong warn |

---

## 10. Python integration risks & fixes

| Risk | Mitigation |
|------|------------|
| Embed path broken | Single `python_runtime` layout; integration test at startup |
| GIL + UI freeze | Run converters on worker thread; marshal results to main |
| Huge PSD RAM | Stream layers; max dimension guard; refuse or proxy open |
| Dependency bloat | Optional extras: `rayv_interop[psd]`, `rayv_interop[kra]` |
| Security | Don’t exec arbitrary Python from PSD; only load data parsers |
| Dual sources of truth | Only C++ writes `.rayp`; Python never patches zlib by hand |

---

## 11. Testing strategy

### 11.1 Fixtures (keep small, legal)

- Synthetic PSD: 3 rasters + group + mask + multiply  
- Synthetic KRA: zip hand-built or exported from Krita  
- 5 ABR files (simple round tips)  
- Golden `.rayp` hashes for IR builder  

### 11.2 Regression

- Visual: composite PNG vs reference (tolerance)  
- Structural: layer count, names, blend, opacity  
- Report: expected warnings for unsupported FX  

### 11.3 Non-goals in tests

- Pixel-perfect vs Photoshop render  
- Smart filter stacks  
- CMYK fidelity  

---

## 12. Recommended priority order

```
0) API_STANDARTIZATION O0–O4   ← operators, context, Python/doc API
1) B2–B5  ABR → RVPBF          ← after O4 brushes
2) C1–C8  PSD lossy import     ← primary foreign doc format
3) D*     KRA (or Krita→PSD interim)
4) E*     Export layered PSD (optional)
```

**Do not** start PSD UI until **API_STANDARTIZATION O4** is done — otherwise Python fights an incomplete host.

---

## 13. Mapping to existing RayV plans

| Existing plan | Relation |
|---------------|----------|
| **`API_STANDARTIZATION.md`** | **Prerequisite** — operators, AppContext, keymap, Python/doc API |
| `Documentation.MD` | Native formats stay canonical; foreign is side door |
| `krita-reference/analyse.md` | Perf for large imports (tiles, no flat 16K forever) |
| `BRUSH_ENGINE_EXPANSION.md` | Complements B*; still no full Krita engine |
| `ASSET_BROWSER.md` / RVPAF | Imported textures land as project/user assets |
| SmartObject foundation | Target for PS SO / Krita file layers (lossy) |

---

## 14. Open decisions (need human product input later)

1. **Primary foreign format:** PSD-first (recommended) vs KRA-first.  
2. **Max import size:** e.g. refuse >8K or force proxy.  
3. **Default for unmapped blend:** Normal vs Multiply vs bake.  
4. **Dependency policy:** pure Python only vs allow numpy/Pillow.  
5. **Whether to expand blend modes before first PSD ship** (quality vs schedule).

---

## 15. One-page honesty statement (for UI copy)

> Import from Photoshop/Krita is **best-effort**.  
> RayV Paint is optimized for **texture-set and layered raster work**, not full Creative Cloud / Krita feature parity.  
> Layer effects, adjustment layers, advanced brushes, and vectors may be **rasterized or skipped**.  
> A full import report lists everything that changed.

---

## 16. Next concrete engineering step (when executing)

1. Execute **`API_STANDARTIZATION.md`** (O0 Context panel → O1 Fill/Undo ops → … → O4 Python/doc).  
2. Spec freeze **DocumentIR v1** + **BrushIR v1** in `plans/ir/`.  
3. IR builder creates 2-layer `.rayp` from fixture via ops/API.  
4. Only then wire ABR or PSD prototype.

---

*End of plan. Implementation is a separate multi-phase program; this document is the interop/fidelity roadmap only.*
