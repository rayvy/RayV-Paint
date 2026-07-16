# RayV Paint — Documentation

**Status:** Work in Progress (not a public release).  
**Policy:** Format and API may change freely. No legacy compatibility for pre-standardization files (`.rvbrush`, bare preset `.json`, loose Core PNGs).

This document is the **canonical** terminology and file-system standard for RayV Paint.

---

## 1. Terminology

| Term | Meaning |
|------|---------|
| **Document / Project** | An open paint session, saved as `.rayp`. Holds layers, maps, and referenced **project** assets. |
| **Raster layer** | Pixel buffer on the canvas. **Not** an asset until converted/imported. |
| **Asset** | Addressable library resource with a stable **key** and ownership **category**. |
| **Package** | On-disk container (`RVPK`) used by RVPCF / RVPAF / RVPBF. |
| **Core** | Built-in read-only library next to the executable (`{exe}/assets/`). |
| **User / Library** | Writable library under `Documents/RayVPaint/`. |
| **Project / Session** | In-memory / packed-into-`.rayp` assets for the current document. |
| **External** | One-shot absolute path outside libraries (prefer promote to Project on save). |
| **Fill** | Parametric layer writing solid / textured color into maps; samples assets by key. |
| **SmartObject** | Non-destructive layer holding source identity + display cache (foundation; SVG deferred). |
| **Config package** | Settings / templates (export, shaders) — **RVPCF**. |
| **Asset package** | Media assets (textures, …) — **RVPAF**. |
| **Brush package** | Brush preset + tip resources — **RVPBF**. |

### 1.1 Asset categories (ownership)

| Category | Root | Writable | In `.rayp` |
|----------|------|----------|------------|
| **Core** | `{exe}/assets/` | No | No |
| **User** | `Documents/RayVPaint/assets/` | Yes | No |
| **Project** | Session table + blobs | Session | **Yes** (referenced only) |
| **External** | Absolute path | N/A | Promote → Project on save |

### 1.2 Stable asset keys

```
core:<relative path under exe assets>
user:<relative path under user assets>
proj:<uuid>
ext:<normalized absolute path>
```

Examples:

- `core:textures/checker.rvpaf`
- `user:textures/my_noise.rvpaf`
- `proj:a1b2c3d4-…`

### 1.3 Asset kinds (consumer filter)

| Kind | Package / extension | Typical consumers |
|------|---------------------|-------------------|
| `texture` | `.rvpaf` | Fill, Outline, future decals |
| `brush` | `.rvpbf` | Brush library / paint engine |
| `export_template` | `.rvpcf` (`kind=export_template`) | File Explorer export flow |
| `shader_preset` | `.rvpcf` (`kind=shader_preset`) | 3D Preview materials |
| `smart_source` | (future) | SmartObject sources |

Fill and Outline accept **`texture` only** (UI + API).

---

## 2. Package formats (standard)

All three product formats share one binary container:

### 2.1 Container: RVPK

```
offset  content
------  -------
0       u32 magic   = 'RVPK' (0x4B505652 little-endian)
4       u32 version = 1
8       u32 manifestSize
12      u8  manifestJson[manifestSize]   // UTF-8
…       u32 resourceCount
        repeated resourceCount times:
          u16 pathLen
          u8  path[pathLen]              // relative, '/' separators
          u32 dataLen
          u8  data[dataLen]
```

**Manifest** (JSON) always includes:

```json
{
  "format": "RVPCF" | "RVPAF" | "RVPBF",
  "version": 1,
  "id": "stable_id",
  "displayName": "Human name",
  "kind": "texture | brush | export_template | shader_preset | …"
}
```

Additional fields are format-specific.

---

### 2.2 RVPCF — RayV Painter Config File (`.rvpcf`)

**Purpose:** export templates, 3D shader / material presets (and future configs).

**Typical layout:**

| Resource path | Role |
|---------------|------|
| `config.json` | Primary settings (required) |
| `shaders/*.hlsl` | Optional override / helper shaders |

**Kinds:**

| `kind` | `config.json` content |
|--------|------------------------|
| `export_template` | Export naming, container, mip defaults |
| `shader_preset` | Channel graph + lighting knobs (MaterialConfig) |

**Example export template config:**

```json
{
  "namePattern": "{set}{suffix}",
  "exportAllSets": true,
  "container": "dds",
  "generateMipMaps": true
}
```

**Locations:**

