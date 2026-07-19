# GPU-Driven Pipeline & Layer FX — Standalone Architecture Plan

**Status:** Proposal / design only — **not** Build 16 continuation  
**Repo path:** `plans/GPU_DRIVEN_PIPELINE.md` (canonical)  
**Work branch:** `GPU-DRIVEN-PARADIGME` (do not land risky code on `main` until phase gates pass)  
**Rollback:** `git checkout main` — main never receives incomplete GPU pipeline work  
**Date:** 2026-07-19  
**Scope class:** Ambitious · multi-session · high risk if rushed  

---

## 0. Why this is a separate plan

This is **not** “port BuildPresentation to HLSL and ship.”

You asked for a system that is **coherent across**:

| Surface | Today | Target |
|---------|--------|--------|
| Layer FX (filters/styles) | CPU bake → upload | GPU interactive stack |
| Tools (brush, stamp, mask paint…) | CPU TileCache only | GPU live preview + ordered commit to tiles |
| Core (undo/composite/export) | Tiles = truth | **Tiles remain truth**; GPU never invents history |
| Huge docs (8K→128K) | Proxy + lag | Op-queue + provisional GPU + catch-up workers |

Past pain (undo tile holes, async FX ghosts, display≠export under proxy) happened when **preview, truth, and history** got out of phase. This plan’s first job is to **lock phases**, not to chase FPS.

Related (do not merge into this plan as scope creep):

- `plans/ForFuture.md` #0 — original FX debt note  
- `ai-answers/GpuFxDispatch.*`, `LayerFx.hlsl` — prototypes **not** in production CMake  
- `plans/EMERGENCY_SAFE_PLAN.MD` — memory / COW budgets  
- Build 16 — **closed enough**; this plan starts only after B16 is stable for you  

---

## 1. Current architecture (facts)

### 1.1 Source of truth (non-negotiable today)

```
tileCache (CPU sparse 256² COW TileData)
    │
    ├─ filteredCache      ← ApplyPixelFilters (CPU)
    ├─ presentationCache  ← BuildPresentation (CPU, proxy ≤1536 interactive)
    └─ GPU textures / GpuTileStore  ← upload only
           └─ composite RT (U8, often proxy-sized)
                  └─ viewport
```

- **Paint** writes only `tileCache` via `PaintEngine` + `BackupTile` / `SealActiveStrokeDeltas`.  
- **Undo** restores tiles and **kills** FX caches (`gpuDisplayKind=0xFF`) so holes don’t reappear.  
- **Export** does **not** read composite RT; it full-quality bakes CPU then `ComposeVisibleLayersRGBA8`.  
- Mid-stroke: FX bake **skipped** (raw tiles). Stroke end: sync filter/style bake (async was abandoned after ghost tiles).

### 1.2 What is already GPU

Composite stack (`PSLayerBlend`), fill patterns, viewport present, optional tiled atlas upload.  
`LayerFxBlur.hlsl` exists but **is not** on the interactive filter path (seams).

### 1.3 Bottlenecks

1. CPU FX bake (styles force full textures, kill tiled GPU path).  
2. Proxy bake quality ≠ export full bake.  
3. Full-doc float peaks on large FX / export.  
4. Stroke-end sync hitch when styles/filters heavy.  
5. Brush itself is fine at tile scale; **perceived** lag is often FX + upload + proxy.

---

## 2. Product vision (your 128K idea, formalized)

### 2.1 Dual clock

| Clock | Job | Latency goal |
|-------|-----|----------------|
| **Interactive (GPU)** | Show “visually honest” provisional result | 1 frame budget |
| **Authoritative (CPU/GPU workers)** | Commit durable pixels into `tileCache` (+ later FX bake) | Catch-up; may lag |

User can keep inputting. System maintains **operation order** (see §5). GPU shows a **provisional** stack; truth advances when ops complete.

### 2.2 What “provisional” means

