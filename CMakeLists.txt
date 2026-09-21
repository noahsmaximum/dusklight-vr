cmake_minimum_required(VERSION 3.26)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(dusklight_vr VERSION 0.1.0 LANGUAGES C CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Dusklight VR currently supports Windows (D3D12 + OpenXR) only")
endif ()

# The Dusklight version to build against: a release tag (e.g. "v2.0.0") or a commit SHA.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.0")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# OpenXR loader, linked statically into the mod so the .dusk is self-contained. The runtime
# (SteamVR, Virtual Desktop, Meta, ...) is still discovered through the registry at run time.
include(FetchContent)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(OpenXR
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.60
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(OpenXR)

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

add_mod(dusklight_vr
        FEATURES game fmt webgpu
        SOURCES
        src/mod.cpp
        src/vr_config.cpp
        src/render_hooks.cpp
        src/xr_runtime.cpp
        src/dawn_d3d12.cpp
        src/d3d12_vtbl.c
        src/iat_hook.cpp
        src/gpu.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
        RES_DIR res
)
target_link_libraries(dusklight_vr PRIVATE OpenXR::openxr_loader d3d12 dxgi)
target_compile_definitions(dusklight_vr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
