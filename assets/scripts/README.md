# Built-in Python plugins

Shipped next to the app as `{exe}/scripts/`.

- Loaded on startup after the scripting engine is ready.
- **Scripting → Refresh Scripts** reloads this folder and `Documents/RayVPaint/scripts/`.
- User scripts with the same file stem **override** builtins when loaded later (registry key = stem).

See `Documentation.MD` § Python scripting for the full API.
