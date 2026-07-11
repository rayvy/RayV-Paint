# Plan 1 — Техническая поддержка (core foundation)

**Цель:** заложить чистую иерархию данных и pipeline, без которых фичи (Shadow, Outline, Fill Layer, group FX, независимая opacity эффектов) либо невозможны, либо будут костылями поверх текущего CPU-full-frame фильтра.

**Не в этом плане:** polish UI, interactive curve editor per-channel, финальные градиенты/текстуры outline в UI, багфиксы «на глаз».  
**В этом плане:** модели, dirty-flags, compose path, сериализация, API, заготовки GPU/CPU, минимальные unit-smoke через headless/script если уже есть.

**Дата:** 2026-07-11  
**Опора на код:** `Canvas.h` / `Canvas.cpp`, `Canvas.hlsl`, `EditorPanels.cpp` (только контракт, не UI polish), `.rayp` v2.

---

## 0. Текущее состояние (as-is)

| Область | Факт |
|--------|------|
| `LayerFilter` | Blur / HSV / Curves / AlphaInvert / Noise — **pixel modifiers** |
| Применение | CPU: full-frame `ExportLayerF` → chain → `filteredCache` → GPU upload |
| Opacity | `layer.opacity` × `col.a` в `PSLayerBlend` — **на весь результат** (content + baked filters) |
| Styles (Shadow/Outline) | **нет** |
| Groups | `isGroup` + `parentGroupId`; дети рисуются как обычные слои; `m_GroupComposite*` **создаётся, но compose не использует** |
| `RasterizeLayer` | только SmartObject/Vector → `Type::Raster`; **не** bake filters |
| Curves FX | один `lut[256]` на RGB; A не трогается; нет channel toggles |
| Fill Layer | **нет** (типы: Raster / Group / SmartObject / VectorSvg) |
| Export | `ComposeVisibleLayersRGBA8` — плоский bottom→top, без group isolation, без styles |

**Главный архитектурный вывод:** нельзя пихать Shadow/Outline в `filteredCache` с умножением на `layer.opacity`. Нужно разделить:

1. **Content stack** (пиксели + pixel-filters)  
2. **Style stack** (визуальные производные: shadow, outline)  
3. **Compose opacity model** (fill vs style opacity)

---

## 1. Целевая иерархия (чёткое дерево)

```
Document
└── LayerStack (order bottom → top)
    ├── Layer (typed)
    │   ├── Identity: name, visible, blendMode
    │   ├── Opacity model: fillOpacity, (optional overall later)
    │   ├── Type payload:
    │   │   ├── Raster: tileCache
    │   │   ├── Fill: FillParams (color / grayscale / future texture + channel target)
    │   │   ├── Group: children via parentGroupId (no paint pixels)
    │   │   └── SmartObject / VectorSvg: source + raster cache (as now)
    │   ├── Mask (optional, paintable)
    │   ├── PixelFilters[]     // Blur, HSV, Curves, … — mutate content before styles
    │   └── LayerStyles[]      // Shadow, Outline — drawn as extra passes
    └── ComposeGraph
        └── for each node: resolve content → apply styles → blend into parent
```

### 1.1 Два класса «эффектов» (не путать)

| Класс | Примеры | Когда | Opacity |
|-------|---------|-------|---------|
| **PixelFilter** (есть) | Blur, Curves, HSV, Noise | Меняет content buffer | участвует в fill |
| **LayerStyle** (новый) | Shadow, Outline | Генерирует **производный** визуал из silhouette/content | **своя** `style.opacity`; **не** × `fillOpacity` |

Имена в коде (предложение):

- `LayerFilter` / `FilterType` — оставить для pixel filters (минимальный churn).
- `LayerStyle` / `StyleType` — новый тип.
- В UI можно звать всё «Effects», внутри — разные списки или tagged union с `category`.

**Почему не один список с enum:** Shadow/Outline требуют **отдельных draw passes** (часто *под* content), mask/alpha semantics другие, dirty/cache другая. Смешение в `RebuildFilteredPixels` даст full-frame CPU bake теней и сломает независимость opacity.

---

## 2. Opacity model (требования 1–2)

### 2.1 Семантика (как Substance / PS Fill)

