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

### 4.2 Two FX strategies — and the dual-implementation tax

| Strategy | When | Pros | Cons |
|----------|------|------|------|
| **A. Preview-only GPU** | P1–P3 (interactive) | Undo/export unchanged; safe rollback | **Two filter/style impls** (CPU `layer_fx` + HLSL) must stay in sync |
| **B. GPU bake into presentation/filtered caches** | P4+ only after goldens | One interactive path | Readback cost; must prove pixel policy; seams |

**Recommendation:** ship **A** first. Promote to B only when goldens stay green for a release cycle.

#### 4.2.1 Dual-impl is not a one-shot migration — it is ongoing load

While A is live (likely **entire P1–P3**, months of calendar time if paced carefully):

| Change type | What you must touch twice | Failure if you forget |
|-------------|---------------------------|------------------------|
| New filter kind / param | `LayerTypes` + `ApplyPixelFilters` **and** HLSL cbuffer/pass | Preview wrong; export “right” → user trust dies |
| Shadow/outline math tweak | `BuildPresentation` **and** GPU style passes | Halo size differs on export |
| Premul / selection weight | Both paths | Fringes on undo/export |
| Bit-depth (U8/F16/F32) | CPU already multi-format; GPU RT formats must match policy | Clipping / banding in preview only |

**Mitigations (mandatory in plan, not optional polish):**

1. **Single source of param structs** — C++ `LayerFilter` / style params serialize once; GPU uploads the same POD layout (document cbuffer layout next to struct, static_assert size).  
2. **Golden matrix CI/smoke** — fixed seeds (HSV Δ, Blur r=1/2/8, Curves, DropShadow, Outline) on 64² and 512²; CPU vs GPU max-abs / PSNR; **block merge** if regress.  
3. **Feature flag per filter** — `GpuFxCaps::hsv`, `::blur`, … CPU fallback if GPU path missing/failed; never half-ship a new filter only on one side.  
4. **“CPU is export authority until P4”** — product copy: preview can be GPU; file always CPU full bake (or proven-identical). Reduces dual-path *user-facing* risk.  
5. **Kill dual as soon as P4 lands** — explicit exit criterion: delete or `#if 0` interactive CPU bake when GPU is authority for interactive + export matches. Do not leave dual forever.  
6. **No new filter in P1–P3 without both impls + goldens** — process rule for agents/humans.

**Cost budget:** every new FX feature costs ~1.5–2× until P4. Accept it; do not “save time” by GPU-only without export path.

### 4.3 Brush / tools — P2 deep dive (highest technical risk)

Claude’s note: **P2 is the riskiest technical junction** (GPU provisional × `PaintEngine` / `BackupTile` / tip parity). The short checklist in older drafts is **not** enough to implement from — this section is the contract.

#### 4.3.1 Goals vs non-goals

| Goal | Non-goal (until much later) |
|------|-----------------------------|
| GPU shows stroke **immediately** (provisional overlay) | GPU readback becomes document pixels |
| Authoritative stamp remains **`PaintEngine` on `tileCache`** | Two stamp engines (GPU tip vs CPU tip) both writing history |
| Undo seal = **bit-identical** to today’s single-threaded stroke | “Close enough” scatter/tip for history |
| Catch-up may lag on huge docs | Reordering dabs across tiles |

#### 4.3.2 Data plane split (hard)

```
                    ┌─────────────────────────────┐
  pointer dab ──►   │  DabRecord (immutable)      │  seq, layer, x,y, pressure,
                    │  brush snapshot, tip id,    │  rotation, scatter seed, …
                    │  selection/mask clip ref    │
                    └──────────┬──────────────────┘
               ┌───────────────┴───────────────┐
               ▼                               ▼
     PROVISIONAL (GPU only)           AUTHORITATIVE (CPU tiles)
     StrokeOverlay RT / atlas         BackupTile → LockTile → DrawStamp
     no COW, no undo                  Seal only after all dabs Committed
     may drop under VRAM              bit-identical to pre-P2 path
```

**Key:** GPU and CPU both consume the **same `DabRecord` list** (or CPU is the only consumer of records and GPU approximates — see parity modes). Prefer **shared DabRecord** so parameters cannot diverge.

#### 4.3.3 Parity modes (choose explicitly per phase)

| Mode | GPU overlay | CPU truth | When |
|------|-------------|-----------|------|
| **P2a Sync dual** | Optional overlay for feel | **Main thread** runs full `PaintEngine` every dab **as today** | First ship — zero race with BackupTile |
| **P2b Async catch-up** | Overlay from DabRecord | Worker(s) replay DabRecords through **same** `PaintEngine` | Only after P2a stable |
| **P2c GPU content** | — | Forbidden until proven stamp shader == PaintEngine | Not in this plan’s default path |

