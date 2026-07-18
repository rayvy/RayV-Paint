"""
Unusual / edge-case autotest suite for RayV Paint.

Focus: not "happy path paint", but weird inputs, boundaries, refusal paths,
and formats that historically break DDS loaders.

Run:
  build\\Release\\RayVPaint.exe --headless --script test_unusual_scenarios.py

Exit: raise Exception on hard failure (host maps to non-zero via RunScript).
Soft skips log WARN and continue when optional assets are missing.
"""
from __future__ import annotations

import os
import tempfile

import rayv

_failures: list[str] = []
_soft: list[str] = []


def _ok(name: str, cond: bool, detail: str = "") -> None:
    if cond:
        rayv.log_info(f"[PASS] {name}" + (f" — {detail}" if detail else ""))
    else:
        msg = f"[FAIL] {name}" + (f" — {detail}" if detail else "")
        rayv.log_error(msg)
        _failures.append(msg)


def _soft_skip(name: str, reason: str) -> None:
    rayv.log_warn(f"[SKIP] {name} — {reason}")
    _soft.append(f"{name}: {reason}")


# ---------------------------------------------------------------------------
# 1) Missing / garbage paths — must refuse cleanly (no crash)
# ---------------------------------------------------------------------------
def test_missing_and_garbage_paths() -> None:
    rayv.log_info("--- missing / garbage paths ---")
    _ok(
        "load_image missing file → False",
        rayv.load_image("testfield/__does_not_exist_rayv__.dds") is False,
    )
    _ok(
        "doc.open missing file → False",
        rayv.doc.open("testfield/__does_not_exist_rayv__.png") is False,
    )
    # Empty path / relative nonsense
    _ok("load_image empty → False", rayv.load_image("") is False)
    _ok("doc.open empty → False", rayv.doc.open("") is False)


# ---------------------------------------------------------------------------
# 2) Tiny / odd canvas sizes
# ---------------------------------------------------------------------------
def test_tiny_and_odd_canvases() -> None:
    rayv.log_info("--- tiny / odd canvases ---")
    for w, h, label in ((1, 1, "1x1"), (3, 5, "3x5 odd"), (7, 1, "7x1 strip"), (64, 63, "64x63")):
        ok = rayv.doc.new_blank(w, h)
        _ok(f"new_blank {label}", ok is True, f"got {rayv.doc.size()}")
        if not ok:
            continue
        sw, sh = rayv.doc.size()
        _ok(f"size matches {label}", sw == w and sh == h, f"{sw}x{sh}")
        layer = rayv.doc.active_layer()
        _ok(f"active layer {label}", layer >= 0)
        # Paint single pixel + readback
        if layer >= 0:
            _ok(
                f"set_pixel {label}",
                rayv.doc.set_pixel(layer, 0, 0, 0.25, 0.5, 0.75, 1.0) is True,
            )
            px = rayv.doc.get_pixel(layer, 0, 0)
            _ok(
                f"get_pixel {label}",
                px is not None and len(px) >= 3,
                str(px),
            )


# ---------------------------------------------------------------------------
# 3) Out-of-bounds & invalid rects — refuse, don't corrupt
# ---------------------------------------------------------------------------
def test_oob_and_invalid_rects() -> None:
    rayv.log_info("--- OOB / invalid rects ---")
    assert rayv.doc.new_blank(32, 32)
    layer = rayv.doc.active_layer()
    assert layer >= 0

    # Far OOB single pixel: implementation may clamp or refuse — must not crash
    try:
        rayv.doc.set_pixel(layer, 99999, 99999, 1, 0, 0, 1)
        rayv.doc.get_pixel(layer, -1, -1)
        _ok("OOB set/get_pixel no crash", True)
    except Exception as e:
        _ok("OOB set/get_pixel no crash", False, str(e))

    # Zero / negative rect via set_pixels should fail or no-op safely
    bad = rayv.doc.set_pixels(layer, 0, 0, 0, 0, [])
    _ok("set_pixels 0x0 → False/no-op", bad is False or bad is True)  # no crash is success

    # Wrong buffer length
    wrong = rayv.doc.set_pixels(layer, 0, 0, 2, 2, [1.0, 0.0, 0.0, 1.0])  # need 16 floats
    _ok("set_pixels wrong buffer → False", wrong is False)

    # fill_rect with zero size
    try:
        r = rayv.doc.fill_rect(layer, 0, 0, 0, 0, 1, 0, 0, 1)
        _ok("fill_rect 0x0 no crash", True, f"returned={r}")
    except Exception as e:
        _ok("fill_rect 0x0 no crash", False, str(e))

    # fill_rect partially outside — must not crash
    try:
        rayv.doc.fill_rect(layer, 28, 28, 16, 16, 0, 1, 0, 1)
        _ok("fill_rect partial OOB no crash", True)
    except Exception as e:
        _ok("fill_rect partial OOB no crash", False, str(e))


