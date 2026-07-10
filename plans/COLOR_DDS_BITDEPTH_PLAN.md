# Plan: DDS formats · PNG ICC · float/bit-depth (memory-aware)

Date: 2026-07-10  
Branch: DX11-upgrade

---

## Goals (user)

1. **All Paint.NET DDS formats** from `testfield/full_list.txt` for save  
2. **PNG + ICC** must not produce white images  
3. **DDS export options** must actually change format / size / mips / quality  
4. **Open** float16/32, 10-bit, unusual DXGI (incl. VRAM dumps)  
5. **PNG** open/export awareness of 8/24/32 bpp  
6. **Project bit depth** (like PS): 8 / 16 / 32 float working space; export free  
7. **Do not kill** speed: 8-bit projects stay tile-sparse + RGBA8 storage; 16/32 opt-in

---

## Root-cause notes (current code)

| Issue | Cause |
|-------|--------|
| PNG white + ICC | Synthetic `IccProfiles` matrix incomplete (no D65→D50 CAT); broken iCCP → CMMs blow out to white |
| DDS options “do nothing” | Often still **RGBA8 uncompressed (~4 MB for 1K²)**; texconv path search fails OR success returned without verifying output; UI format list incomplete; mip filter names mismatch (`Cubic` vs `Bicubic`) |
| Float open weak | `LoadDDSToTileCache` only a few DXGI codes; always **quantizes to RGBA8** in cache |
| Project depth | Only `CanvasPixelFormat::{RGBA8,RGBA32F}`; no document-level policy |

---

## Architecture (bit depth without killing 8-bit)

```
DocumentBitDepth { U8, F16, F32 }
  → TileCache storage format
  → brush write quantize (U8: 1/255, F16: half, F32: full)
  → GPU layer texture DXGI matching depth when possible

Default NEW / typical open image → U8 (current perf)
Open HDR/float DDS → auto F16 or F32
User can convert project depth (with undo later)
Export ALWAYS free (any DDS/PNG target regardless of project depth)
```

**Memory rule:** never allocate full-doc float32 for U8 projects. Composite export paths stay format-aware (RGBA8 stream for U8).

---

## Phases

### P0 — Correctness (do now)

1. **PNG ICC fix**  
   - sRGB: prefer PNG `sRGB`+`gAMA`+`cHRM` chunks (no broken iCCP)  
   - Or embed known-good ICC binary  
   - None: no colorimetry chunks  
   - P3/Adobe: valid profiles or temporary disable inject with warn  

2. **Texconv pipeline fix**  
   - Locate `texconv.exe`: bin, parent, repo root, PATH  
   - Wide UTF-8 paths for src/dst  
   - Map **entire** `full_list.txt` → DXGI `-f` names  
   - Wire UI combo to full list  
   - Align mip filter strings with UI  
   - **Verify** output file exists + size; log format + bytes  
   - On failure: return false, never leave wrong RGBA8 as “success”  

3. **DDS open: R11G11B10_FLOAT, D32_FLOAT_S8X24**  
   - Decode to float working buffer → store as project depth F16/F32 or quantize U8 with warn  
   - Depth: R = depth normalized, G=B=R, A=1 (visual); no false “unsupported”  

### P1 — Format coverage

4. Expand `DdsFormat` / export enum for all list entries  
5. PNG load: detect channels (1/3/4), bit depth via STB; export 8-bit RGBA for now, later 16-bit PNG  
6. Smoke: export BC1/BC7/no-mips/mips size differs  

### P2 — Document bit depth

7. `DocumentBitDepth` in Canvas + .rayp meta — **done** (scaffold + real convert)  
8. TileCache: **RGBA16F** storage — **done** (`HalfFloat.h`, 8 B/px)  
9. Brush: paint float; U8 quantize only — **done** (core); UI HDR pickers later  
10. Color UI: float RGB when depth > U8 (pickers) — **TODO UI**  
11. Convert project depth dialog — API `SetDocumentBitDepth` **done**; menu later  

See also `plans/P2_BITDEPTH.md`.

### P3 — Perf guards

12. Proxy composite stays ≤2048 regardless of depth  
13. Forbid accidental full 16K F32 composite (existing guards)  
14. Optional: F16 as default “HDR open” instead of F32  

---

## Acceptance

- [ ] PNG + sRGB ICC opens colored (not white) in browsers/viewers  
- [ ] BC1 file << BC7 << RGBA8 for same resolution  
- [ ] Mips on ≈ +33% size vs mips off  
- [ ] `non-usual-case` DDS open + paint stroke preserves relative values  
- [ ] U8 project memory ≈ today; F32 opt-in only  

---

## Implementation order (this session)

1. Write this plan  
2. Fix PNG ICC  
3. Fix + expand TexconvHelper + UI format list  
4. Open R11G11B10 + depth DXGI  
5. Scaffold `DocumentBitDepth` (meta + default U8) without full F16 pipeline yet  
