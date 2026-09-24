
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
# Android does not need this — Plume has native VK_KHR_android_surface support.
# Apple must not get this either: plume_render_interface_types.h and
# plume_vulkan.cpp both check PLUME_SDL_VULKAN_ENABLED in the same #elif chain
# as __APPLE__, before it. Defining it here would swap RenderWindow from
# Plume's native Apple {window, view} pair (fed to vkCreateMetalSurfaceEXT via
# a CAMetalLayer) to a bare SDL_Window*, which Plume's Apple surface-creation
# code does not accept — see native_renderer_plume.cpp for what actually needs
# to build that {window, view} pair from the SDL window (e.g. SDL_Metal_CreateView).
if(UNIX AND NOT APPLE AND NOT ANDROID)
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
        ${CMAKE_SOURCE_DIR}/src/fonts/Roboto-Medium.ttf
    COMMENT "Generating src/fonts.generated.h"
    VERBATIM
)

# Embedded UI art: src/images/*.png are the checked-in source of truth;
# src/images.generated.h is a gitignored build artifact regenerated whenever a
# .png or the generator script changes.
set(ETERNALSONATA_IMAGES_HEADER ${CMAKE_SOURCE_DIR}/src/images.generated.h)
# Globbed so that adding a .png cannot leave a stale header behind: naming them
# one by one means a new image silently never rebuilds it.
file(GLOB ETERNALSONATA_IMAGE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/src/images/*.png)
add_custom_command(
    OUTPUT ${ETERNALSONATA_IMAGES_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-images.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-images.py
        ${ETERNALSONATA_IMAGE_SOURCES}
    COMMENT "Generating src/images.generated.h"
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
# The pack is linked into the exe: gen-guest-shaders.py also writes a `.S` that
# `.incbin`s the pack's bytes into read-only data (see write_pack_asm there), so
# there is no loose file to deploy, stage into an APK, or go missing on a user's
# machine. `.incbin` is assembled rather than compiled, which is what keeps this
# cheap despite the pack being a few megabytes per format; a C++ array
# initialiser of the same bytes would cost far more build time.
set(ETERNALSONATA_GUEST_SHADER_DIR ${CMAKE_BINARY_DIR}/guest_shaders)
set(ETERNALSONATA_GUEST_SHADER_PACK ${CMAKE_BINARY_DIR}/guest_shaders.bin)
set(ETERNALSONATA_GUEST_SHADER_PACK_ASM ${CMAKE_BINARY_DIR}/guest_shaders.bin.S)

# `.S` files need the ASM language enabled; pin its compiler to the same clang
# used for CXX; and since clang always uses its own integrated assembler for
# `.S`, `-Wno-ignored-attributes`-style CXX-only flags are not injected here.
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER ${CMAKE_CXX_COMPILER})
endif()
enable_language(ASM)

# The sidecar the F2 shader debugger reads: per shader, the microcode
# disassembly and the emitted HLSL. Separate from the pack above because it is
# the larger of the two and only the overlay reads it, not the renderer. Linked
# the same way as the main pack; see src/gpu/native_renderer_shader_debug.cpp.
set(ETERNALSONATA_GUEST_SHADER_DEBUG_PACK
    ${CMAKE_BINARY_DIR}/guest_shaders_debug.bin)
set(ETERNALSONATA_GUEST_SHADER_DEBUG_PACK_ASM
    ${CMAKE_BINARY_DIR}/guest_shaders_debug.bin.S)

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
    OUTPUT ${ETERNALSONATA_GUEST_SHADER_PACK} ${ETERNALSONATA_GUEST_SHADER_PACK_ASM}
           ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK} ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK_ASM}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-guest-shaders.py
        --xex ${CMAKE_SOURCE_DIR}/assets/default.xex
        --out ${ETERNALSONATA_GUEST_SHADER_DIR}
        --pack ${ETERNALSONATA_GUEST_SHADER_PACK}
        --pack-asm ${ETERNALSONATA_GUEST_SHADER_PACK_ASM}
        --debug-pack ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK}
        --debug-pack-asm ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK_ASM}
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
    DEPENDS ${ETERNALSONATA_GUEST_SHADER_PACK} ${ETERNALSONATA_GUEST_SHADER_PACK_ASM}
            ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK} ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK_ASM})

# The patches that let a USA copy play (scripts/gen-usa-patches.py): default.xex
# for the SDK's game data selector, the layout-sensitive containers for the
# asset system. Generating them takes both releases' full data, so the bundle is
# prebuilt and fetched like default.xex; without it USA copies are refused.
set(ETERNALSONATA_USA_PATCHES ${CMAKE_SOURCE_DIR}/assets/usa-patches.bin CACHE FILEPATH
    "Prebuilt USA to PAL patch bundle")
if(NOT EXISTS ${ETERNALSONATA_USA_PATCHES})
    set(ETERNALSONATA_USA_PATCHES ${CMAKE_BINARY_DIR}/usa-patches.empty)
    file(TOUCH ${ETERNALSONATA_USA_PATCHES})
endif()
set(ETERNALSONATA_USA_PATCHES_ASM ${CMAKE_BINARY_DIR}/usa-patches.S)
file(CONFIGURE OUTPUT ${ETERNALSONATA_USA_PATCHES_ASM} CONTENT [=[
#if defined(__APPLE__)
#define SYM(name) _##name
.section __TEXT,__const
#elif defined(_WIN32)
#define SYM(name) name
.section .rdata,"dr"
#else
#define SYM(name) name
.section .rodata
#endif

.p2align 4
.global SYM(kUsaPatchData)
SYM(kUsaPatchData):
.incbin "@ETERNALSONATA_USA_PATCHES@"
.global SYM(kUsaPatchDataEnd)
SYM(kUsaPatchDataEnd):
]=] @ONLY)
set_source_files_properties(${ETERNALSONATA_USA_PATCHES_ASM}
    PROPERTIES OBJECT_DEPENDS ${ETERNALSONATA_USA_PATCHES})

# Sources
set(ETERNALSONATA_SOURCES
    src/core/main.cpp
    src/engine/eternalsonata_hooks.cpp
    src/engine/eternalsonata_framerate.cpp
    src/engine/eternalsonata_lipsync.cpp
    src/engine/aim_input.cpp
    src/engine/eternalsonata_voice.cpp
    src/engine/eternalsonata_options.cpp
    src/engine/eternalsonata_presence.cpp
    src/engine/room_presence.cpp
    src/engine/party_system.cpp
    src/engine/battle_system.cpp
    src/engine/overworld_system.cpp
    src/engine/cutscene_system.cpp
    src/engine/enemy_system.cpp
    src/engine/save_system.cpp
    src/engine/achievement_translation.cpp
    src/assets/eternalsonata_asset_container.cpp
    src/assets/eternalsonata_asset_mesh.cpp
    src/assets/eternalsonata_asset_texture.cpp
    src/assets/eternalsonata_asset_system.cpp
    src/engine/photo_system.cpp
    src/engine/piano_music_system.cpp
    src/engine/music_system.cpp
    src/engine/option_strip.cpp
    src/engine/achievements_menu.cpp
    src/engine/achievement_system.cpp
    src/engine/item_system.cpp
    src/engine/equipment_system.cpp
    src/core/game_settings.cpp
    src/core/loading_screen.cpp
    src/engine/flag_system.cpp
    src/core/guest_main_thread.cpp
    src/core/guest_profiler.cpp
    src/core/host_timer_resolution.cpp
    src/engine/force_load_area.cpp
    src/engine/field_player_model_override.cpp
    src/platform/host_menu.cpp
    src/platform/input_overlay.cpp
    src/engine/dlc_auto_install.cpp
    src/gpu/guest_shaders.cpp
    src/gpu/native_renderer.cpp
    src/gpu/native_renderer_batch.cpp
    src/gpu/native_renderer_d3d.cpp
    src/gpu/native_renderer_draw.cpp
    src/gpu/native_renderer_frame.cpp
    src/gpu/native_renderer_pipeline.cpp
    src/gpu/native_renderer_readback.cpp
    src/gpu/native_renderer_plume.cpp
    src/gpu/native_renderer_overlay.cpp
    src/gpu/native_renderer_shader_debug.cpp
    src/gpu/native_renderer_state.cpp
    src/gpu/native_renderer_texture.cpp
    src/core/settings.cpp
    ${ETERNALSONATA_GUEST_SHADER_PACK_ASM}
    ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK_ASM}
    ${ETERNALSONATA_USA_PATCHES_ASM}
    ${ETERNALSONATA_AREA_NAMES_HEADER}
    ${ETERNALSONATA_ICON_HEADER}
    ${ETERNALSONATA_FONTS_HEADER}
    ${ETERNALSONATA_IMAGES_HEADER}
)

if(WIN32)
    add_executable(eternalsonata WIN32 ${ETERNALSONATA_SOURCES} ${ETERNALSONATA_ICON_RC})
elseif(ANDROID)
    # Android: build as a shared library so NativeActivity / SDL can dlopen it.
    # Gradle's externalNativeBuild (or a manual jniLibs copy) packages the
    # resulting libeternalsonata.so into the APK.
    add_library(eternalsonata SHARED ${ETERNALSONATA_SOURCES})
    # Everything linked into that .so has to be position independent, and
    # rexglue_setup_target() below puts the recompiled guest code in an OBJECT
    # library, which does not inherit PIC from the target consuming it. Without
    # this, ld.lld rejects the generated objects (R_AARCH64_ADR_PREL_PG_HI21
    # against rex::ppc::VectorMaskL). Must be set before rexglue_setup_target()
    # creates that library.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # Plume sets CMAKE_CXX_VISIBILITY_PRESET to hidden globally, which hides
    # SDL_main from the dynamic symbol table. SDLActivity needs to dlsym it,
    # so restore default visibility for this target.
    set_target_properties(eternalsonata PROPERTIES
        CXX_VISIBILITY_PRESET default
        C_VISIBILITY_PRESET default
    )
else()
    add_executable(eternalsonata ${ETERNALSONATA_SOURCES})
    # ELF executables do not auto-export their own symbols, so without this the
    # REX_MOD_PLUGIN_EXPORT mod API is invisible to mods' dlsym(RTLD_DEFAULT).
    set_target_properties(eternalsonata PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
        ENABLE_EXPORTS ON
    )
endif()

rexglue_setup_target(eternalsonata GPU_PLUGINS xenos)

target_link_libraries(eternalsonata PRIVATE plume)

# Release version the self-updater compares GitHub tags against: the CI tag
# (ES_VERSION_NAME, also what the Android Gradle build stamps), else the
# newest reachable v* tag, else empty, which leaves the updater off.
set(ETERNALSONATA_VERSION "$ENV{ES_VERSION_NAME}" CACHE STRING "Version embedded for the self-updater (vX.Y.Z or X.Y.Z)")
if(ETERNALSONATA_VERSION STREQUAL "")
    execute_process(
        COMMAND git describe --tags --abbrev=0 --match "v*"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE ETERNALSONATA_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
string(REGEX REPLACE "^v" "" ETERNALSONATA_VERSION "${ETERNALSONATA_VERSION}")
target_compile_definitions(eternalsonata PRIVATE ETERNALSONATA_VERSION="${ETERNALSONATA_VERSION}")

# src/ is split into core/engine/gpu/platform/assets/api subdirectories, but
# translation units still #include each other's headers by flat filename
# (e.g. "settings.h") rather than a path relative to their own subdirectory.
# Adding every subdirectory here keeps that working without rewriting every
# include site across the codebase.
target_include_directories(eternalsonata PRIVATE
    src/core
    src/engine
    src/gpu
    src/platform
    src/assets
    src/api
)

if(WIN32)
    # timeBeginPeriod / timeGetDevCaps, for src/host_timer_resolution.cpp.
    target_link_libraries(eternalsonata PRIVATE winmm)

    # SymFromAddr, for the guest profiler's symbolisation of sampled stacks.
    # The unwinding itself is Rtl* and needs nothing. See src/guest_profiler.h.
    target_link_libraries(eternalsonata PRIVATE dbghelp)

    # Plume's CMake does not link the D3D12 import library itself, leaving
    # D3D12CreateDevice and D3D12SerializeRootSignature to the consumer.
    target_link_libraries(eternalsonata PRIVATE d3d12)
endif()

# Both the guest shader pack and its F2 debugger sidecar are linked in via the
# *_ASM sources above.
add_dependencies(eternalsonata eternalsonata_guest_shaders)
# The .S stubs only name the packs, so their text does not change when the
# pack does; the assembled objects have to depend on the packs themselves.
set_source_files_properties(${ETERNALSONATA_GUEST_SHADER_PACK_ASM}
    PROPERTIES OBJECT_DEPENDS ${ETERNALSONATA_GUEST_SHADER_PACK})
set_source_files_properties(${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK_ASM}
    PROPERTIES OBJECT_DEPENDS ${ETERNALSONATA_GUEST_SHADER_DEBUG_PACK})

# Overlay shaders. Plume compiles nothing at runtime, so these are built ahead
# of time to DXIL and SPIR-V and embedded as byte arrays -- small enough that
# embedding is the simpler answer, unlike the guest's own 260 above. Plume's
# helper (initialised near the top) is reused rather than reimplemented.
plume_compile_vertex_shader(eternalsonata
    "${CMAKE_SOURCE_DIR}/src/shaders/imgui.vert.hlsl" "imguiVert" "VSMain")
plume_compile_pixel_shader(eternalsonata
    "${CMAKE_SOURCE_DIR}/src/shaders/imgui.frag.hlsl" "imguiFrag" "PSMain")

# The present blit, which scales the guest's image into whatever size the window
# is. Same reasoning as the overlay's: too small to be worth a pack.
plume_compile_vertex_shader(eternalsonata
    "${CMAKE_SOURCE_DIR}/src/shaders/blit.vert.hlsl" "blitVert" "VSMain")
plume_compile_pixel_shader(eternalsonata
    "${CMAKE_SOURCE_DIR}/src/shaders/blit.frag.hlsl" "blitFrag" "PSMain")

# The depth resolve, which cannot be a copy now that depth carries stencil.
plume_compile_vertex_shader(eternalsonata
    "${CMAKE_SOURCE_DIR}/src/shaders/depth_resolve.vert.hlsl" "depthResolveVert" "VSMain")
plume_compile_pixel_shader(eternalsonata
    "${CMAKE_SOURCE_DIR}/src/shaders/depth_resolve.frag.hlsl" "depthResolveFrag" "PSMain")