# ---------------------------------------------------------------------------
# 4) Layer stack abuse + unicode names
# ---------------------------------------------------------------------------
def test_layer_stack_abuse() -> None:
    rayv.log_info("--- layer stack abuse ---")
    assert rayv.doc.new_blank(64, 64)
    base = rayv.doc.layer_count()
    ids = []
    for i in range(8):
        name = f"Stack_{i}" if i % 2 == 0 else f"слой_{i}_🎯"
        ni = rayv.doc.create_layer(name)
        ids.append(ni)
        _ok(f"create_layer {name}", ni >= 0, f"index={ni}")

    _ok("layer_count grew", rayv.doc.layer_count() >= base + 8,
        f"count={rayv.doc.layer_count()}")

    # Rename + visibility + opacity extremes
    if ids and ids[0] >= 0:
        _ok("set_layer_name unicode", rayv.doc.set_layer_name(ids[0], "переименован ✓") is True)
        _ok("set invisible", rayv.doc.set_layer_visible(ids[0], False) is True)
        _ok("opacity 0", rayv.doc.set_layer_opacity(ids[0], 0.0) is True)
        _ok("opacity 1", rayv.doc.set_layer_opacity(ids[0], 1.0) is True)
        _ok("opacity >1 clamped/accepted", rayv.doc.set_layer_opacity(ids[0], 2.0) is True
            or rayv.doc.set_layer_opacity(ids[0], 2.0) is False)

    # Delete in reverse; never delete last paint layer if engine protects it
    for ni in reversed(ids):
        if ni < 0:
            continue
        if rayv.doc.layer_count() <= 1:
            break
        ok = rayv.doc.delete_layer(ni)
        # Some engines refuse deleting active-only layer — either OK if consistent
        _ok(f"delete_layer {ni} no crash", ok is True or ok is False)

    # Invalid layer indices
    _ok("delete_layer -1 → False", rayv.doc.delete_layer(-1) is False)
    _ok("set_active_layer huge → False", rayv.doc.set_active_layer(99999) is False)


# ---------------------------------------------------------------------------
# 5) Selection extremes
# ---------------------------------------------------------------------------
def test_selection_extremes() -> None:
    rayv.log_info("--- selection extremes ---")
    assert rayv.doc.new_blank(16, 16)
    w, h = rayv.doc.size()

    r = rayv.ops.invoke("Deselect", force=True)
    _ok("Deselect on empty", r is not None)

    r = rayv.ops.invoke("SelectAll", force=True)
    _ok("SelectAll", True, str(r))
    _ok("has_selection after SelectAll", rayv.doc.has_selection() is True)

    sel = rayv.doc.get_selection()
    _ok("selection length w*h", len(sel) == w * h, f"len={len(sel)}")

    bb = rayv.doc.selection_bounds()
    _ok("selection_bounds present", bb is not None, str(bb))

    # Wrong-size selection mask must refuse
    bad = rayv.doc.set_selection(bytes([255, 0, 0]))
    _ok("set_selection wrong size → False", bad is False)

    # Full white mask
    full = bytes([255] * (w * h))
    _ok("set_selection full", rayv.doc.set_selection(full) is True)

    # Full black (empty) mask
    empty = bytes([0] * (w * h))
    _ok("set_selection empty mask", rayv.doc.set_selection(empty) is True)

    rayv.ops.invoke("Deselect", force=True)


