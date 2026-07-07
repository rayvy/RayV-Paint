cmake_minimum_required(VERSION 3.20)
project(RayVPaint CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(NOMINMAX)

# Configure output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# FetchContent for dependencies
include(FetchContent)

# Fetch GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
set(GLFW_BUILD_DOCS OFF CACHE INTERNAL "")
set(GLFW_BUILD_TESTS OFF CACHE INTERNAL "")
set(GLFW_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(GLFW_INSTALL OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(glfw)

# Fetch Dear ImGui docking snapshot at a fixed commit for reproducible builds.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 4be08b1ecf7709f15e4274fb2ddac37e121d7d9a
)
FetchContent_MakeAvailable(imgui)

# Fetch nlohmann/json (Header-only)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Fetch STB (Header-only image loader/writer)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
)
FetchContent_MakeAvailable(stb)

# Build Dear ImGui static library
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)

# Set include directories for imgui
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# Define GLFW and DX11 definitions for ImGui backends
target_compile_definitions(imgui PUBLIC 
    IMGUI_IMPL_API=
)

# Link ImGui to GLFW to get its public headers (e.g. GLFW/glfw3.h)
target_link_libraries(imgui PUBLIC glfw)


# Source files for RayVPaint
set(SOURCES
    src/main.cpp
    src/Canvas.cpp
    src/CanvasLayers.cpp
    src/CanvasSelection.cpp
    src/CanvasPaint.cpp
    src/CanvasAdjustments.cpp
    src/CanvasTransforms.cpp
    src/CanvasFileIO.cpp
    src/CanvasUndo.cpp
    src/Canvas.h
    src/CanvasRendererDX12.cpp
    src/CanvasRendererDX12.h
    src/render/Dx12Device.cpp
    src/render/Dx12Device.h
    src/core/TileCache.cpp
    src/core/TileCache.h
    src/core/Logger.cpp
    src/core/Logger.h
    src/core/ThreadPool.cpp
    src/core/ThreadPool.h
    src/core/ConfigManager.cpp
    src/core/ConfigManager.h
    src/core/DdsHelper.cpp
    src/core/DdsHelper.h
    src/core/TexconvHelper.cpp
    src/core/TexconvHelper.h
    src/core/ImageManager.cpp
    src/core/ImageManager.h
    src/core/PaintEngine.cpp
    src/core/PaintEngine.h
    src/core/UndoRedoManager.cpp
    src/core/UndoRedoManager.h
    src/core/KeymapManager.cpp
    src/core/KeymapManager.h
    src/core/ClipboardHelper.cpp
    src/core/ClipboardHelper.h
    src/ui/EditorPanels.cpp
    src/ui/EditorPanels.h
)

# Shaders
set(SHADER_FILES
    src/shaders/Canvas.hlsl
    src/shaders/CanvasTiles.hlsl
)
set_source_files_properties(${SHADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)

# OpenCV integration
set(OPENCV_INSTALL_DIR "C:/Users/Rayvy/Documents/GitHub/opencv/install")
set(OPENCV_LIBS_DIR "${OPENCV_INSTALL_DIR}/x64/vc17/staticlib")

# Create Core executable (WIN32 target ensures it runs without console window by default)
add_executable(RayVPaint_Core WIN32 ${SOURCES} ${SHADER_FILES})
set_target_properties(RayVPaint_Core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/bin"
)

# Link dependencies to Core
target_link_libraries(RayVPaint_Core PRIVATE
    glfw
    imgui
    d3d12
    dxgi
    d3dcompiler
    nlohmann_json::nlohmann_json
    "${OPENCV_LIBS_DIR}/opencv_core4140.lib"
    "${OPENCV_LIBS_DIR}/opencv_imgproc4140.lib"
    "${OPENCV_LIBS_DIR}/zlib.lib"
)

# Set include directories for Core
target_include_directories(RayVPaint_Core PRIVATE
    src
    src/core
    ${stb_SOURCE_DIR}
    "${OPENCV_INSTALL_DIR}/include"
)

# Copy OpenCV LICENSE file to the output directory
add_custom_command(
    TARGET RayVPaint_Core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENCV_INSTALL_DIR}/LICENSE"
    "$<TARGET_FILE_DIR:RayVPaint_Core>/OPENCV_LICENSE"
    COMMENT "Copying OpenCV LICENSE to output directory"
)

# Create Launchers
add_executable(RayVPaint WIN32 src/launchers/RayVPaint_Launcher.cpp)
add_executable(RayVPaint_Console src/launchers/RayVPaint_Console_Launcher.cpp)
set_target_properties(RayVPaint RayVPaint_Console PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

# Ensure launchers depend on Core so it builds first
add_dependencies(RayVPaint RayVPaint_Core)
add_dependencies(RayVPaint_Console RayVPaint_Core)

# Custom command to copy shader files to the output directory
foreach(shader_file ${SHADER_FILES})
    get_filename_component(shader_name ${shader_file} NAME)
    add_custom_command(
        TARGET RayVPaint_Core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/${shader_file}
        $<TARGET_FILE_DIR:RayVPaint_Core>/shaders/${shader_name}
        COMMENT "Copying shader ${shader_name} to output directory"
    )
endforeach()

# ---------------------------------------------------------------------------
# Precompile HLSL shaders to .cso using fxc (Windows SDK)
# Falls back gracefully if fxc is not found — runtime D3DCompile will be used.
# ---------------------------------------------------------------------------
find_program(FXC_EXE fxc
    HINTS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/x64"
    DOC "FXC HLSL shader compiler"
)

if(FXC_EXE)
    message(STATUS "Found fxc: ${FXC_EXE}")

    set(CANVAS_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/Canvas.hlsl")
    set(TILES_HLSL  "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/CanvasTiles.hlsl")
    set(SHADER_INC  "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders")
    set(SHADER_OUT  "$<TARGET_FILE_DIR:RayVPaint_Core>/shaders")

    # Helper macro to add one fxc compilation step
    macro(add_fxc_target SRC ENTRY PROFILE CSO)
        add_custom_command(TARGET RayVPaint_Core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT}"
            COMMAND "${FXC_EXE}" /nologo /T "${PROFILE}" /E "${ENTRY}" /I "${SHADER_INC}"
                    /Fo "${SHADER_OUT}/${CSO}" "${SRC}"
            COMMENT "FXC: ${ENTRY} -> ${CSO}"
        )
    endmacro()

    add_fxc_target("${CANVAS_HLSL}" VSMain             vs_5_0 Canvas_VSMain.cso)
    add_fxc_target("${CANVAS_HLSL}" PSMain             ps_5_0 Canvas_PSMain.cso)
    add_fxc_target("${CANVAS_HLSL}" VSLayerMain        vs_5_0 Canvas_VSLayerMain.cso)
    add_fxc_target("${CANVAS_HLSL}" PSLayerBlend       ps_5_0 Canvas_PSLayerBlend.cso)
    add_fxc_target("${CANVAS_HLSL}" PSSelectionOutline ps_5_0 Canvas_PSSelectionOutline.cso)
    add_fxc_target("${TILES_HLSL}"  VSTileMain         vs_5_0 Canvas_VSTileMain.cso)
    add_fxc_target("${TILES_HLSL}"  PSTileBlend        ps_5_0 Canvas_PSTileBlend.cso)
else()
    message(WARNING "fxc not found — shaders will be compiled at runtime by D3DCompile.")
endif()
