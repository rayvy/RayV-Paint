# Plan 2 — Фичи, polish, UI/UX, багфиксы

**Предусловие:** Plan 1 (foundation) завершён — модели, compose graph, Fill/Styles/Group isolation, curves channel data, rasterize bake, serialize.

**Цель:** довести фичи до usable product surface: полный UI, качество Shadow/Outline, curves editor, Fill workflow, group FX UX, растеризация в меню, performance polish, багфиксы.

**Дата:** 2026-07-11

---

## 0. Карта фич → Plan 2 deliverables

| # | Запрос | Plan 1 | Plan 2 |
|---|--------|--------|--------|
| 1 | Opacity для visual derivatives (FX) | `style.opacity` + compose | UI sliders per style; master FX visibility |
| 2 | Opacity слоя ≠ opacity FX | fillOpacity semantics | Labels «Fill / Content»; optional dual Opacity+Fill |
| 3 | Shadow | core pass | Full params UI; live preview; quality (spread, noise optional) |
| 4 | Outline | solid core | Position; gradient UI; texture pick; quality dilate |
| 5 | Curves FX channels | bits + LUTs | Channel tabs RGB/A; A default off in UI; point editor sync |
| 6 | Fill Layer | create + solid + mask | Create button; properties panel; channel target UI (future-ready); texture |
| 7 | Rasterize | bake API | Menu/context; progress; undo; group flatten UX |
| 8 | Group FX | compose | Enable FX modal for groups; badges; thumb behavior |

---

## 1. UI / UX — Layer panel

### 1.1 Создание слоёв

| Action | UI |
|--------|-----|
| New Raster | existing |
| New Group | existing |
| **New Fill Layer** | кнопка / `+` menu: «Fill Layer» |
| Icon | solid swatch (можно SVG `layer_fill.svg`) |

Fill row в списке:

- Thumbnail = solid color (или grayscale value), не paint tiles.  
- Badge `F` / icon.  
- Mask thumb как у raster.  
- Двойной клик по thumb → Fill properties (color).  
- Content paint blocked: tooltip «Fill layers are not paintable — paint the mask».

### 1.2 Opacity header (Photoshop-like)

Текущий slider Opacity:

- **v2.1 default:** label **«Fill»** или tooltip: *Affects paint/content only; effects keep their own opacity*.  
- **v2.2 optional:** два слайдера:
  - **Opacity** — overall (content + styles)  
  - **Fill** — content only  

Рекомендация: сначала правильные labels (v2.1), dual — если пользователи просят.

### 1.3 FX entry points

- Кнопка **FX** на header слоя (уже есть для non-group) → **включить для Group**.  
- Убрать `Groups have no pixel filters` / `isGroup` early-out.  
- Layer list badge: `fx` если `!filters.empty() || !styles.empty()`.  
- Context menu: **Rasterize Layer…** / **Rasterize Group…**

### 1.4 Layer Effects modal — редизайн

Сейчас: один список `LayerFilter`.

**Новая структура (2 секции или tabs):**

```
┌─────────────────────────────────────────────┐
│ Layer Effects — "Layer 1"                   │
├──────────────┬──────────────────────────────┤
│ Styles       │  parameters                  │
│ ☑ Drop Shadow│                              │
│ ☑ Outline    │                              │
│ ───────────  │                              │
│ Filters      │                              │
│ ☑ Curves     │                              │
│ ☑ Blur       │                              │
│ [+ Add ▾]    │                              │
└──────────────┴──────────────────────────────┘
```

**Add menu:**

- Styles: Drop Shadow, Outline  
- Filters: Blur, HSV, Curves, Alpha Invert, Noise  

Drag-reorder **внутри** секции (styles order; filters order).  
Checkbox enabled; Del; focus → right pane.

---

## 2. Shadow UI + polish

### 2.1 Parameters (все из запроса)

| Param | Control | Range default |
|-------|---------|---------------|
| Enabled | checkbox | on |
| Opacity | slider | 0–100%, def 75% |
| Color | color picker | black |
| Distance | slider | 0–500 px, def 8 |
| Angle | angle dial / slider | 0–360°, def 120° |
| Offset X/Y | number / slider | extra manual offset |
| Spread | slider | 0–100% |
| Size (blur) | slider | 0–250 px, def 8 |
| (opt) Blend mode | combo | Normal / Multiply |