- **Visually:** same blend modes, opacity, mask, as close as possible to final.  
- **Technically:** may be lower-res, lower bit-depth, approximate blur kernel, missing distant tiles.  
- **Never** written into undo history until authoritative commit finishes.

### 2.3 Scope of this program (phased products)

| Phase | Name | Ships value without 128K |
|-------|------|---------------------------|
| **P0** | Contracts + single truth | Docs, invariants, telemetry, no behavior change |
| **P1** | GPU FX preview (layers) | Interactive styles/filters on GPU; export still CPU full bake |
| **P2** | GPU stroke preview (tools) | Live brush ring on GPU over base; CPU still stamps tiles |
| **P3** | Operation queue + catch-up | Ordered async commit for large docs |
| **P4** | Unified export path | Export = same graph as preview (GPU full-res readback **or** proven-identical CPU) |
| **P5** | Extreme sizes (32K–128K) | Sparse everything; op-order UX; hard memory policy |

**Do not skip P0–P1 for P5.** That is how undo holes return.

---

## 3. Answers to your four questions

### 3.1 Are other systems consistent with this?

| System | Compatible if… | Breaks if… |
|--------|----------------|------------|
| **Undo / COW** | Only seals `tileCache` after authoritative write; restores still nuke FX caches | Seal before CPU write finishes; mutate shared `TileData` from GPU path |
| **Selection / mask** | Stay CPU `uint8_t` / `MaskTiles`; GPU uploads mirrors | Selection only on GPU without `SelectionCommand` |
| **Composite** | Preview SRVs feed same blend shader | Composite RT used as document pixels |
| **Export** | Full-quality bake/readback into same cache graph | Export while provisional queue non-empty without flush |
| **.rayp** | Stores tiles + FX *params*, not provisional textures | Serializing GPU RTs as content |
| **Operators / AppContext** | New ops: `core.ops.fx.rebuild`, `core.ops.pipeline.flush` | Ad-hoc main.cpp bypass of queue |
| **Python** | `rayv.doc` reads truth after flush | Sample composite mid-queue |

**Verdict:** Yes, **if** we keep “tiles = truth, GPU = presentation.” Same lesson as undo/FX ghost bugs.

### 3.2 Conflicts, crashes, “optimizing into death”?

| Risk | Likelihood if rushed | Mitigation |
|------|----------------------|------------|
| Tile holes after undo | High | Keep `ApplyPaintStrokeDeltas` invalidation; never share provisional buffers with history |
| Ghost tiles from async FX | High | Explicit queue states; no “mark clean” until upload complete |
| VRAM explosion (per-layer full FX RT @ 16K) | High | Dirty-region / atlas / proxy tiers; styles without full classic textures |
| Mid-frame SRV free (ImGui thumb) | Medium | Same rules as layer delete; generation IDs |
| Double memory (CPU float + GPU) | High | Soft budgets from EMERGENCY plan; drop provisional first |
| Race paint vs export | Medium | `JobManager` document lock; flush queue before export |
| False FPS (skipping truth) | Medium | HUD: “catch-up N ops”; never claim “saved” until flush |

**Verdict:** Safe **only** behind phases + invariants. Full GPU FX *and* async brush *and* 128K in one PR = high crash probability.

### 3.3 Display accuracy = export?

| Mode | Viewport | Export (required) |
|------|----------|-------------------|
| **Today** | Proxy FX, Effects Preview OFF possible | Full CPU bake — can already differ |
| **P1** | GPU FX preview (proxy OK) | Full CPU bake **must** match **params** 1:1; pixel-diff test on 512–2K |
| **P4** | Same pipeline | Full-res GPU readback **or** CPU bake with golden comparison |

**Hard rule:** Until P4 acceptance tests pass, **export never trusts provisional GPU alone**.

Acceptance:

1. Unit: CPU vs GPU filter/style on fixed seed image — max abs error ≤ threshold (U8: 1–2 levels; F32: relative eps).  
2. Manual: Effects Preview ON, paint, Quick Export, A/B zoom.  
3. Undo 10× after FX + paint: no holes, no stale outline.

