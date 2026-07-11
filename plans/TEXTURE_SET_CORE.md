# Plan 0 — Texture Set Core (cross-texture / multi-file)

**Date:** 2026-07-12  
**Status:** Maps-as-layers simplification in tree (no mapComposites).  
**Goal:** First-class **Texture Set** in core, shared by Advanced and Advanced Mod Mode. 3D preview only *consumes* sets.

### Implemented so far
- `src/texset/*` — MapKind, soft ChannelRole labels, MapSlot size/export, templates
- `Project.textureSets` + `.rayp` meta; maps imported as real Layers with `workSpace`
- Viewport filters by `workSpace` + active map; Fill multi-map + per-channel write masks
- Native-res `nativeMapCache` kept on import when map size ≠ document (export prefers it)
- Sparse tiled masks (`MaskTiles`) + delta mask undo; layer lifecycle undo
- Ctrl+E batch export; File Explorer for save/open/import/export
- **Still open:** paint *into* native resolution (viewport still document UV), AdvMod multi-set UX polish, full 3D material live bind

---

## 1. Product model (user intent → rules)

| Mode | Organization | Tools |
|------|----------------|-------|
| **Simple** | 1 implicit Texture Set, 1 Diffuse map, multi-layer | Full paint (Photoshop-like) |
| **Advanced** | N files → 1+ Texture Sets, export templates, import channel split | Same + Fill targets + Instances later |
| **Advanced Mod Mode** | Many Texture Sets (BelleBodyLegs Diffuse+LightMap…), **no dedup** of sets by path | Same + mod/3D binding |

**Modes differ by workspace layout, not by tool capability** (exceptions: Fill/Instances matter more in multi-map).

### 1.1 Texture Set (unit of work)

```
TextureSet "BelleBodyLegs"
├── Map slots (fixed kinds, 0..1 each)
│   ├── Diffuse   RGB (+A optional)   [always present]
│   ├── LightMap  RGBA free channels  [optional]
│   ├── MaterialMap
│   ├── NormalMap   (often ½ resolution)
│   ├── ExtraMap
│   ├── GlowMap
│   └── WengineFX
├── Channel semantics (per map: R/G/B/A → Role)
├── **Shared Layer Stack** (one stack for the whole set)
│   └── Layer.workSpace = which MapKinds + which roles this layer writes
└── Active view = which map (or solo role) is shown in 2D viewport
```

**Not:** separate layer stacks per LightMap / MaterialMap.  
**Yes:** one stack; each layer declares its **work space** (like SP channel participation).

### 1.2 Fixed map kinds (UI categories)

| Kind | Default on | Notes |
|------|------------|--------|
| Diffuse | **Yes** | RGB = BaseColor always; A optional (opacity/unused) |
| LightMap | Off | free R/G/B/A roles |
| MaterialMap | Off | free |
| NormalMap | Off | often ½ size |
| ExtraMap | Off | free |
| GlowMap | Off | free |
| WengineFX | Off | free |

Templates (`Default`, `ZZZ`, `GI`, …) enable slots + default channel→role tables (same idea as 3D presets, but **core**).

### 1.3 Channel roles (logical, not file packing)

Examples: `BaseColor`, `Opacity`, `ShadowRamp`, `Metallic`, `Roughness`, `Glossiness`, `Specular`, `NormalX`, `NormalY`, `NormalZ`, `AO`, `Height`, `Emission`, `Custom`, `None`.

Maps **pack** roles into R/G/B/A; tools paint **roles** or **map.rgba** depending on layer work space.

### 1.4 Layers + work space

```
Layer
  type: Raster | Fill | Group | …
  workSpace:
    maps: bitset of MapKind   // e.g. Diffuse only, or Diffuse+Normal
    roles: bitset of ChannelRole // e.g. Roughness only (→ Material.G via pack)
  storage:
    // Only maps this layer participates in, at **that map's native resolution**
    content[MapKind] → TileCache (or empty if Fill/procedural)
```

| Layer type | Behavior |
|------------|----------|
| Paint (Raster) | Stamps into `content[map]` for maps in workSpace; RGB or grayscale by role |
| Fill | Solid/gradient/texture → roles; **all maps in set** that pack those roles |
| Instance (later) | Shared fill/source across sets or maps |

**Fill across set:** one Fill layer can drive Metallic on MaterialMap and leave Diffuse alone if workSpace = {MaterialMap, Metallic}.

### 1.5 Resolution policy (no paint mismatch)

| Rule | Detail |
|------|--------|
| Each `MapSlot` has native `width×height` | Independent |
| Brush stroke in **document UV space** [0,1]² | Same logical UV for the set |
| Stamp scale | `mapPixel = uv * mapSize` so half-res Normal gets half-res stamp footprint |
| Viewport | Shows active map at its native pixels (or upscaled display only) |
| Never force all maps to same pixel size | Avoids bake blur on Normal |

Logical UV space is the contract; **pixel sizes differ per map**.

### 1.6 Import (Advanced)

Drop / import with **context**:

| Context | Behavior |
|---------|----------|
| Drop on viewport (Advanced) | Import into **active map** of **active set** (or ask if multi) |
| Drop with channel split | Wizard: file → roles (e.g. LightMap.R → ShadowRamp as new Fill or raster layer) |
| Mod import | Bind files into sets named after component.part (no dedup) |