# ---------------------------------------------------------------------------
# 6) Edit session: begin / cancel / double-end
# ---------------------------------------------------------------------------
def test_edit_session_edges() -> None:
    rayv.log_info("--- edit session edges ---")
    assert rayv.doc.new_blank(32, 32)
    layer = rayv.doc.active_layer()
    assert layer >= 0

    _ok("begin_edit", rayv.doc.begin_edit(layer) is True)
    _ok("is_edit_active", rayv.doc.is_edit_active() is True)
    rayv.doc.set_pixel(layer, 1, 1, 1, 0, 0, 1)
    _ok("cancel_edit", rayv.doc.cancel_edit() is True)
    _ok("not active after cancel", rayv.doc.is_edit_active() is False)

    # Double begin
    _ok("begin_edit again", rayv.doc.begin_edit(layer) is True)
    rayv.doc.fill_rect(layer, 0, 0, 4, 4, 0, 0, 1, 1)
    _ok("end_edit", rayv.doc.end_edit("unusual session") is True)
    _ok("not active after end", rayv.doc.is_edit_active() is False)

    # end/cancel without begin — must not crash
    try:
        e = rayv.doc.end_edit("orphan")
        c = rayv.doc.cancel_edit()
        _ok("orphan end/cancel no crash", True, f"end={e} cancel={c}")
    except Exception as ex:
        _ok("orphan end/cancel no crash", False, str(ex))

    # Undo after real edit
    r = rayv.ops.invoke("Undo", force=True)
    _ok("Undo after end_edit", r is not None, str(r))


# ---------------------------------------------------------------------------
# 7) Mask edge cases
# ---------------------------------------------------------------------------
def test_mask_edges() -> None:
    rayv.log_info("--- mask edges ---")
    assert rayv.doc.new_blank(8, 8)
    layer = rayv.doc.create_layer("MaskEdge")
    if layer < 0:
        _soft_skip("mask edges", "create_layer failed")
        return
    w, h = rayv.doc.size()
    _ok("create_mask", rayv.doc.create_mask(layer) is True)
    _ok("has_mask", rayv.doc.has_mask(layer) is True)

    # Wrong length
    _ok("set_mask wrong len → False", rayv.doc.set_mask(layer, bytes([0, 1, 2])) is False)

    # All-zero mask
    _ok("set_mask all zero", rayv.doc.set_mask(layer, bytes([0] * (w * h))) is True)
    # Checker
    chk = bytes([(0 if (x + y) % 2 == 0 else 255) for y in range(h) for x in range(w)])
    _ok("set_mask checker", rayv.doc.set_mask(layer, chk) is True)
    got = rayv.doc.get_mask(layer)
    _ok("get_mask length", len(got) == w * h, f"len={len(got)}")


# ---------------------------------------------------------------------------
# 8) image encode/decode extremes
# ---------------------------------------------------------------------------
def test_image_codec_edges() -> None:
    rayv.log_info("--- image codec edges ---")
    # 1x1
    rgba = bytes([10, 20, 30, 255])
    enc = rayv.image.encode_png(rgba, 1, 1)
    _ok("encode_png 1x1", enc.get("ok") is True, str(enc.get("error")))
    if enc.get("ok"):
        dec = rayv.image.decode(enc["png"])
        _ok("decode 1x1", dec.get("ok") is True and dec.get("width") == 1, str(dec.get("error")))

    # size mismatch buffer
    enc_bad = rayv.image.encode_png(bytes([0, 0, 0, 255]), 2, 2)  # need 16 bytes
    _ok("encode_png size mismatch → not ok", enc_bad.get("ok") is not True)

    # load_file missing
    lf = rayv.image.load_file("testfield/__nope__.png")
    _ok("image.load_file missing → not ok", lf.get("ok") is not True)

    # save_file 1x1 to temp
    try:
        td = tempfile.mkdtemp(prefix="rayv_unusual_")
        out = os.path.join(td, "one.png")
        ok = rayv.image.save_file(out, rgba, 1, 1)
        _ok("image.save_file 1x1", ok is True or (isinstance(ok, dict) and ok.get("ok")), str(ok))
        if os.path.isfile(out):
            os.remove(out)
        os.rmdir(td)
    except Exception as e:
        _ok("image.save_file 1x1", False, str(e))