### 3.4 Can one agent session (~100K effective tokens) carry this?

**No — not end-to-end.** Honest capacity:

| Session type | Fits in ~100K | Deliverable |
|--------------|---------------|-------------|
| Design + this plan | Yes | This document |
| P0 contracts + tests skeleton | Yes | Invariants + golden harness stub |
| P1a one GPU filter (e.g. HSV) dirty-rect | Yes | Vertical slice |
| P1b shadow/outline GPU | Stretch / 2 sessions | Styles |
| P2 stroke GPU preview | Separate session | Tools |
| P3 op-queue | Separate session | Ordering |
| P4 export unify | Separate session | Fidelity |
| P5 32K–128K | Multi-session | Extreme |

**Anti-amnesia strategy (§8):** permanent plan in `plans/`, phase checklists, **invariants as code comments + tests**, session handoff block. Each session starts with “read §8 + open PRs for phase X only.”

---

## 4. Architecture target

### 4.1 Layers of data

```
┌─────────────────────────────────────────────────────────────┐
│  INPUT LAYER                                                │
│  Pointer events → OpIntent {id, tool, params, bounds, t}    │
└────────────────────────────┬────────────────────────────────┘
                             ▼
┌─────────────────────────────────────────────────────────────┐
│  OPERATION ORDER (ring / queue)                             │
│  seq · state: Pending | ProvisionalShown | Committed | Fail │
│  never reorder paint vs mask vs transform without barriers  │
└──────────────┬─────────────────────────────┬────────────────┘
               │                             │
               ▼                             ▼
┌──────────────────────────┐   ┌──────────────────────────────┐
│ PROVISIONAL RENDER (GPU) │   │ AUTHORITATIVE COMMIT         │
│ Base tiles (uploaded)    │   │ PaintEngine → tileCache COW  │
│ + stroke overlay RT      │   │ BackupTile / Seal → Undo     │
│ + FX stack (HLSL)        │   │ FX param cmds (LayerProps)   │
│ Dirty rect / atlas       │   │ Optional GPU→CPU readback    │
│ Drop anytime under VRAM  │   │ never provisional into undo  │
└──────────────┬───────────┘   └──────────────┬───────────────┘
               │                              │
               └──────────► COMPOSITE ◄───────┘
                            PSLayerBlend
                            viewport / export path
```

### 4.2 Two FX strategies (pick per phase)

| Strategy | When | Pros | Cons |
|----------|------|------|------|
| **A. Preview-only GPU** | P1–P2 | Safe; undo/export unchanged | Two implementations until golden match |
| **B. GPU = bake into presentationCache** | P4 optional | One path | Must match CPU; readback cost; seams |

**Recommendation:** ship **A** first with continuous golden tests against current CPU `layer_fx`. Only promote to B when tests green.

### 4.3 Brush / tools (your “GPU paints, cores catch up”)

**P2 model (recommended first tool slice):**

1. On dab: enqueue `OpIntent::Paint`.  
2. GPU: draw dab into **stroke overlay RT** (or tile atlas) immediately (provisional).  
3. Worker/main: `BackupTile` + `PaintEngine` stamp into `tileCache` in order.  
4. When commit for seq N done: upload dirty tiles; drop overlay for those tiles.  
5. Stroke End: wait for queue catch-up (or soft-wait with “syncing…”); then seal undo **once**.

**Not for P2:** writing GPU readback as content without CPU paint engine (breaks exact stamp/scatter/tip parity).

**P3+ for 8K–128K:** same model; overlay may be lower resolution; catch-up may be multi-threaded tile writers with **strict seq order per tile**.

### 4.4 Layer FX GPU (P1)

Port interactive path only:

- Filters: HSV, curves LUT, blur separable, noise, alpha invert (map 1:1 to `LayerTypes`).  
- Styles: shadow (alpha extract → blur → offset → color), outline (dilate/erode → color/grad/tex).  

