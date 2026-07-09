# RayV-Paint — UI Rework Plan (полная поэтапная переработка)

**Дата:** 2026-07-10 (rev 2 — детальный roadmap + Stage 1.5)  
**Ветка:** `DX11-upgrade`  
**Стек:** C++ / Dear ImGui / DX11  
**Core API:** `plans/CORE_UI_CONTRACT.md` (не ломать)

---

## 0. Концепция продукта (зафиксировать)

**Умный минимализм в духе Photoshop / Affinity:**

| Принцип | Практика |
|---------|----------|
| Мало chrome | Иконки вместо текста; secondary — tooltip / hold / RMB |
| Упаковка | Один слот = tool group (wand ∥ quick ∥ smart); click cycle / hold menu |
| Контекст | RMB на thumb слоя, mask, blend chip — разные меню |
| Модификаторы | Ctrl / Shift / Alt на click (multi-select, add/sub, isolate…) |
| Скрытая мощь | Сложное за «безобидной» иконкой; детали в tooltip |
| DRY UI | Все motion/полупрозрачность/outlines/dropdown — **классы kit**, не inline в `RenderAll` |

**Полная переработка интерфейса разрешена.** Алгоритмы canvas/tiles/undo — только при крайней необходимости (например `DuplicateLayer`).

---

## 0.1 Ограничения исполнителя (рационально)

| Факт | Следствие для плана |
|------|---------------------|
| Контекст агента конечен | **Малые вертикальные срезы** + gate «ты потыкал → OK» перед следующим |
| Тесты UI — **ручные** (ты) | После каждого gate: чеклист 3–8 пунктов; без твоего OK не раздуваем scope |
| ImGui immediate-mode | Анимации/hold-dropdown — state map по `ImGuiID` внутри widget-классов |
| Один большой diff = риск | Не «переписать EditorPanels за раз»; **пилот → миграция панель за панелью** |

**Правило gate:** агент сдаёт slice → build OK → ты подтверждаешь UX → только потом следующий slice.

---

## 0.2 Что ImGui гарантированно умеет (заложено в kit)

| Возможность | Как |
|-------------|-----|
| Полупрозрачность панелей | `bgElevated` α 0.72–0.90; fade через `AnimFloat` |
| Outlines | stroke tokens: idle / hover / active / danger; thickness control |
| Dropdown **click** | open list → click item = commit → outside/Esc = cancel |
| Dropdown **hold** | hold > threshold → scrub list → **release** = commit → RMB/Esc = cancel |
| Press + bounce | scale ease-out press → EaseOutBack release |
| SVG icons + theme tint | cache SRV; dark=white / light=black multiply |

Режимы dropdown — **глобально в классе** (`ClickOnly` / `HoldOnly` / `ClickAndHold`), call site только flags.

**Easing:** cubic / quint / OutBack. **Не** linear lerp как финальный motion.

---

## 0.3 Карта этапов (обзор)

```
STAGE 1     Foundation: tokens, motion, SVG cache, core widgets
    │
    ▼  [GATE 1 — ты OK]
STAGE 1.5   Shell: default dock layout, chrome, Toolbar+groups на kit,
            dual-mode dropdown live, kill procedural glyphs
    │
    ▼  [GATE 1.5]
STAGE 2a    Colors + Channels + Tool Settings (visual, low risk)
    │
    ▼  [GATE 2a]
STAGE 2b    Layers (multi-select, bottom bar, mask/blend UX) + Dup core if needed
    │
    ▼  [GATE 2b]
STAGE 2c    FX window (PS-like) + Viewport Navigation split
    │
    ▼  [GATE 2c]
STAGE 2d    Polish: stroke preview, pipette HUD, icon pass, dead code purge
```

Оценка объёма (грубо, agent sessions):

| Stage | Сложность | Зависит от твоего QA |
|-------|-----------|----------------------|
| 1 | M | 1 gate (кнопки/motion ощущаются OK?) |
| 1.5 | M–L | 1 gate (toolbar/groups/dropdown hold) |
| 2a | M | 1 gate |
| 2b | L | 1–2 gates (layers = самое жирное) |
| 2c | M–L | 1 gate |
| 2d | S–M | 1 gate |

