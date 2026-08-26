
# eternalsonata - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
# C is here only for the embedded shader blobs: Plume's shader helper emits each
# compiled blob as a .c byte array, and CMake needs a C compiler to build them.
project(eternalsonata LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Plume, the RHI the native renderer is built on (see src/native_renderer.h).
# It is a low level interface over D3D12, Vulkan and Metal that does no shader
# compilation of its own, which is what makes it the right fit here: this
# project compiles the guest's shaders offline (scripts/xenos_hlsl.py) rather
# than translating microcode at runtime.
#
# Plume is tracked as a submodule, including its own dependencies (volk,
# Vulkan-Headers, VMA and D3D12MemoryAllocator). Keeping the renderer revision
# in the superproject makes local and CI builds use the same source.
set(ETERNALSONATA_PLUME_DIR "${CMAKE_SOURCE_DIR}/plume")
if(NOT EXISTS "${ETERNALSONATA_PLUME_DIR}/plume_render_interface.h")
    message(FATAL_ERROR
        "Plume submodule is missing. Run: git submodule update --init --recursive")
endif()
message(STATUS "eternalsonata - using Plume submodule at ${ETERNALSONATA_PLUME_DIR}")
# ReXGlue owns this project's SDL window. Have Plume create its Vulkan surface
# from that SDL_Window directly instead of asking the host to expose raw X11
# handles, which are not represented by Window::GetNativeWindowHandle().
if(UNIX AND NOT APPLE)
    set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "Enable SDL Vulkan integration" FORCE)
endif()
add_subdirectory("${ETERNALSONATA_PLUME_DIR}" "${CMAKE_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

# Plume's D3D12 backend returns D3D12_HEAP_TYPE_GPU_UPLOAD from its heap type
# mapping without guarding it, so it only builds against a Windows SDK new
# enough to declare it (10.0.26100+) or with the Agility SDK enabled, which
# needs vcpkg. Rather than take on either, supply the one missing enumerator --
# but only after checking it is actually missing, so this disappears by itself
# on a newer SDK instead of quietly shadowing the real declaration.
#
# 5 is the value the Agility SDK assigns it. Nothing here requests a GPU upload
# heap, so this only has to compile; if that changes it needs the real header,
# because the runtime support behind it is a driver feature and not a constant.
if(WIN32)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles(
        "#include <d3d12.h>\nint main() { return D3D12_HEAP_TYPE_GPU_UPLOAD; }"
        ETERNALSONATA_HAVE_D3D12_GPU_UPLOAD_HEAP)
    unset(CMAKE_REQUIRED_QUIET)
    if(NOT ETERNALSONATA_HAVE_D3D12_GPU_UPLOAD_HEAP)
        message(STATUS "eternalsonata - Windows SDK predates D3D12_HEAP_TYPE_GPU_UPLOAD, shimming it for Plume")
        target_compile_definitions(plume PRIVATE
            "D3D12_HEAP_TYPE_GPU_UPLOAD=((D3D12_HEAP_TYPE)5)")
    endif()
endif()

# The D3D12 debug layer, opt in with -DETERNALSONATA_D3D12_DEBUG_LAYER=ON.
#
# Plume only enables it when NDEBUG is off, which this project's default build
# is not, so without this there is no way to find out *why* the driver rejected
# a pipeline or a resource -- and a rejected pipeline state object is a null
# pointer that crashes at bind time rather than an error at creation. Break on
# error is off so the messages are printed rather than trapping the debugger.
option(ETERNALSONATA_D3D12_DEBUG_LAYER "Enable Plume's D3D12 debug layer" OFF)
if(WIN32 AND ETERNALSONATA_D3D12_DEBUG_LAYER)
    target_compile_definitions(plume PRIVATE
        D3D12_DEBUG_LAYER_ENABLED
        D3D12_DEBUG_LAYER_BREAK_ON_ERROR=false
        D3D12_DEBUG_LAYER_BREAK_ON_WARNING=false
        D3D12_DEBUG_LAYER_SUPRESS_SAMPLE_POSITIONS_ERROR)
endif()

# Embedded window icon: extracted directly from assets/default.xex (already a
# required codegen input) so no extra asset needs to be checked in or fetched.
# src/icon.generated.h is a gitignored build artifact regenerated whenever the
# XEX or the generator script changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(ETERNALSONATA_ICON_HEADER ${CMAKE_SOURCE_DIR}/src/icon.generated.h)
set(ETERNALSONATA_ICON_ICO ${CMAKE_SOURCE_DIR}/src/icon.generated.ico)
add_custom_command(
    OUTPUT ${ETERNALSONATA_ICON_HEADER} ${ETERNALSONATA_ICON_ICO}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
        ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Generating src/icon.generated.h and src/icon.generated.ico"
    VERBATIM
)

# On Windows, also embed the same icon as the .exe's own resource (Explorer,
# taskbar before the window opens, Alt-Tab), separate from the in-window icon
# applied at runtime via Window::SetIcon (see eternalsonata_app.h). The .rc
# needs an absolute path to the .ico since it's not necessarily emitted next
# to it in an out-of-source build.
if(WIN32)
    set(ETERNALSONATA_ICON_ICO_PATH ${ETERNALSONATA_ICON_ICO})
    set(ETERNALSONATA_ICON_RC ${CMAKE_SOURCE_DIR}/src/eternalsonata.generated.rc)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/eternalsonata.rc.in
        ${ETERNALSONATA_ICON_RC}
        @ONLY
    )
    # rc.exe needs icon.generated.ico to exist by the time it compiles this
    # .rc; configure_file only stamped the path into it above, it doesn't
    # establish a build-order dependency on the custom command that (re)writes
    # the .ico itself.
    set_source_files_properties(${ETERNALSONATA_ICON_RC} PROPERTIES OBJECT_DEPENDS ${ETERNALSONATA_ICON_ICO})