| Параметр | Действует на | Не действует на |
|----------|--------------|-----------------|
| `layer.fillOpacity` (новое; default 1) | content после pixel-filters (+ mask) | LayerStyles |
| `layer.opacity` | **решение:** v1 = синоним fill **или** overall | см. ниже |
| `style.opacity` | только этот style | content |

**Требование пользователя:** «прозрачность основного слоя мазков не влияет на FX».

**Рекомендуемый v1 (минимум ломающих изменений):**

1. Переименовать смысл текущего `layer.opacity` → **fill/content opacity** в compose (document + UI label «Opacity» пока = fill).  
2. LayerStyles рисуются **отдельными pass** с `style.opacity` (и `style.color.a` если нужно).  
3. Позже (Plan 2 optional): Photoshop-пара «Opacity + Fill» — `overallOpacity` × (content∪styles), `fillOpacity` × content only.

**Compose pseudo:**

```
content = ResolveLayerContent(layer)           // raster | fill | group flatten
content = ApplyPixelFilters(content)
contentA = content.a * mask * layer.fillOpacity

// Styles from silhouette of *pre-style* alpha (PS-like: from layer shape before fill fade? )
// Spec: silhouette = content.a * mask  (без fillOpacity)  ← FX не гаснут при fill↓
silhouette = content.a * mask

DrawShadow(silhouette, style)   // under content, style.opacity
DrawContent(content.rgb, contentA)
DrawOutline(silhouette, style)  // over content, style.opacity
```

**Критично:** silhouette для FX **не** умножать на `fillOpacity`. Тогда ползунок прозрачности мазков гасит только краску, тень/обводка остаются.

### 2.2 GPU contract (`LayerBuffer` / HLSL)

Сейчас: `u_LayerParams.x = opacity` → `col.a *= opacity`.

План:

- Content pass: `u_LayerParams.x = fillOpacity`.  
- Style pass: отдельный shader или тот же с `fillOpacity=1` и альфой уже в texture/const.  
- Не смешивать style bake в `layer.texture` с последующим × fill.

---

## 3. Модели данных (C++)

### 3.1 `Layer::Type`

```cpp
enum class Type : uint8_t {
    Raster = 0,
    Group  = 1,
    SmartObject = 2,
    VectorSvg   = 3,
    Fill   = 4   // NEW
};
```

`isGroup` оставить как convenience (`type == Group`), постепенно не плодить вторую истину.

### 3.2 Fill Layer

```cpp
enum class FillChannelTarget : uint8_t {
    Diffuse = 0,      // RGB color (default)
    Transparency,     // future multi-map
    Metallic,
    Roughness
    // expand later
};

enum class FillValueMode : uint8_t {
    RGB = 0,          // color[0..2], alpha optional
    Grayscale01,      // value in [0,1] → all channels / target
    GrayscaleSigned   // value in [-1,1] → map to storage
};

struct FillLayerParams {
    FillChannelTarget target = FillChannelTarget::Diffuse;
    FillValueMode mode = FillValueMode::RGB;
    float color[4] = {1.f, 1.f, 1.f, 1.f}; // RGB mode
    float gray = 1.f;                      // grayscale modes
    // Texture (skeleton only in Plan 1 — load path + GPU SRV later Plan 2)
    bool useTexture = false;
    std::string texturePath;
    // UV: scale/offset reserved
    float texScale[2] = {1.f, 1.f};
    float texOffset[2] = {0.f, 0.f};
};
```

На `Layer`:

```cpp
// only meaningful when type == Fill
FillLayerParams fill;
// tileCache: nullptr OR optional low-res override — v1: no paint tiles for fill content
```

**Правила:**

- `PaintOnActiveLayer` content → **no-op / reject** если `type == Fill` и target == LayerContent.  
- Mask paint — **разрешён** (как в SP).  
- Content resolution: procedural full-canvas color (GPU constant / full-quad shader), **без** аллокации W×H ради solid fill.  
- `LayerHasPixels` / visibility: Fill **всегда** «имеет content», если visible (даже без tiles).

### 3.3 LayerStyle

