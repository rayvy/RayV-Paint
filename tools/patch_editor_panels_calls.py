#!/usr/bin/env python3
from pathlib import Path

ep = Path(__file__).resolve().parents[1] / "src" / "ui" / "EditorPanels.cpp"
lines = ep.read_text(encoding="utf-8").splitlines(keepends=True)


def replace_range(a: int, b: int, replacement: list[str]) -> None:
    global lines
    lines = lines[: a - 1] + replacement + lines[b:]


# Bottom → top so earlier line numbers stay valid
replace_range(
    2699,
    2975,
    [
        "        // 10. Colors (extracted)\n",
        "        UI::DrawColorsPanel(state, canvas, brush);\n",
        "\n",
    ],
)
replace_range(
    2465,
    2657,
    [
        "        // Project Setup (extracted)\n",
        "        UI::DrawProjectSetupPanel(state, canvas, device);\n",
        "\n",
    ],
)
replace_range(
    2079,
    2461,
    [
        "        // Layer Effects modal (extracted)\n",
        "        UI::DrawLayerEffectsPanel(state, canvas);\n",
        "\n",
    ],
)

text = "".join(lines)
old_inc = (
    '#include "panels/LayersPanel.h"\n'
    '#include "panels/ChannelsPanel.h"\n'
    '#include "panels/ToolSettingsPanel.h"\n'
)
new_inc = (
    '#include "panels/LayersPanel.h"\n'
    '#include "panels/ChannelsPanel.h"\n'
    '#include "panels/ToolSettingsPanel.h"\n'
    '#include "panels/LayerEffectsPanel.h"\n'
    '#include "panels/ProjectSetupPanel.h"\n'
    '#include "panels/ColorsPanel.h"\n'
)
if "LayerEffectsPanel.h" not in text:
    if old_inc not in text:
        raise SystemExit("include block not found")
    text = text.replace(old_inc, new_inc, 1)

ep.write_text(text, encoding="utf-8", newline="\n")
print("EditorPanels lines:", len(text.splitlines()))
for s in (
    "DrawLayerEffectsPanel",
    "DrawProjectSetupPanel",
    "DrawColorsPanel",
    "Layer Effects — modal",
    "BeginPopupModal(\"Layer Effects",
    "BeginDockPanel(\"Colors",
):
    print(f"  {text.count(s):3d}  {s}")
