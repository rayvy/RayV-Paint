# Document surface smoke (Python O4 doc API)
# RayVPaint_Core.exe --script test_doc_smoke.py --headless
import rayv

rayv.log_info("=== rayv.doc smoke ===")

w, h = rayv.doc.size()
rayv.log_info(f"size={w}x{h} bit_depth={rayv.doc.bit_depth()} tiles={rayv.doc.active_tile_count()}")

layers = rayv.doc.layers()
rayv.log_info(f"layers={len(layers)}")
for L in layers:
    rayv.log_info(f"  [{L['index']}] {L['name']} vis={L['visible']} mask={L['has_mask']} paint={L['can_paint']}")

idx = rayv.doc.active_layer()
if idx < 0:
    raise SystemExit("no active layer")

# Write a small red square via float pixels
sq = 32
rgba = []
for y in range(sq):
    for x in range(sq):
        rgba.extend([1.0, 0.1, 0.1, 1.0])
ok = rayv.doc.set_pixels(idx, 8, 8, sq, sq, rgba)
rayv.log_info(f"set_pixels red square -> {ok}")

px = rayv.doc.get_pixel(idx, 10, 10)
rayv.log_info(f"get_pixel(10,10) -> {px}")

# Round-trip a tiny rect
got = rayv.doc.get_pixels(idx, 8, 8, 4, 4)
rayv.log_info(f"get_pixels 4x4 floats={len(got)} first={got[:4] if got else None}")

# Selection via ops + doc
r = rayv.ops.invoke("SelectAll", force=True)
rayv.log_info(f"SelectAll -> {r} has_sel={rayv.doc.has_selection()}")
sel = rayv.doc.get_selection()
rayv.log_info(f"selection bytes={len(sel)}")
r = rayv.ops.invoke("Deselect", force=True)
rayv.log_info(f"Deselect -> {r} has_sel={rayv.doc.has_selection()}")

# New layer + mask
ni = rayv.doc.create_layer("ScriptLayer")
rayv.log_info(f"create_layer -> {ni}")
if ni >= 0:
    rayv.doc.set_pixel(ni, 0, 0, 0.0, 1.0, 0.0, 1.0)
    rayv.log_info(f"green pixel {rayv.doc.get_pixel(ni, 0, 0)}")
    if rayv.doc.create_mask(ni):
        m = bytearray(w * h)
        # darken left half of mask
        for y in range(min(h, 64)):
            for x in range(min(w, 64)):
                m[y * w + x] = 128 if x < 32 else 255
        okm = rayv.doc.set_mask(ni, bytes(m))
        rayv.log_info(f"set_mask -> {okm} has_mask={rayv.doc.has_mask(ni)}")

# Full-layer rgba8 sample size check (may be large — only log length)
raw = rayv.doc.get_layer_rgba8(idx)
rayv.log_info(f"get_layer_rgba8 active len={len(raw)} expect={w*h*4}")

rayv.log_info("=== rayv.doc smoke done ===")
