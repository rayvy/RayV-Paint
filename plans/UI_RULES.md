# UI Rules (mandatory from Build 16+)

**Audience:** any human or agent touching `src/ui/**` or adding ImGui in the app.  
**Stack:** Dear ImGui + DX11 + existing kit under `src/ui/style`, `src/ui/widgets`.  
**Product feel:** Apple-like smart minimalism (Build 9–10) — motion, bounce, soft elevation, not “raw Dear ImGui demo”.

---

## 1. Non-negotiables

1. **No new hard-coded UI chrome** in panels/tools. Colors, radii, spacing, motion durations → **`Ui::Tokens()`** / theme.
2. **No new one-off controls** if a kit class exists. Missing control? **Extend a class or add a new widget class** under `src/ui/widgets/`.
3. **OOP / kit-first:** panels *compose* widgets; widgets own draw + interaction + animation state (by `ImGuiID`).
4. **UI does not implement core.** Call `Canvas` / `ProjectManager` / AssetStore APIs. Do not re-decode DDS for paint inside a panel.
5. **Core does not grow ImGui** for new features. New UI lives in `src/ui/`.
6. **DRY:** one combo, one color field, one modal footer pattern, one path/explorer entry point.
7. **Input / hotkeys / context (mandatory):**
   - New bindable action → **`core::ops::ActionCatalog`** first (`src/core/ops/ActionCatalog.*`). Never invent free strings only in `main.cpp`.
   - Dispatch hotkeys with **`core::ops::TryConsumeAction(id)`** (polls `AppContext`). Do not gate only on `io.WantTextInput`.
   - UI that owns keyboard (text fields, File Explorer open, settings, adjust modals, slider exact-entry) must be visible to **`AppContext`**. Widgets that eat keys call **`AppContext::NotifyUiKeyboardCapture()`**.
   - Canvas paint / marquee while FE or blocking modals open → check **`AppContext::BlocksCanvasInteraction()`**.
8. **Sliders:** new float params use **`Ui::SmartSliderFloat`** (or `Ui::SliderFloat`). Supports double-click / Ctrl+click exact entry, safe parse (reject garbage; round float→int), Backspace default + capture. **No raw `ImGui::SliderFloat` in new panel code.**

---

## 2. Kit map (use these)

| Concern | API / folder |
|---------|----------------|
| Colors, radii, spacing, scrim, motion durations | `Ui::Tokens()`, `UiTokens.cpp` |
| Ease / press / bounce | `Ui::Motion` (`UiMotion.h`) |
| Icon buttons / toggles | `UiIconButton`, `UiIconToggle` |
| Dropdowns (click + hold) | `UiDropdown` |
| Sliders | `Ui::SmartSliderFloat` / `Ui::SmartSliderInt` / `VisualSlider` |
| Hotkeys / action SSOT | `core::ops::ActionCatalog` + `KeymapManager` |
| Execute façade | `core::ops::OperatorRegistry` + `RegisterEditorOperators` |
| Menus | `core::ops::MenuAction(id)` / `Invoke(id)` |
| Python ops | `rayv.ops.list()` / `rayv.ops.invoke(id, force=False)` |
| Python document | `rayv.doc.*` (`ScriptDocApi`) — open/save, layers, pixels, mask, selection |
| Input ownership | `core::ops::AppContext` + `TryConsumeAction` / DispatchKeymapFrame |
| Color + optional pipette | `UiColorField` (`ColorFieldFlags_Pipette` / `FullPicker`) |
| Tooltips (delay) | `UiTooltip` |
| Dock panels | `UiPanel` |
| Path pick | `UiPathField` → prefer **File Explorer** modes for projects/maps |
| Icons | `SvgIconCache` |

**Banned in new code (outside widget impl):** raw `ImGui::Combo`, ad-hoc `ColorPicker4` without `UiColorField`, magic `ImVec4(0.2f, 0.7f, 1.f, 1.f)` for “accent”, duplicate modal chrome.

---

## 3. Motion (Apple-like)