### 1.7 Export

| Action | Behavior |
|--------|----------|
| **Ctrl+E** (Quick Export All) | For each enabled map in each set (or active set): pack roles → file via export template |
| Template | Substance-like: name pattern `{set}_{map}.dds`, format BC7, mips… |
| Per-map export path | Stored on MapSlot |

### 1.8 3D preview

- Reads **TextureSet composites** (or live SRV later) by name/id.  
- Channel remap in 3D can **default from set semantics** (ZZZ template).  
- Preview stays optional / abstracted; **sets are core**.

### 1.9 Advanced Mod Mode

- Many Texture Sets (`BelleBodyLegs`, `BelleHairA`, …).  
- **No deduplication** by path: two sets may reference similar names independently.  
- One component.part may map to **multiple** sets (multi-material).  
- Switching set switches layer stack + map tabs.

---

## 2. Core architecture

### 2.1 Ownership

```
Project (ProjectManager tab)
├── type: Simple | Advanced | AdvancedModMode
├── textureSets[]: TextureSet
├── activeTextureSetId
├── exportTemplate
└── (mod) ini/dump/preview binding → set ids
```

**Simple:** always exactly one TextureSet with only Diffuse; Canvas API can keep working as today by treating `Canvas` as the Diffuse composite + layer stack of that set.

**Migration path:**

1. Scaffold types + TextureSet holds maps + thin wrap around existing Canvas for Diffuse only.  
2. Move layer stack into TextureSet; Canvas becomes **view** onto active set/map.  
3. Multi-map compose + paint routing.  
4. Export all + import wizard.  
5. Instances.

### 2.2 Performance invariants (do not break)

1. Sparse TileCache per map content (no full float W×H for idle maps).  
2. Inactive sets: drop GPU textures; keep tiles.  
3. Paint: only upload dirty tiles of maps in layer.workSpace.  
4. Compose: only **active map** (or maps dirty for export) at interactive rates.  
5. Half-res Normal: half the tiles, not a second full-res ghost.  
6. Undo: COW TileData per map cache (existing undo model extended with map id).

### 2.3 Compose graph (per map)

```
for map in set.enabledMaps:
  if not (interactive && map != active && !exporting): skip or use cached
  RT = map.nativeSize
  for layer in stack bottom→top:
    if layer.workSpace includes map:
      src = layer.ResolveContent(map)  // raster tiles | fill expand | group
      blend into RT with layer opacity/blend/mask
  map.composite = RT
```

Fill expand: generate pixels at **map native size** from solid/gray/texture UV.

### 2.4 Paint routing

```
OnStroke(uv, brush):
  layer = activeLayer
  for map in layer.workSpace.maps:
    px = uv * map.size
    stamp into layer.content[map] at px (scale brush radius by map relative size)
```

Active **view map** can differ from workSpace (paint Diffuse while viewing LightMap only if workSpace allows — or force view to first workSpace map).

---

## 3. Implementation phases

| Phase | Deliverable | Breaks Simple? |
|-------|-------------|----------------|
| **P0.1** | Types: MapKind, ChannelRole, MapSlot, TextureSet, SetTemplate; design doc | No |
| **P0.2** | Project owns `vector<TextureSet>`; Simple = 1 set Diffuse; serialize meta in .rayp | Soft |
| **P0.3** | UI: set list + map tabs (switch active map composite) | No |
| **P0.4** | Layer.workSpace + multi TileCache per layer; paint routes | Careful |
| **P0.5** | Fill → role → pack into map | No |
| **P0.6** | Import channel split + drop context | No |
| **P0.7** | Export template + Ctrl+E all maps | No |
| **P0.8** | 3D binds to TextureSet composites (live SRV) | No |
| **P0.9** | Instances / SoftLink | Later |

**This PR/session target:** P0.1–P0.3 scaffold (types + project ownership + minimal UI switcher).

---

## 4. Relation to existing Fill / 3D

| Existing | Action |
|----------|--------|
| `FillChannelTarget` {Diffuse, Transparency, Metallic, Roughness} | Expand to full ChannelRole / MapKind |
| `FillLayerParams` | Target role + value mode; compose writes into set maps |
| 3D `MaterialConfig` channel remap | Initialize from TextureSet template (ZZZ); still overridable in preview |
| `modio` DrawBatch textures | On mod load → create/update TextureSets (Advanced Mod) |

---

## 5. Open decisions (defaults locked for scaffold)

| Topic | Default |
|-------|---------|
| Simple internal model | 1 TextureSet, Diffuse only, layers on set |
| Active paint map | Follow viewport map tab unless layer workSpace is single other map |
| Normal half-res | Native size; UV-space paint |
| Ctrl+E | All maps of **active set** first; option all sets later |
| Dedup Advanced Mod sets | **Never** by path |

---

## 6. Success criteria (Plan 0 done)

1. Advanced: open 2 DDS into one set (Diffuse + LightMap), switch tabs, paint each without resolution bugs.  
2. Shared layer stack: one layer list; Fill Roughness affects Material packing only.  
3. Ctrl+E writes all enabled maps of set.  
4. Advanced Mod: N sets from character, switch sets, no forced merge.  
5. Simple mode unchanged UX.  
6. 3D preview can bind set maps (even if still file SRV initially).