---

# STAGE 1 — Foundation (kit only + micro-pilot)

**Цель:** инфраструктура. Почти без «красивой перекладки» всех панелей.  
**Риск:** низкий. **Откат:** легко (новые файлы + тонкий pilot).

## 1.1 Файлы

```
src/ui/
  style/
    UiTokens.h              # цвета, α, radii, space, durations, ease kinds, press/bounce
    UiMotion.h / .cpp       # Ease*, AnimFloat, AnimBool, dt clock
  icons/
    SvgIconCache.h / .cpp   # path resolve, rasterize, SRV cache, tint draw
  widgets/
    UiIconButton.h / .cpp
    UiIconToggle.h / .cpp
    UiDropdown.h / .cpp     # click + hold modes (API complete; polish in 1.5)
    UiVisualSlider.h / .cpp # base track only (hue/alpha skins → 2a)
src/resources/icons/
    placeholder.svg
    (tool_*.svg copies from testfield/svg or placeholders)
```

CMake: добавить исходники в `RayVPaint_Core` / UI target.  
Копировать `icons/` в output при build (или resolve `testfield/svg` + `src/resources/icons`).

## 1.2 `UiTokens`

- Color: window, elevated (α), hairline stroke, accent, text, danger, overlay scrim  
- Radius / spacing / icon sizes (`iconSm`…`iconDock`)  
- Motion: `durFast/Med/Slow`, ease enum  
- Press: `pressScale`, `bounceOvershoot`  
- Dropdown: `holdThresholdMs` (~200), panel α  
- Theme hook: при `ApplyTheme` обновлять light/dark icon tint + token set  

**Один файл = «как выглядит app».**

## 1.3 `UiMotion`

- `EaseOutCubic`, `EaseInCubic`, `EaseInOutCubic`, `EaseOutQuint`, `EaseOutBack`  
- `AnimFloat`: from→to, duration, ease, Update(dt)  
- `AnimBool`: open ease-out / close ease-in  
- State storage: `unordered_map<ImGuiID, T>` **внутри** widget .cpp  
- dt clamp (0..0.05) — стабильность при hitches  

## 1.4 `SvgIconCache`

Resolve order:
1. `src/resources/icons/`  
2. exe-dir `icons/`  
3. `testfield/svg/`  

- Key: path + pixel size  
- Tint: ignore SVG fill; multiply by theme  
- Missing → `placeholder.svg`  
- **Запрет** новых procedural `AddLine` icons в feature code  

## 1.5 Виджеты

### `Ui::IconButton`
- SVG, size, enabled, tooltip  
- Down → scale ease-out to press  
- Up → bounce EaseOutBack  
- return edge-click  

### `Ui::IconToggle`
- on/off fill/stroke accent  
- same press motion  

### `Ui::Dropdown` (API в Stage 1, UX polish в 1.5)
Flags:
- `ClickAndHold` (default)  
- `ClickOnly` / `HoldOnly`  

| Gesture | Behavior |
|---------|----------|
| Short click | Open panel (fade+height ease-out). Item click = commit. Outside / Esc = cancel |
| Hold ≥ threshold | Selector mode: highlight under cursor; **release** = commit; **RMB / Esc** = cancel |

Panel: semi-transparent elevated, rounded, outline hairline, clip during expand.

### `Ui::VisualSlider` (stub)
- Hit track + value 0..1  
- Skins (hue / opacity checker) — **Stage 2a**

## 1.6 Pilot (минимум в UI)

Подключить kit **только** к 1–2 кнопкам Tool Settings или одному tool button на Toolbar — проверить motion + SVG load + build.

**Не** мигрировать весь Toolbar в Stage 1 (это 1.5).

## 1.7 Stage 1 — Done / Gate 1 (ты проверяешь)

- [ ] Release build  
- [ ] SVG icon видна (или placeholder)  
- [ ] IconButton: press + bounce ощущается «не linear»  
- [ ] Theme dark/light: иконка tint ок  
- [ ] Dropdown API компилируется; demo open/close (можно debug-only)  