```cpp
enum class StyleType : uint8_t {
    Shadow = 0,
    Outline = 1
};

enum class OutlinePosition : uint8_t { Outside = 0, Inside, Center };
enum class OutlineFillMode : uint8_t { Solid = 0, Gradient = 1, Texture = 2 };

struct GradientStop {
    float t = 0.f;      // 0..1 along outline normal or perimeter (define in Plan 2 UI)
    float rgba[4] = {0,0,0,1};
};

struct LayerStyle {
    StyleType type = StyleType::Shadow;
    bool enabled = true;
    float opacity = 1.f;          // independent

    // --- Shadow ---
    float shadowColor[4] = {0,0,0,1};
    float distance = 8.f;         // px
    float angleDeg = 120.f;       // vector from angle
    float offsetX = 0.f;          // extra offset (user/manual)
    float offsetY = 0.f;
    float spread = 0.f;           // 0..100% choke-like (v1: map to blur pre-bias)
    float size = 8.f;             // blur / soft radius px

    // --- Outline ---
    float outlineColor[4] = {0,0,0,1};
    float outlineSize = 2.f;      // width px
    OutlinePosition outlinePos = OutlinePosition::Outside;
    OutlineFillMode outlineFill = OutlineFillMode::Solid;
    std::vector<GradientStop> outlineGradient; // Plan1: store + serialize; render solid fallback OK
    std::string outlineTexturePath;
    // blend mode reserved: BlendMode outlineBlend = Normal;
};
```

На `Layer`:

```cpp
std::vector<LayerStyle> styles;
bool stylesDirty = true; // GPU style resources / alpha silhouette cache
```

**Порядок стилей (PS-like default):**

1. All enabled Shadows (back → front as listed, or single pass per style)  
2. Content  
3. All enabled Outlines  

Порядок внутри списка — как в UI (reorder). Plan 1: фиксированный draw order по type, внутри type — vector order.

### 3.4 Curves filter expansion (данные, Plan 1)

Расширить `LayerFilter` **без** ломки старых `.rayp`:

```cpp
// Curves: was single lut for RGB
// New optional fields (empty = legacy behavior):
std::vector<float> lutR, lutG, lutB, lutA; // each 256 or empty
uint8_t curvesChannels = 0x7; // bit0=R bit1=G bit2=B bit3=A; default RGB on, A OFF
// Legacy: if lut non-empty and lutR empty → apply lut to enabled RGB; A only if bit3
```

`RebuildFilteredPixels` / Curves case:

- Для каждого канала: если bit set и LUT есть → remap.  
- A по умолчанию **выключен** (`curvesChannels & 0x8 == 0`).  
- Identity LUT если points empty.

Хранить control points опционально (`std::vector<pair<float,float>> ptsR…`) — можно Plan 2; в Plan 1 достаточно LUT + flags + serialize.

---

## 4. Compose pipeline (ядро Plan 1)

### 4.1 Единый граф (viewport + export должны сходиться)

```
ComposeStack(layers):
  walk top-level only (parentGroupId < 0), bottom → top
  for each layer L:
    if !L.visible: skip (and skip whole group subtree)
    if L.isGroup:
       buf = ComposeChildren(L)          // isolated RT / CPU buffer
       buf = ApplyPixelFilters(L, buf)   // group filters
       EmitLayerWithStyles(L, buf)
    else:
       buf = ResolveContent(L)           // raster tiles | fill procedural
       buf = ApplyPixelFilters(L, buf)
       EmitLayerWithStyles(L, buf)

EmitLayerWithStyles(L, content):
  sil = Alpha(content) * Mask(L)         // no fillOpacity
  for style in Shadows(L): DrawStyleShadow(sil, style)
  DrawContent(content, fillOpacity * mask)
  for style in Outlines(L): DrawStyleOutline(sil, style)
```

### 4.2 Group virtual rasterize (треб. 8)

**Substance-логика:** сначала сумма детей, потом FX группы.

Реализация:

1. **GPU path (viewport):**  
   - `m_GroupCompositeTexture` → size = **proxy** (`m_CompositeWidth/Height`), не 16K.  
   - Clear group RT → draw children (рекурсия) **без** применения parent styles → result SRV.  
   - Pixel filters группы:  
     - v1 pragmatic: если group filters empty — skip;  
     - если есть — **proxy-res** filter (или dirty flag + downsample content).  
   - Styles группы — на silhouette group buffer.  
   - Blend group result в main composite с `group.fillOpacity` + blendMode.

2. **CPU path (export):**  
   - `ComposeGroupRGBA8(groupIdx, w, h)` → children tile composite → filters → styles → return buffer.  
   - Интегрировать в `ComposeVisibleLayersRGBA8` (сейчас groups **игнорируются** как headers, дети лезут плоско — **баг/gap**).