**Default ship order:** **P2a first** (overlay is pure candy; truth path unchanged). **P2b only** when overlay already trusted and we need lag hiding on large docs.

P2a still needs care: composite must draw `tileCache` upload **plus** overlay without double-painting (overlay only for dabs not yet uploaded, or overlay = full stroke and tiles underneath cleared/hidden for active stroke — pick one policy and document it).

**Recommended composite policy for P2a:**

- Mid-stroke: composite uses **raw tiles + StrokeOverlay** (do not bake FX mid-stroke — already true today).  
- Overlay covers only the **active stroke** AABB.  
- On stroke End: flush any pending CPU dabs (none in P2a), seal undo, **destroy overlay**, upload dirty tiles, then FX as today.

**P2b composite policy:**

- Overlay holds dabs with `state < Committed`.  
- When dab N commits: upload that dab’s dirty tiles; clear overlay texels for those tiles (or rebuild overlay from remaining Pending records).  
- Never leave overlay covering committed tiles (double opacity).

#### 4.3.4 BackupTile / Seal ordering (race kill list)

Today (must preserve semantics):

```
Stroke Begin: clear m_ActiveStrokeDeltas
  dab: BackupTile (first touch only, shared snapshot) → PaintEngine write
Stroke End: SealActiveStrokeDeltas (deep-copy old/new) → PaintStrokeCommand
```

**P2b race conditions and required rules:**

| Race | Bad outcome | Rule |
|------|-------------|------|
| Seal while worker still writing tile | History incomplete / torn pixels | **I9:** Seal only when all stroke dabs `Committed` or `Failed` |
| Two workers `LockTile` same tile out of order | COW chaos / lost dab | **Per-tile serial queue** (see 4.3.5) |
| `BackupTile` on worker without main-thread exclusive layer | Double backup / wrong oldState | Backup on **owner thread of tileCache** only (main), **or** hold layer mutex for Backup+stamp as one critical section |
| Undo mid catch-up | Worker writes after restore | Generation token: cancel workers; discard Pending; clear overlay |
| Export mid catch-up | Partial stroke on disk | `FlushPipeline` + document lock (existing JobManager pattern) |
| GPU overlay uses different tip sampling | “Looks good, undoes wrong” | CPU remains stamp authority; overlay may be cheaper approx **only if** labeled / same DabRecord params and user accepts preview soft-diff — default: **same tip bytes**, cheaper only in P5 LOD |

**Practical threading rule for P2b (conservative):**

1. **Main thread only** mutates `tileCache` / calls `BackupTile` / `Seal*` (same as today).  
2. Workers may **only** prepare dab math (optional) or stay unused until we have a proven tile-mutex design.  
3. If workers later stamp: one **tile owner** model — each tile has a serial chain of dabs; worker acquires tile lock, main never touches that tile concurrently; seal still on main after barrier.

**Do not** start with multi-worker `LockTile` in the first P2 PR. That is how undo holes return.

#### 4.3.5 Per-tile sequence (for P2b/P3 catch-up)

```text
Global seq:  1,2,3,4,5  (dab order, stroke-global)
Tile (3,7):  dabs 1,3,5 must apply as 1→3→5 never 1→5→3
Tile (4,7):  dabs 2,3   as 2→3
```

- Global order defines happens-before for overlapping tiles.  
- Independent tiles **may** parallelize **only if** no shared dab and no shared BackupTile map without locking `m_ActiveStrokeDeltas`.  
- Simpler correct design: **single consumer** replays dabs in global seq on main (async only for “not blocking input” via overlay). Parallel tile writers are a **P3/P5 optimization**, not P2 requirement.

#### 4.3.6 Bit-identical CPU path (acceptance for P2)

Before enabling async:

1. Record stroke as DabRecords; replay offline through PaintEngine → hash tiles.  
2. Live stroke without overlay → same hash.  
3. Live stroke with overlay disabled mid-way → same hash after End.  
4. Scatter/angle jitter: seed must live in DabRecord, not `rand()` at stamp time twice.

If (1)≠(2), fix before any GPU overlay ships.

#### 4.3.7 Rollback for P2

- Feature flag `GpuStrokeOverlay=0` → identical to pre-P2 main.  
- Branch `GPU-DRIVEN-PARADIGME`; never merge P2b to main until thrash undo stress passes.  
- Overlay alloc fail → silent disable, CPU-only (no crash).

