# Smoke: operator surface (Phase O4)
# Run: RayVPaint_Core.exe --script test_ops_smoke.py  (or project CLI equivalent)
import rayv

rayv.log_info("=== rayv.ops smoke ===")

ops = rayv.ops.list()
rayv.log_info(f"catalog actions: {len(ops)}")
with_exec = [o for o in ops if o.get("has_execute")]
rayv.log_info(f"with execute: {len(with_exec)}")

# Sample known ids
for oid in ("SelectAll", "Deselect", "Undo", "FillSecondary", "AdjustNoise", "SwapColors"):
    can = rayv.ops.can_invoke(oid)
    has = rayv.ops.has_execute(oid)
    rayv.log_info(f"  {oid}: has_execute={has} can_invoke={can}")

# Safe invokes (force=True for headless / pre-frame context)
r1 = rayv.ops.invoke("SelectAll", force=True)
rayv.log_info(f"SelectAll -> {r1}")
r2 = rayv.ops.invoke("Deselect", force=True)
rayv.log_info(f"Deselect -> {r2}")

# Unknown op
r3 = rayv.ops.invoke("ThisDoesNotExist", force=True)
rayv.log_info(f"missing -> {r3}")

rayv.log_info("=== rayv.ops smoke done ===")
