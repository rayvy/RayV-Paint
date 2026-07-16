# API Standardization (Operators, Context, Registry)

**Status:** In progress — O0–O3 largely landed:
- `ActionCatalog` + `AppContext` + `TryConsumeAction`
- **`OperatorRegistry` + `RegisterEditorOperators`** + `DispatchKeymapFrame` (main no longer grows if-chains)
- **`MenuAction` / `Invoke`** for File/Image/Select menus
- SmartSlider exact entry across tool settings, adjust modals, settings, brush popup, preview3d
- Keybindings UI by category; footer Context panel  
Remaining: Python O4, Deselect as catalog op, drain any leftover raw sliders, optional operator palette (O5).  
**Motivation:** The editor “works”, but control flow is vibe-coded: UI/hotkeys call Canvas ad-hoc, context is implicit, duplication grows. Inspiration: **Blender** (operators + context + register), not a full Blender port.  
**Related:** `Documentation.MD` (file formats). Interchange converters: `plans/Standartization(Krita/Photoshop).MD` (**depends on this plan**, does not replace it).

---

## 0. Problem statement (honest)

| Symptom | Root cause today |
|---------|------------------|
| Backspace fills canvas while editing text in a popup | Keymap fires `FillSecondary` with weak context; `WantTextInput` is not enough / wrong order / modal still leaks |
| Sliders: some allow text edit, some don’t | Ad-hoc controls vs kit (`Ui::SmartSliderFloat` / raw ImGui) |
| “Where is Fill implemented?” | No single registry; menus + keymap + main.cpp each wire separately |
| Python can’t drive the editor | `rayv` exposes smoke helpers only; no operator/document surface |
| Mental model opaque | No global **AppContext**; hover/focus/tool/document live in scattered globals (`g_IsViewportHovered`, `g_ActiveTool`, …) |

**Goal:** Make every user-visible action an **operator** with stable id, metadata, poll (can-run), and execute — invokable from UI, hotkey, Python, and (later) scripts/macros. Keymap binds **operator ids**, not free strings that only `main.cpp` understands.

---

## 1. Vision (Blender-shaped, RayV-sized)

```
┌──────────────────────────────────────────────────────────────┐
│                        AppContext (per frame)                 │
│  focus region · hover · modal stack · active tool · doc id   │
│  selection flags · text_input · mouse buttons · paint target │
└────────────────────────────┬─────────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────────┐
│                    OperatorRegistry                           │
│  id → OperatorType { label, tooltip, poll, exec, category }  │
│  e.g. "core.ops.graphical2d.fill_secondary"                   │
└───────┬────────────────────┬────────────────────┬────────────┘
        │                    │                    │
   KeymapManager        UI buttons/menus     Python rayv.ops
   binds id+shortcut    invoke(id)           ops.invoke("…")
```

### 1.1 Non-goals

- Full Blender RNA / RNA properties system  
- Full undo macro recording in v1 (hooks only)  
- Moving all of Canvas into operators overnight  
- Replacing ImGui with a custom UI framework  

### 1.2 Goals (definition of done)

| # | Criterion |
|---|-----------|
| 1 | Every default keymap entry resolves to a **registered operator id** |
| 2 | Operators have **poll(ctx)** — Backspace does nothing when text field / modal text owns keyboard |
| 3 | Footer **Show Context** opens debug panel with live context dump |
| 4 | UI invokes `ops.invoke("…")` instead of open-coding `Canvas::FillSelection` in 5 places |
| 5 | Python can list ops + invoke a safe subset with same poll rules |
| 6 | New operators are added by registration, not by growing the main loop switch |

---

## 2. Current architecture (as-is map)

### 2.1 Input / keymap

