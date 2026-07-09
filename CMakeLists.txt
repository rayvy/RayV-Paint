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

# Fetch Dear ImGui (docking branch is required for editor docking features)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# Fetch nlohmann/json (Header-only)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Fetch pybind11 (For Python embedding)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.12.0
)
FetchContent_MakeAvailable(pybind11)

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
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
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
    src/Canvas.h
    src/core/TileCache.cpp
    src/core/TileCache.h
    src/core/Logger.cpp
    src/core/Logger.h
    src/core/MemoryStats.cpp
    src/core/MemoryStats.h
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
    src/scripting/ScriptingEngine.cpp
    src/scripting/ScriptingEngine.h
    src/ui/EditorPanels.cpp
    src/ui/EditorPanels.h
)

# Shaders
set(SHADER_FILES
    src/shaders/Canvas.hlsl
)
set_source_files_properties(${SHADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)

# OpenCV integration
set(OPENCV_INSTALL_DIR "C:/Users/Rayvy/Documents/GitHub/opencv/install")
set(OPENCV_LIBS_DIR "${OPENCV_INSTALL_DIR}/x64/vc17/staticlib")

# Create Core executable (WIN32 target ensures it runs without console window by default)
add_executable(RayVPaint_Core WIN32 ${SOURCES} ${SHADER_FILES} src/resources/resources.rc)
set_target_properties(RayVPaint_Core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/bin"
)

# Link dependencies to Core
target_link_libraries(RayVPaint_Core PRIVATE
    glfw
    imgui
    d3d11
    dxgi
    d3dcompiler
    nlohmann_json::nlohmann_json
    pybind11::embed
    "${OPENCV_LIBS_DIR}/opencv_core4140.lib"
    "${OPENCV_LIBS_DIR}/opencv_imgproc4140.lib"
    "${OPENCV_LIBS_DIR}/zlib.lib"
)

# Set include directories for Core
target_include_directories(RayVPaint_Core PRIVATE
    src
    src/core
    src/scripting
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
add_executable(RayVPaint WIN32 src/launchers/RayVPaint_Launcher.cpp src/resources/resources.rc)
add_executable(RayVPaint_Console src/launchers/RayVPaint_Console_Launcher.cpp src/resources/resources.rc)
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

# Locate and copy Python DLL and ZIP for portability on Windows
if(WIN32)
    find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
    
    # We want to download the embeddable package matching the found Python version
    set(PY_VER "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}.${Python3_VERSION_PATCH}")
    set(PY_EMBED_ZIP_NAME "python-${PY_VER}-embed-amd64.zip")
    set(PY_EMBED_URL "https://www.python.org/ftp/python/${PY_VER}/${PY_EMBED_ZIP_NAME}")
    set(PY_EMBED_DOWNLOAD_PATH "${CMAKE_BINARY_DIR}/${PY_EMBED_ZIP_NAME}")
    
    message(STATUS "Checking for Python embeddable package: ${PY_EMBED_ZIP_NAME}")
    
    if(NOT EXISTS "${PY_EMBED_DOWNLOAD_PATH}")
        message(STATUS "Downloading Python embeddable package from ${PY_EMBED_URL}...")
        file(DOWNLOAD "${PY_EMBED_URL}" "${PY_EMBED_DOWNLOAD_PATH}" SHOW_PROGRESS STATUS download_status)
        list(GET download_status 0 download_code)
        if(download_code EQUAL 0)
            message(STATUS "Python embeddable package downloaded successfully.")
        else()
            message(WARNING "Failed to download Python embeddable package. Status: ${download_status}")
        endif()
    endif()

    if(EXISTS "${PY_EMBED_DOWNLOAD_PATH}")
        # Extract and organize Python runtime directory
        add_custom_command(
            TARGET RayVPaint_Core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E tar xf "${PY_EMBED_DOWNLOAD_PATH}"
            WORKING_DIRECTORY $<TARGET_FILE_DIR:RayVPaint_Core>
            
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:RayVPaint_Core>/python_runtime
            
            COMMAND powershell -NoProfile -Command "Move-Item -Path *.pyd, *.zip -Destination python_runtime -Force -ErrorAction SilentlyContinue; Move-Item -Path libcrypto-3.dll, libffi-8.dll, libssl-3.dll, sqlite3.dll, vcruntime140.dll, vcruntime140_1.dll -Destination python_runtime -Force -ErrorAction SilentlyContinue; Set-Content -Path python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}._pth -Value @('python_runtime/python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}.zip', 'python_runtime', '.')"
            WORKING_DIRECTORY $<TARGET_FILE_DIR:RayVPaint_Core>
            
            COMMENT "Organizing Python embeddable runtime environment into python_runtime subdirectory"
        )
    else()
        # Fallback to copy the local DLL only if download failed
        get_filename_component(PYTHON_DIR "${Python3_EXECUTABLE}" DIRECTORY)
        set(PYTHON_DLL_NAME "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}.dll")
        set(PYTHON_DLL_PATH "${PYTHON_DIR}/${PYTHON_DLL_NAME}")
        if(EXISTS "${PYTHON_DLL_PATH}")
            add_custom_command(
                TARGET RayVPaint_Core POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PYTHON_DLL_PATH}"
                "$<TARGET_FILE_DIR:RayVPaint_Core>/${PYTHON_DLL_NAME}"
                COMMENT "Fallback: Copying local ${PYTHON_DLL_NAME} to output directory"
            )
        endif()
    endif()
endif()
