# EMERGENCY SAFE PLAN — No Crashes as a Product Feature

**Status:** ACTIVE (P0 over all UI polish)  
**Date:** 2026-07-18  
**Trigger:** UX questionnaire — «убрать краши как явление»; 8K/16K + Fill = лаги/вылеты; PDN wins on reliability even when laggy.

---

## 0. Mission

RayV Paint must **never crash** and must **never freeze input > 2s** in critical paths, even under abusive ops on **16K** documents.

Lag is allowed. Hang is not. Crash is not.

| Outcome | OK? |
|---------|-----|
| Frame 100–500 ms under 16K paint | OK (soft degrade) |
| Single frame / op **> 2000 ms** without yielding | **FAIL** (hang) |
| Access violation / abort / GPU TDR process death | **FAIL** (crash) |
| Soft-limit exceeded → continue with warn / spill | OK |
| Hard refuse that leaves user stuck with no path | **FAIL** (bad safety) |

---

## 1. Order of actions (do not reorder casually)

### Wave 0 — Instrumentation & kill-switch suite (THIS WAVE)

| # | Action | Done when |
|---|--------|-----------|
| 0.1 | Write this plan (`plans/EMERGENCY_SAFE_PLAN.MD`) | ✓ |
| 0.2 | Hellish stress runner on `testfield/16Ktest.dds` | `--stress-16k` exits 0, no crash |
| 0.3 | Stress covers: load 16K, paint, **Fill layers**, **undo/redo thrash**, masks, select/transform, layer spam | Report lists phases |
| 0.4 | Hang budget: any frame work **> 2000 ms** → non-zero exit + log | Enforced in StressRunner |
| 0.5 | Soft RAM policy: hard refuses → **soft warn + proceed** (first cut) | Mask / flat composite paths |
| 0.6 | Wire `run_tests.bat stress` | Documented |

### Wave 1 — Crash taxonomy & hotfixes

| # | Action | Notes |
|---|--------|-------|
| 1.1 | Mine `Documents/RayVPaint/user/crash_last.log` + minidumps after stress fails | CrashGuard already writes these |
| 1.2 | Classify: OOM, GPU create fail, undo COW bug, fill composite, transform float buffer, race | One ticket per class |
| 1.3 | Fix **highest-frequency** crash first (not prettiest) | Usually 16K + Fill + undo or mask |
| 1.4 | Every crash fix adds a **minimal repro** to stress or a unit script | No silent “fixed locally” |

### Wave 2 — Soft memory architecture (policy)

| # | Action | Notes |
|---|--------|-------|
| 2.1 | Document soft vs hard budgets in `MemoryStats` | Soft = warn/evict; Hard only for absurd math overflow |
| 2.2 | VRAM pressure → keep tiles in **system RAM** (already partly true for TileCache); GPU upload lazy / budgeted | No hard fail CreateTexture if fallback path exists |
| 2.3 | RAM pressure → **tile spill to disk** (out-of-core), GIMP-style | New subsystem — see §3 |
| 2.4 | Undo stack: memory-aware cap that **spills or drops oldest** instead of refusing paint | Prefer drop history over crash |
| 2.5 | Export / full flat compose: prefer tiled path; soft-try flat only if RAM soft-ok | Never AV on alloc fail — catch & toast |

### Wave 3 — Responsiveness budget

| # | Action | Notes |
|---|--------|-------|
| 3.1 | Main thread: no op may hold > **32–50 ms** without pump/yield on critical path | Slice large undos / compose |
| 3.2 | 16K transform / selection: tile-budgeted apply | Matches “never >2s freeze” |
| 3.3 | Fill@4K+ composite: dirty rects / proxy LOD already exist — verify under stress | |
| 3.4 | Windows Ink / tablet dead-zone (~32px) tracked separately (UX) — not crash | |

### Wave 4 — Fill UI + polish (only after Wave 1 green)

Fill panel 0/10 from questionnaire is real, but **after** stability green on stress.

---

## 2. Soft limits policy (non-negotiable direction)

### Definitions