| Piece | Location | Behavior |
|-------|----------|----------|
| `KeymapManager` | `src/core/KeymapManager.*` | String action names → `KeyCombination`; `ProcessKeyEvent` sets trigger flags |
| Defaults | `KeymapManager::Initialize` | e.g. `FillSecondary` = Backspace, `DeleteContent` = Delete |
| Dispatch | `main.cpp` huge `if (ConsumeActionTrigger(…))` | Direct Canvas/UI calls |
| Text gate | `if (!io.WantTextInput)` | Partial; popups/modals still leak; no region-aware poll |

### 2.2 UI

| Piece | Role |
|-------|------|
| `UI::RenderAll` | God-orchestrator of panels/modals |
| `Ui::*` kit | Partial standardization (tokens, SmartSlider, ColorField) |
| Status bar | `EditorPanels.cpp` `##StatusBar` — FPS/tool/zoom only |
| Globals | `g_ActiveTool`, `g_IsViewportHovered`, `g_IsLayersHovered`, brush, pipette state |

### 2.3 Document / paint

| Piece | Role |
|-------|------|
| `Canvas` | Document + layers + compose + file I/O (large surface) |
| `PaintEngine` | Dab math |
| `BrushLibrary` | Presets `.rvpbf` |
| Packages | RVPAF/RVPCF/RVPBF (`src/package`) |

### 2.4 Scripting

| Piece | Role |
|-------|------|
| `ScriptingEngine` | pybind11 embed, module `rayv` |
| Exposed | log, config size, zoom, load/save **flat** image, diagnostics |
| Missing | operators, document graph, keymap, context |

---

## 3. Core design

### 3.1 Operator identity (stable strings)

**Convention (Blender-like dotted id):**

```
core.ops.<domain>.<name>
```

Examples:

| Operator id | Human label | Tooltip (example) |
|-------------|-------------|-------------------|
| `core.ops.edit.undo` | Undo | Undo last command |
| `core.ops.edit.redo` | Redo | Redo last undone command |
| `core.ops.file.save_project` | Save Project | Save document as .rayp |
| `core.ops.graphical2d.fill_secondary` | Fill Secondary | Fill selection or canvas with secondary color |
| `core.ops.graphical2d.delete_content` | Delete Content | Clear selected pixels / content |
| `core.ops.tool.set_brush` | Brush Tool | Activate brush tool |
| `core.ops.layer.duplicate` | Duplicate Layer | Duplicate active layer |

**Domains (initial):**

| Domain | Contents |
|--------|----------|
| `edit` | undo, redo |
| `file` | new/open/save/export |
| `tool` | set active tool / cycle groups |
| `graphical2d` | paint-stack ops: fill, delete, invert, transform commits… |
| `layer` | duplicate, merge, mask ops… |
| `select` | select all, invert selection, deselect… |
| `view` | reset view, toggle panels… |
| `debug` | show_context, dump_ops… |

Python mirror (phase later):

```python
rayv.ops.invoke("core.ops.graphical2d.fill_secondary")
# or namespaced helpers generated from registry:
# rayv.ops.graphical2d.fill_secondary()
```

### 3.2 OperatorType (metadata + behavior)

```cpp
// Conceptual — not final headers
namespace core::ops {

struct OperatorType {
    std::string id;           // unique
    std::string label;        // UI
    std::string description;  // tooltip / docs
    std::string category;     // "Edit", "Paint", "File"
    // Optional: icon id, search tags

    // Return true if invoke is allowed for this context
    bool (*poll)(const AppContext& ctx) = nullptr;

    // Execute; return OperatorResult { FINISHED, CANCELLED, PASS_THROUGH, RUNNING_MODAL }
    OperatorResult (*execute)(AppContext& ctx, const OperatorProperties& props) = nullptr;

    // Optional: default properties schema (JSON or typed bag)
};

class OperatorRegistry {
public:
    static OperatorRegistry& Get();
    void Register(OperatorType type);
    const OperatorType* Find(std::string_view id) const;
    std::vector<const OperatorType*> List() const;
    OperatorResult Invoke(std::string_view id, AppContext& ctx,
                          const OperatorProperties& props = {});
};

} // namespace core::ops
```

