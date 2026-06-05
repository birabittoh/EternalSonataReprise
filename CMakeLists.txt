
# eternalsonata - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(eternalsonata LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(ETERNALSONATA_SOURCES
    src/main.cpp
    src/eternalsonata_hooks.cpp
    ${GENERATED_SOURCES}
)

if(WIN32)
    add_executable(eternalsonata WIN32 ${ETERNALSONATA_SOURCES})
else()
    add_executable(eternalsonata ${ETERNALSONATA_SOURCES})
    set_target_properties(eternalsonata PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

rexglue_setup_target(eternalsonata)
