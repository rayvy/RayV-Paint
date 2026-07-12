# For Future — open holes before / during combat testing

Date: 2026-07-12  
Branch: main (post Build 15)

**Build 16 plan (cleanup / UI kit / Asset Browser):**  
→ `plans/BUILD16_CLEANUP_OPTIMIZATION.md` · `plans/UI_RULES.md` · `plans/ASSET_BROWSER.md`

**Out of scope here:** exhaustive DDS format matrix, embedding DirectXTex (tracked separately as product goal).

---

## High risk (real projects)

| # | Item | Notes |
|---|------|--------|
| 0 | **GPU-driven Layer Effects / Filters** | **TODO (priority).** Today styles + filters are **CPU bake** (`BuildPresentation` / `ApplyPixelFilters` → float buffer → tile upload). Composite is GPU, but FX themselves are not. Peers (Photoshop Live, Substance, game post) run outline/shadow/HSV/curves as **GPU passes** (often dirty-region / stack). Target: full **GPU-driven FX** — no full-doc CPU re-bake on paint; paint base, shader stack for preview, bake only for export/freeze. Especially **styles** (shadow/outline); then **filters** (HSV/curves/blur). Interim mitigations already: stroke skips filter rebuild, Effects Preview off, proxy bake ≤1536. |
| 1 | **Large docs (4K–16K)** | Full GPU layer texture; flat selection/mask; full-frame float export can OOM |
| 2 | **Depth dump = U8 view** | D32 opens as grayscale U8 (not raw F32 Z). Height maps: use R32F |
| 3 | **Bit-depth convert has no undo** | Accidental U8↔F32 cannot be undone |
| 4 | **HDR viewport** | No tone-map; truth is pipette / float UI only |
| 5 | **Select / wand / filters** | Many paths still U8/OpenCV; float bucket OK, wand/filters may quantize |
| 6 | **Export composite memory** | Some paths still allocate full-doc float |

---

## Medium — core polish

| # | Item |
|---|------|
| 7 | SVG smart object = checker placeholder (bytes kept) |
| 8 | Transform floating = full-doc GPU texture |
| 9 | Mask paint undo incomplete *(partially fixed B15 — tiled delta; keep watch)* |
| 10 | Brush dynamics (flow, size jitter, Phase C–F) unfinished |
| 11 | Smudge / some tools vs undo history edge cases |
| 12 | .rayp multi-layer F16 edge cases |
| 13 | 16-bit PNG load/export |
| 14 | Operators (blur/HSV/curves) not full float pipeline |
| 15 | Optional: open depth dumps as F32 storage |
| 16 | Optional: undo for document bit-depth convert |
| 17 | Optional: height tool preset |

---

## UI / UX (Part 2-ish)

| # | Item |
|---|------|
| 18 | Float color picker polish (HSV still 0..1) |
| 19 | Gizmo / hotkeys / panels remaining chrome |
| 20 | Layer thumbs (opaque RGB list) — done |
| 21 | Channel preview from tiles (no lag on composite) — done |
| 22 | ~~Invent A=0 RGB~~ — **reverted** (real transparency when A channel on) |
| 23 | Merge layers + Alpha Rewrite — done |

---

## Combat-test focus list

1. U8 BC7/PNG open → paint → Quick export  
2. R32F height (NVIDIA/PS FourCC 114) open → paint → save  
3. F32 new doc multi-layer → .rayp  
4. Convert U8↔F32 mid-size only first  
5. UTF-8 paths  
6. Long undo chains + transform  

---

## Architecture debt — Layer FX GPU (expanded)

**Status:** TODO · not Build 16 scope unless time · do **after** stability of stroke-skip / Effects Preview OFF.

**Current (CPU-driven):**
- Styles: `layer_fx::BuildPresentation` on CPU (proxy optional)
- Filters: full-frame `ApplyPixelFilters` on dirty
- Then `TileCache` / D3D upload; DX11 only composites baked RGBA

**Target (GPU-driven):**
1. Keep **base content** tiles as source of truth (paint target unchanged)
2. Per-layer or stack **FX pass(es)** in HLSL (outline expand, shadow blur separable, HSV/curves LUT textures)
3. Interactive path: re-run GPU FX only for dirty layers/tiles — **not** full CPU float doc
4. Export/rasterize: optional high-quality bake or same GPU path readback
5. Compatibility: existing style/filter params map 1:1; no user-facing API break

**Why:** peer tools feel instant on 1K–2K with FX; our CPU bake cannot scale. Interim UI toggle does not replace this.

Log: `%USERPROFILE%/Documents/RayVPaint/user/rayv_paint.log`