**Registration** at startup (static init or explicit `RegisterCoreOperators()` called from main after systems ready):

```cpp
void RegisterCoreOperators() {
  OperatorRegistry::Get().Register({
    .id = "core.ops.graphical2d.fill_secondary",
    .label = "Fill Secondary",
    .description = "Fill selection or background with the secondary color",
    .category = "Paint",
    .poll = [](const AppContext& c) {
      return c.hasDocument
          && !c.keyboardOwnedByText
          && !c.modalBlocksDocumentOps
          && c.focusRegion != FocusRegion::TextField;
    },
    .execute = [](AppContext& c, const OperatorProperties&) {
      c.canvas->FillSelection(c.secondaryColor);
      return OperatorResult::Finished;
    },
  });
}
```

### 3.3 AppContext (shared picture of “what is happening”)

Updated **once per frame** (and optionally after modal open/close):

| Field | Source (today → tomorrow) |
|-------|---------------------------|
| `focusRegion` | Viewport / Layers / Channels / Modal / FileExplorer / TextField / Other |
| `hoverRegion` | Same taxonomy from ImGui hovered window + canvas hover flags |
| `activeTool` | `g_ActiveTool` |
| `paintTarget` | Layer content vs mask |
| `hasDocument` | ProjectManager active canvas size > 0 |
| `hasSelection` | Canvas selection state |
| `keyboardOwnedByText` | `io.WantTextInput` **or** active InputText id set |
| `wantCaptureKeyboard/Mouse` | ImGui IO |
| `modalStack` | Names of open modals (Layer Effects, Curves, FE, …) |
| `modalBlocksDocumentOps` | Policy: true for text-heavy modals / any modal that owns keyboard |
| `mouseDownL/R/M` | GLFW / ImGui |
| `mouseCanvasUV` | If over canvas |
| `activeLayerIndex` | Canvas |
| `keymapLastOp` | Last successfully invoked operator id (debug) |

**Rule:** Modules that care about “should I run?” take `const AppContext&`, not raw globals.

### 3.4 Keymap ↔ operators

| Today | Tomorrow |
|-------|----------|
| `BindAction("FillSecondary", Backspace)` | `BindOperator("core.ops.graphical2d.fill_secondary", Backspace)` |
| `ConsumeActionTrigger("FillSecondary")` in main | `KeymapManager` calls `OperatorRegistry::Invoke` after **poll** |
| Parallel string namespaces | **One** id space |

Migration: keep old action names as **aliases** for one release of WIP, map them in registry, then delete.

### 3.5 Input ownership policy (kills Backspace-fill bug)

Priority (highest first):

1. **Text input** (`WantTextInput` or tracked InputText) → **no document operators**  
2. **Modal with `blocksDocumentOps`** → only modal-local / cancel ops  
3. **File Explorer open** → FE navigation keys only (if any)  
4. **Viewport-focused paint ops** → require `focusRegion == Viewport` **or** explicit “global” flag on operator  
5. Else → default poll  

Operators declare:

```cpp
enum class OperatorScope : uint8_t {
  Global,          // undo? careful — often still blocked by text
  Document,        // needs doc; blocked by text/modal
  Viewport,        // needs viewport focus
  ModalOnly,       // only when named modal open
};
```

**Fill secondary** → `Document` or `Viewport` + poll `!keyboardOwnedByText`.

**Also fix UI kit:** Backspace on **slider default reset** must **capture** key when slider is active (set WantCaptureKeyboard / local handle) so it never reaches Fill.

### 3.6 UI kit consistency (anti-duplication)

| Control | Standard |
|---------|----------|
| Float param | `Ui::SmartSliderFloat` — **always** supports double-click / ctrl text if we enable one path |
| Color | `Ui::ColorField` + pipette where paint |
| Path | File Explorer / `Ui::PathField` fallback only |
| Buttons that run ops | `Ui::OperatorButton(opId)` → label/tooltip/shortcut from registry |

