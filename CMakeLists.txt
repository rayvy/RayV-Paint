cmake_minimum_required(VERSION 3.20)
project(RayVPaint CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(NOMINMAX)

# Configure output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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
    src/core/Logger.cpp
    src/core/Logger.h
    src/core/ThreadPool.cpp
    src/core/ThreadPool.h
    src/core/ConfigManager.cpp
    src/core/ConfigManager.h
    src/core/DdsHelper.cpp
    src/core/DdsHelper.h
    src/core/ImageManager.cpp
    src/core/ImageManager.h
    src/core/PaintEngine.cpp
    src/core/PaintEngine.h
    src/scripting/ScriptingEngine.cpp
    src/scripting/ScriptingEngine.h
)

# Shaders
set(SHADER_FILES
    src/shaders/Canvas.hlsl
)
set_source_files_properties(${SHADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)

# Create executable (WIN32 target ensures it runs without console window by default)
add_executable(RayVPaint WIN32 ${SOURCES} ${SHADER_FILES})

# Link dependencies
target_link_libraries(RayVPaint PRIVATE
    glfw
    imgui
    d3d11
    dxgi
    d3dcompiler
    nlohmann_json::nlohmann_json
    pybind11::embed
)

# Set include directories
target_include_directories(RayVPaint PRIVATE
    src
    src/core
    src/scripting
    ${stb_SOURCE_DIR}
)

# Custom command to copy shader files to the output directory
foreach(shader_file ${SHADER_FILES})
    get_filename_component(shader_name ${shader_file} NAME)
    add_custom_command(
        TARGET RayVPaint POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/${shader_file}
        $<TARGET_FILE_DIR:RayVPaint>/shaders/${shader_name}
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
        # Extract the zip directly to the target bin directory
        add_custom_command(
            TARGET RayVPaint POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E tar xf "${PY_EMBED_DOWNLOAD_PATH}"
            WORKING_DIRECTORY $<TARGET_FILE_DIR:RayVPaint>
            COMMENT "Extracting Python embeddable runtime environment for portability"
        )
    else()
        # Fallback to copy the local DLL only if download failed
        get_filename_component(PYTHON_DIR "${Python3_EXECUTABLE}" DIRECTORY)
        set(PYTHON_DLL_NAME "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}.dll")
        set(PYTHON_DLL_PATH "${PYTHON_DIR}/${PYTHON_DLL_NAME}")
        if(EXISTS "${PYTHON_DLL_PATH}")
            add_custom_command(
                TARGET RayVPaint POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PYTHON_DLL_PATH}"
                "$<TARGET_FILE_DIR:RayVPaint>/${PYTHON_DLL_NAME}"
                COMMENT "Fallback: Copying local ${PYTHON_DLL_NAME} to output directory"
            )
        endif()
    endif()
endif()
