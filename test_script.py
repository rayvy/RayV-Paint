import rayv

rayv.log_info("=== Python API Integration Test Starting ===")

# Test logging
rayv.log_debug("This is a debug message from Python.")
rayv.log_warn("This is a warning from Python.")

# Test config access
w = rayv.get_default_width()
h = rayv.get_default_height()
rayv.log_info(f"Config default dimensions: {w}x{h}")

# Test canvas resizing via Python
rayv.log_info("Resizing canvas via Python to 2048x1024...")
rayv.resize_canvas(2048, 1024)

# Adjust zoom & pan
rayv.set_zoom(2.5)
rayv.set_pan(100.0, -50.0)

rayv.log_info("=== Python API Integration Test Completed Successfully ===")