Use / adapt `ai-answers/LayerFx.hlsl` + `GpuFxDispatch` **after** bringing into `src/` with CMake, not as silent patches to Canvas.

**Dirty region:** expand by blur radius + outline size; never full 16K unless forced.

**Group FX:** flatten children to group provisional RT (GPU) — harder; schedule after single-layer P1.

---

## 5. Operation order (formal)

```text
Op {
  seq: u64                // global monotonic
  kind: Paint | MaskPaint | FilterParam | StyleParam | Transform | ...
  layerIdx, bounds
  state: Pending → Provisional → Committing → Committed | Failed
  dependsOn: optional seq  // barrier
}
```

**Rules:**

1. Global `seq` never reordered for **same layer content**.  
2. Cross-layer: composite may draw latest provisional; export waits all Committed.  
3. **Barrier ops:** undo, export, save .rayp, bit-depth convert, crop — `FlushPipeline()` until empty.  
4. Undo of committed stroke: mark superseding provisional ops **Failed**; rebuild provisional from tiles.  
5. UI: show catch-up depth; optional “pause input when backlog > N” for extreme sizes.

---

## 6. Invariants (paste into code as comments; test in P0)

```
I1  Document pixel truth lives only in tileCache / MaskTiles / selection CPU buffers.
I2  Undo payloads are only snapshots of I1 (or props), never GPU provisional RTs.
I3  PaintEngine is the only path that mutates content tiles from brushes (until a proven GPU stamp).
I4  Provisional GPU may be dropped at any time without changing I1.
I5  Export / Save / Python get_composite call FlushPipeline() then full-quality path.
I6  Undo restore always invalidates filtered/presentation/gpuDisplayKind (existing).
I7  No MarkDirty-clear on GPU upload unless all intended tiles uploaded (existing hole fix).
I8  Effects params changes = LayerPropsCommand; pixels change = PaintStrokeCommand.
I9  Mid-stroke: never seal incomplete async commits.
I10 Display may be proxy; Export must be full-res for user-facing files until user opts into proxy export.
```

---

## 7. Risk matrix & “don’t do”

| Don’t | Why |
|-------|-----|
| One mega-PR “everything GPU” | Exceeds token/ rational QA budget; undebuggable holes |
| GPU write into history tiles without COW | Silent undo corruption |
| Async FX without stroke barrier (already failed once) | Ghost tiles |
| Replace export with viewport blit | Proxy / U8 composite RT ≠ document |
| 128K before op-queue | OOM / thrash |
| Skip golden CPU↔GPU tests | Display≠export forever |
| Free SRV while ImGui still holds thumb | Crash (known class) |

---

## 8. Anti-amnesia handoff (for every future session)

### 8.1 Files to open first (≤5 min)

1. `plans/GPU_DRIVEN_PIPELINE.md` (this plan)  
2. `Canvas.h` Layer caches + `m_EffectsPreviewEnabled`  
3. `PaintOnActiveLayer` stroke End block (~3075)  
4. `UndoRedoManager::ApplyPaintStrokeDeltas` FX invalidation  
5. `ai-answers/LayerFx.hlsl` only as reference, not truth  

### 8.2 Session template

```
Phase: P?
Goal: one vertical slice (one filter OR stroke overlay OR queue skeleton)
Invariants touched: I?
Out of scope: list explicitly
Acceptance: build + N manual checks
Do not: ...
```

### 8.3 Progress checklist (update when shipping)

- [ ] P0: invariants doc + golden test harness skeleton  
- [ ] P1a: GpuFx in `src/`, HSV GPU preview matches CPU  
- [ ] P1b: blur dirty-rect GPU (no seams at tile borders in proxy)  
- [ ] P1c: shadow/outline GPU preview  
- [ ] P1d: Effects Preview uses GPU path; stroke-end no full CPU rebuild when GPU OK  
- [ ] P2a: stroke overlay RT for Brush  
- [ ] P2b: mask paint overlay  
- [ ] P3: OpQueue + FlushPipeline on export/undo  
- [ ] P4: export golden vs viewport full-res  
- [ ] P5: memory policy + backlog UI for extreme docs  

