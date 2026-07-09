#!/usr/bin/env python3
"""Generate simple monochrome path-only SVGs for RayV-Paint UI kit.
Rasterizer supports path d: M/L/H/V/C/Z only (no arcs, no strokes).
Thin strokes are approximated as filled quads.
"""
from __future__ import annotations
import math
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / "src" / "resources" / "icons"


def line(x1: float, y1: float, x2: float, y2: float, w: float = 1.2) -> str:
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy) or 1.0
    px, py = -dy / L * w / 2, dx / L * w / 2
    return (
        f"M{x1+px:.2f} {y1+py:.2f} L{x2+px:.2f} {y2+py:.2f} "
        f"L{x2-px:.2f} {y2-py:.2f} L{x1-px:.2f} {y1-py:.2f} Z"
    )


def svg(paths: list[str], vb: str = "0 0 24 24") -> str:
    body = "\n".join(f'  <path d="{d}"/>' for d in paths)
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" fill="#000000" '
        f'viewBox="{vb}" width="24" height="24">\n'
        f"{body}\n"
        "</svg>\n"
    )


SPECS: dict[str, list[str]] = {
    "placeholder.svg": ["M12 3 L21 12 L12 21 L3 12 Z"],
    "tool_gradient.svg": [
        # Frame + lower triangle (shaded half)
        "M4 4 L20 4 L20 6 L4 6 Z",
        "M4 18 L20 18 L20 20 L4 20 Z",
        "M4 4 L6 4 L6 20 L4 20 Z",
        "M18 4 L20 4 L20 20 L18 20 Z",
        "M5 12 L19 19 L5 19 Z",
    ],
    "tool_smudge.svg": [
        "M7 20 C7 13 10 11 12 6 C14 11 17 13 17 20 Z",
        "M10 6 C10 4 14 4 14 6 C14 8.2 10 8.2 10 6 Z",
    ],
    "tool_pipette.svg": [
        "M15 2.5 L21.5 9 L18 12.5 L11.5 6 Z",
        "M12 6 L4.5 13.5 L3.5 20.5 L10.5 19.5 L18 12 Z",
    ],
    "tool_pan.svg": [
        "M12 2 L15 8 L12 6.5 L9 8 Z",
        "M12 22 L9 16 L12 17.5 L15 16 Z",
        "M2 12 L8 9 L6.5 12 L8 15 Z",
        "M22 12 L16 15 L17.5 12 L16 9 Z",
        "M9.5 9.5 L14.5 9.5 L14.5 14.5 L9.5 14.5 Z",
    ],
    "tool_transform.svg": [
        "M3 3 L9 3 L9 5.5 L5.5 5.5 L5.5 9 L3 9 Z",
        "M15 3 L21 3 L21 9 L18.5 9 L18.5 5.5 L15 5.5 Z",
        "M3 15 L5.5 15 L5.5 18.5 L9 18.5 L9 21 L3 21 Z",
        "M18.5 15 L21 15 L21 21 L15 21 L15 18.5 L18.5 18.5 Z",
        "M8.5 8.5 L15.5 8.5 L15.5 15.5 L8.5 15.5 Z",
    ],
    "tool_select_rect.svg": [
        "M4 4 L20 4 L20 6.2 L4 6.2 Z",
        "M4 17.8 L20 17.8 L20 20 L4 20 Z",
        "M4 4 L6.2 4 L6.2 20 L4 20 Z",
        "M17.8 4 L20 4 L20 20 L17.8 20 Z",
    ],
    "tool_select_ellipse.svg": [
        "M12 3 C17.5 3 21 7 21 12 C21 17 17.5 21 12 21 "
        "C6.5 21 3 17 3 12 C3 7 6.5 3 12 3 Z",
    ],
    "tool_lasso.svg": [
        "M8 18 C5 15 5 9 10 6.5 C15 4 20.5 7 19.5 12.5 "
        "C18.5 17 13.5 19 11.5 16.5 C10 14.5 13 11 16 12.5 "
        "C16.5 12.8 16.2 13.8 15.5 13.5 C12.5 12 10.5 14.5 12 16.5 "
        "C13.5 18.5 17.5 16.5 18.2 12.5 C19 7.5 14.5 5.2 10.5 7.2 "
        "C6.5 9.2 6.5 14.5 9 17 Z",
    ],
    "tool_lasso_poly.svg": [
        "M5 19 L7 5 L13 9.5 L19.5 4 L18 17.5 L11 21 Z",
    ],
    "tool_wand.svg": [
        "M14 2 L15.8 8.2 L22 10 L15.8 11.8 L14 18 L12.2 11.8 L6 10 L12.2 8.2 Z",
        "M5 16 L7.5 21 L11.5 18.5 L10 17 L7.5 18.5 L6.5 16.5 Z",
    ],
    "tool_quick_select.svg": [
        "M5 9 L9 5 L13 9 L9 13 Z",
        line(13, 5, 20, 5, 1.5),
        line(20, 5, 20, 12, 1.5),
        line(5, 13, 5, 20, 1.5),
        line(5, 20, 12, 20, 1.5),
    ],
    "tool_smart_select.svg": [
        "M4 5 L13 5 L13 7.2 L6.2 7.2 L6.2 13 L4 13 Z",
        "M14 6 C18 6 20.5 9 20.5 12.5 C20.5 17 16.5 20.5 12 20.5 "
        "C9 20.5 7 18.5 6 16.5 L7.5 15.5 C8.2 17.2 9.8 18.5 12 18.5 "
        "C15.5 18.5 18.5 15.8 18.5 12.5 C18.5 9.8 16.8 8 14 8 Z",
        "M14 10 L17 13 L14 16 L11 13 Z",
    ],
    "tool_reset.svg": [
        "M12 5 C8.2 5 5 8.2 5 12 C5 15.8 8.2 19 12 19 "
        "C14.8 19 17.2 17.4 18.2 15 L16.5 14.2 C15.8 15.9 14 17 12 17 "
        "C9.2 17 7 14.8 7 12 C7 9.2 9.2 7 12 7 C13.5 7 14.8 7.7 15.6 8.8 "
        "L13 9.5 L18 11.5 L17.2 6 L15 8 C13.9 6.5 13 5 12 5 Z",
    ],
    "layer_add.svg": [
        # Hollow square frame + plus
        "M5 5 L19 5 L19 7 L5 7 Z",
        "M5 17 L19 17 L19 19 L5 19 Z",
        "M5 5 L7 5 L7 19 L5 19 Z",
        "M17 5 L19 5 L19 19 L17 19 Z",
        line(12, 8.5, 12, 15.5, 1.8),
        line(8.5, 12, 15.5, 12, 1.8),
    ],
    "layer_group_add.svg": [
        "M5 6 L19 6 L19 8.2 L5 8.2 Z",
        "M5 11 L19 11 L19 13.2 L5 13.2 Z",
        "M5 16 L13 16 L13 18.2 L5 18.2 Z",
        line(17, 14.5, 17, 21, 1.6),
        line(14, 17.5, 20, 17.5, 1.6),
    ],
    "layer_duplicate.svg": [
        # Front square frame
        "M6 8 L16 8 L16 10 L6 10 Z",
        "M6 16 L16 16 L16 18 L6 18 Z",
        "M6 8 L8 8 L8 18 L6 18 Z",
        "M14 8 L16 8 L16 18 L14 18 Z",
        # Back offset frame
        "M9 4.5 L20 4.5 L20 6.5 L9 6.5 Z",
        "M18 4.5 L20 4.5 L20 15 L18 15 Z",
        "M9 4.5 L11 4.5 L11 7.5 L9 7.5 Z",
        "M17.5 13 L20 13 L20 15 L17.5 15 Z",
    ],
    "layer_delete.svg": [
        line(6, 7, 18, 7, 1.5),
        "M9 7 L9 5 L15 5 L15 7 L13.2 7 L13.2 6.5 L10.8 6.5 L10.8 7 Z",
        "M8.2 7.5 L9.2 19 L14.8 19 L15.8 7.5 L14 7.5 L13.2 17.2 "
        "L10.8 17.2 L10 7.5 Z",
    ],
    "ts_pressure_radius.svg": [
        # Outer disc only (inner hole not subtracted — solid concentric reads as target)
        "M12 3.5 C16.7 3.5 20.5 7.3 20.5 12 C20.5 16.7 16.7 20.5 12 20.5 "
        "C7.3 20.5 3.5 16.7 3.5 12 C3.5 7.3 7.3 3.5 12 3.5 Z",
        "M12 10 C13.1 10 14 10.9 14 12 C14 13.1 13.1 14 12 14 "
        "C10.9 14 10 13.1 10 12 C10 10.9 10.9 10 12 10 Z",
    ],
    "ts_pressure_hardness.svg": [
        "M4 19.5 L12 3.5 L20 19.5 Z",
        "M8.5 19.5 L12 12 L15.5 19.5 Z",
    ],
    "ts_pressure_opacity.svg": [
        # Hollow square + diagonal
        "M5 5 L19 5 L19 7 L5 7 Z",
        "M5 17 L19 17 L19 19 L5 19 Z",
        "M5 5 L7 5 L7 19 L5 19 Z",
        "M17 5 L19 5 L19 19 L17 19 Z",
        line(7.5, 7.5, 16.5, 16.5, 1.6),
    ],
    "ts_mirror_h.svg": [
        line(12, 3, 12, 21, 1.4),
        "M4.5 8 L11 12 L4.5 16 Z",
        "M19.5 8 L13 12 L19.5 16 Z",
    ],
    "ts_mirror_v.svg": [
        line(3, 12, 21, 12, 1.4),
        "M8 4.5 L12 11 L16 4.5 Z",
        "M8 19.5 L12 13 L16 19.5 Z",
    ],
}


def main() -> None:
    ICONS.mkdir(parents=True, exist_ok=True)
    for name, paths in SPECS.items():
        path = ICONS / name
        path.write_text(svg(paths), encoding="utf-8", newline="\n")
        print(f"wrote {name} ({path.stat().st_size} bytes)")
    print(f"done: {len(SPECS)} icons")


if __name__ == "__main__":
    main()