**Стоп до твоего OK.**

## 1.8 Out of scope Stage 1

Layers multi-select, FX window, Colors layout, dock shell redesign, delete procedural toolbar wholesale.

---

# STAGE 1.5 — Shell + Toolbar + groups + live dual-dropdown

**Зачем отдельный этап:** между «есть классы» и «ломаем Layers/FX» нужен **видимый** новый chrome и проверка hold-dropdown на реальных tool groups — иначе Stage 2b утонет.

**Цель:** Photoshop-like tool grouping + весь Toolbar/Tool Settings chrome на kit; дефолтный dock sensibly; semi-transparency/outlines везде единообразно.

## 1.5.1 Global style pass (тонкий)

- `ImGuiStyle`: WindowRounding, FrameRounding, ItemSpacing из tokens  
- Panel backgrounds ближе к elevated where makes sense  
- Не redesign каждой панели — только **shared style** + outline consistency  

## 1.5.2 Toolbar rework

| Было | Станет |
|------|--------|
| Procedural glyphs / mixed | Только SVG `Ui::IconButton` |
| Grouped tools | **Click:** activate last / cycle · **Hold:** dual-mode dropdown list of variants |
| Badge hotkey | optional small badge или tooltip only (minimal) |

Tool groups (PS packing):

| Slot | Variants |
|------|----------|
| Select | Rect, Ellipse |
| Lasso | Free, Polygonal |
| Wand | Magic Wand, Quick Select, Smart Select |
| Brush-like | Brush (Eraser may stay separate — PS-like E key) |

Hold on slot → list; release on item → select tool (hold mode).  
Click → activate current / cycle (как сейчас Keymap groups).

## 1.5.3 Tool Settings — partial migrate

- Pressure / mirror → `IconToggle` + SVG placeholders  
- Sliders ещё старые **или** base `VisualSlider` without fancy skins  
- Full visual opacity/hue — 2a  

## 1.5.4 Dropdown production use

- Toolbar group picker = **first real** ClickAndHold consumer  
- Blend mode chip later (2b) — same class  

## 1.5.5 Icons layout on disk

```
src/resources/icons/
  placeholder.svg
  tool_brush.svg … (from testfield or placeholder copies)
  tool_wand.svg, tool_quick_select.svg, …
```

Список «замени арт» — § Icons inventory.

## 1.5.6 Dead code

- Удалить `DrawToolIcon` vector branches **после** полной миграции toolbar  
- Оставить fallback один frame только если icon missing → placeholder  

## 1.5.7 Stage 1.5 — Done / Gate 1.5

- [ ] Все tool slots SVG  
- [ ] Hold на Wand-группе: scrub + release выбирает; RMB/Esc cancel  
- [ ] Click: tool switches / cycles  
- [ ] IconToggle pressure в Tool Settings  
- [ ] Нет «дешёвого» linear bounce  
- [ ] Build + smoke paint/select  

**Стоп до твоего OK.**

---

# STAGE 2a — Colors + Channels + Tool Settings visuals

**Цель:** панели с **низким** structural risk (мало selection model).  
**Риск:** средний (Colors interaction). **Зависимость:** kit + icons.

## 2a.1 Channels

- Checkbox убрать  
- Thumb `GetChannelPreviewSRV` = toggle button  
- Inactive: крест α **0.25** (draw list)  
- Adaptive list vs horizontal row — сохранить  

**QA:** solo R/G/B/A, viewport channel masks still work.

## 2a.2 Colors

- SV region **fills** avail; rectangle allowed (not forced square)  
- Hue: rainbow strip (visual slider skin)  
- Alpha: color→transparent on **checkerboard**  
- Swatch + palette compact  
- Semi-transparent panel tokens  

**QA:** pick hue/sat/val/alpha; brush uses color; resize dock stretches SV.

## 2a.3 Tool Settings finish

- Visual opacity bar (checker)  
- Nice-to-have: stroke preview strip (small ImDrawList or tiny texture)  
- Remaining letter UI → icons  

