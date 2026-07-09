# Plan: Brush system expansion (core + UI)

Date: 2026-07-10  
Status: **planning** — UI placeholders exist; paint engine mostly ignores dynamics  
Related: `plans/BRUSH_PRESETS.md`, `src/core/BrushLibrary.*`, `src/core/PaintEngine.*`, `DrawBrushPickerPopup`

---

## 0. Current state

| Layer | What works |
|-------|------------|
| **UI** | RMB brush picker, list (blue/orange), Create/Save/Delete, full param panel |
| **Library** | `.rvbrush` v1, staging, disk, builtins, tip none/builtin/embedded |
| **Engine (done A+B)** | Soft circle + tip stamp, **tip rotation**, hardness envelope on tip, **pressure→rotation**, **scatter**, **angle jitter**, spacing, pressure→radius/hardness/opacity |
| **Still later** | flow, size jitter, tip PNG load path polish, more builtins |

---

## 1. Goals (product)

1. Parameters marked `[placeholder]` become **real** in the paint path.  
2. Custom tip textures feel like PS/Krita (rotation, scatter, spacing).  
3. More presets without rewriting UI.  
4. No regression: procedural soft circle path remains default when `tip == nullptr`.  
5. Performance: stamp still tile-local; no full-layer locks.

---

## 2. Phased plan

### Phase A — Engine: rotation + tip sampling (P0)

**Scope:** Make `rotationDeg` and tip texture orientation real.

| Task | Detail |
|------|--------|
| A1 | In `StampAt`, sample tip in local space after rotation by `rotationDeg` (+ optional per-dab angle) |
| A2 | Hardness on tip path: multiply tip intensity by radial hardness envelope (optional blend) |
| A3 | `pressureRotation`: map pen pressure → angle delta (e.g. ±45°) when flag set |
| A4 | Unit/smoke: stamp soft circle vs rotated tip bit-differs as expected |

**API:** no UI change; already wired in picker.

### Phase B — Engine: scatter + angle jitter (P0/P1)

| Task | Detail |
|------|--------|
| B1 | In `DrawStrokeSegment`, for each dab offset position by random (or seeded) disk: `scatter * radius` |
| B2 | Per-dab `angleJitter * 360°` added to rotation (seeded per stroke for stability) |
| B3 | Deterministic RNG from stroke begin seed (reproducible undo feel) |
| B4 | Preview in UI already fakes scatter — keep visual aligned with engine formula |

### Phase C — Dynamics pack (P1)

| Task | Detail |
|------|--------|
| C1 | **Flow** (0–1): multi-pass opacity build-up separate from opacity |
| C2 | **Size jitter / min size** under pressure |
| C3 | **Spacing** auto-scale by tip `spacingMul` (already partial) |
| C4 | Optional **smudge** preset params in same library later |

Extend `BrushPresetParams` + `.rvbrush` version → **2** when new fields ship.

### Phase D — Tips pipeline (P1)

| Task | Detail |
|------|--------|
| D1 | Load tip from PNG grayscale via `ImageManager` → `BrushLibrary` embed |
| D2 | UI “Load tip texture…” already partial — ensure `tipSourcePath` + embed on Save |
| D3 | Optional tips folder: `%AppData%/RayVPaint/brushes/tips/` referenced as `tip.type=file` |
| D4 | Thumbnail cache for tip SRV (GPU) if list becomes heavy |

### Phase E — More builtins & organization (P2)

| Task | Detail |
|------|--------|
| E1 | Extra builtins: Soft Airbrush, Ink Pen, Chalk (procedural tip gens) |
| E2 | Optional tags/categories in meta (`ink`, `texture`, `air`) for UI filter |
| E3 | Favorites / recent ids in `config.json` (UI) |

### Phase F — Performance (P1/P2)

| Task | Detail |
|------|--------|
| F1 | Precompute rotated tip for common angle buckets when radius large |
| F2 | Skip empty tip texels earlier (already partial) |
| F3 | Spacing floor by device pixel / zoom for tiny brushes |

---

## 3. `.rvbrush` versioning

```
version 1: current (radius, hardness, opacity, spacing, pressure*, tip, rotation/scatter placeholders stored)
version 2: + flow, sizeJitter, seedPolicy, tip.file path scheme
```

Readers: accept `version >= 1`, ignore unknown fields.

---

## 4. Ownership split

| Owner | Work |
|-------|------|
| **Core (this agent)** | Phases A–B–C–D1/D3–F (engine + library schema) |
| **UI agent** | Picker polish, tip load dialog UX, categories, favorites, live preview matching engine |

Do **not** reimplement `BrushLibrary` in UI; only call APIs.

---

## 5. Suggested implementation order (core)

```
A1 rotation sampling  →  A3 pressureRotation
  → B1 scatter  →  B2 angleJitter
  → C1 flow (optional)
  → D1 tip PNG load API
  → F1 cache if needed
```

Each step: `BrushLibrary::RunSmokeTest` still green; add paint-unit checks if available.

---

## 6. Explicit non-goals (now)

- AI brush / style transfer  
- Vector brushes  
- Full Krita brush engine port  
- Per-project brush packs in `.rayp` (library stays user-global)

---

## 7. Acceptance criteria

- [x] Rotation slider changes stamp orientation on canvas (not only preview) — Phase A  
- [x] Scatter > 0 spreads dabs; 0 = current line — Phase B  
- [x] Angle jitter randomizes tip orientation along stroke — Phase B  
- [x] Save/load `.rvbrush` keeps new params (already in library)  
- [x] Builtins still non-deletable; empty folder → 4 builtins  
- [x] UTF-8 paths open images with Cyrillic names (PathUtil fix)

---

## 8. Quick reference — placeholder → engine field

| UI label | `BrushSettings` | Engine use (after plan) |
|----------|-----------------|-------------------------|
| Rotation | `rotationDeg` | tip/local UV rotate |
| Pressure → Rotation | `pressureRotation` | angle *= f(pressure) |
| Scatter | `scatter` | dab XY offset |
| Angle jitter | `angleJitter` | dab angle noise |
| Tip texture | `tip` / embedded | already stamps |
