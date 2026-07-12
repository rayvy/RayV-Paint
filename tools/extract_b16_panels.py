#!/usr/bin/env python3
"""One-shot extract LayerEffects / ProjectSetup / Colors from EditorPanels.cpp."""
from __future__ import annotations
from pathlib import Path

root = Path(__file__).resolve().parents[1]
ep = root / "src" / "ui" / "EditorPanels.cpp"
lines = ep.read_text(encoding="utf-8").splitlines(keepends=True)
panels = root / "src" / "ui" / "panels"


def slice_lines(a: int, b: int) -> str:
    """1-based inclusive."""
    return "".join(lines[a - 1 : b])


def unindent(text: str, n: int = 8) -> str:
    out: list[str] = []
    pad = " " * n
    for line in text.splitlines(keepends=True):
        if line.startswith(pad):
            out.append(line[n:])
        else:
            out.append(line)
    return "".join(out)


def write(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8", newline="\n")
    print(f"  {len(content.splitlines()):4d}  {path.relative_to(root)}")


# --- Layer Effects: 2079-2461 ---
fx_body = unindent(slice_lines(2079, 2461)).replace("UiCombo(", "Ui::Combo(")
write(
    panels / "LayerEffectsPanel.h",
    """#pragma once

class Canvas;

namespace UI {
struct UIState;

// Layer Effects modal (styles + filters for active layer).
void DrawLayerEffectsPanel(UIState& state, Canvas& canvas);

} // namespace UI
""",
)
write(
    panels / "LayerEffectsPanel.cpp",
    f"""#include "LayerEffectsPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiDropdown.h"
#include "../widgets/UiVisualSlider.h"
#include "../widgets/UiTooltip.h"
#include "../../layer/LayerTypes.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace UI {{

void DrawLayerEffectsPanel(UIState& state, Canvas& canvas) {{
{fx_body}}}

}} // namespace UI
""",
)

# --- Project Setup: 2465-2657 ---
setup_body = unindent(slice_lines(2465, 2657)).replace("UiCombo(", "Ui::Combo(")
write(
    panels / "ProjectSetupPanel.h",
    """#pragma once
#include <d3d11.h>

class Canvas;

namespace UI {
struct UIState;

// Non-modal Project Setup (maps / labels / export).
void DrawProjectSetupPanel(UIState& state, Canvas& canvas, ID3D11Device* device = nullptr);

} // namespace UI
""",
)
write(
    panels / "ProjectSetupPanel.cpp",
    f"""#include "ProjectSetupPanel.h"
#include "../EditorPanels.h"
#include "../FileExplorer.h"
#include "../widgets/UiDropdown.h"
#include "../widgets/UiTooltip.h"
#include "../../core/ProjectManager.h"
#include "../../texset/TextureSetTypes.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace UI {{

void DrawProjectSetupPanel(UIState& state, Canvas& canvas, ID3D11Device* device) {{
    (void)device;
{setup_body}}}

}} // namespace UI
""",
)

# --- Colors: 2699-2975 ---
colors_body = unindent(slice_lines(2699, 2975)).replace("UiCombo(", "Ui::Combo(")
write(
    panels / "ColorsPanel.h",
    """#pragma once

class Canvas;
struct BrushSettings;

namespace UI {
struct UIState;

// Colors dock: SV square, hue/alpha, primary/secondary, HEX/RGB.
void DrawColorsPanel(UIState& state, Canvas& canvas, BrushSettings& brush);

} // namespace UI
""",
)
write(
    panels / "ColorsPanel.cpp",
    f"""#include "ColorsPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiTooltip.h"
#include "../style/UiTokens.h"
#include "../../core/PaintEngine.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern float g_SecondaryColor[4];

namespace UI {{

void DrawColorsPanel(UIState& state, Canvas& canvas, BrushSettings& brush) {{
{colors_body}}}

}} // namespace UI
""",
)

print("OK extract")
