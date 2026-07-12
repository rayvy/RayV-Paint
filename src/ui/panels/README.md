# UI panels (Build 16)

**Rule:** each dock/window is its own `.cpp/.h` under this folder.  
`EditorPanels::RenderAll` only **orchestrates** (dockspace, order, FE open flags).

## Planned modules

| File | Content |
|------|---------|
| `LayersPanel.*` | Layers list, fill chips, mask row |
| `ChannelsPanel.*` | Map switch + RGBA solo |
| `ToolSettingsPanel.*` | Brush strip |
| `AppMenus.*` | Main menu bar items |
| `Modals.*` | Remaining modals (settings, canvas edit, recovery) |

## Migration status

- **S3:** Save/Load Project/Config → File Explorer; Win32 dialogs → `ui/dialogs/`
- **S2:** `Ui::Combo` in kit; FE hotspots migrated
- **S4:** extract Layers next (largest block in EditorPanels)

See `plans/UI_RULES.md` and `plans/BUILD16_CLEANUP_OPTIMIZATION.md`.