3. **Дети в group:**  
   - При обходе main stack **не** рисовать `parentGroupId == group` слои повторно.  
   - Nested groups: рекурсия + stack overflow guard (depth limit ~32).

4. **Isolation / visibility:**  
   - `LayerEffectivelyVisible` уже гасит детей при `!parent.visible` — сохранить.  
   - Group opacity = fillOpacity группы на **итоговый** buffer (не на styles — см. §2).

### 4.3 Fill resolve

**GPU:**

- Новый PS `PSFillLayer` или reuse `PSLayerBlend` с solid color CB:  
  `float4 fillColor; float hasMask; float fillOpacity`.  
- Fullscreen quad; sample mask if any.  
- No `layer.texture` required for solid fill.

**CPU export:**

- Fill tile stream: constant color × mask × opacity, без tiles.

**Texture fill (skeleton Plan 1):**

- Fields + serialize only; render = solid fallback until Plan 2 loads texture.

### 4.4 Shadow resolve (минимум для foundation)

**Алгоритм v1 (CPU export + GPU proxy):**

1. Silhouette alpha mask (R8).  
2. Offset by `(cos(angle)*distance + offsetX, -sin(angle)*distance + offsetY)`.  
3. Box/Gaussian blur ≈ `size` (reuse existing box blur passes).  
4. Spread: threshold/choke pre-blur (optional v1.1).  
5. Premult: `rgb = shadowColor.rgb`, `a = blurred * style.opacity * shadowColor.a`.  
6. Composite under content with Normal (or Multiply later).

**GPU v1:**  
- Render silhouette to R8 RT (proxy).  
- Separable blur CS/PS (можно 3× box как сейчас).  
- Offset sample + color tint pass.  
- Если GPU blur тяжело в одном PR — CPU shadow **только export**, viewport **упрощённый** (hard offset without blur) → **нежелательно**. Лучше: GPU blur на proxy (≤2048) всегда дёшево.

### 4.5 Outline resolve (foundation)

**v1 Solid Outside:**

1. Dilate silhouette by `outlineSize` (max filter / jump flood later).  
2. `outlineAlpha = dilated - original` (outside) / symmetric for center / `original - eroded` for inside.  
3. Color × style.opacity.  
4. Draw over content.

**Gradient / Texture:** data model + serialize; render solid until Plan 2 shaders.

**Perf:** proxy for viewport; full-res CPU for export (or GPU full-res only if ≤4K budget).

### 4.6 Dirty flags

| Flag | Когда | Что |
|------|-------|-----|
| `needsUpload` | paint tiles | GPU layer tex |
| `filtersDirty` | pixel filter params | rebuild `filteredCache` |
| `stylesDirty` | style params | rebuild style silhouette / blur cache |
| `m_CompositeDirty` | any visible change | recompose |
| `fillParams` change | fill color/mode | composite only (no tile rebuild) |
| group child dirty | child upload/filter | parent group recompose |

**Оптимизация:** не вызывать `RebuildFilteredPixels` (full W×H float) если изменился только Shadow.color.

---

## 5. Rasterize (треб. 7) — core API

Расширить `Canvas::RasterizeLayer`:

| Исходный type | Действие |
|---------------|----------|
| SmartObject / VectorSvg | как сейчас + optional bake current raster |
| **Fill** | materialize solid/gray/texture × mask → `tileCache`, type=Raster, clear fill params |
| **Raster + filters/styles** | bake `ResolveContent+Filters+Styles` → tiles; clear filters & styles |
| **Group** | flatten children (visible) + group filters/styles → **один** Raster layer на месте group **или** replace group with single raster (API: `RasterizeGroup(groupIdx)`) |

API proposal:

```cpp
// Bake non-destructive into pixels. Group: flattens into one raster, removes children.
bool RasterizeLayer(ID3D11Device* device, int layerIdx);
// Explicit group flatten
bool RasterizeGroup(ID3D11Device* device, int groupIdx);
```

Undo: document-level command (snapshot affected layers) — Plan 1 минимум: mark modified + push coarse undo if infrastructure allows; иначе TODO + log.

**Merge path:** `MergeLayerDown` уже bake filters upper — расширить styles + fill.

---

## 6. Serialization `.rayp`

### 6.1 Layer JSON extensions