**Live:** `stylesDirty` + `MarkCompositeDirty` on change; debounced heavy size>32 if needed.

### 2.2 Quality polish

- Spread: pre-threshold / choke before blur (PS-like).  
- Soft knee at mask edges.  
- Optional noise on shadow (Plan 2.x, low priority).  
- Vector display: small arrow preview from angle+distance.  
- Export parity check script.

### 2.3 Independent opacity demo cases (QA)

1. fillOpacity=0, shadow on → только тень.  
2. style.opacity=0 → тень invisible, content ok.  
3. layer hidden → styles hidden.  
4. mask black region → no shadow there.

---

## 3. Outline UI + advanced fill

### 3.1 Base params

| Param | Control |
|-------|---------|
| Opacity | slider |
| Color | picker (Solid mode) |
| Size | slider px |
| Position | Outside / Inside / Center |
| Fill mode | Solid / Gradient / Texture |

### 3.2 Gradient mode

- Stops editor (reuse any gradient widget if exists; else minimal: 2–8 stops, t + color).  
- Mapping options (v2):
  - **Along normal** (distance from edge) — preferred for stroke  
  - **Angle** (linear across bbox) — secondary  
- Core: Plan 1 solid; Plan 2 implement sampling in outline PS / CPU.

### 3.3 Texture mode

- Path field + browse image.  
- Scale / offset / wrap.  
- Alpha from texture optional.  
- Load → GPU SRV cache on layer style (not document tiles).  
- Missing path: fallback solid + warning.

### 3.4 Quality

- Outside dilate: multi-pass max / distance field if size large.  
- Anti-aliased edge (proxy bilinear).  
- Large size on 16K export: warn / use separable approx.

---

## 4. Curves FX — «сделать нормальными»

### 4.1 Channel selector UI

```
[ RGB ] [ R ] [ G ] [ B ] [ A ]
```

- **A default OFF** (checkbox next to A or channel not auto-enabled).  
- Enabling A: explicit user action.  
- Per-channel curve points stored; switching channel shows that curve.  
- RGB master curve (optional): applies to R+G+B when «RGB» tab; separate from per-channel (PS model: RGB + individual).  
  - **v2.1:** master LUT + channel enable bits (Plan 1 data).  
  - **v2.2:** true multi-curve PS (RGB + R + G + B + A each with points).

### 4.2 Editor behavior

- Existing spline widget in modal — extend:
  - points map per channel key  
  - rebuild only dirty channel LUT  
  - Reset channel / Reset all  
  - Histogram optional (later)

### 4.3 Operator Curves vs Effect Curves

- Destructive `ApplyCurves` (Image menu): also add channel toggles for parity (small fix).  
- Non-destructive effect: primary focus.

### 4.4 Bugs to fix

- Points map keyed only `ai*10007+fi` — loses points on reorder/delete; **persist points in LayerFilter** or regenerate from LUT inverse (hard).  
  **Fix:** store `std::vector<std::pair<float,float>> curvePoints[5]` in filter or serialize points.  
- Reorder effects invalidates UI state — rebind focus.  
- Float docs: LUT sample in float domain not 8-bit only (if document F16/F32).

---

## 5. Fill Layer — product surface

### 5.1 Properties panel (dock or modal)

When active layer is Fill:

```
Fill Layer
  Channel target: [ Diffuse ▾ ]     // Transparency/Metallic/Roughness disabled or "soon"
  Value mode:     [ RGB | Gray 0–1 | Gray −1–1 ]
  Color / Value:  picker or slider
  Texture:        [ ] Use texture  path…  scale/offset
  Mask:           create / paint (same as raster)
```

### 5.2 Channel targets (future-ready)

- Diffuse only fully wired.  
- Other targets: store selection, show ImGui disabled + tooltip *«Multi-map system not ready»* — **no fake paint**.  
- When multi-map lands: Fill writes into target buffer without redesign.