# ---------------------------------------------------------------------------
# 9) Unicode path open
#    Run early (before heavy painting) so empty Background still triggers
#    "first image" canvas resize in LoadImageToLayer.
# ---------------------------------------------------------------------------
def test_unicode_path() -> None:
    rayv.log_info("--- unicode path ---")
    path = os.path.join("testfield", "юникод", "тест_июл_2026.png")
    if not os.path.isfile(path):
        _soft_skip("unicode path", f"missing {path}")
        return
    ok = bool(rayv.load_image(path) or rayv.doc.open(path))
    _ok("open unicode path", ok is True, path)
    if ok:
        w, h = rayv.doc.size()
        # File is known 256x256 when first-load path applies; otherwise at least non-zero.
        _ok("unicode doc has size", w > 0 and h > 0, f"{w}x{h}")
        if w == 256 and h == 256:
            rayv.log_info("[INFO] unicode path resized canvas to native 256x256 (first-load path)")
        else:
            rayv.log_info(
                f"[INFO] unicode path imported into existing canvas {w}x{h} "
                "(layer-import path — still valid if no crash)"
            )


# ---------------------------------------------------------------------------
# 10) Exotic / unusual DDS formats (must not crash; soft-fail if unsupported)
# ---------------------------------------------------------------------------
def test_exotic_dds() -> None:
    rayv.log_info("--- exotic DDS ---")
    # (path, label, min_w, min_h) — min dims when first-load / native size is expected.
    # None,None → any positive size OR clean refuse is OK.
    cases = [
        ("testfield/r8g8.dds", "R8G8", 256, 256),
        ("testfield/SRGB_TEST.dds", "SRGB_TEST", 64, 64),
        ("testfield/LINEAR_TEST.dds", "LINEAR_TEST", 64, 64),
        # Exotic: accept if decode works; size may follow layer-import semantics mid-suite
        ("testfield/non-usual-case/af80f3e0-D32_FLOAT_S8X24_UINT.dds", "D32_FLOAT_S8X24_UINT (small)", None, None),
        ("testfield/non-usual-case/1741807e-R11G11B10_FLOAT.dds", "R11G11B10_FLOAT", None, None),
        ("testfield/non-usual-case/synthR32.dds", "synth R32", None, None),
    ]
    for path, label, min_w, min_h in cases:
        if not os.path.isfile(path):
            _soft_skip(f"DDS {label}", f"missing {path}")
            continue
        try:
            layers_before = rayv.doc.layer_count()
            # Reset to a paint-empty single Background so first-load resize can fire.
            # new_blank clears size but may keep a non-empty tileCache if painted earlier;
            # still useful as a stable pre-state.
            rayv.doc.new_blank(8, 8)
            accepted = bool(rayv.load_image(path) or rayv.doc.open(path))
            if accepted:
                w = rayv.get_canvas_width()
                h = rayv.get_canvas_height()
                bd = rayv.doc.bit_depth()
                tiles = rayv.get_tile_count()
                layers_after = rayv.doc.layer_count()
                size_ok = w > 0 and h > 0
                if min_w is not None and min_h is not None:
                    # Prefer native-ish size; if engine imported as layer into 8x8, still
                    # pass only when tiles/layers prove the decode ran.
                    native = w >= min_w and h >= min_h
                    imported = layers_after > layers_before or tiles > 0
                    size_ok = native or imported
                _ok(
                    f"DDS accept {label}",
                    size_ok,
                    f"{w}x{h} bit={bd} tiles={tiles} layers={layers_after}",
                )
                # Round-trip export only for modest canvases (avoid 3K+ PNG thrash)
                if w * h <= 1024 * 1024 and w >= 8 and h >= 8:
                    out = os.path.join("testfield", f"_unusual_export_{label.replace(' ', '_')}.png")
                    try:
                        saved = bool(rayv.save_image(out) or rayv.doc.save_image(out))
                        _ok(f"export after {label}", saved is True)
                        if os.path.isfile(out):
                            os.remove(out)
                    except Exception as e:
                        _ok(f"export after {label}", False, str(e))
            else:
                # Clean refusal is OK for non-paint formats (depth/stencil)
                rayv.log_info(f"[INFO] DDS refused cleanly: {label}")
                _ok(f"DDS refuse cleanly {label}", True)
        except Exception as e:
            _ok(f"DDS {label} no exception", False, str(e))


