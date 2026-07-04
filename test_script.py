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

# Native DDS Decompression & Export test
import os
dds_path = "testfield/SandroneBangsADiffuse.dds"
png_out = "testfield/temp_diffuse_export.png"
dds_out = "testfield/temp_diffuse_export.dds"

rayv.log_info(f"Testing native DDS load: {dds_path}")
if not rayv.load_image(dds_path):
    rayv.log_error("DDS native loading failed!")
    exit(1)

if not rayv.save_image(png_out):
    rayv.log_error("PNG saving failed!")
    exit(1)

# Test all new DDS formats (RGBA16, RGBA16F, RGBA32F, R8, R16F, R32F)
for fmt_idx in range(1, 7):
    dds_temp = f"testfield/temp_diffuse_export_{fmt_idx}.dds"
    rayv.log_info(f"Testing save_dds with format index {fmt_idx} to {dds_temp}")
    if not rayv.save_dds(dds_temp, fmt_idx):
        rayv.log_error(f"DDS saving with format index {fmt_idx} failed!")
        exit(1)
    if os.path.exists(dds_temp):
        os.remove(dds_temp)

# Test PNG saving with ICC profile
icc_test_path = "testfield/dummy_test.icc"
png_icc_out = "testfield/temp_icc_export.png"
with open(icc_test_path, "wb") as f:
    f.write(b"ICC_PROFILE_DUMMY_DATA_" * 10) # 230 bytes of dummy profile

rayv.log_info(f"Testing save_image with ICC profile: {icc_test_path}")
if not rayv.save_image(png_icc_out, icc_test_path):
    rayv.log_error("PNG saving with ICC profile failed!")
    exit(1)

# Verify the iCCP chunk exists in the output PNG file
with open(png_icc_out, "rb") as f:
    png_content = f.read()
    if b"iCCP" in png_content:
        rayv.log_info("Verified: 'iCCP' chunk is embedded in the exported PNG!")
    else:
        rayv.log_error("Error: 'iCCP' chunk not found in the exported PNG!")
        exit(1)

# Cleanup ICC test files
if os.path.exists(icc_test_path):
    os.remove(icc_test_path)
if os.path.exists(png_icc_out):
    os.remove(png_icc_out)

rayv.log_info("DDS native validation check and ICC profile test: PASSED.")

rayv.log_info("=== Python API Integration Test Completed Successfully ===")
