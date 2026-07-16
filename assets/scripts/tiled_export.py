# Built-in plugin: Tiled composite export (UI + logic in Python).
# Loaded from {exe}/scripts/ on startup and Scripting → Refresh Scripts.

PLUGIN = {
    "name": "Tiled Export",
    "menu": "Tiled Export…",
    "id": "tiled_export",
}

import rayv
import os

_open = False
_want_popup = False
_tiles_x = 2
_tiles_y = 2
_mode_idx = 0  # 0=count, 1=size
_modes = ["count", "size"]
_out_dir = ""
_pattern = "tile_{x}_{y}.png"
_status = ""


def on_load():
    global _out_dir
    # Default export next to user library
    try:
        _out_dir = os.path.join(rayv.plugins.user_dir(), "..", "export_tiles")
        _out_dir = os.path.normpath(_out_dir)
    except Exception:
        _out_dir = ""


def on_open():
    global _open, _want_popup, _status
    _open = True
    _want_popup = True
    _status = ""
    w, h = rayv.doc.size()
    rayv.log_info(f"Tiled Export opened (document {w}x{h})")


def on_ui():
    global _open, _want_popup, _tiles_x, _tiles_y, _mode_idx
    global _out_dir, _pattern, _status

    if not _open:
        return

    if _want_popup:
        rayv.ui.open_popup("Tiled Export##builtin")
        _want_popup = False

    visible, _open = rayv.ui.begin_popup_modal("Tiled Export##builtin", _open)
    if not visible:
        return

    w, h = rayv.doc.size()
    rayv.ui.text(f"Document: {w} × {h}  ·  bit depth: {rayv.doc.bit_depth()}")
    rayv.ui.separator()

    rayv.ui.text_wrapped(
        "Split the composite into tiles. "
        "Mode count = number of tiles on each axis. "
        "Mode size = tile size in pixels (grid covers the image)."
    )
    rayv.ui.spacing()

    ch, _mode_idx = rayv.ui.combo("Split mode", _mode_idx, ["count (tiles X×Y)", "size (tile W×H px)"])
    mode = _modes[_mode_idx] if 0 <= _mode_idx < len(_modes) else "count"

    label_x = "Tiles X" if mode == "count" else "Tile width (px)"
    label_y = "Tiles Y" if mode == "count" else "Tile height (px)"
    ch, _tiles_x = rayv.ui.input_int(label_x, _tiles_x)
    ch, _tiles_y = rayv.ui.input_int(label_y, _tiles_y)

    # Preview computed grid
    if mode == "count" and _tiles_x > 0 and _tiles_y > 0 and w > 0 and h > 0:
        cw, chh = w // _tiles_x, h // _tiles_y
        rayv.ui.text_disabled(f"Approx cell: {cw}×{chh}px  (last cells absorb remainder)")
    elif mode == "size" and _tiles_x > 0 and _tiles_y > 0 and w > 0 and h > 0:
        nx = (w + _tiles_x - 1) // _tiles_x
        ny = (h + _tiles_y - 1) // _tiles_y
        rayv.ui.text_disabled(f"Would write about {nx}×{ny} = {nx * ny} files")

    rayv.ui.spacing()
    ch, _out_dir = rayv.ui.input_text("Output folder", _out_dir, 512)
    ch, _pattern = rayv.ui.input_text("File name pattern", _pattern, 256)
    rayv.ui.text_disabled("Pattern must include {x} and {y}. Example: tile_{x}_{y}.png")

    rayv.ui.separator()
    if _status:
        rayv.ui.text_wrapped(_status)

    if rayv.ui.button("Export"):
        res = rayv.doc.export_tiles(_out_dir, _pattern, int(_tiles_x), int(_tiles_y), mode)
        if res.get("ok"):
            _status = f"OK — wrote {res.get('count', 0)} file(s) to {_out_dir}"
            rayv.log_info(_status)
        else:
            _status = f"Refused: {res.get('error') or 'see log'}"
            rayv.log_error(_status)

    rayv.ui.same_line()
    if rayv.ui.button("Close"):
        _open = False
        # Closing modal: ImGui needs open flag false
        pass

    rayv.ui.end_popup()