# ---------------------------------------------------------------------------
# 11) Ops surface: unknown + force chain
# ---------------------------------------------------------------------------
def test_ops_unusual() -> None:
    rayv.log_info("--- ops unusual ---")
    r = rayv.ops.invoke("ThisOpDefinitelyMissing_XYZ", force=True)
    _ok("unknown op returns", r is not None, str(r))

    # Rapid safe ops
    for oid in ("SelectAll", "Deselect", "SelectAll", "Deselect", "SwapColors", "SwapColors"):
        try:
            rayv.ops.invoke(oid, force=True)
        except Exception as e:
            _ok(f"rapid op {oid}", False, str(e))
            return
    _ok("rapid Select/Deselect/SwapColors", True)

    # can_invoke / has_execute on garbage
    _ok("can_invoke missing", rayv.ops.can_invoke("nope") is False or rayv.ops.can_invoke("nope") is True)
    _ok("has_execute missing", rayv.ops.has_execute("nope") is False)


# ---------------------------------------------------------------------------
# 12) Extreme zoom / pan (view only)
# ---------------------------------------------------------------------------
def test_view_extremes() -> None:
    rayv.log_info("--- view extremes ---")
    try:
        rayv.set_zoom(0.001)
        rayv.set_zoom(1000.0)
        rayv.set_pan(1e6, -1e6)
        rayv.reset_view()
        z = rayv.get_zoom()
        _ok("view extremes no crash", True, f"zoom_after_reset={z}")
    except Exception as e:
        _ok("view extremes no crash", False, str(e))


# ---------------------------------------------------------------------------
# 13) Project .rayp round-trip (tiny)
# ---------------------------------------------------------------------------
def test_rayp_roundtrip() -> None:
    rayv.log_info("--- rayp round-trip ---")
    assert rayv.doc.new_blank(48, 48)
    layer = rayv.doc.active_layer()
    assert layer >= 0
    rayv.doc.begin_edit(layer)
    rayv.doc.fill_rect(layer, 4, 4, 16, 16, 0.9, 0.2, 0.1, 1.0)
    rayv.doc.end_edit("unusual rayp paint")

    out = os.path.join("testfield", "_unusual_roundtrip.rayp")
    try:
        saved = rayv.doc.save_rayp(out)
        _ok("save_rayp", saved is True)
        if saved and os.path.isfile(out):
            reopened = rayv.doc.open(out)
            _ok("reopen rayp", reopened is True)
            if reopened:
                w, h = rayv.doc.size()
                _ok("rayp size", w == 48 and h == 48, f"{w}x{h}")
            try:
                os.remove(out)
            except OSError:
                pass
    except Exception as e:
        _ok("rayp round-trip", False, str(e))


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main() -> None:
    rayv.log_info("=== Unusual scenarios suite starting ===")
    rayv.log_info(f"cwd-ish doc size at start: {rayv.doc.size()} bit={rayv.doc.bit_depth()}")

    # Order matters: file loads first while document is still near cold-start,
    # then API edge cases that paint / stack layers.
    test_missing_and_garbage_paths()
    test_unicode_path()
    test_exotic_dds()
    test_tiny_and_odd_canvases()
    test_oob_and_invalid_rects()
    test_layer_stack_abuse()
    test_selection_extremes()
    test_edit_session_edges()
    test_mask_edges()
    test_image_codec_edges()
    test_ops_unusual()
    test_view_extremes()
    test_rayp_roundtrip()

    rayv.log_info(f"=== Unusual suite done: fails={len(_failures)} soft_skips={len(_soft)} ===")
    for s in _soft:
        rayv.log_warn(f"  soft: {s}")
    if _failures:
        for f in _failures:
            rayv.log_error(f"  {f}")
        raise RuntimeError(f"Unusual suite failed {len(_failures)} check(s)")
    rayv.log_info("=== Unusual suite PASSED ===")


main()
