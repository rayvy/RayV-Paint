# Minimal DirectXTex static library for RayVPaint
# Sources: ../../DirectXTex-main/DirectXTex (library only, CPU + optional OpenMP)
# No Texconv, no GPU BC (DirectCompute), no D3D11/12 resource helpers.

cmake_minimum_required(VERSION 3.20)

set(DXTEX_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../DirectXTex-main/DirectXTex")
if(NOT EXISTS "${DXTEX_SRC_ROOT}/DirectXTex.h")
    message(FATAL_ERROR
        "DirectXTex sources not found at:\n  ${DXTEX_SRC_ROOT}\n"
        "Place upstream checkout at repo root as DirectXTex-main/ (see third_party/DirectXTex/README.md).")
endif()

add_library(DirectXTex STATIC
    "${DXTEX_SRC_ROOT}/BC.h"
    "${DXTEX_SRC_ROOT}/DDS.h"
    "${DXTEX_SRC_ROOT}/DirectXTex.h"
    "${DXTEX_SRC_ROOT}/DirectXTex.inl"
    "${DXTEX_SRC_ROOT}/DirectXTexP.h"
    "${DXTEX_SRC_ROOT}/filters.h"
    "${DXTEX_SRC_ROOT}/scoped.h"
    "${DXTEX_SRC_ROOT}/BC.cpp"
    "${DXTEX_SRC_ROOT}/BC4BC5.cpp"
    "${DXTEX_SRC_ROOT}/BC6HBC7.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexCompress.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexConvert.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexDDS.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexHDR.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexImage.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexMipmaps.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexMisc.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexNormalMaps.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexPMAlpha.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexResize.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexTGA.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexUtil.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexFlipRotate.cpp"
    "${DXTEX_SRC_ROOT}/DirectXTexWIC.cpp"
)

target_include_directories(DirectXTex PUBLIC
    $<BUILD_INTERFACE:${DXTEX_SRC_ROOT}>
)

target_compile_features(DirectXTex PUBLIC cxx_std_17)
target_compile_definitions(DirectXTex PRIVATE NOMINMAX)

# Match RayVPaint static CRT
set_property(TARGET DirectXTex PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    target_compile_options(DirectXTex PRIVATE /W3 /permissive- /Zc:__cplusplus)
endif()

# CPU BC6H/BC7 OpenMP (same as upstream BC_USE_OPENMP)
option(RAYV_DXTEX_OPENMP "Build DirectXTex BC compress with OpenMP" ON)
if(RAYV_DXTEX_OPENMP)
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(DirectXTex PUBLIC OpenMP::OpenMP_CXX)
        message(STATUS "DirectXTex: OpenMP ON for BC6H/BC7 software compress")
    else()
        message(STATUS "DirectXTex: OpenMP not found — BC compress single-threaded")
    endif()
endif()

# Windows Imaging Component used by WIC helpers (we compile WIC.cpp)
if(WIN32)
    target_link_libraries(DirectXTex PUBLIC ole32 windowscodecs)
endif()

message(STATUS "DirectXTex: library-only CPU build from ${DXTEX_SRC_ROOT}")