- Press: scale down (`pressScale`), release **EaseOutBack** bounce.
- Hovers / opens: short ease-out (`durFast` / `durMed`), not linear.
- Modals: **scrim darken** from tokens, not bleach white.
- Hold-dropdown: `holdThresholdSec` from tokens.
- If animation is missing on a control that used to have it — **regression**, fix via kit, don’t invent parallel timers in the panel.

---

## 4. Color + Pipette (standard)

**Single entry for editable colors:**

```text
UiColorField(label, float rgba[4], UiColorFieldFlags)
  - swatch opens popup
  - popup: ColorPicker (themed)
  - optional: Pipette button → arm global pipette
```

**Pipette:**

| Target | Behavior |
|--------|----------|
| Brush primary | sample → `g_Brush.color` |
| Fill map slot | sample → `fill.mapColor[i].rgba` + dirty fill |
| Future assets | callback |

- Sample source: **active Channels / view map composite** (`SampleCanvasColor` / map-filtered stack).
- Arming pipette **closes** color popup so canvas receives click.
- Every color popup that edits paint/fill **must** expose pipette (not “sometimes”).

---

## 5. File / path UI

- Project, maps, export, config: **File Explorer** (`UI::FileExplorerMode::*`), not Win32 dialogs, unless emergency fallback.
- Do not reintroduce `BeginPopupModal("Save Project")` if FE owns the flow.

---

## 6. Layout

- Prefer measured wrap (item rect max + content region), not fixed column grids that break at narrow widths.
- Side panels resizable with min/max (see File Explorer splitters).
- Layers Fill chips: fixed chip size + wrap; no SameLine overflow off the panel.

---

## 7. When you need something new

```
1. Does Tokens cover it? → use token
2. Does a widget almost cover it? → add flag/param to that class
3. Else → new class in src/ui/widgets/UiSomething.{h,cpp}
4. Panel only calls UiSomething(...); no 100-line inline draw
5. Update this doc if the kit surface area grows
```

---

## 8. Anti-patterns (seen in tree; do not add more)

- `static bool UiCombo` local to `EditorPanels.cpp` while `UiDropdown` exists.
- Color picker only on Fill, not on brush/tool color chips.
- Full image bytes owned only by Fill layer with no shared cache (use Asset Browser from B16).
- UI logic inside `Canvas.cpp` for panel layout.
- God-file growth of `EditorPanels.cpp` (split into `ui/panels/*` in B16).

---

## 9. AppContext + ActionCatalog (agent contract)

```
Frame:
  AppContext::BeginFrame()
  BindOperatorHostFrame(&ActiveCanvas(), device)
  UI::RenderAll(...)          // MenuAction / NotifyUiKeyboardCapture
  AppContext::UpdateFromFrame(io, uiState, ...)
  OperatorRegistry::DispatchKeymapFrame()  // poll + execute registered ops
  UI one-shot flags → Invoke(id)
  if (!BlocksCanvasInteraction()) handle paint / selection
```

| Do | Don't |
|----|--------|
| Add action to **ActionCatalog** + **RegisterEditorOperators** one-liner | New `if (TryConsumeAction)` chain in main |
| `MenuAction("Foo")` / `Invoke("Foo")` for menus | Hardcoded labels + direct Canvas calls that skip poll |
| Python automation via `rayv.ops.invoke` | Parallel Python API that bypasses registry |
| Block document ops via ActionScope + AppContext | Special-case Backspace in three places |
| SmartSliderFloat for params | New raw SliderFloat without exact entry |

Footer **Context** button opens live dump + poll matrix — use it when debugging “why did Fill run?”.

---

## 10. Review checklist (PR)

- [ ] No new raw Combo/ColorPicker outside widgets  
- [ ] Colors from tokens / theme  
- [ ] Motion uses UiMotion where interactive  
- [ ] Pipette on color fields that paint/fill  
- [ ] No core algorithms reimplemented in UI  
- [ ] File flows use File Explorer when applicable  
- [ ] New hotkeys registered in ActionCatalog + TryConsumeAction  
- [ ] No new raw ImGui::SliderFloat (use SmartSliderFloat)  
- [ ] Text/FE/modal paths do not leak document hotkeys (AppContext)  