**Audit pass:** ban new raw `ImGui::SliderFloat` in panels; migrate existing offenders.

### 3.7 Footer: Show Context

Status bar (`##StatusBar`, right side):

```
[ … status text … ]                    [ Context ]
```

- Button **`Context`** / `show_context`  
- Opens dock or popup **Context Debug** (`core.ops.debug.show_context`)  
- Live fields from `AppContext` + list of **poll-true** operators + last invoke result  
- Optional: “Why blocked?” for a selected op (poll failed because text input)

This is the **understandability tool** you asked for — not only for users, for future-you debugging vibe-code residue.

---

## 4. Layering (where code lives)

```
src/core/ops/
  OperatorTypes.h
  OperatorRegistry.h/.cpp
  AppContext.h/.cpp
  RegisterCoreOperators.cpp   // or split by domain

src/core/KeymapManager.*      // bind/invoke operators
src/ui/widgets/UiOperatorButton.*  // optional thin wrapper
src/ui/panels/ContextDebugPanel.*  // show_context UI
src/scripting/                  // expose ops.list / ops.invoke
```

**Canvas** remains the **implementation backend** (`FillSelection`, `Undo`, …). Operators are a **thin command façade** — do not reimplement paint math inside ops.

---

## 5. Phased delivery (this plan only)

### Phase O0 — Context spine (1–2 sessions)

| Task | Outcome |
|------|---------|
| O0.1 | `AppContext` struct + `AppContext::UpdateFromFrame(ImGuiIO, Canvas, UIState, …)` |
| O0.2 | Replace ad-hoc hover flags gradually **or** fill context from existing flags first |
| O0.3 | Footer **Context** button + debug panel dump (read-only) |
| O0.4 | Document poll rules in this plan (done) |

**Exit:** You can open Context panel and see hover/focus/tool/text ownership live.

### Phase O1 — Registry + 10 critical operators

Migrate first:

1. `edit.undo` / `edit.redo`  
2. `graphical2d.fill_secondary` (Backspace)  
3. `graphical2d.delete_content` (Delete)  
4. `file.save_project` / `open_project` / `new_project`  
5. `tool.set_brush` / `set_eraser`  
6. `select.select_all`  

| Task | Outcome |
|------|---------|
| O1.1 | OperatorRegistry + RegisterCoreOperators |
| O1.2 | KeymapManager stores operator ids; Invoke path with poll |
| O1.3 | main.cpp dispatch shrinks for migrated ops |
| O1.4 | FillSecondary never runs under text/modal (regression test checklist) |

**Exit:** Backspace in Layer Effects / path fields does **not** fill canvas; still fills when viewport owns context.

### Phase O2 — Keymap + UI wiring

| Task | Outcome |
|------|---------|
| O2.1 | All default bindings in Initialize map to op ids |
| O2.2 | Menu items use registry labels + shortcuts |
| O2.3 | Settings keybind UI lists operators (label + id) |
| O2.4 | OperatorButton helper for toolbars |

### Phase O3 — Kit cleanup

| Task | Outcome |
|------|---------|
| O3.1 | Inventory raw sliders/combos in `src/ui` |
| O3.2 | SmartSlider: guaranteed text path + Backspace = reset (capture key) |
| O3.3 | UI_RULES.md update: “new chrome only via kit / operators” |

### Phase O4 — Python / document surface (was “Phase A” of interop)

This is the **API surface for automation and later PSD/KRA converters** — not buried inside interop plan.

| Task | Outcome |
|------|---------|
| O4.1 | `rayv.ops.list()`, `rayv.ops.invoke(id, props?)` with same poll |
| O4.2 | Document API: open/save rayp, layer list, read/write rgba tiles, masks |
| O4.3 | Brush: apply/import rvpbf |
| O4.4 | Headless test: invoke fill + undo via Python |

**Exit:** Converters in `Standartization(Krita/Photoshop).MD` can depend on O4 without inventing a parallel API.

