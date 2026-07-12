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

Log: `%USERPROFILE%/Documents/RayVPaint/user/rayv_paint.log`
