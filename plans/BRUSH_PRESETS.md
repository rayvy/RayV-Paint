# Brush Preset System (core)

## Paths

| Priority | Path |
|----------|------|
| **Default** | `%AppData%/Roaming/RayVPaint/brushes` (Windows `FOLDERID_RoamingAppData`) |
| **Fallback** | `Documents/RayVPaint/brushes` (same tree as `ConfigManager::GetUserDirectory()`) |
| **Override** | `BrushLibrary::SetRootDir(path)` before `LoadAll()` |

- Folder is created on first `EnsureRootExists()` / disk scan.
- Customs only appear as `*.rvbrush` files; builtins are **code-registered**, never deleted from disk.
- Empty folder ⇒ only the 4 builtins in `List()`.

### Startup (fast path)
1. `LoadBuiltins()` — sync, instant (4 code presets)
2. `StartAsyncDiskLoad()` — background thread scans `*.rvbrush`
3. Main loop `PollAsyncDiskLoad()` — merges customs when ready

Log line on init: `BrushLibrary: builtins ready (...), disk scan deferred`.

## Format: `.rvbrush` (Option A — single JSON file)

Filename: `{id}.rvbrush`  
**Portable:** one file = full brush. Tip texture is **embedded** as base64 (not a separate PNG path dependency). Share the `.rvbrush` file between users.

```json
{
  "magic": "RVBRUSH",
  "version": 1,
  "id": "8f3a…-uuid",
  "name": "My Brush",
  "params": {
    "radius": 20.0,
    "hardness": 0.5,
    "opacity": 1.0,
    "spacing": 0.1,
    "stabilization": 1,
    "erase": false,
    "writeR": true, "writeG": true, "writeB": true, "writeA": false,
    "pressureRadius": false,
    "pressureHardness": false,
    "pressureOpacity": false,
    "rotationDeg": 0.0,
    "pressureRotation": false,
    "scatter": 0.0,
    "angleJitter": 0.0
  },
  "tip": {
    "type": "none | builtin | embedded",
    "builtin_id": "soft_round | hard_round | pencil | airbrush",
    "size": 64,
    "encoding": "raw8_base64",
    "spacing_mul": 1.0,
    "data": "<base64 grayscale tip pixels>"
  }
}
```

| Field | Role |
|-------|------|
| `magic` | Must be `"RVBRUSH"` |
| `version` | **Mandatory.** Current = `1`. Readers accept `version >= 1`, ignore unknown fields. Bump when adding breaking fields. |
| `id` | Stable uuid (or `builtin.*`) |
| `name` | Display name |
| `params.*` | Full paint snapshot (no FG color) |
| `tip.type=embedded` | Custom PNG→grayscale tip **inside file** (`data` base64) — portable |
| `tip.type=builtin` | Reference to code tip (no bytes) |
| `tipSourcePath` | Optional original path metadata only (not required to load) |

Constants: `BrushLibrary::kFormatVersion`, `BrushLibrary::kFormatMagic`.

## Identity & meta

```cpp
struct BrushPresetMeta {
  std::string id;          // "builtin.soft_round" or uuid
  std::string displayName;
  bool isBuiltin;          // UI: blue, non-deletable
  bool isDirty;            // UI: orange unsaved / staging
  std::string sourcePath;  // empty if never saved
};
```

Builtins (fixed ids):
- `builtin.soft_round` — Soft Round  
- `builtin.hard_round` — Hard Round  
- `builtin.pencil` — Pencil  
- `builtin.airbrush` — Airbrush  

## Tip pointer lifetime

`BrushSettings::tip` is a raw `const BrushTip*`.

| Tip type | Pointer targets |
|----------|-----------------|
| none | `nullptr` (procedural circle) |
| builtin | static `BrushPresets::*` |
| embedded | `BrushLibrary` entry `ownedTip` |

**Rules for UI:**
1. After `ApplyTo(id, g_Brush)`, do not free/replace library entries while painting with that tip.
2. Mutating APIs (`CreateFromCurrent`, `UpdateStaging`, `DeleteCustom`, `LoadAll`) may invalidate previous `tip` pointers for **embedded** customs — re-`ApplyTo` after mutate if needed.
3. Staging entries stay in library until `DiscardStaging` / `DeleteCustom` / successful replace on load.

## RAM staging workflow

1. `CreateFromCurrent(g_Brush, "Name")` → new uuid, `isDirty=true`, not on disk.
2. User edits size/hardness live on `g_Brush`; call `UpdateStaging(id, g_Brush)` to keep snapshot in sync.
3. `SaveToDisk(id)` → write `.rvbrush`, `isDirty=false`.
4. `DiscardStaging(id)` → drop if never saved, else reload from disk.
5. `DeleteCustom(id)` → refuse if `isBuiltin`.

## Library vs project (.rayp)

| | BrushLibrary | .rayp |
|--|--------------|-------|
| Scope | **User-global** | Per project |
| What | Preset list + files | Optional `brush_tip_id` + custom tip pixels (legacy) |
| Active color | Not stored | Not stored in brush file |

UI may set `BrushLibrary::SetActiveId` for session; later can mirror active preset id into .rayp if product wants.

## Public API (header)

`src/core/BrushLibrary.h` — `BrushLibrary::Get()`, `List`, `ApplyTo`, `CreateFromCurrent`, `UpdateStaging`, `SaveToDisk`, `DiscardStaging`, `DeleteCustom`, `RunSmokeTest`.

## App integration

- `main`: after Logger/Config → `BrushLibrary::Get().LoadAll()`
- CLI: `--test-brushes` runs `BrushLibrary::RunSmokeTest()` and exits
- Keymap stub: action `"BrushPopup"` reserved for UI agent (no handler required in core)

## Out of scope (UI agent)

- RMB viewport popup, blue/orange chips, animations  
- Create Brush button layout  
- Hold-to-preview hover  