- Core: `{exe}/assets/configs/`
- User: `Documents/RayVPaint/presets/` (shaders under `presets/shaders/`)

---

### 2.3 RVPAF — RayV Painter Asset File (`.rvpaf`)

**Purpose:** library textures and similar media. **Raster layers are not RVPAF** until import/convert.

**Typical layout:**

| Resource path | Role |
|---------------|------|
| `image.png` or `image.dds` | Full asset (required) |
| `thumbnail.png` | 32×32 browser grid (recommended) |
| `thumbnail_h.png` | 128×128 hover preview (optional) |

**Manifest extras:**

```json
{
  "format": "RVPAF",
  "kind": "texture",
  "width": 256,
  "height": 256,
  "image": "image.png",
  "thumbnail": "thumbnail.png",
  "thumbnail_h": "thumbnail_h.png"
}
```

**Import policy:**

- User import of PNG/JPG/DDS → **always written as `.rvpaf`** into User library.
- Fill/Outline file pick → Project session (`proj:`) for portable `.rayp`.

**Locations:**

- Core: `{exe}/assets/textures/*.rvpaf`
- User: `Documents/RayVPaint/assets/textures/*.rvpaf`

---

### 2.4 RVPBF — RayV Painter Brush File (`.rvpbf`)

**Purpose:** brush presets. Self-contained for sharing.

**Typical layout:**

| Resource path | Role |
|---------------|------|
| `config.json` | Brush parameters + tip type |
| `tip.raw8` | Optional grayscale tip (`size×size` bytes) |

**config.json tip types:**

- `none` — procedural round
- `builtin` — engine tip id (`soft_round`, …)
- `embedded` — pixels in `tip.raw8`

**Location:**

- User: `Documents/RayVPaint/brushes/*.rvpbf`

Built-in brushes remain in-engine (not disk packages).

---

## 3. Document format (`.rayp`)

Unchanged at high level: `RAYP` magic + JSON metadata + zlib blobs.

**Assets:**

- Layers store **keys** (`texture_asset_key`, `outline_texture_asset_key`, `smart_asset_key`).
- On save, referenced **`proj:`** assets are packed as `project_assets[]` + zlib blobs **before** layer pixels.
- **`ext:`** keys are promoted to **`proj:`** on save when payload/path is available.
- **`core:` / `user:`** are not packed (machine/app must provide library).

---

## 4. Runtime systems

| Component | Role |
|-----------|------|
| `rvp::PackageIO` | Read/write RVPK |
| `assets::AssetManager` | Façade: list, import, load, kind gate |
| `assets::AssetStore` | Decode/cache payloads (async) |
| `assets::AssetLibraryIndex` | Scan Core/User catalogs |
| `assets::AssetThumbCache` | 32/128 thumbs (from package or generate) |
| `BrushLibrary` | Builtins + `*.rvpbf` disk presets |
| `preview3d::ShaderPresetLibrary` | Builtins + `*.rvpcf` shader presets |

**Async contract:** UI never blocks on full decode. Missing/pending assets → solid fallback (Fill) or skip sample (Outline bake).

**Fill texture GPU path:** pattern uploaded at source resolution; composite samples with wrap UV (no full-document CPU bake unless FX requires it).

---

## 5. Directory map

```
{exe}/
  assets/                          # Core
    textures/*.rvpaf
    configs/*.rvpcf                # export templates etc.
    configs/shaders/*.rvpcf        # optional shipped shaders
  shaders/                         # app HLSL (engine)
  …

Documents/RayVPaint/
  user/config.json                 # app prefs (not RVPCF)
  assets/textures/*.rvpaf          # User library
  brushes/*.rvpbf
  presets/shaders/*.rvpcf
  temp/
```

---

## 6.1 Related plans

| Plan | Topic |
|------|--------|
| `plans/API_STANDARTIZATION.md` | Operators, AppContext, keymap, Python/document API — **foundation in code:** `src/core/ops/` (ActionCatalog + AppContext + TryConsumeAction) |
| `plans/Standartization(Krita/Photoshop).MD` | Lossy PSD/KRA/ABR interop (**after** API plan) |

## 6. What we deliberately do **not** support

- Opening old `.rvbrush` JSON brushes  
- Loading bare MaterialConfig `.json` as presets (use `.rvpcf`)  
- Treating loose Core PNGs as library assets (use `.rvpaf`)  
- SVG SmartObject rasterization (disabled until a real vector pipeline)

---

## 7. Extending the system

