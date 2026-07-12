# Build 16 — CleanUP · Optimisation · Deep Improvements

**Status after Build 15:** Texture sets / Fill multi-map / File Explorer / mask tiles / layer undo / doc-size from Diffuse — **shipped**.  
**Build 16 philosophy:** **No new product features.** Any “new” code is either (a) structural refactor, (b) docs for future work, (c) **forced optimization** that unblocks existing tools.

| Priority | Focus | Rule |
|----------|--------|------|
| **P0 High** | Refactor + architecture readability (split responsibilities) | Not refactor-for-sport; move/dedupe/standardize only |
| **P0 High** | Documentation for future agents/humans | Contracts, UI rules, asset paths |
| **P1 Mid** | Optimisation | Only **after** structure is cleaner where it helps |
| **P2 Low** | UI/UX depth + layout fixes | Polish existing surfaces |

**Exception (one forced feature):** **Asset Browser** — not a fun feature; required so Fill Layer + textures (and later brushes/fonts) stop thrashing RAM/CPU.

---

## 0. Context snapshot (Build 15 baseline)

| Area | State | Smell for B16 |
|------|--------|----------------|
| `EditorPanels.cpp` | **~4.6k lines** | God-file: menus, modals, layers, tools, setup |
| `FileExplorer.cpp` | **~1.6k lines** | OK as module; still ImGui-raw combos |
| `Canvas.cpp` | **~8k lines** | Core god-file; UI must not grow inside it |
| `main.cpp` | **~2.3k lines** | Input + tools + some UI coupling |
| UI kit | `UiTokens`, `UiMotion`, widgets (`UiDropdown`, `UiIconButton`, …) | **Exists but underused** — raw `ImGui::Combo` / `ColorPicker` proliferate |
| Fill + texture | `fill.textureRgba` full decode in layer | **Known lag** → Asset Browser + shared GPU/CPU cache |
| Pipette / color | Works in Fill popup; missing/inconsistent elsewhere | Standardize **one** `UiColorField` + pipette arm |

**Build 9–10 UI intent (do not lose):** Apple-like motion — tokens, bounce/press, scrim, hold-dropdown, icon tint. B16 restores discipline; no hard-coded one-off chrome.

---

## 1. Philosophy of refactor (hard rules)

1. **No refactor for vanity.** Prefer: *move file*, *extract class*, *delete duplicate*, *route through existing widget*.
2. **OOP for UI kit, not for Canvas internals first.** New UI surface = **class / free function in `ui/`**, not a 200-line lambda in `RenderAll`.
3. **UI does not own core.** UI may: call Canvas/ProjectManager APIs, pass context, show state. UI must **not**: reimplement tile upload, invent parallel document stores, parse DDS for paint, etc.
4. **Core does not own ImGui.** No `#include <imgui.h>` growth in Canvas for new chrome; existing ImGui in Canvas only if already frozen (prefer shrink over time).
5. **DRY:** one combo, one color field, one modal shell, one path field — variants via parameters/theme, not copy-paste.
6. **Gate slices:** small PR-sized steps; build + manual smoke after each.

---

## 2. Work packages (ordered)

### WP-A — Documentation (do first; cheap context compression)

| Doc | Purpose |
|------|---------|
| **`plans/UI_RULES.md`** | **Mandatory** for any UI change: kit-only, motion, color/pipette, no hardcode |
| **`plans/BUILD16_CLEANUP_OPTIMIZATION.md`** (this file) | Scope + non-goals + Asset Browser |
| Update **`plans/TEXTURE_SET_CORE.md`** | Already partially updated; keep “maps-as-layers” truth |
| Short **`plans/ASSET_BROWSER.md`** | Categories, paths, .rayp packing, Fill consumer |

### WP-B — UI standardization (P0)

**Goal:** every control goes through kit; raw ImGui only inside widget implementations.