### 4.4 Layer FX GPU (P1)

Port interactive path only:

- Filters: HSV, curves LUT, blur separable, noise, alpha invert (map 1:1 to `LayerTypes`).  
- Styles: shadow (alpha extract → blur → offset → color), outline (dilate/erode → color/grad/tex).  

Use / adapt `ai-answers/LayerFx.hlsl` + `GpuFxDispatch` **after** bringing into `src/` with CMake, not as silent patches to Canvas.

**Dirty region:** expand by blur radius + outline size; never full 16K unless forced.

**Group FX:** flatten children to group provisional RT (GPU) — harder; schedule after single-layer P1.

### 4.5 Op-queue ↔ existing threading / single-document model

Claude’s note: do **not** assume op-queue is “compatible by default” with today’s concurrency. Explicit join points:

#### 4.5.1 What exists today

| Mechanism | Role | Thread | Document interaction |
|-----------|------|--------|----------------------|
| **Main / UI / D3D** | Paint, compose, ImGui, most Canvas mutation | Main | Authoritative |
| **`JobManager`** | Named jobs; `locksDocument` blocks edits via `AppContext` / `BlocksCanvasInteraction` | Job may be async; lock is logical | Export, open, autosave |
| **`AsyncFilterQueue`** | Snapshot tiles on main → worker filter → **apply on main** via `Poll` | Worker + main poll | Generation cancel; was source of ghost tiles when mis-applied |
| **`ThreadPool`** | Asset thumbs, decode chunks, etc. | Pool | Must not touch live `tileCache` without snapshot |
| **Single active document** | `ProjectManager` / active Canvas | — | One paint target; simplifies barriers |

There is **no** general multi-document paint pipeline. Op-queue designs for **one Canvas** first.

#### 4.5.2 How OpQueue should plug in (P3 contract)

```text
                    Main thread                              Workers
                    ───────────                              ───────
 Input → DabRecord / OpIntent
      → Provisional GPU (D3D: main only unless deferred context policy)
      → OpQueue push
      → each frame: Poll completes → apply tiles / clear overlay
      → FlushPipeline: spin/poll until empty (like job lock)
                                                          optional:
                                                          prepare / filter snapshot
                                                          (never Seal, never free SRV)
```

**Rules:**

1. **D3D11 immediate context:** treat as **main-thread only** unless we introduce a dedicated render thread + deferred contexts (out of scope until needed). GPU provisional submit = main.  
2. **`JobManager::locksDocument`:** `FlushPipeline` must complete **before** or **as part of** starting a document-lock job; paint path already checks `BlocksCanvasInteraction`.  
3. **`AsyncFilterQueue`:** either (a) **subsumed** by OpQueue (preferred long-term), or (b) stays separate but **ordered after** paint commits for that layer (filter job generation bumps when paint seals). Do not run filter apply while paint catch-up pending for same layer.  
4. **ThreadPool tasks** that need pixels: **snapshot** on main (`SnapTile` bytes), process offline, return results; main applies — same pattern as `FilterJobInput`.  
5. **Single-document:** OpQueue is a member of `Canvas` (or AppContext pointing at active canvas). Switching document: flush or cancel all ops; never process ops against a destroyed Canvas.  
6. **Python / scripting:** `get_composite` / save → `FlushPipeline` (same as export).  
7. **Autosave:** already `locksDocument`; must flush pipeline first so .rayp matches tiles not overlay.

#### 4.5.3 Compatibility checklist (explicit — do not skip at P3 start)

- [ ] Document lock blocks new OpIntent enqueue (or enqueues only cancel).  
- [ ] Undo/Redo: cancel workers, flush or discard queue, clear overlay, then apply command.  
- [ ] Layer delete: cancel ops for that `layerIdx`.  
- [ ] `AsyncFilterQueue` idle or ordered after paint for that layer.  
- [ ] No worker calls `BackupTile` / `PushCommand` / `ImGui`.  
- [ ] Stress: export during scribble → either blocked or flush-complete before encode.

#### 4.5.4 What “single-document-per-core” means here

Interpret as: **one live editable document core graph**, not “one OS thread total.” Multiple cores may:

- decode assets,  
- run filter snapshots,  
- later (P5) prepare dab bounds,

…but **one** ordered authority stream commits into that document’s tiles. OpQueue is that stream’s scheduler, not a free-for-all work-stealing heap of paint.

---

## 5. Operation order (formal)