### 5.3 Texture fill (Plan 2 feature)

- Load image → style/fill texture cache.  
- UV scale/offset.  
- Multiply with color.  
- Memory: one shared texture, not per-tile.

### 5.4 Workflow parity (Substance-like)

1. Add Fill Layer → canvas full color.  
2. Add mask → paint black/white to shape.  
3. Change color → instant recolor (no repaint).  
4. Rasterize → becomes normal paint layer.  
5. Effects on Fill: Curves/Shadow/Outline work on resolved content.

### 5.5 Guards / UX

- Brush on Fill content: status bar message, no silent fail.  
- Bucket/gradient/smudge: blocked on fill content; allowed on mask.  
- Merge Fill down: resolve fill → merge pixels (core may already via rasterize-on-merge).

---

## 6. Rasterize — UX

### 6.1 Entry points

- Layer context menu: **Rasterize Layer**  
- Group: **Rasterize Group** (flatten)  
- Layer Effects modal footer: **Rasterize…** (bake FX)  
- Confirm dialog if many children / large doc:

  > Rasterize will bake effects and convert to a regular paint layer. This cannot keep smart/fill parameters.

### 6.2 Progress

- Large docs: `LoadProgressFn`-style callback or status «Rasterizing…».  
- Don't freeze UI: if heavy, background + disable paint (optional).

### 6.3 Undo

- Full snapshot of affected layers (or tile COW of result + structure command).  
- Plan 2 **must** land undo if Plan 1 skipped it.

### 6.4 Result

- type=Raster, filters/styles empty, fill cleared, smart bytes cleared.  
- Group: one layer, children deleted, parentGroupId of outsiders remapped.

---

## 7. Group FX — UX + polish

### 7.1 UI