### Phase O5 — Expand coverage

| Task | Outcome |
|------|---------|
| O5.1 | Migrate remaining main.cpp ConsumeActionTrigger block |
| O5.2 | Optional macro/history of last N ops (debug) |
| O5.3 | Search “operator palette” (Ctrl+Space?) — optional |

---

## 6. Example: fixing Backspace / Fill

**Today:**

```
Keymap FillSecondary → main Consume → Canvas::FillSelection
gate: !WantTextInput only
```

**Tomorrow:**

```
Keymap → core.ops.graphical2d.fill_secondary
poll: hasDocument && !keyboardOwnedByText && !modalBlocksDocumentOps
      && (scope allows Document)
execute: canvas.FillSelection(secondary)
```

**UI kit:** slider/text field sets capture so Backspace is reset-local, never falls through.

---

## 7. Context Debug panel (spec)

**Title:** Context  

**Sections:**

1. **Focus / Hover** — region enum + ImGui window name  
2. **Input** — WantTextInput, WantCapture*, mouse buttons, last key  
3. **Document** — path, size, bit depth, active layer, selection yes/no  
4. **Tool** — active tool, paint target  
5. **Modals** — stack  
6. **Operators** — filterable table: id, label, poll ✓/✗, reason if false  
7. **Last invoke** — id, result, timestamp  

**Operator:** `core.ops.debug.show_context` (toggle panel).  
**Keymap:** unbound by default; footer button always available.

---

## 8. Migration strategy (avoid big-bang rewrite)

1. Introduce registry + context **alongside** existing code.  
2. Migrate **high-pain** ops first (Fill, Delete, Undo).  
3. Dual-path: old action names alias → new ids for WIP.  
4. Delete `ConsumeActionTrigger` only when zero call sites remain.  
5. Do **not** block product features on full O5 completion.

---

## 9. Relation to other plans

| Plan | Relationship |
|------|----------------|
| `Documentation.MD` | File/package standards; ops use them, don’t redefine |
| `Standartization(Krita/Photoshop).MD` | Interop converters; **depends on Phase O4** of this plan; **no longer owns “Phase A”** |
| `UI_RULES.md` | Update when kit + OperatorButton land |
| `CORE_UI_CONTRACT.md` | Align “UI does not implement core” with operators |

---

## 10. Risks

| Risk | Mitigation |
|------|------------|
| Registry becomes second god-object | Keep execute thin; logic stays in Canvas/services |
| Poll too strict → “dead” hotkeys | Context debug shows why; unit-list of poll matrix |
| Performance | Context update is O(1) fields; no deep scans |
| Incomplete migration forever | Track op coverage % in Context panel |

---

## 11. Immediate next steps (when implementing)

1. Add `AppContext` + footer Context dump (**O0**) — pure visibility, low risk.  
2. Register Fill/Delete/Undo with poll (**O1**) — fixes Backspace pain.  
3. Then expand registry and Python (**O2–O4**).

---

*This plan is the abstraction layer that makes the rest of the project legible — to you, to Python, and to future importers.*


# Claude Post Review:


План хороший — это ровно тот случай, когда стоит один раз навести архитектурный порядок, вместо того чтобы латать Backspace-баг точечно. По сути ты делаешь то же самое, что уже интуитивно понимаешь по Blender: operator+context+registry — это не culture cargo, а реальный способ убить N мест дублирования одним абстрактным слоем. Несколько мыслей по существу:

**Что сильно:**

- Диагноз в разделе 0 точный — Backspace-баг, "где реализован Fill", неспособность Python достучаться до редактора — это всё симптомы одной болезни (implicit context + no single source of truth). Ты не лечишь симптом, а меняешь корень.
- `AppContext` как read-only снапшот на кадр — правильный выбор. Не пытаешься делать реактивность/сигналы, просто "текущая картина мира", это дёшево и предсказуемо.
- Explicit non-goals (3.1) — важно. Без этого легко скатиться в переизобретение Blender RNA целиком, а тебе это не нужно на масштабе RayVPaint.
- Миграция через aliasing старых action names, а не big-bang rewrite — разумно, особенно для solo-разработчика без QA команды.
- Context Debug panel — это не только дебаг-инструмент, это ещё и живая документация "какие операторы вообще есть", что решает твою же боль "где искать Fill".