### 8.4 Token budget rule

If a phase needs >~1 focused session: **split** (e.g. P1b blur H only, then V).  
Never implement P3+P1 in the same session.

---

## 9. Suggested implementation order (detail)

### P0 — Contracts (1 session)

- Add `plans/GPU_DRIVEN_PIPELINE.md` (done with this plan).  
- `Pipeline` types header (empty queue + Flush no-op).  
- Test: fixed PNG, CPU filter outputs hash; place for GPU compare later.  
- Telemetry: log when presentation rebuild / export bake runs.

### P1 — GPU FX preview (2–4 sessions)

1. Move/adapt `GpuFxDispatch` + `LayerFx.hlsl` into `src/shaders` + CMake.  
2. Wire `Init` on Canvas device create.  
3. Interactive `RebuildLayerPresentation` / `RefreshFilteredCache`:  
   - if GPU ready → provisional presentation SRV  
   - else CPU fallback  
4. Golden: HSV, Blur r=2, DropShadow on 512².  
5. Keep export on CPU full bake.  
6. Stroke policy: mid-stroke still raw; optional live GPU FX over raw without rebaking tiles (better than today’s hitch).

### P2 — Tool provisional (2–3 sessions)

1. `StrokeOverlay` RT per active layer (or shared).  
2. Brush dabs draw GPU overlay + enqueue CPU stamp.  
3. Sync path when queue empty = today’s behavior (bit-identical tiles).  
4. Stress: fast scribble + undo.

### P3 — Op queue (2 sessions)

1. Global seq, states, FlushPipeline.  
2. Export/Save/Undo/Redo call flush.  
3. HUD backlog counter.  

### P4 — Export fidelity (1–2 sessions)

1. Full-res GPU path + readback **or** prove CPU full bake matches GPU preview params.  
2. A/B harness.  

### P5 — Extreme (later)

1. Soft/hard RAM from emergency plan.  
2. Overlay LOD.  
3. Optional input throttle.  
4. Never full-doc float without tiled streaming.

---

## 10. Dependencies & non-goals

**Depends on (stable first):**

- Undo COW + seal (done)  
- Stroke-skip FX mid-paint (done)  
- Effects Preview OFF escape hatch (done)  
- Asset-backed fill (B16, done)  

**Non-goals for this program:**

- Full Krita brush engine port  
- Replacing TileCache with pure GPU memory as SoT  
- Cloud DAM  
- PSD/KRA importers (separate plan)  
- 3D preview FX  

---

## 11. Success metrics

| Metric | Target |
|--------|--------|
| Interactive FX on 2K with shadow+blur | No multi-second freeze on slider |
| Paint mid-stroke FPS | ≥ current; no worse |
| Undo after FX+paint | Zero tile holes (stress thrash) |
| Export vs full-quality reference | ≤ tolerance in golden tests |
| Session discipline | No phase implements next phase’s features |

---

## 12. Recommendation (decision for you)

1. **Accept this as the standalone program** (not a B16 leftover).  
2. **Start P0 → P1a only** when you greenlight implementation.  
3. Treat **brush GPU provisional** as P2, same architecture, not a side hack.  
4. Treat **128K / op-order** as P3–P5; design now, build after P1–P2 prove no holes.  
5. Keep **CPU full bake export** until P4 tests pass — this protects “картинка = файл.”

---

## 13. One-page cheat sheet (print)

```
TRUTH = tileCache (+ mask/selection CPU)
PREVIEW = GPU provisional (droppable)
HISTORY = sealed tile snapshots only
EXPORT = flush + full quality (CPU until P4)
STROKE = mid: raw tiles + optional GPU overlay; end: commit + FX
NEVER = provisional → undo; composite RT → document; async seal early
PHASE = P0 contracts → P1 FX GPU → P2 stroke GPU → P3 queue → P4 export → P5 extreme
SESSION = one vertical slice; re-read this plan §6–§8 first
```