- FX button enabled on groups.  
- Tree: group header shows FX badge.  
- Collapse: FX still applied (compose doesn't depend on `groupExpanded`).  
- Thumbnail: group thumb = composite of children (cheap proxy sample) — optional, cap cost.

### 7.2 Behavior QA

| Case | Expected |
|------|----------|
| Child blend modes | resolve inside group RT first |
| Group opacity (fill) | fades flattened content, not style opacities independently per Plan 1 |
| Nested group + FX | inner flatten → outer FX |
| Hide group | children not drawn |
| Move layer out of group | leaves group composite |

### 7.3 Known hard edges (document + fix if cheap)

- Clipping masks between group children — if not supported, document.  
- Pass-through blend (PS) — **out of scope** unless already partial; default «group as unit».

---

## 8. Performance polish (Plan 2)

После foundation часто всплывает:

| Issue | Fix |
|-------|-----|
| Filter full-frame on every param drag | debounce 50–100ms; show low-res preview |
| Shadow size huge | cap viewport blur kernel; full only export |
| Style cache thrash | dirty only changed style; keep silhouette if only color/opacity change |
| Group recompose every child dab | dirty group flag; recompose group RT only |
| Fill texture large | downsample for proxy |
| Curves drag | rebuild filteredCache throttled |

**Profiling hooks:** log tag `fx` ms for shadow blur, filter rebuild, group compose.

---

## 9. Bugfix backlog (связанный)

1. **Groups double semantics:** export path ignores group isolation — fixed in Plan 1; verify UI isolation mode + groups.  
2. **FX modal blocked on groups** — remove.  
3. **Curves points not serialized** — lose on reload.  
4. **Rasterize no-op for filters** — Plan 1 API; UI wiring Plan 2.  
5. **Merge down drops styles** if not baked — fix merge to apply styles.  
6. **Duplicate layer** must clone styles + fill params (check Plan 1; fix if missing).  
7. **Thumb for fill/styles** — show approximated color/FX or content-only; document.  
8. **filterTypeNames out of range** if new FilterTypes — use safe name table.  
9. **AlphaInvert vs Outline** interaction order documented.  
10. **CPU export vs GPU viewport** parity tests for shadow offset Y sign (DX vs image space).

---

## 10. Scripting / headless (optional but valuable)

Expose to Python if ScriptingEngine allows:

```python
canvas.create_fill_layer("BaseColor", color=(1,0,0,1))
canvas.add_style(layer, "shadow", opacity=0.5, distance=10, size=6)
canvas.set_fill_opacity(layer, 0.0)
canvas.rasterize(layer)
```

Smoke scripts in `testfield/` for CI-ish manual.

---

## 11. Порядок работ Plan 2

| Phase | Work | Depends |
|-------|------|---------|
| **P2.1** | Layer Effects modal split Styles/Filters; enable groups | Plan1 styles/filters |
| **P2.2** | Shadow full UI + QA opacity independence | P2.1 |
| **P2.3** | Outline solid UI (position/size/color/opacity) | P2.1 |
| **P2.4** | Curves channel tabs + persist points + A default off UX | Plan1 curves bits |
| **P2.5** | Fill Layer create + properties + texture load | Plan1 fill |
| **P2.6** | Outline gradient + texture modes | P2.3 |
| **P2.7** | Rasterize UX + undo + merge fixes | Plan1 rasterize |
| **P2.8** | Group FX badges/thumbs; nested QA | Plan1 group compose |
| **P2.9** | Perf debounce, style cache, profiling | after P2.2–2.6 |
| **P2.10**| Bugfix sweep + export parity + docs handoff | end |

Можно параллелить: P2.4 ‖ P2.5; P2.6 after outline solid.

---

## 12. UI contract updates

Обновить `plans/core_architecture_handoff.md` / `CORE_UI_CONTRACT.md`:

```cpp
// Fill
CreateFillLayer(device, name);
layer.type == Layer::Type::Fill
layer.fill, layer.fillOpacity
CanPaintLayerContent(idx)

// Styles
layer.styles[]  // Shadow, Outline
// Filters still layer.filters[]

// Groups
// filters + styles allowed; compose isolates children

// Rasterize
RasterizeLayer / RasterizeGroup  // bake all
```

UI agent rules:

- Do not reimplement blur/shadow math.  
- Only mutate params + `filtersDirty` / `stylesDirty` + `MarkCompositeDirty`.  
- Fill color change → dirty composite only.

---

## 13. Definition of Done — Plan 2

- [ ] Пользователь создаёт Fill Layer, красит mask, меняет цвет, вешает Shadow/Outline  
- [ ] fillOpacity не гасит shadow/outline; style.opacity независим  
- [ ] Outline: solid + gradient + texture управляемы  
- [ ] Shadow: color, distance, angle, offset, spread, size, opacity  
- [ ] Curves FX: R/G/B/A toggles, A off by default, points survive save  
- [ ] Group: FX на сумме детей; UI не блокирует  
- [ ] Rasterize из меню с undo  
- [ ] Viewport ≈ export на тестовых сценах  
- [ ] Нет регресса open old .rayp / blend modes / paint  

---

## 14. Связь двух планов (одной картинкой)

```
                    ┌─────────────────────┐
                    │   Product features  │
                    │  (user-visible)     │
                    └──────────▲──────────┘
                               │ Plan 2
                    ┌──────────┴──────────┐
                    │ UI · quality · UX   │
                    │ bugfix · perf      │
                    └──────────▲──────────┘
                               │
                    ┌──────────┴──────────┐
                    │ Compose graph       │
                    │ Fill · Styles       │
                    │ Group isolate       │
                    │ Opacity model       │
                    │ Serialize · API     │
                    └─────────────────────┘
                               Plan 1
```

**Правило:** не начинать тяжёлый UI градиентов outline, пока P1.4–P1.6 не зелёные.  
**Правило:** не bake styles в `filteredCache` «чтобы быстрее нарисовать UI».

---

## 15. Оценка объёма (грубо)

| Plan | Effort (инженерные дни, порядок) |
|------|----------------------------------|
| Plan 1 full | ~8–14 d (split PR) |
| Plan 2 full | ~10–16 d |
| MVP slice | Plan1 P1.1–1.5 + Plan2 P2.1–2.3 + P2.5 solid fill only |

**MVP для «уже похоже на PS/SP»:** Fill solid + mask, Shadow, Outline solid, fillOpacity independent, group FX basic, curves channel toggles, rasterize.
