# Asset Browser (Build 16 — forced optimization)

**Not a feature drop for its own sake.** Fill Layer + large textures currently store full CPU RGBA on the layer and force expensive bake paths → unusable lag. Asset Browser = **shared identity + cache** for textures (and later fonts / materials).

---

## 1. Categories (folders of ownership, not file types)

| Category | Root | Mutability | Ship in `.rayp` |
|----------|------|------------|-----------------|
| **built-in** | `{exe_dir}/assets/` | Read-only | No (user already has app) |
| **user** | `{Documents/AppData}/RayVPaint/assets/` | User r/w | No (local library) |
| **project** | In-memory / project temp | Session + save | **Yes** — packed into `.rayp` |

“Types” later (texture, font, material graph) live *under* categories. **B16 type scope: textures only.**

---

## 2. Goals B16

1. Browse + preview + pick texture for **Fill → Use Texture**.
2. One decode / one GPU (or tile) cache per asset key; refcount consumers.
3. Project assets serialized in `.rayp` so projects move between machines.
4. Fill samples via asset key — no private multi-megabyte `textureRgba` thrash every frame.

### Non-goals B16

- Fonts, node materials (hooks only in schema comments).
- Cloud / store / licensing.
- Replacing File Explorer for general file open (FE stays for maps/projects).

---

## 3. Paths (concrete)

```
{exe}/assets/textures/...          # built-in
{user}/assets/textures/...         # user library  
project: asset table in .rayp      # project
```

User root: prefer existing `ConfigManager::GetUserDirectory()` / AppData pattern used by brushes/keymap.

---

## 4. Data model (sketch)

```cpp
enum class AssetCategory : uint8_t { BuiltIn, User, Project };

struct AssetId {
  AssetCategory cat;
  std::string key; // relative path or uuid for project
};

struct TextureAsset {
  AssetId id;
  int w, h;
  // CPU optional after GPU upload policy
  // GPU: ID3D11ShaderResourceView* or TileCache*
  int refCount;
};
```

**Layer.fill (target):**

```cpp
// Prefer:
std::string fillTextureAssetKey; // or AssetId
// Deprecate long-term as sole storage:
// textureRgba / textureW / textureH — migrate on load
```

---

## 5. UI

- Panel or File-Explorer-like browser filtered to images.
- Tabs or sidebar: Built-in | User | Project.
- Thumb grid reuses FE thumb cache patterns (cap LRU).
- Pick → returns `AssetId` to caller (Fill properties).

---

## 6. .rayp packing (project category)

- Section or blob list: `project_assets[] { key, mime, bytes, w, h }`
- On open: rehydrate into memory store, keys stable.
- On save: only **referenced** project assets (or all registered — prefer referenced).

---

## 7. Fill performance contract

| Bad (today) | Good (B16) |
|-------------|------------|
| Every layer holds full RGBA | Shared asset, N layers → 1 blob |
| Dirty fill → full buffer bake often | Sample asset in cheap path; GPU 1×1 fill still for solid color |
| Import path string only | Asset key + category |

Solid color Fill stays 1×1 GPU path. Textured Fill must not call full-document `FillSolidBuffer` every frame.

---

## 8. Implementation slices

1. `AssetStore` + path resolution (built-in/user)  
2. Register + load texture + thumb  
3. UI browser shell  
4. Fill wiring + stop full CPU rebake  
5. Project category + `.rayp`  
6. Migrate old `texture_path` on document load  

---

## 9. Future (document only)

- Fonts under `assets/fonts/`  
- Node materials / graphs as assets  
- Brush tip textures already partially in BrushLibrary — consider unify later  
