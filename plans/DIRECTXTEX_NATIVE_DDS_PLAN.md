# Plan: DirectXTex native DDS · kill texconv · Explorer analyzer · Register UI

Date: 2026-07-12  
Status: **In progress — Phase 0–3 + Register UI + shell format props**

### Known regression (emergency follow-up)

| Symptom | Cause from DirectXTex switch |
|---------|------------------------------|
| Ctrl+E / export ~1–2 min | Software **BC7 encode** (CPU OpenMP) vs texconv; worse if **full mip chain** regenerated |
| Auto mips on open | Was `mipCount > 1` → every export rebuilt all BC7 mips — **fixed** (no longer auto-enable) |
| RSS ~30→~142 MB | Static DirectXTex + OpenMP + ScratchImage temps during compress; decompressed full map in RAM |

**Next emergency:** Fast BC7 preset default, optional GPU compress, avoid full-image ScratchImage copies, strip unused DXTex objects from link if possible.  
Positioning: **DDS-native editor + texture analyzer**, not “PNG paint + CLI compressor”

### Locked decisions (2026-07-12)

| # | Choice |
|---|--------|
| 1 Vendor | **`third_party/DirectXTex` library-only** (sources from `DirectXTex-main/DirectXTex`, no tools/samples) |
| 2 Compress | **CPU + OpenMP** first; no DirectCompute/GPU BC in v1 |
| 3 Shell | **Light shell** — thumbs stay header+bcdec; no DirectXTex in explorer DLL |

---

## Why

| Today | Problem |
|-------|---------|
| `texconv.exe` on compressed export | Black box, process spawn, path hunting, not “native” |
| `DdsHelper` + `bcdec` load path | Partial DXGI coverage; exotic dumps fail |
| Tiny `enum class DdsFormat` (8 values) | Cannot represent R10G10B10A2, BC6H, BGRA, typeless, etc. |
| Export format = free-text UI strings | Drift from real DXGI; round-trip is incomplete |
| Shell Property Handler | Format label ok for common types; not full analyzer |
| No Register UI | Admin/HKLM + associations are invisible to users |

**DirectXTex** (Microsoft, v2.1.x in `DirectXTex-main/`) is what texconv wraps. In-process = same quality, full format matrix, no CLI.