1. New media type → prefer **RVPAF** `kind` + resource paths.  
2. New settings pack → **RVPCF** `kind` + `config.json` schema.  
3. New tool consumer → call `AssetManager` with `AssetFilter.kind`; never re-decode paths in UI.  
4. Update this document when adding a `kind` or resource path convention.

---

## 8. Source map (implementation)

| Area | Path |
|------|------|
| Package I/O | `src/package/PackageIO.*`, `PackageTypes.h` |
| Asset core | `src/assets/*` |
| Brushes | `src/core/BrushLibrary.*` |
| Shader presets | `src/preview3d/MaterialConfig.*` |
| Fill / outline bind | `src/Canvas.*`, `src/layer/LayerStyles.*` |
| Browser UI | `src/ui/panels/AssetBrowserPanel.*`, `src/ui/widgets/UiAsset*` |
| Python host | `src/scripting/ScriptingEngine.*`, `ScriptDocApi.*`, `ScriptPluginHost.*`, `ScriptMainThread.*`, `ScriptImage.*`, `ScriptUiPreview.*` |
| Crash capture | `src/core/CrashGuard.*` → `Documents/RayVPaint/user/crash_last.log` + `crash_*.dmp` |
| Built-in plugins | `{exe}/scripts/*.py` (shipped from `assets/scripts/`) |
| User plugins | `Documents/RayVPaint/scripts/*.py` |

---

## 9. Python scripting (authoritative API for writers)

**Audience:** humans and AI assistants writing plugins **using only this document** (and `user/rayv_paint.log` / `user/crash_last.log` if something fails).  
Runtime: **`import rayv`**.  

**Safety:** invalid arguments are **refused** (`False` / empty / `{ok: False, error: ...}`) and **logged**. They must not crash the host.

**Crash diagnostics:** on fatal faults the host writes  
`Documents/RayVPaint/user/crash_last.log` and a minidump `crash_*.dmp`, plus a MessageBox.

### 9.1 Where scripts live

| Location | Path | When loaded |
|----------|------|-------------|
| **Built-in** | `{exe}/scripts/*.py` | Startup + **Scripting → Refresh Scripts** |
| **User** | `Documents/RayVPaint/scripts/*.py` | Same |
| **CLI one-shot** | any path | `--script path.py` (after operators registered) |

Rules:

- Filename stem = plugin id (`tiled_export.py` → id `tiled_export`).
- Files starting with `_` are ignored as plugins.
- Each plugin is a module. Optional:

```python
PLUGIN = {
    "name": "Human Title",   # menu label base
    "menu": "Menu entry…",   # optional
}

def on_load():
    """Called once when the plugin is (re)imported."""
    pass

def on_open():
    """Called when user picks the plugin in Scripting menu."""
    pass

def on_ui():
    """Called every ImGui frame. Draw windows only here (via rayv.ui)."""
    pass
```

Menu: **Scripting → Refresh Scripts**, then the plugin list.  
**Scripting → Open user scripts folder** opens the User directory.

### 9.2 Logging

```python
rayv.log_debug("…")
rayv.log_info("…")
rayv.log_warn("…")
rayv.log_error("…")
```

Messages appear in the app log / console with a `[Python]` prefix when relevant.

### 9.3 Operators — `rayv.ops`

Same registry as hotkeys/menus (`ActionCatalog` + `OperatorRegistry`).

| Call | Meaning |
|------|---------|
| `rayv.ops.list()` | List of dicts: `id`, `label`, `category`, `shortcut`, `has_execute`, `can_invoke`, `note` |
| `rayv.ops.has_execute(id)` | Handler registered? |
| `rayv.ops.can_invoke(id)` | Poll would pass **now** (text field / File Explorer / modals block document ops) |
| `rayv.ops.invoke(id, force=False)` | Run op. Returns `"finished"` \| `"blocked"` \| `"cancelled"` \| `"pass_through"` |

- `force=False` (default): respects **AppContext** (safe for UI-driven code).
- `force=True`: skip poll — use for **headless / CLI tests** only.

Examples of ids: `SelectAll`, `Deselect`, `FillSecondary`, `Undo`, `BrushTool`, `AdjustNoise`, `SwapColors`, `Paste`.

```python
rayv.ops.invoke("SelectAll", force=True)
rayv.ops.invoke("Deselect", force=True)
```

### 9.4 Threading contract — `rayv.host` (**required for async / ComfyUI-style tools**)