**QA:** brush pressure toggles, opacity visual matches paint, smudge still no color.

## 2a.4 Gate 2a

- [ ] Channels thumbs only  
- [ ] Colors adaptive + hue/alpha strips  
- [ ] Tool Settings icons + visual opacity  
- [ ] Build  

---

# STAGE 2b — Layers (самый тяжёлый продуктовый кусок)

**Цель:** multi-select + bottom bar + smart packing (mask/blend/FX entry).  
**Риск:** высокий (state + DnD + core dup). **Резать на 2b-i / 2b-ii если надо.**

## 2b-i — Structure + bottom bar + icons

- Bottom dock strip (fixed icon size): **Add Layer, Add Group, Duplicate, Delete**  
- List = scroll child above  
- Visibility / thumb / mask / name / blend chip / FX icon — одна строка  
- Letters → SVG  
- Opacity slider **только active**  

**QA:** add/delete single, mask +M smart (sel vs empty), layout compact.

## 2b-ii — Multi-select + Dup/Del + blend dropdown

| Input | Behavior |
|-------|----------|
| Click | select one + set active |
| Ctrl+click | toggle in selection |
| Shift+click | range |
| Delete icon | selection if any, else active |
| Ctrl+J / Dup icon | duplicate selection or active |
| Blend chip | `Ui::Dropdown` ClickAndHold → blend modes |
| RMB thumb | context: opaque select, mask ops, rasterize, group… |

**Core (если нет):** `DuplicateLayer` / `DuplicateLayers` — минимальный API, COW tiles.

**QA:** multi delete/dup; active border vs selection fill; Ctrl+J; blend hold-select.

## 2b Gate

- [ ] Bottom bar works  
- [ ] Multi-select modifiers  
- [ ] Dup/Del correct  
- [ ] Blend dropdown dual-mode  
- [ ] Groups DnD still OK (`MoveLayerIntoGroup` / `ReorderLayer`)  

---

# STAGE 2c — FX window + Viewport Navigation

## 2c.1 Layer Effects (PS-like)

- Dock/window **Layer Effects** for **active** layer  
- List: enable checkbox, type name/icon, reorder drag-drop, multi FX same type OK  
- Right/bottom: params for selected FX  
- Add menu / delete  
- Data: `layer.filters` order = composite order  

**QA:** add 2 blur+hsv, reorder, disable one, undo if core supports mutation on apply… (filters non-destructive — toggle dirty).

## 2c.2 Properties split

| Dock | Content |
|------|---------|
| **Viewport Navigation** (new) | Zoom readout, pan, flip H/V, rotation, reset |
| **Properties** | Project type/path, output PNG/DDS, ICC, Quick Export |

**QA:** both docks independently; export still works.

## 2c Gate

- [ ] FX window usable  
- [ ] Viewport Navigation separated  
- [ ] Properties only project/export  

---

# STAGE 2d — Polish + pipette HUD + cleanup

## 2d.1 Pipette tool HUD (инструмент)

- Sample point indicator on canvas  
- Small color circle  
- HEX + RGB/HSV chip  

## 2d.2 Cleanup

- Remove dead ImGui letter UI  
- Icon inventory complete; list placeholders for art  
- Optional micro-animations on panel open (fade) — only via kit  
- Pass: tooltips everywhere secondary  

## 2d Gate (final)

- [ ] Full smoke: open project.rayp, paint, wand, layers multi, FX, export  
- [ ] No procedural tool icons left  
- [ ] You sign off «UI rework baseline»  

---

# Icons inventory

Канон: `src/resources/icons/` (POST_BUILD → next-to-exe `icons/`). Dev fallback: `testfield/svg/`.  
Regen geometric set: `python scripts/gen_ui_icons.py` (path-only SVGs for kit rasterizer).