endif()

# Embedded overlay font: src/fonts/*.ttf are the checked-in source of truth;
# src/fonts.generated.h is a gitignored build artifact regenerated whenever a
# .ttf or the generator script changes.
set(ETERNALSONATA_FONTS_HEADER ${CMAKE_SOURCE_DIR}/src/fonts.generated.h)
add_custom_command(
    OUTPUT ${ETERNALSONATA_FONTS_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-fonts.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-fonts.py
        ${CMAKE_SOURCE_DIR}/src/fonts/PTSerif-Regular.ttf
    COMMENT "Generating src/fonts.generated.h"
    VERBATIM
)

# Embedded id -> display-name table for the Discord Rich Presence, generated
# from docs/cfdata_names.txt (itself generated by scripts/cfdata_names.py).
set(ETERNALSONATA_AREA_NAMES_HEADER ${CMAKE_SOURCE_DIR}/src/area_names.generated.h)
add_custom_command(
    OUTPUT ${ETERNALSONATA_AREA_NAMES_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-area-names.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-area-names.py
        ${CMAKE_SOURCE_DIR}/docs/cfdata_names.txt
    COMMENT "Generating src/area_names.generated.h"
    VERBATIM
)

# Plume's shader helper, used both for the overlay shaders below and for the
# DXC it fetches -- the Windows SDK's DXC has no SPIR-V backend, so it cannot
# produce the Vulkan half. Initialised here rather than next to the overlay
# shaders because the guest shader pack, generated further down, needs
# PLUME_DXC_EXECUTABLE to exist by then.
include(${ETERNALSONATA_PLUME_DIR}/examples/cmake/PlumeShaders.cmake)
plume_shaders_init()

# The guest's own shaders, translated and compiled ahead of time.
#
# The game's whole shader inventory is a static blob inside default.xex (see
# scripts/extract_shaders.py), so this is a finite offline job rather than a
# runtime translator: extract the 260 containers, emit HLSL from the Xenos
# microcode (scripts/xenos_hlsl.py), compile each with DXC, and pack the results
# into guest_shaders.bin.
#
# The pack is deployed next to the exe rather than embedded as a byte array.
# The compiled blobs come to a few megabytes per format, and turning that into
# C++ array initialisers would cost more build time on every rebuild than
# reading the file back costs once at startup.
set(ETERNALSONATA_GUEST_SHADER_DIR ${CMAKE_BINARY_DIR}/guest_shaders)
set(ETERNALSONATA_GUEST_SHADER_PACK ${CMAKE_BINARY_DIR}/guest_shaders.bin)

# The sidecar the F2 shader debugger reads: per shader, the microcode
# disassembly and the emitted HLSL. Separate from the pack above because it is
# the larger of the two and the renderer never reads it; only the overlay does,
# a span at a time. See src/native_renderer_shader_debug.h.
set(ETERNALSONATA_GUEST_SHADER_DEBUG_PACK
    ${CMAKE_BINARY_DIR}/guest_shaders_debug.bin)

# DXIL is Windows-only; SPIR-V is built everywhere, since Plume reaches Vulkan
# through volk on every platform it supports.
if(WIN32)
    set(ETERNALSONATA_SHADER_FORMATS "dxil,spirv")
else()
    set(ETERNALSONATA_SHADER_FORMATS "spirv")
endif()

if(PLUME_DXC_LIB_DIR)
    set(ETERNALSONATA_DXC_LIB_ARG --dxc-lib-dir ${PLUME_DXC_LIB_DIR})
else()
    set(ETERNALSONATA_DXC_LIB_ARG "")
endif()

add_custom_command(
    OUTPUT ${ETERNALSONATA_GUEST_SHADER_PACK} ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-guest-shaders.py
        --xex ${CMAKE_SOURCE_DIR}/assets/default.xex
        --out ${ETERNALSONATA_GUEST_SHADER_DIR}
        --pack ${ETERNALSONATA_GUEST_SHADER_PACK}
        --debug-pack ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK}
        --dxc ${PLUME_DXC_EXECUTABLE}
        ${ETERNALSONATA_DXC_LIB_ARG}
        --formats ${ETERNALSONATA_SHADER_FORMATS}
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-guest-shaders.py
        ${CMAKE_SOURCE_DIR}/scripts/extract_shaders.py
        ${CMAKE_SOURCE_DIR}/scripts/xenos_hlsl.py
        ${CMAKE_SOURCE_DIR}/scripts/xenos_ucode.py
        ${CMAKE_SOURCE_DIR}/scripts/xex_image.py
        ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Translating and compiling the guest's 260 shaders"
    VERBATIM
)
add_custom_target(eternalsonata_guest_shaders
    DEPENDS ${ETERNALSONATA_GUEST_SHADER_PACK}
            ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK})