| Rule | Detail |
|------|--------|
| **Main thread** | UI frame + `on_ui()` + host job drain. |
| **Mutating `rayv.doc.*`** | **Only on main thread.** Off-main → refused + log: *use `rayv.host.call_on_main`*. |
| **Workers** | Free to run `urllib` / sockets / long CPU. **Do not** call `doc.set_*` from the worker. |
| **Apply results** | Worker finishes → `rayv.host.call_on_main(lambda: apply())`. Host drains the queue **every frame** (before and after plugin `on_ui`). |

| Call | Notes |
|------|--------|
| `rayv.host.is_main_thread()` | bool |
| `rayv.host.call_on_main(fn)` | Thread-safe; queues `fn` for main. Returns job id (int). |
| `rayv.host.pending_jobs()` | Queue depth (debug) |

**Canonical async pattern (external generator):**

```python
import threading, rayv

_state = {"busy": False, "progress": 0.0, "status": "", "preview_key": "gen_preview"}

def _worker(prompt, model, init_png_bytes):
    try:
        # … HTTP to Comfy / other service (seconds) …
        # result_png = response body
        dec = rayv.image.decode(result_png)
        if not dec["ok"]:
            raise RuntimeError(dec["error"])
        rgba, w, h = dec["rgba8"], dec["width"], dec["height"]
        def apply():
            layer = rayv.doc.active_layer()
            if not rayv.doc.begin_edit(layer):
                _state["status"] = "begin_edit failed"
                _state["busy"] = False
                return
            # place at selection origin or (0,0)
            bb = rayv.doc.selection_bounds()
            x = bb[0] if bb else 0
            y = bb[1] if bb else 0
            ok = rayv.doc.set_layer_rgba8(layer, w, h, rgba, x, y)
            if ok:
                rayv.doc.end_edit("AI Generate")
                rayv.ui.set_image_rgba8(_state["preview_key"], rgba, w, h)
                _state["status"] = "applied"
            else:
                rayv.doc.cancel_edit()
                _state["status"] = "set_layer_rgba8 refused"
            _state["busy"] = False
            _state["progress"] = 1.0
        rayv.host.call_on_main(apply)
    except Exception as e:
        def fail():
            _state["busy"] = False
            _state["status"] = str(e)
            rayv.log_error(str(e))
        rayv.host.call_on_main(fail)

def on_ui():
    # progress_bar + Generate button; start thread only if not busy
    rayv.ui.progress_bar(_state["progress"], _state["status"])
    if _state["busy"]:
        return
    if rayv.ui.button("Generate"):
        _state["busy"] = True
        _state["progress"] = 0.05
        # build init image on main, then start worker
        reg = rayv.doc.get_region_for_generate(rayv.doc.active_layer())
        # … encode reg to PNG via rayv.image.encode_png if needed …
        threading.Thread(target=_worker, args=(...), daemon=True).start()
```

### 9.5 Document — `rayv.doc`

Active project tab only. Coordinates: **integer document pixels**.  
Float colors: **straight RGBA 0..1**. Large buffers rejected (~8K² / RAM budget).

#### Document meta

| Call | Returns / notes |
|------|-----------------|
| `width()`, `height()`, `size()` → `(w,h)` | |
| `bit_depth()` | `"U8"` \| `"F16"` \| `"F32"` |
| `path()`, `is_modified()` | |
| `tile_size()`, `active_tile_count()` | Engine tile edge (256) |
| `open` / `save_rayp` / `save_image` / `new_blank` | **Main thread** |

#### Layers

| Call | Notes |
|------|--------|
| `layer_count()`, `layers()` | dicts: `index`, `name`, `visible`, `opacity`, `is_group`, `has_mask`, `can_paint`, `active` |
| `active_layer()`, `set_active_layer(i)` | set_* main thread |
| `create_layer(name)`, `delete_layer(i)` | main thread |
| name / visible / opacity getters & setters | setters main thread |

Groups / Fill: content paint APIs refuse.

#### Pixels

| Call | Notes |
|------|--------|
| `get_pixel` / `get_pixels` / `get_layer_rgba8` | Prefer main thread (document may change) |
| `set_pixel` / `set_pixels` / `set_layer_rgba8` / `fill_rect` | **Main thread only** |
| `get_region_for_generate(layer)` | dict `{x,y,w,h,rgba}` — selection bbox if any, else full layer |
| `notify_changed(layer)` | Force GPU dirty |

#### Undoable pixel session (**use for AI apply**)