| Logical name | File | Art status | Intended final art |
|--------------|------|------------|--------------------|
| tool_brush | tool_brush.svg (+brush.svg) | **hand SVG** | keep / refine |
| tool_eraser | tool_eraser.svg (+eraser.svg) | **hand SVG** | keep / refine |
| tool_fill | tool_fill.svg (+fill_bucket.svg) | **hand SVG** | keep / refine |
| tool_gradient | tool_gradient.svg | geometric | linear gradient well |
| tool_smudge | tool_smudge.svg | geometric | finger / smudge tip |
| tool_pipette | tool_pipette.svg | geometric | eyedropper |
| tool_pan | tool_pan.svg | geometric | open hand |
| tool_transform | tool_transform.svg | geometric | move/scale handles |
| tool_select_rect | tool_select_rect.svg | geometric | marquee rect |
| tool_select_ellipse | tool_select_ellipse.svg | geometric | marquee ellipse |
| tool_lasso | tool_lasso.svg | geometric | freehand lasso |
| tool_lasso_poly | tool_lasso_poly.svg | geometric | polygonal lasso |
| tool_wand | tool_wand.svg | geometric | magic wand |
| tool_quick_select | tool_quick_select.svg | geometric | brush+marquee |
| tool_smart_select | tool_smart_select.svg | geometric | smart contour |
| tool_reset | tool_reset.svg | geometric | view reset / home |
| layer_add | layer_add.svg | geometric | + layer |
| layer_group_add | layer_group_add.svg | geometric | folder + |
| layer_duplicate | layer_duplicate.svg | geometric | copy layers |
| layer_delete | layer_delete.svg | geometric | trash |
| ts_pressure_* | ts_pressure_*.svg | geometric | tablet pressure glyphs |
| ts_mirror_h/v | ts_mirror_*.svg | geometric | mirror axes |
| placeholder | placeholder.svg | diamond | missing-icon fallback |
| layer_mask_add / layer_fx / layer_visible / layer_blend | — | **not used yet** | optional when chrome migrates |

Replace geometric rows by hand anytime; keep `viewBox` + filled `<path d>` only (no stroke-only, no arc `A`).

---

# Core touches (только extreme need)

| API | Stage | Why |
|-----|-------|-----|
| `DuplicateLayer` / `DuplicateLayers` | 2b | Ctrl+J |
| optional `DeleteLayers` batch undo | 2b | multi-delete one undo step |

ICC / wand / transform / masks — уже в contract; UI only calls.

---

# Manual QA protocol (для тебя)

После каждого Gate агент пишет короткий список. Твой ответ:

- `OK` / `OK with notes: …` / `FAIL: …`  

Без `OK` агент **не** начинает следующий stage (можно hotfix текущего).

Минимальный smoke всегда:

1. Launch Release  
2. Open `testfield/project.rayp`  
3. Brush stroke  
4. One selection tool  
5. Save/export not required every gate — only 2c/2d  

---

# Anti-patterns

- Весь `EditorPanels.cpp` rewrite in one PR  
- Easing/copy-paste outside `UiMotion`  
- Linear motion as final  
- New vector icons in cpp  
- Full-doc pixel readback for UI chrome  
- Layer* pointer remap after reorder  
- Skip Gate because «almost works»  

---

# Priority rationale (why this order)

1. **Kit first** — иначе Stage 2 = 5 копий dropdown  
2. **1.5 shell/toolbar** — ты рано видишь «новый app», hold-dropdown на реальных tools  
3. **2a visual panels** — высокий UX impact, низкий data-model risk  
4. **2b layers last among mid** — max state complexity, needs kit+dropdown proven  
5. **2c FX + nav** — depends on active layer model stable  
6. **2d HUD/polish** — non-blocking  

---

# References

- `plans/CORE_UI_CONTRACT.md`  
- `plans/PART2_UI_AGENT_PROMPT.md`  
- `src/ui/EditorPanels.*`, `src/main.cpp`, `src/Canvas.h`  
- Icons: `testfield/svg/` → `src/resources/icons/`  

---

## Changelog

| Rev | Change |
|-----|--------|
| 1 | Initial Stage 1 + 2 outline |
| 2 | Full phased roadmap; Stage 1.5; gates; dual-mode dropdown; PS minimalism; agent limits + manual QA |
| 3 | Stage 2d: floating toolbar square accent, pipette HUD, geometric SVG pass, delayed tooltips on IconButton |