**paintnet-dds-plugin/** is a **reference only** (API shape: load info, save options, BC7 speed, swizzles). We use **fresh** `DirectXTex-main`, not the plugin’s vendored older tree.

---

## Non-goals (this plan)

- Xbox / GDK / D3D12 resource helpers  
- OpenEXR / libjpeg / libpng inside DirectXTex (we already have ImageManager/stb)  
- Shipping `texconv.exe` / Texassemble / DDSView  
- Rewriting TileCache architecture (only wire formats into it)  
- Making shell DLL depend on full D3D11 device (thumbs stay CPU)

---

## Target architecture

```
┌─────────────────────────────────────────────────────────────┐
│ UI (File Explorer / Export / Properties / Register)         │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│ Canvas / Project  ·  SourceDdsInfo + DocumentBitDepth       │
│  open → auto depth + export snapshot                         │
│  save → ExportWithProjectSettings / batch                    │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│ DdsCodec  (NEW facade — single source of truth)             │
│  Load → ScratchImage → RGBA8/F16/F32 tiles                   │
│  Save → Image → Compress / Convert → DDS                     │
│  Sniff / Analyze → SourceDdsInfo (header + DXGI name)        │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│ DirectXTex (static lib, CPU + optional BC GPU)              │
│  LoadFromDDS*  Compress  GenerateMipMaps  Convert  Save*     │
└─────────────────────────────────────────────────────────────┘

Shell (RayVPaint_DdsThumb.dll) — keeps light path:
  Header parse + optional decompress for thumb
  PropertyStore ← shared Analyze() labels (no full app deps)
```

---

## Phase 0 — Vendor DirectXTex correctly

### 0.1 What to take (minimal)

From `DirectXTex-main/`, **only the library**:

```
DirectXTex/
  DirectXTex.h / .inl
  DirectXTexP.h, DDS.h, BC.h, filters.h, scoped.h
  BC.cpp, BC4BC5.cpp, BC6HBC7.cpp
  DirectXTexCompress.cpp
  DirectXTexCompressGPU.cpp   # optional: BC6/7 GPU path if fxc available
  DirectXTexConvert.cpp
  DirectXTexDDS.cpp
  DirectXTexImage.cpp
  DirectXTexMipmaps.cpp
  DirectXTexMisc.cpp
  DirectXTexResize.cpp
  DirectXTexUtil.cpp
  (+ FlipRotate / PMAlpha / NormalMaps / HDR / TGA / WIC if cheap — not required for DDS core)
```

**Do not ship:** `Texconv/`, `Texassemble/`, `Texdiag/`, `DDSView/`, `DDSTextureLoader/`, `ScreenGrab/`, Xbox Auxiliary, full repo root samples.

**Recommended layout:**

```
third_party/DirectXTex/     # copy or git subtree of library only
  CMakeLists.txt            # thin wrapper OR add_subdirectory with options
```

Keep `DirectXTex-main/` as vendor cache **or** replace with `third_party/` and document “sync from upstream”. Do **not** add the whole tree to RayVPaint sources as loose files without CMake isolation.

### 0.2 CMake (root `CMakeLists.txt`)

```cmake
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_DX12 OFF CACHE BOOL "" FORCE)   # we are D3D11 app
set(BUILD_DX11 ON CACHE BOOL "" FORCE)    # optional GPU BC compress
set(BC_USE_OPENMP ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)

add_subdirectory(third_party/DirectXTex EXCLUDE_FROM_ALL)
# or: add_subdirectory(DirectXTex-main) with same options

target_link_libraries(RayVPaint_Core PRIVATE DirectXTex)
```

- Static CRT alignment: match RayVPaint (`MultiThreaded$<$<CONFIG:Debug>:Debug>`)  
- OpenMP for BC7 software (Paint.NET-style speed tiers map to flags)  
- **GPU compress** (DirectCompute): optional Phase 1.5; needs prebuilt shaders / fxc — can start **CPU-only** for parity with texconv Medium/Slow  

### 0.3 paintnet-dds-plugin usefulness

| Useful | Ignore |
|--------|--------|
| Format list + user labels (`DdsFileFormat.cs`, `DXGI_FORMAT.cs`) | C# / PDN plugin host |
| Save options: BC7 speed, error metric, legacy DX9, RXGB | Full 3rdParty tree (older DXTex) |
| Swizzle handling (RXGB normals) | Their build system |
| Pattern: ScratchImage load → edit → compress → save | |

**Action:** treat as **spec reference**, not a dependency.

### 0.4 Deliverable

- Core links DirectXTex statically  
- Smoke: `LoadFromDDSFile` on `testfield/*.dds` including previously “unsupported”  
- CI/local build time note (BC7 OpenMP is heavy — OK)

---

## Phase 1 — `SourceDdsInfo` + format tables (replace tiny enum)

### 1.1 New types (`src/core/DdsTypes.h`)

```cpp
struct SourceDdsInfo {
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  int width = 0, height = 0, depth = 1;
  int mipCount = 1, arraySize = 1;
  bool isCube = false, isVolume = false, isDx10 = false;
  bool srgb = false;                 // derived from *_SRGB formats
  bool premultipliedAlpha = false;
  std::string fourCC;                // "DXT5", "DX10", …
  std::string formatLabel;           // "BC7_UNORM_SRGB", "R10G10B10A2_UNORM", …
  std::string uiLabel;               // Paint.NET-style for combos
  DocumentBitDepth suggestedDepth; // U8 / F16 / F32
  // Export snapshot (round-trip defaults)
  bool generateMips = false;
  std::string compressionSpeed = "Medium"; // Fast/Medium/Slow/Best
};
```

- **Kill** (or demote) `enum class DdsFormat` with 8 values  
- Map table: `DXGI_FORMAT` ↔ UI label ↔ category (Color / Normal / HDR / Single-channel)  
- Source of truth for labels: merge `full_list.txt` + DirectXTex `GetName` + PDN list  

### 1.2 `DdsCodec` facade (`src/core/DdsCodec.h/.cpp`)

| API | Role |
|-----|------|
| `Analyze(path/bytes) → SourceDdsInfo` | Header + DXGI (DirectXTex `GetMetadataFromDDS*` or our header + DXGI map) |
| `LoadToTileCache(...)` | Decompress/convert to project storage |
| `SaveFromRgba*(..., SourceDdsInfo or explicit DXGI)` | Compress/mips/save |
| `FormatUiList()` | Full combo data |
| `SuggestDocumentBitDepth(dxgi)` | Auto U8/F16/F32 |

**Load policy:**

1. `LoadFromDDSMemory/File`  
2. If block-compressed → `Decompress` to R8G8B8A8 or float intermediate  
3. Convert to document storage:
   - UNORM 8-bit family → `DocumentBitDepth::U8` / RGBA8 tiles  
   - FLOAT16 / half → F16  
   - FLOAT32 / R11G11B10 / depth-ish → F32 (or F16 with clamp option later)  
4. Never silent-quantize HDR to U8 without log + auto depth switch  

**Save policy:**

1. Composite to linear RGBA matching export needs  
2. `Convert` to compressible intermediate if needed  
3. `Compress` (BC*) or pack uncompressed DXGI  
4. `GenerateMipMaps` if requested  
5. `SaveToDDSFile` (DX10 header when required)  

### 1.3 Delete / shrink tails

| Remove / stop using | Replacement |
|---------------------|-------------|
| `TexconvHelper.*` | `DdsCodec::Save*` |
| `texconv.exe` shipping in Release | none |
| Stringly `MapFormatToTexconv` | DXGI enum |
| Partial DXGI switches in `DdsHelper::Load*` | DirectXTex |
| Duplicate format arrays in 4 UI files | one `DdsFormatCatalog` |
| Batch `SaveWithCodec` temp PNG + texconv | DirectXTex from RGBA8 memory |

**Keep temporarily:** `bcdec` in **shell thumb DLL only** (no DirectXTex in explorer process for v1) — or later link DirectXTex CPU-only into shell if thumbs need rare formats.

### 1.4 Deliverable

- Open any `testfield` DDS that failed before  
- Save BC7/BC5/BC1/R10G10B10A2/RGBA16F without texconv  
- Unit/smoke script: load → sniff label → save → re-sniff matches  

---

## Phase 2 — Replace texconv (export / batch)

### 2.1 Call sites

| Site | Change |
|------|--------|
| `Canvas::SaveCanvasCompressed` | → `DdsCodec::Save` |
| `Canvas::SaveCanvas` (native uncompressed) | → same facade (or keep thin path) |
| `TextureSetIO` batch DDS | → `DdsCodec` per map |
| `ExportWithProjectSettings` | uses snapshot `SourceDdsInfo` / `m_Export*` |

### 2.2 Settings mapping (texconv → DirectXTex)

| Old UI | DirectXTex |
|--------|------------|
| Format combo | `DXGI_FORMAT` |
| Mipmaps on/off | `GenerateMipMaps` / single level |
| Mip filter | `TEX_FILTER_*` (POINT/LINEAR/CUBIC/FANT/…) |
| Quality Fast/Medium/Slow/Best | BC7: `TEX_COMPRESS_BC7_QUICK` vs default vs exhaustive; BC6 similar; optional GPU |

### 2.3 Color / sRGB

- Preserve current correctness: composite is display-referred sRGB bytes for U8 color maps  
- For `*_SRGB` targets: set alpha mode / metadata flags per DirectXTex conventions (document in code)  
- Linear data maps (normals, masks): never force sRGB DXGI  

### 2.4 Deliverable

- No process spawn on export  
- Binary size + time acceptable (log compress ms)  
- Remove `texconv.exe` from `build.bat` / Release packaging  

---

## Phase 3 — Explorer: Property Handler as texture analyzer

### 3.1 Already done (baseline)

- ProgID `RayVPaint.dds`, InfoTip, KindMap  
- Props: Dimensions, Format, MipCount, Flags  
- Thumb handler (HKLM)

### 3.2 Upgrade (this plan)

Expand `ddsinfo` / PropertyStore to match **full analyzer** (header-only, still no GPU):

| Property | Source |
|----------|--------|
| `RayV.Dds.Format` | DXGI name + FourCC |
| `RayV.Dds.DxgiValue` | numeric DXGI_FORMAT |
| `RayV.Dds.MipCount` | header |
| `RayV.Dds.ArraySize` | DX10 |
| `RayV.Dds.Flags` | Cube / Volume / Array / DX10 / sRGB |
| `RayV.Dds.BitDepth` | approx bpp |
| `System.Image.*` | w/h/dimensions |

Optional later: first-mip color space hint, block size, pitch.

**propdesc:** extend `RayVPaint.Dds.propdesc` for new keys; re-`PSRegisterPropertySchema`.

**Shared code:** `DdsHeaderInfo` already in shell; keep shell **independent** of full DirectXTex for stability. Format name table can be **generated** or shared header-only `DdsFormatNames.h` included by both Core and shell.

### 3.3 Deliverable

- Explorer tooltip/columns show **BC7_UNORM_SRGB**, **R10G10B10A2_UNORM**, etc.  
- No regression on thumb registration  

---

## Phase 4 — Register / Unregister button (Blender-style)

### 4.1 UX

**Help** (or **File → System integration**):

```
[ Register file associations & Explorer DDS ]
  Status: ● Admin rights  ● DDS thumbs (HKLM)  ● Property schema  ● PNG Photo handler
[ Unregister ]
```

- Green/red/yellow chips (no admin / partial / full)  
- Register: if not elevated → **UAC once** (`regsvr32` or embedded elevate)  
- Unregister: remove our CLSIDs, ProgID, property handler, schema; do **not** wipe user’s UserChoice  

### 4.2 What Register does (single entry)

1. `RayV_RegisterDdsThumbnails` (thumbs + props + ProgID + KindMap + propdesc)  
2. PNG/JPG restore Photo handler if DriveFS hijacked  
3. Optional: OpenWithProgids for `.dds` / `.png` pointing at launcher  
4. **Do not** put ShellEx on `Applications\RayVPaint*.exe`  
5. `SHChangeNotify(ASSOCCHANGED)`  

### 4.3 Status API (`DdsThumbRegister` expand)

```cpp
struct IntegrationStatus {
  bool dllPresent;
  bool hklmThumbClsid;
  bool hklmPropClsid;
  bool propdescRegistered;  // best-effort
  bool isElevated;          // current process
  bool pngPhotoHandlerOk;
};
```

### 4.4 Deliverable

- Non-admin user sees clear “need Admin”  
- Admin click → full Explorer DDS experience without manual regedit  

---

## Phase 5 — UI: import/export + internal File Explorer + round-trip

### 5.1 Single format catalog

`DdsFormatCatalog` used by:

- Advanced Export FE  
- Batch Export FE  
- Properties panel  
- Project Setup Export  
- Internal File Explorer details column  

### 5.2 Open path (auto)

```
open .dds
  → Analyze → SourceDdsInfo
  → SetDocumentBitDepth(suggested)   // U8/F16/F32
  → Load tiles
  → m_SourceDdsInfo = info
  → m_Export* synced from info (format, mips if file had >1, container=DDS)
  → FE status / Properties: "Source: BC7_UNORM_SRGB 2048×2048 · 11 mips"
```

User does **not** have to re-pick format to “save back the same”.

### 5.3 Save path (defaults)

- **Save / Quick Export:** if `m_SourceDdsInfo` valid and container DDS → save with that DXGI (+ current mip/quality prefs)  
- **Export dialog:** preselect source format; user can override  
- **PNG open:** container PNG + ICC; depth U8 (unless 16-bit PNG later)  

### 5.4 Internal File Explorer

- Column **Format** for `.dds` via `Sniff` / `Analyze` (header only, async cache)  
- Tooltip: dimensions + DXGI + mips  
- Reuse shell naming table  

### 5.5 Export UI cleanup

- One format combo source  
- Hide/show mip/quality only for block formats  
- Remove dead texconv-only filter names  

### 5.6 Deliverable

- Open game BC7 → paint → Ctrl+E → BC7 out without touching format UI  
- Open R16F → project F16 → export R16F or BC6 by choice  
- FE shows format before open  

---

## Implementation order (matches your 1–6)

| # | Phase | Effort | Risk |
|---|--------|--------|------|
| **1** | DirectXTex CMake + link Core | M | Build time / CRT |
| **2** | Shell format props polish (names + DxgiValue) | S | propdesc cache |
| **3** | DdsCodec save path; kill TexconvHelper | L | Color/mips parity |
| **4** | Expand load; SourceDdsInfo; delete tails | L | Bit-depth edge cases |
| **5** | Register / Unregister UI + status | M | UAC UX |
| **6** | FE column + auto round-trip + UI catalog | M | State sync bugs |

**Suggested PR splits:**

1. **PR-A:** Vendor DirectXTex + empty link smoke  
2. **PR-B:** `DdsCodec` load via DirectXTex (keep save=texconv briefly if needed)  
3. **PR-C:** Save via DirectXTex; delete texconv  
4. **PR-D:** SourceDdsInfo + auto depth + export snapshot  
5. **PR-E:** Shell analyzer props + FE format column  
6. **PR-F:** Register button UI  

(You can merge B+C if preferred.)

---

## Testing matrix

| Asset | Expect open | Expect default save |
|-------|-------------|---------------------|
| BC7 sRGB color | U8, label BC7_UNORM_SRGB | BC7 sRGB |
| BC5 normal | U8, RG | BC5 |
| R8G8 | U8 | R8G8 |
| R10G10B10A2 | U8 or F16 policy | same DXGI |
| RGBA16F / BC6H | F16 | same family |
| R32F depth dump | F32 | R32F |
| 16K BC7 | open without OOM; thumb still header/mip | compress works |
| Batch multi-map ZZZ | per-map codec or global | no texconv |

Regression: PNG ICC, batch pack, thumb HKLM, no Applications ShellEx pollution.

---

## Risks & mitigations

| Risk | Mitigation |
|------|------------|
| BC7 CPU slow on 4K/8K | OpenMP; optional GPU compress later; quality presets |
| Shell + DirectXTex size/crash | Keep shell on header+bcdec; share names only |
| DXGI typeless / planar | Explicit unsupported list + clear error |
| Round-trip sRGB washout | Golden image tests vs previous texconv output |
| Admin registration friction | Status UI; installer still best for end users |

---

## Success criteria

1. **No `texconv.exe`** in product or code paths  
2. **Open** any DXGI DDS DirectXTex supports (or explicit “unsupported planar”)  
3. **Save** full catalog including R10G10B10A2, BC6H, BC7, float  
4. **Explorer** shows real format string + dimensions + mips  
5. **Register** button works elevated; status honest when not  
6. **Round-trip:** open → save without UI format changes preserves DXGI family  
7. Marketing-true: **DDS-native**, library in-process, not CLI wrapper  

---

## Out of scope follow-ups

- In-app “DDS Inspector” panel (histogram, mip viewer) — natural next after SourceDdsInfo  
- GPU BC7 always-on  
- Writing custom swizzled RXGB with PDN parity  
- 16-bit PNG  

---

## Decision needed from you (before code)

1. **Vendor path:** `add_subdirectory(DirectXTex-main)` with tools off, **or** copy library-only into `third_party/DirectXTex`?  
   - Recommendation: **`third_party/DirectXTex` library-only** (cleaner tree; leave `DirectXTex-main` as reference zip).  
2. **GPU BC compress in v1?** Recommendation: **CPU first**, GPU as fast-follow.  
3. **Shell:** stay bcdec thumbs vs link DirectXTex into explorer DLL? Recommendation: **stay light** for v1.

Once you confirm those three, implementation can start at Phase 0.