**Где стоит подумать жёстче:**

1. **`poll`/`execute` как raw function pointers (`bool (*poll)(...)`)** — это исключает захват состояния (капчер по lambda с состоянием), что обычно и нужно для доменных операторов (например, "какой канал активен для этого fill"). Скорее всего тебе нужен `std::function` или type-erased функтор, иначе ты быстро упрёшься в то, что каждый оператор снова тащит state через globals — то есть частично воспроизведёшь ту же болезнь, только через registry. Учитывая что ты и так уже полагаешься на PBR multi-channel editing, где операции параметризуются по каналу, это всплывёт быстро.

2. **`OperatorProperties`** нигде толком не специфицирован (просто "JSON или typed bag" в комментарии). Для v1 c 10 операторами это можно отложить, но уже на fill/delete тебе скорее всего понадобится хотя бы "channel target" и "fill color source" как параметры — иначе operator façade превращается в тонкую обёртку над Canvas-методом с нулевой параметризацией, и тогда непонятно зачем он вообще (кроме poll). Стоит явно решить: v1 — вообще без properties (bake context only), и вернуться к этому в O4 вместе с Python.

3. **Modal ownership / focus region** — самая мутная часть плана концептуально, хотя технически описана хорошо. `modalBlocksDocumentOps` как policy-флаг per-modal — это ещё одна ручная таблица, которую придётся поддерживать при добавлении новых модалок (Curves, Layer Effects, FE...). Со временем это тот же тип дублирования, который ты пытаешься убить, просто на уровне modal registration вместо keymap dispatch. Возможно стоит сразу проектировать так, что каждая модалка **декларирует** свой `OperatorScope`, а не редактор её угадывает извне — грязный хак, но меньше ручного мейнтенанса.

4. **Экзит-критерий O1** ("Backspace никогда не фильтрует под text/modal") — это единственный по-настоящему проверяемый критерий во всём плане. Стоит формализовать это как маленький regression-чеклист/юнит-тест сразу в O1, а не полагаться на ручную проверку — у тебя и так эмпирическая методология (RenderDoc, много вариантов + валидация), тут она отлично ложится: напиши 5-10 сценариев ("Backspace в Curves", "Backspace в path field", "Backspace на viewport без документа") как таблицу истинности для poll, и гоняй её при каждом изменении context/poll логики.

5. **Risk-раздел честный**, но "Registry становится вторым god-object" — реальный риск именно потому, что у тебя уже есть прецедент (Canvas как большая поверхность). Стоит с самого начала жёстко ограничить: registry содержит только id→metadata mapping и dispatch, zero business logic, execute — buквально one-liner в Canvas/service call. Это уже написано в разделе 4, просто стоит вынести как жёсткое правило code review, а не как заметку.

**По фазировке** — O0→O1→O2 звучит правильно с точки зрения "низкий риск, быстрый выигрыш, докажи модель на 10 операторах прежде чем расширять". O4 (Python surface) явно и разумно отложен до появления надёжной core-модели — не пытаешься одновременно чинить архитектуру и городить scripting API.

В целом план — это правильная инвестиция именно потому что ты уже прошёл MVP и функциональность растёт быстрее архитектуры (сам это фиксируешь в мотивации). Единственное, что я бы поправил перед тем как начинать кодить — пункты 1 и 2 выше (тип для poll/execute и минимальный scope для properties), потому что raw fn pointers без захвата состояния — это то место, где через 3-4 оператора ты обнаружишь, что снова тащишь глобалы внутрь execute-лямбд.