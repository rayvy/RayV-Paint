# Built-in Python plugins

Shipped next to the app as `{exe}/scripts/`.

- Loaded on startup after the scripting engine is ready.
- **Scripting → Refresh Scripts** reloads this folder and `Documents/RayVPaint/scripts/`.
- User scripts with the same file stem **override** builtins when loaded later (registry key = stem).

See `Documentation.MD` § Python scripting for the full API.

**Canvas context HUD:** `rayv.view.selection_screen_rect()` + `rayv.ui.begin_overlay`  
(§9.8) — e.g. “AI Generative Fill” under a selection.
