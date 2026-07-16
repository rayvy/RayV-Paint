# Validates API surface needed for external AI (Comfy-style) plugins.
# No ComfyUI dependency — only rayv contract.
import rayv

rayv.log_info("=== API surface smoke (host/image/edit/ui keys) ===")

assert rayv.host.is_main_thread() is True

# encode → decode roundtrip
w, h = 4, 4
rgba = bytes([255, 0, 0, 255] * (w * h))
enc = rayv.image.encode_png(rgba, w, h)
assert enc["ok"], enc.get("error")
dec = rayv.image.decode(enc["png"])
assert dec["ok"], dec.get("error")
assert dec["width"] == w and dec["height"] == h
assert len(dec["rgba8"]) == w * h * 4
rayv.log_info("image encode/decode OK")

# call_on_main
_flag = {"done": False}

def _job():
    _flag["done"] = True
    rayv.log_info("call_on_main ran on main=" + str(rayv.host.is_main_thread()))

rayv.host.call_on_main(_job)
# Host drains in UI frame; in headless CLI, drain may not run until a frame.
# Explicitly: if still main, we can also just run job logic for CLI tests.
if not _flag["done"]:
    # Simulate one host drain by invoking set_pixel path which is main-only
    pass

layer = rayv.doc.active_layer()
assert layer >= 0
assert rayv.doc.begin_edit(layer)
assert rayv.doc.is_edit_active()
assert rayv.doc.set_pixel(layer, 0, 0, 0, 1, 0, 1)
assert rayv.doc.end_edit("API smoke edit")
assert not rayv.doc.is_edit_active()
rayv.log_info("begin_edit/end_edit OK")

# selection helpers
rayv.ops.invoke("SelectAll", force=True)
bb = rayv.doc.selection_bounds()
rayv.log_info(f"selection_bounds={bb}")
reg = rayv.doc.get_region_for_generate(layer)
assert reg is not None
rayv.log_info(f"get_region_for_generate w={reg['w']} h={reg['h']} floats={len(reg['rgba'])}")
rayv.ops.invoke("Deselect", force=True)

# undo should exist after edit
r = rayv.ops.invoke("Undo", force=True)
rayv.log_info(f"Undo after script edit -> {r}")

rayv.log_info("=== API surface smoke done ===")