| Level | Meaning | Behavior |
|-------|---------|----------|
| **Soft budget** | “We prefer not to cross this” (e.g. 40–55% RAM for one alloc) | Log `soft_budget`, try spill/evict/LOD, **still proceed** if user-driven |
| **Hard ceiling** | Only for impossible/corrupt sizes (overflow, negative, > engineering max e.g. 32K² * insane bpp) | Refuse with human+numeric error; never silent |
| **Spill ladder** | VRAM → RAM → disk | Always try lower tier before fail |

### Rules

1. **No hard RAM refuse** for normal document ops (mask create, paint, undo, fill) solely because `ExceedsRamBudget` is true.
2. Soft overshoot is **expected** under 16K stress; process must stay alive.
3. If `new` / `vector::assign` would throw `bad_alloc`: catch, log, degrade (drop undo / refuse only that op), **do not abort**.
4. GPU `CreateTexture2D` fail: fall back to CPU tiles + dirty flag; log; continue.
5. Future: disk spill directory under `Documents/RayVPaint/cache/tiles/` with eviction LRU.

### First-cut code changes (Wave 0.5)

- `CreateLayerMask`: soft warn if over budget, **still create**.
- `ComposeVisibleLayersRGBA8` / flat composite preflight: soft warn, **still try**; on `bad_alloc` return false cleanly.
- `CanAllocateFlatComposite`: soft warn on budget; keep only true hard refuse for pixel-count engineering max if any.

---

## 3. Out-of-core (GIMP-like) — design sketch (Wave 2.3+)

```
TileCache tile
  ├─ hot:  in VRAM (GpuTileStore)     // display / paint brush
  ├─ warm: in RAM (TileData COW)      // edit / undo
  └─ cold: on disk (compressed blob)  // spill under soft RAM pressure

Compositor
  └─ pulls only tiles in dirty/viewport AABB; never requires full 16K RGBA8 resident
```

**Not in Wave 0.** Stress suite will expose current crash/hang points without full OOC.

---

## 4. Hellish stress suite (`--stress-16k`)

### Asset

- Primary: `testfield/16Ktest.dds` (16384×16384 BC7, ~256 MiB on disk).
- If missing → exit code **3** (env fail, not product fail).

### Launch

```cmd
build\Release\RayVPaint.exe --stress-16k
:: or Core:
build\Release\bin\RayVPaint_Core.exe --stress-16k
run_tests.bat stress
```

Prefer `--perf` companion for uncapped present when measuring pure work.

### Live journal (do not rely on MessageBox)

| File | Role |
|------|------|
| `testfield/stress_journal.txt` | Session log: **every** phase/action/undo/hang, flush each line |
| `Documents/RayVPaint/user/stress_last.txt` | Same stream (append), stable path |
| `Documents/RayVPaint/user/crash_last.log` | CrashGuard short report |
| Journal tail on crash | CrashGuard appends `########## CRASH ##########` block |

**Silent crash mode** under `--stress-16k`: **no MessageBox**. Process exits so CI/you are not stuck waiting for OK.  
Watch the journal file in a second editor while the suite runs.

### Phase sequence (abuse order)

| Phase | Abuse |
|-------|--------|
| Load16K | Open `16Ktest.dds`; assert size ≥ 16384 or fail |
| PaintBurst | Wide fast strokes; tile thrash |
| FillSpawn | Create **multiple** Fill layers; enable multi-map colors; dirty composite |
| FillMutate | Rapid fill color/scale thrash + undoable prop commits |
| UndoRedoHell | **Dozens–hundreds** of undo/redo with minimal delay (serialization / COW stress) |
| MaskAbuse | Create mask on large doc (soft budget); paint mask dabs |
| LayerSpam | Create/delete raster layers; stack churn |
| SelectTransform | Mid rect select + move/scale commit |
| Chaos | Interleaved paint + undo + fill mutate + redo |
| Report | Metrics + exit code |

### Pass criteria