```json
{
  "layer_type": "fill",
  "fill_opacity": 1.0,
  "opacity": 1.0,
  "fill": {
    "target": "Diffuse",
    "mode": "RGB",
    "color": [1,1,1,1],
    "gray": 1.0,
    "use_texture": false,
    "texture_path": "",
    "tex_scale": [1,1],
    "tex_offset": [0,0]
  },
  "filters": [ /* existing + curves channels */ ],
  "styles": [
    {
      "type": "Shadow",
      "enabled": true,
      "opacity": 0.75,
      "color": [0,0,0,1],
      "distance": 8,
      "angle": 120,
      "offset": [0,0],
      "spread": 0,
      "size": 8
    },
    {
      "type": "Outline",
      "enabled": true,
      "opacity": 1,
      "color": [0,0,0,1],
      "size": 2,
      "position": "Outside",
      "fill_mode": "Solid",
      "gradient": [],
      "texture_path": ""
    }
  ]
}
```

### 6.2 Curves filter JSON

```json
{
  "type": "Curves",
  "enabled": true,
  "curves_channels": 7,
  "lut": [ ... ],
  "lut_r": [ ... ],
  "lut_g": null,
  "lut_b": null,
  "lut_a": null
}
```

**Backward compat:** old files without `styles` / `fill` / `curves_channels` load defaults.  
**Forward:** unknown style types skip with warning.

### 6.3 Version bump

- Metadata `rayp_version` → **3** if breaking; иначе soft v2+ optional fields (предпочтительно **soft**: v2 + optional keys, no forced migration).

---

## 7. Файловая структура (чистая иерархия кода)

Не раздувать `Canvas.cpp` дальше без разреза. Plan 1 — **выделить** (даже если .cpp include/partial):

```
src/
  Canvas.h / Canvas.cpp          // orchestration, public API
  layer/
    LayerTypes.h                 // Layer, FillParams, LayerFilter, LayerStyle enums
    LayerCompose.h/.cpp          // Compose stack, group recurse, export RGBA8
    LayerFilters.h/.cpp          // RebuildFilteredPixels, blur/HSV/curves/noise
    LayerStyles.h/.cpp           // shadow/outline CPU + GPU helpers
    LayerFill.h/.cpp             // fill resolve, paint guards
    LayerRasterize.h/.cpp        // bake flatten
  shaders/
    Canvas.hlsl                  // + PSFillSolid, PSStyleShadow, PSStyleOutline (or separate)
```

Если полный split слишком большой для одного PR — **минимум Plan 1:**

1. `LayerTypes.h` (structs)  
2. `LayerStyles.cpp` (algorithms)  
3. hooks in `ComposeLayers` / `ComposeVisibleLayersRGBA8`  

CMake: добавить .cpp в `RayVPaint_Core`.

---

## 8. API surface (для UI / Plan 2)

```cpp
// Fill
void CreateFillLayer(ID3D11Device*, const std::string& name,
                     FillChannelTarget = Diffuse, FillValueMode = RGB);
bool IsFillLayer(int idx) const;
bool CanPaintLayerContent(int idx) const; // false for Fill/Group

// Styles
int  AddLayerStyle(int layerIdx, StyleType);
void RemoveLayerStyle(int layerIdx, int styleIdx);
void SetLayerStyleEnabled(...);
// getters via Layer::styles

// Opacity
void SetLayerFillOpacity(int idx, float);
float GetLayerFillOpacity(int idx) const;

// Groups + effects
// filters/styles already on Layer — enable for isGroup (remove UI block only in Plan 2;
// Plan 1: allow data + compose)

// Rasterize
bool RasterizeLayer(...);  // expanded
bool RasterizeGroup(...);

// Curves helpers
void InitCurvesFilterDefaults(LayerFilter&); // A off
```

**Paint guard:**

```cpp
// PaintOnActiveLayer begin:
if (!CanPaintLayerContent(active) && target == LayerContent) return;
```

---

## 9. Порядок работ Plan 1 (PR-последовательность)

Оптимально **не** одним монолитом.