# Sources
set(ETERNALSONATA_SOURCES
    src/main.cpp
    src/eternalsonata_hooks.cpp
    src/eternalsonata_framerate.cpp
    src/eternalsonata_options.cpp
    src/eternalsonata_presence.cpp
    src/room_presence.cpp
    src/party_system.cpp
    src/battle_system.cpp
    src/guest_main_thread.cpp
    src/host_timer_resolution.cpp
    src/force_load_area.cpp
    src/field_player_model_override.cpp
    src/debug_area_overlay.cpp
    src/dlc_auto_install.cpp
    src/guest_shaders.cpp
    src/native_renderer.cpp
    src/native_renderer_d3d.cpp
    src/native_renderer_draw.cpp
    src/native_renderer_frame.cpp
    src/native_renderer_pipeline.cpp
    src/native_renderer_plume.cpp
    src/native_renderer_overlay.cpp
    src/native_renderer_shader_debug.cpp
    src/native_renderer_state.cpp
    src/native_renderer_texture.cpp
    src/settings.cpp
    ${ETERNALSONATA_AREA_NAMES_HEADER}
    ${GENERATED_SOURCES}
    ${ETERNALSONATA_ICON_HEADER}
    ${ETERNALSONATA_FONTS_HEADER}
)

if(WIN32)
    add_executable(eternalsonata WIN32 ${ETERNALSONATA_SOURCES} ${ETERNALSONATA_ICON_RC})
else()
    add_executable(eternalsonata ${ETERNALSONATA_SOURCES})
    set_target_properties(eternalsonata PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

rexglue_setup_target(eternalsonata)

target_link_libraries(eternalsonata PRIVATE plume)

if(WIN32)
    # timeBeginPeriod / timeGetDevCaps, for src/host_timer_resolution.cpp.
    target_link_libraries(eternalsonata PRIVATE winmm)

    # Plume's CMake does not link the D3D12 import library itself, leaving
    # D3D12CreateDevice and D3D12SerializeRootSignature to the consumer.
    target_link_libraries(eternalsonata PRIVATE d3d12)
endif()

# The guest shader pack has to exist before the exe runs, and lands next to it.
add_dependencies(eternalsonata eternalsonata_guest_shaders)
add_custom_command(TARGET eternalsonata POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ETERNALSONATA_GUEST_SHADER_PACK} $<TARGET_FILE_DIR:eternalsonata>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK} $<TARGET_FILE_DIR:eternalsonata>
    COMMENT "Deploying guest_shaders.bin"
    VERBATIM
)

# Overlay shaders. Plume compiles nothing at runtime, so these are built ahead
# of time to DXIL and SPIR-V and embedded as byte arrays -- small enough that
# embedding is the simpler answer, unlike the guest's own 260 above. Plume's
# helper (initialised near the top) is reused rather than reimplemented.
plume_compile_vertex_shader(eternalsonata
    "${CMAKE_SOURCE_DIR}/src/shaders/imgui.vert.hlsl" "imguiVert" "VSMain")
plume_compile_pixel_shader(eternalsonata
    "${CMAKE_SOURCE_DIR}/src/shaders/imgui.frag.hlsl" "imguiFrag" "PSMain")