| Check | Threshold |
|-------|-----------|
| Process exit | Clean (no crash) |
| Any single frame work | **≤ 2000 ms** (hard fail if exceeded) |
| Soft: frames > 500 ms | Logged; do not fail alone |
| Undo/Redo WS permanent growth | Warn if > 256 MiB across hell phase |
| Hang watchdog | Wall phase timeout; force advance; never infinite loop |

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | Survived; no hang over 2s |
| 1 | Hang: ≥1 frame work > 2000 ms |
| 2 | Suspected severe leak across undo hell |
| 3 | Missing 16K asset / setup |
| 4 | Load 16K failed |
| other | Unexpected |

---

## 5. Why current `--benchmark` is not enough

| Gap | Benchmark (8K blank) | Stress-16K |
|-----|----------------------|------------|
| Document | Synthetic 8K empty | Real **16K BC7** dense tiles |
| Fill layers | None | Spawn + mutate |
| Undo depth | ~8–16 mild | Hell thrash |
| Serialization / COW | Light | Aggressive |
| Hang gate 2s | Soft timeout 48s total only | **Per-frame 2s fail** |
| User crash repro | Partial | Closer to daily hell |

Keep `--benchmark` for interactive FPS/feel. Stress is **CI/reliability gate**.

---

## 6. Acceptance for “краши как явление убраны”

- [ ] `run_tests.bat stress` green on dev machine with 16K asset  
- [ ] 3 consecutive stress runs without crash  
- [ ] Manual: open 16K, add Fill, paint, undo×20, redo×20 — no exit  
- [ ] Soft budgets never refuse mask/paint with silent no-op that looks like a hang  
- [ ] CrashGuard log empty after stress (or only soft_budget warnings)

---

## 7. Explicit non-goals (this emergency track)

- Pretty Fill panel layout (backlog after green)  
- Full OOC disk compositor in one PR  
- Matching Krita 32K performance day-one  
- Removing OpenCV smart-select in this track  

---

## 8. Immediate next commits after this document

1. `StressRunner` + `--stress-16k`  
2. Soft-limit first cut (mask / compose)  
3. `run_tests.bat stress` + SETUP note  
4. Run stress; file crash classes from log into Wave 1 tickets  

**Owner rule:** any agent touching memory, tiles, undo, fill, or 16K paths must re-run `--stress-16k` before claiming done.

---

## 9. Findings from first stress run (2026-07-18)

| Finding | Evidence | Class |
|---------|----------|--------|
| **CRASH SIGSEGV** mid UndoRedoHell after FillSpawn | `crash_last.log` checkpoint=`imgui_new_frame`; breadcrumbs ACCESS_VIOLATION ×4 then SIGSEGV | Wave 1 #1 |
| Peak RAM ~9.5 GiB WS / ~10.6 GiB Private on 16K + 3 Fill layers | mem snapshot `stress_FillSpawn_end` | Soft budget / OOC need |
| Frame hang 2758 ms during FillSpawn | Coincided with **periodic autosave** of 16K project | Fixed: disable autosave in stress; still fix autosave budget for users |
| PaintBurst finished in 1 ms (no strokes) | Bug: `BeginPhase` cleared stroke queue after build | Fixed in StressRunner |

### Wave 1 ticket A — Fill + Undo on 16K → AV  **[FIXED 2026-07-19]**

**Root cause:** `UI::RenderAll` (Layers thumbs queue `layer.srv` as ImTextureID) → later mid-frame **Undo/delete `Release()`s that SRV** → `ImGui::Render` / next `NewFrame` = ACCESS_VIOLATION (`read at 0x10`).

**Fix landed:**

1. `Canvas::DeferReleaseSRV/Tex` + `FlushDeferredGpuReleases()` **after** `ImGui_ImplDX11_RenderDrawData`  
2. Delete layer / thumb rebuild / fill pattern use deferred free  
3. Fill prop undo keeps 1×1 GPU, re-uploads color (no tear-down)  
4. Layers panel: fill rows = solid swatch, not raw `layer.srv`  

**Verified:** `--stress-16k` full run **no CRASH** (undo hell 66 steps, mask, chaos).  
Hang budget exit `1` only if a frame still >2s (SelectTransform on 16K — mitigated for huge docs).
