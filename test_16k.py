"""
Large-texture open test for RayVPaint.
Target: testfield/16Ktest.dds (16384x16384 BC7).

Exit behavior: raise Exception on failure so the host process returns non-zero
via ScriptingEngine::RunScript (do not call sys.exit).
"""
import os
import rayv

DDS = "testfield/16Ktest.dds"
EXPECT_W = 16384
EXPECT_H = 16384

rayv.log_info("=== 16K open test starting ===")
rayv.log_info(f"Working set before load: {rayv.get_memory_mb():.1f} MiB")

if not os.path.exists(DDS):
    raise RuntimeError(f"Missing test asset: {DDS}")

rayv.log_info(f"Loading {DDS} ...")
ok = rayv.load_image(DDS)
if not ok:
    raise RuntimeError("load_image returned False for 16K DDS")

w = rayv.get_canvas_width()
h = rayv.get_canvas_height()
tiles = rayv.get_tile_count()
mem = rayv.get_memory_mb()

rayv.log_info(f"Canvas after load: {w}x{h}, tiles={tiles}, WS={mem:.1f} MiB")

if w != EXPECT_W or h != EXPECT_H:
    raise RuntimeError(f"Unexpected size {w}x{h}, expected {EXPECT_W}x{EXPECT_H}")

if tiles <= 0:
    raise RuntimeError("Tile count is 0 after dense 16K load — TileCache empty")

# Dense 16K RGBA8 at 256² tiles => 64x64 = 4096 tiles if fully allocated.
# Accept a lower bound in case of sparse edge rounding.
min_tiles = 1000
if tiles < min_tiles:
    raise RuntimeError(f"Tile count suspiciously low ({tiles} < {min_tiles})")

rayv.log_info("=== 16K open test PASSED ===")