| Extract / standardize | Replace |
|----------------------|---------|
| `UiCombo` / `UiDropdown` | Scattered `ImGui::Combo`, local `static bool UiCombo` in EditorPanels |
| `UiColorField` | `ColorEdit4` / `ColorPicker4` + optional **Pipette** arm (same API everywhere) |
| `UiModal` / panel shell | `BeginPopupModal` copy-pasta (padding, scrim, footer buttons) |
| `UiButton` primary/secondary | Ad-hoc button colors |
| Layers Fill chips | Already custom; ensure uses tokens + wrap rules only |

**Pipette rule (item 6):**  
- One arm API: `UI::ArmPipette(target: BrushPrimary | FillMap{layer,map} | Custom callback)`.  
- One sample path: active Channels view composite.  
- Color popups **always** offer pipette when editing a color that paints or fills.

**Audit targets (grep-driven):**
```
ImGui::Combo(
ImGui::ColorPicker
ImGui::ColorEdit
BeginPopupModal
ShowOpenFileWin32 / ShowSaveFileWin32  // prefer FileExplorer modes
```

### WP-C — DRY + layout cleanup (P0, pairs with B)

- Map every dual path: Export DDS modal vs FE AdvancedExport; Load/Save Project modal vs FE (dead modals → delete if FE owns them).
- Select boxes that “look different” → force through `UiDropdown` / themed combo.
- Layers panel: no layout hacks that push to right edge; use measured wrap (already partially fixed for Fill).

### WP-D — Code structure / responsibility split (P0)

**Not** a full rewrite of Canvas. Target moves:

| From | To | Why |
|------|-----|-----|
| `EditorPanels.cpp` layers block | `ui/panels/LayersPanel.cpp` | Size + ownership |
| `EditorPanels.cpp` menus/modals | `ui/panels/AppMenus.cpp`, `ui/panels/Modals.cpp` | |
| Tool settings strip | `ui/panels/ToolSettingsPanel.cpp` | |
| Channels / Project Setup | `ui/panels/ChannelsPanel.cpp`, `SetupPanel.cpp` | |
| Fill GPU/upload thrash notes | stay in Canvas; UI only toggles params | |

**Rule after split:** `EditorPanels::RenderAll` ≈ orchestration only (~thin).

Optional later (mid, only if needed for opt):
- `Canvas` paint path vs I/O vs compose still in one TU is OK for B16 unless compile times force split.

### WP-E — Restore / enforce Apple-like motion (P0 docs + P2 code)

- Document in `UI_RULES.md`: press scale, EaseOutBack, scrim, elevated α, hold-dropdown threshold from `UiTokens` / `UiMotion`.
- Code: any new control must use `Ui::Motion` / tokens; ban magic `ImVec4(0.2f, 0.7f, …)` outside themes.
- Audit File Explorer + Fill chips for hard-coded colors → map to tokens.

### WP-F — Asset Browser (**forced** optimization feature) (P1)

**Problem:** Fill Layer “Use Texture” keeps full `textureRgba` on the layer and rebakes presentation / full-buffer paths → severe lag.

**Solution:** shared **Asset** identity + cache; layers hold **asset id / path key**, not private megabyte blobs (or blobs only until first GPU upload then optional discard of CPU if policy allows).

#### Categories (not types)

| Category | Location | Lifetime |
|----------|----------|----------|
| **built-in** | next to `.exe` → `assets/` | Read-only ship content |
| **user** | Documents / app data → `assets/` | User library, cross-project |
| **project** | In-memory + packed into `.rayp` | Portable; temp until save |

#### Scope B16 (textures only)

- Register / browse / preview / pick texture for **Fill Layer** (and optional brush tip later).
- GPU: one SRV/tile cache per asset key, refcounted.
- CPU: decode once; Fill samples via asset API (or GPU-only sample later).
- `.rayp`: serialize project-category assets (blob + id + relative name).

#### Non-goals B16

- Fonts, node materials (document only in ASSET_BROWSER.md as future).
- Full DAM / cloud.

#### Fill path change

```
Before: Layer.fill.textureRgba (full) → FillSolidBuffer / presentation every dirty
After:  Layer.fill.assetId → AssetStore::Get(id) → sample/cached GPU
```

### WP-G — Optimisation (P1, after B–D where possible)

Order matters: **wrong doc size was fixed in B15**; B16 opts:

1. **Asset-backed Fill textures** (WP-F) — biggest Fill lag fix.
2. Style bake: keep proxy; avoid full-doc float (already improved B15).
3. MaskTiles: avoid dual write flat+tile every dab if flat unused that frame.
4. Composite: skip invisible layers earlier; no thrash on `presentationDirty` for solid fills (done; re-verify).
5. FileExplorer thumbs: already capped; ensure DDS sniff never full-decode on list scroll.

### WP-H — UI/UX polish (P2)

- File Explorer padding / consistency with tokens.
- Color popups + pipette parity.
- Layout wrap in Layers (Fill chips) under all panel widths.
- Remove remaining legacy modals if FE covers them.

---

## 3. Explicit non-goals (Build 16)

- New paint tools, new map kinds, 3D pipeline features.
- Full multi-res **paint** into native map cache (storage exists; paint-at-native = later build).
- Rewrite TileCache/undo from scratch.
- Graphite-style mega PR of whole EditorPanels in one go.

---

## 4. Suggested slice order (for agents)

```
S0  Write UI_RULES.md + ASSET_BROWSER.md          [docs] ✅
S2  Ui::Combo kit API + all call sites            [DRY]  ✅ (0 raw ImGui::Combo left)
S3  Dead modals → File Explorer                   [cleanup] ✅
    + Win32 dialogs → ui/dialogs/Win32FileDialogs ✅
S4a LayersPanel extract                           [structure] ✅ (~767 lines)
S4b ChannelsPanel extract                         [structure] ✅ (~135 lines)
S4c ToolSettingsPanel extract                     [structure] ✅
S4d LayerEffects + ProjectSetup + Colors extracts [structure] ✅
    EditorPanels ~2.7k; panels: Layers/Channels/ToolSettings/FX/Setup/Colors
S4e next (optional): ModSetup / Properties / ViewportNav / Toolbar
S4fix Mojibake UTF-8 fix (вЂ/В·/в† → —, ·, →, …)   [bugfix] ✅
S1  UiColorField kit + Fill/FX migrate             [UI kit] ✅ started
    (pipette still ArmFillPipette; unify brush arm later)
S5  AssetStore + built-in/user paths               [opt foundation]
S6  Fill Layer uses AssetStore                     [opt consumer]
S7  Project assets ↔ .rayp packing                 [opt portable]
S8  Motion/token audit on FE + Fill                [UI polish]
```

Each slice: compile + short manual checklist (below).

---

## 5. Manual smoke checklist (every slice)

- [ ] Open 2K Advanced project (doc size = Diffuse native, not blank 4K)
- [ ] Paint, mask paint, undo layer delete
- [ ] Fill multi-map + channel masks + pipette
- [ ] Add Layer Style (no crash, no multi-second freeze on 2K)
- [ ] File Explorer open/save/import
- [ ] (After S6) Fill + large texture: FPS acceptable, no WS multi-GB spike

---

## 6. Success metrics

| Metric | Target |
|--------|--------|
| `EditorPanels.cpp` lines | Trend down (e.g. &lt; 2k after splits) |
| Raw `ImGui::Combo` outside widgets | Near zero in new code; shrinking legacy |
| Fill + 2K/4K texture | No full-buffer rebake every frame |
| Docs | UI_RULES + ASSET_BROWSER exist and are linked from README/plans |
| Features | Zero “cool but new” tools |

---

## 7. Handoff note for next session

**Build 16 mid-flight — structure largely done.** Release green.  
**Done:** S0–S4d panels + Combo audit + mojibake fix + UiColorField (Fill/FX).  
**EditorPanels ~2.6k** (was ~4.6k). Panels: Layers, Channels, ToolSettings, LayerEffects, ProjectSetup, Colors.  
**Next:** S4e optional (ModSetup/Properties) **or** S5–S6 Asset Browser (Fill lag).  
Do **not** open Canvas for fun; touch Canvas only for AssetStore hooks + Fill asset id.  
User priority quote: *refactor only to reshuffle, dedupe, standardize UI; OOP for UI; Asset Browser forced for Fill lag.*
