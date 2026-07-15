# RayVHelpers — isolated micro-apps (PNG→DDS + atlas). No paint-core linkage.

cmake_minimum_required(VERSION 3.20)

# Dear ImGui (Win32 + DX11 only — no GLFW)
set(IMGUI_HELPERS_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

add_library(imgui_helpers STATIC ${IMGUI_HELPERS_SOURCES})
target_include_directories(imgui_helpers PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui_helpers PUBLIC IMGUI_IMPL_API=)
set_property(TARGET imgui_helpers PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(RAYV_HELPERS_SOURCES
    main.cpp
    common/SettingsStore.cpp
    common/BatchCollector.cpp
    shell/HelperShellRegister.cpp
    convert/TexconvRunner.cpp
    atlas/PngIo.cpp
    atlas/AtlasPacker.cpp
    ui/Dx11ImGuiApp.cpp
    ui/ConverterUi.cpp
    ui/AtlasUi.cpp
    ui/HubUi.cpp
    resources.rc
)

add_executable(RayVHelpers WIN32 ${RAYV_HELPERS_SOURCES})
set_target_properties(RayVHelpers PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/bin"
    OUTPUT_NAME "RayVHelpers"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(RayVHelpers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(RayVHelpers PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(RayVHelpers PRIVATE
    imgui_helpers
    d3d11
    dxgi
    windowscodecs
    ole32
    shell32
    shlwapi
    comdlg32
    advapi32
    uuid
)

# Ensure helpers build with the main product
if(TARGET RayVPaint_Core)
    add_dependencies(RayVPaint_Core RayVHelpers)
endif()

message(STATUS "RayVHelpers: micro-apps (convert + atlas) enabled")