```text
Op {
  seq: u64                // global monotonic
  kind: Paint | MaskPaint | FilterParam | StyleParam | Transform | ...
  layerIdx, bounds
  state: Pending → Provisional → Committing → Committed | Failed
  dependsOn: optional seq  // barrier
  // Paint: optional dabIndex, tileKeys[], strokeId
}
```

**Rules:**

1. Global `seq` never reordered for **same layer content**.  
2. Cross-layer: composite may draw latest provisional; export waits all Committed.  
3. **Barrier ops:** undo, export, save .rayp, bit-depth convert, crop, autosave — `FlushPipeline()` until empty.  
4. Undo of committed stroke: mark superseding provisional ops **Failed**; rebuild provisional from tiles.  
5. UI: show catch-up depth; optional “pause input when backlog > N” for extreme sizes.  
6. **Per-tile:** if parallel commit ever exists, apply dabs in global seq order per tile (§4.3.5).  
7. **Join JobManager:** document-lock jobs require empty pipeline (§4.5).

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
I11 Dual FX (CPU+GPU): every interactive GPU filter has CPU export twin + golden until P4 exit.
I12 DabRecord is the single param source for overlay and PaintEngine replay (no second rand).
I13 tileCache mutation / BackupTile / Seal run only on the document owner thread (main) unless a documented tile-lock protocol is introduced.
I14 Document-lock jobs and OpQueue cannot race: flush or block enqueue.
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
| **P2b multi-worker LockTile without per-tile seq** | Torn stamps / undo holes — worst class of bug |
| **P2 overlay double-composite with committed tiles** | Double opacity, “fat” strokes |
| **New filter GPU-only “we’ll add CPU later”** | Dual-tax debt + export lies |
| **Assume OpQueue ⊥ JobManager** | Autosave/export partial docs |

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
- [ ] P1 dual-tax: golden matrix + feature flags for each GPU filter  
- [ ] P2a: stroke overlay RT + **main-thread PaintEngine unchanged** + no double paint  
- [ ] P2a′: DabRecord capture + offline replay hash == live stroke  
- [ ] P2b: async catch-up only after P2a′; still main-thread BackupTile/Seal  
- [ ] P2 mask paint overlay (same rules as content)  
- [ ] P3: OpQueue + FlushPipeline; join JobManager document lock checklist §4.5.3  
- [ ] P4: export golden vs viewport full-res; plan dual-FX exit  
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
4. Golden: HSV, Blur r=2, DropShadow on 512² (CPU vs GPU).  
5. Keep export on CPU full bake.  
6. Stroke policy: mid-stroke still raw; optional live GPU FX over raw without rebaking tiles.  
7. Document dual-tax: caps flags + “no new filter without both sides.”

### P2 — Tool provisional (split; 3–5 sessions)

**P2a (safe):**  
1. DabRecord log (can be debug-only first).  
2. StrokeOverlay RT; composite = tiles + overlay mid-stroke.  
3. **CPU stamp path 100% as today** on main.  
4. End: drop overlay, seal, FX.  
5. Stress scribble + undo; flag off == bit-identical.

**P2a′ (parity gate):** offline DabRecord replay hash == live.

**P2b (only after gate):**  
1. Overlay stays; CPU catch-up may lag **but still main-thread PaintEngine** unless tile protocol exists.  
2. Seal only when backlog empty.  
3. No multi-worker LockTile in first P2b.

### P3 — Op queue (2–3 sessions)

1. Global seq, states, FlushPipeline.  
2. Export/Save/Undo/Redo/Autosave call flush.  
3. Wire `JobManager` document-lock checklist §4.5.3.  
4. Order vs `AsyncFilterQueue` (subsume or barrier).  
5. HUD backlog counter.

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
STROKE = P2a: overlay + main PaintEngine same as today
         P2b: catch-up only after DabRecord parity gate; seal after empty backlog
DUAL FX = CPU export + GPU preview until P4; goldens block merge
THREADS = D3D+Seal+BackupTile on main; workers snapshot-only; JobManager lock ⇒ flush
NEVER = provisional→undo; composite RT→document; async seal early; multi-worker LockTile unordered
PHASE = P0→P1 FX→P2a/P2b stroke→P3 queue→P4 export→P5 extreme
SESSION = one slice; re-read §4.2–4.5 + §6–§8 first
```

---

## 14. Revision notes

| Date | Change |
|------|--------|
| 2026-07-19 | Initial plan on branch `GPU-DRIVEN-PARADIGME` |
| 2026-07-19 | Claude review: expand P2 races/parity (§4.3), dual-FX maintenance tax (§4.2), OpQueue×JobManager/threading (§4.5); I11–I14 |