| PR | Содержание | DoD |
|----|------------|-----|
| **P1.1** | `LayerTypes.h`: Fill + Style structs; `Layer::Type::Fill`; serialize soft; fillOpacity field (= opacity mapping) | load/save old .rayp; new fields round-trip |
| **P1.2** | Opacity split in compose: fillOpacity on content only; styles list empty still OK | lowering opacity не требует styles |
| **P1.3** | Fill Layer create + GPU solid + CPU export + paint guard + mask works | headless create fill → export solid color |
| **P1.4** | LayerStyle Shadow: data + GPU proxy + CPU export; independent opacity | shadow visible when fillOpacity=0 |
| **P1.5** | LayerStyle Outline solid (outside/inside/center v1) | outline independent opacity |
| **P1.6** | Group compose isolation + group filters + group styles | child not double-drawn; group FX on sum |
| **P1.7** | Curves channel bits + per-channel LUT storage + filter apply | A default off; RGB selectable |
| **P1.8** | Rasterize bake (fill/filters/styles/group flatten) | type becomes Raster; styles cleared |
| **P1.9** | Code split / cmake / logging / ForFuture update | build green; no 16K full float for fill solid |

Параллелить можно: P1.3 ‖ P1.4 после P1.1–1.2; P1.7 независимо после P1.1.

---

## 10. Performance invariants (не ломать)

1. Solid Fill **без** `W×H` float allocation.  
2. Viewport composite остаётся **proxy ≤2048**.  
3. Styles blur на proxy в viewport; full-res только export / explicit rasterize.  
4. `filtersDirty` ≠ `stylesDirty`.  
5. Group RT = proxy size (как main composite), не document 16K.  
6. Export: tile-stream где возможно; styles на tile — если blur kernel выходит за tile, использовать full-layer silhouette R8 (1 byte/px, не 16).  
7. Не вызывать `RebuildFilteredPixels` на каждый кадр — только dirty.

### 10.1 Иерархия кэшей

```
tileCache (source, COW)
  → filteredCache (pixel filters only)
      → GPU layer.texture (content)
silhouetteR8 (from filtered alpha × mask)   [optional cache]
  → shadowBlurR8 / outlineMaskR8            [style caches, proxy or full]
groupComposite (proxy RT)
documentComposite (proxy RT)
```

---

## 11. Тесты / verify Plan 1

```bat
# existing
RayVPaint.exe --headless --script testfield/_export_project.py
RayVPaint.exe --headless --script testfield/_load_rayp.py
```

Добавить scripts (Plan 1):

- `_fill_layer_export.py` — create fill RGB, export PNG, assert pixels.  
- `_shadow_opacity.py` — layer fillOpacity=0 + shadow → non-zero shadow region.  
- `_group_fx.py` — two layers in group + group blur/shadow.  
- `_curves_channels.py` — A-off default; R-only curve.  
- `_rasterize_fill.py` — fill → rasterize → paint works.

---

## 12. Риски и решения

| Риск | Митигация |
|------|-----------|
| Full-frame filter already OOM on 16K | Styles never go through `RebuildFilteredPixels`; silhouette R8 + separable blur |
| Double-draw group children | Single walk: only top-level; children only inside group compose |
| Opacity confusion in UI | Plan 1: document that Opacity = Fill; Plan 2 labels |
| Outline texture/gradient scope creep | Data-only Plan 1; solid render |
| Multi-map Metallic/Roughness not ready | `FillChannelTarget` enum + store; compose only Diffuse path for now |
| Undo for rasterize | Coarse snapshot or disable undo first release with log |

---

## 13. Explicit non-goals Plan 1

- Полный UI Layer Effects для Shadow/Outline/Fill.  
- Interactive multi-channel curve editor UX.  
- Outline gradient/texture sampling quality.  
- GPU compute jump-flood outline.  
- Photoshop dual Opacity+Fill dual sliders.  
- Tiled mask rewrite (known gap).  
- Per-style blend modes beyond Normal.

---

## 14. Definition of Done — Plan 1

- [ ] `Layer::Type::Fill` + create API + no content paint + mask paint  
- [ ] fillOpacity не гасит LayerStyles  
- [ ] Shadow + Outline (solid) в viewport proxy и export  
- [ ] Group: virtual composite → filters → styles  
- [ ] Curves: channel enable bits, A default off, per-channel LUT storage  
- [ ] Rasterize bakes fill/filters/styles; group flatten API  
- [ ] `.rayp` round-trip soft-compat  
- [ ] Нет регресса: open old project.rayp, blend modes, existing filters  
- [ ] Структура кода: types/styles/fill не свалены только в UI  

**После DoD → Plan 2.**