| Call | Notes |
|------|--------|
| `begin_edit(layer)` | Snapshot tiles; **main thread** |
| `end_edit(action_name="Script Edit")` | One **Ctrl+Z** step |
| `cancel_edit()` | Restore snapshot |
| `is_edit_active()` | bool |

Without `begin_edit`/`end_edit`, pixel writes still work but **are not undoable**.

#### Mask & selection

| Call | Notes |
|------|--------|
| `has_mask` / `create_mask` / `get_mask` / `set_mask` | |
| `has_selection` / `get_selection` / `set_selection` | |
| `selection_bounds()` | `(x,y,w,h)` or `None` |

#### Tiled composite export

```python
result = rayv.doc.export_tiles(out_dir, "tile_{x}_{y}.png", tiles_x, tiles_y, mode="count")
# → { "ok": bool, "count": int, "error": str }
```

| `mode` | `tiles_x` / `tiles_y` |
|--------|------------------------|
| `"count"` | Number of tiles on each axis (**1..64**); last cells absorb remainder |
| `"size"` | Tile size in **pixels**; ≤64 tiles per axis |

### 9.6 Images — `rayv.image` (no pip / PIL required)

Host embeds STB. Suitable for Comfy PNG responses.

| Call | Result |
|------|--------|
| `decode(data: bytes)` | `{ok, width, height, rgba8: bytes, error}` |
| `encode_png(rgba8, w, h)` | `{ok, png: bytes, error}` |
| `load_file(path)` | same shape as decode |
| `save_file(path, rgba8, w, h)` | `{ok, error}` |

`rgba8` length must be `w*h*4`. Max dimension class ~8K²; oversize refused.

### 9.7 UI — `rayv.ui` (only inside `on_ui`)

| Call | Returns |
|------|---------|
| `begin` / `end` | `(visible, is_open)` |
| `open_popup` / `begin_popup_modal` / `end_popup` | modal: `(visible, is_open)` |
| `text` / `text_wrapped` / `text_disabled` | |
| `separator` / `spacing` / `same_line` | |
| `button` → bool | |
| `checkbox` / `input_int` / `input_text` / `combo` | `(changed, value)` |
| **`progress_bar(fraction, overlay="")`** | `fraction` in 0..1 |
| **`set_image_rgba8(key, rgba8, w, h)`** | bool — GPU preview upload |
| **`image(key, display_w=0, display_h=0)`** | bool — draw preview |
| **`destroy_image(key)`** | |

Preview keys are strings (e.g. `"gen_preview"`). Max texture side 8192.

### 9.8 Plugin host — `rayv.plugins`

| Call | Notes |
|------|--------|
| `reload()` → `(ok, summary)` | Rescan `{exe}/scripts` + `Documents/RayVPaint/scripts` |
| `open(id)` | Queue `on_open` |
| `list()` | id, title, path, source (`builtin`\|`user`) |
| `builtin_dir()`, `user_dir()` | Absolute paths |

### 9.9 Logging & crashes

```python
rayv.log_debug / log_info / log_warn / log_error
```

| File | Role |
|------|------|
| `Documents/RayVPaint/user/rayv_paint.log` | Normal app log |
| `Documents/RayVPaint/user/crash_last.log` | Last fatal (ACCESS_VIOLATION, abort, …) |
| `Documents/RayVPaint/user/crash_*.dmp` | Minidump for debugging |

If a plugin misbehaves, the host tries to **disable plugin UI** for the session rather than silent exit.

### 9.10 What is intentionally **out of scope** (for now)

| Missing | Workaround / future |
|---------|---------------------|
| Register new keymap operators from Python | Use plugin menu + modal |
| Dirty-tile push events | Poll `active_tile_count` / full region if needed |
| `rayv.assets` AssetManager | Save files under user dirs; assets later |
| Guaranteed third-party pip in embed | Use `rayv.image` + stdlib `urllib` |

### 9.11 Built-in samples

| Plugin | Path | Role |
|--------|------|------|
| Tiled Export | `{exe}/scripts/tiled_export.py` | Sync export UI |
| Tiled Export + Paint | `Documents/RayVPaint/scripts/tiled_export_paint.py` | Export + `fill_rect` demo |

### 9.12 CLI

```text
RayVPaint_Core.exe --script path\to\file.py --headless
RayVPaint_Core.exe --benchmark --perf
```

On main thread in CLI, `doc.*` mutations are allowed after host registration.
