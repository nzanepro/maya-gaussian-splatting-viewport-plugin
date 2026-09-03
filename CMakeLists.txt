cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# vcpkg integration (Windows only) — must be configured before project().
#
# If the VCPKG_ROOT environment variable is set, the toolchain and triplet are
# applied automatically.  You can still override them on the command line:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE="..." -DVCPKG_TARGET_TRIPLET="..."
#
# Default triplet: x64-windows-static-md
#   Static GLEW linked into the .mll — no glew32.dll needed at runtime.
#   Uses /MD (dynamic CRT) to match Maya's own runtime, avoiding CRT conflicts.
#
# macOS/Linux get GLEW from the system package manager instead (see below).
# ---------------------------------------------------------------------------
if(WIN32)
    if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain (auto-detected from VCPKG_ROOT)")
        message(STATUS "[vcpkg] Auto-detected at $ENV{VCPKG_ROOT}")
    endif()

    if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED VCPKG_TARGET_TRIPLET)
        set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING
            "vcpkg triplet — static lib + dynamic CRT (/MD) to match Maya")
    endif()
endif()

project(GaussianSplatPlugin)

# C++ standard — Maya 2024 ships C++17, Maya 2027 requires C++20.
# Override from command line: -DCMAKE_CXX_STANDARD=20
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (Makefiles/Ninja, the macOS + Linux default) leave
# CMAKE_BUILD_TYPE empty, which produces an unoptimised build with no warning.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Maya DevKit — required, must be set explicitly.
# Example: cmake -B build -DMAYA_DEVKIT="C:/Users/you/devkitBase2024"
#          cmake -B build -DMAYA_DEVKIT="$HOME/maya-devkit/2024"
# ---------------------------------------------------------------------------
set(MAYA_DEVKIT "" CACHE PATH "Maya DevKit root (headers + import libs)")

if(NOT MAYA_DEVKIT)
    message(FATAL_ERROR
        "MAYA_DEVKIT is not set.\n"
        "Run: cmake -B build -DMAYA_DEVKIT=\"<path/to/devkitBase>\"")
endif()

if(NOT EXISTS "${MAYA_DEVKIT}/include/maya/MObject.h")
    message(FATAL_ERROR
        "Maya DevKit not found at '${MAYA_DEVKIT}'.\n"
        "Check that the path is correct and the DevKit is fully extracted.")
endif()

# The DevKit archive keeps its libraries in lib/.  A Maya *installation* on
# macOS keeps the same dylibs inside the app bundle instead, so accept either
# and let MAYA_DEVKIT point at /Applications/Autodesk/maya20XX as a fallback.
set(MAYA_LIB_DIR "${MAYA_DEVKIT}/lib")
if(APPLE AND NOT EXISTS "${MAYA_LIB_DIR}/libOpenMaya.dylib"
         AND EXISTS "${MAYA_DEVKIT}/Maya.app/Contents/MacOS/libOpenMaya.dylib")
    set(MAYA_LIB_DIR "${MAYA_DEVKIT}/Maya.app/Contents/MacOS")
endif()
message(STATUS "[Maya] Headers:   ${MAYA_DEVKIT}/include")
message(STATUS "[Maya] Libraries: ${MAYA_LIB_DIR}")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
if(APPLE)
    # Homebrew is the usual source of GLEW on macOS, and it is not on CMake's
    # default search path when Homebrew lives in /opt/homebrew.  Ask brew.
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(COMMAND "${BREW_EXECUTABLE}" --prefix glew
                        OUTPUT_VARIABLE GLEW_BREW_PREFIX
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
        if(GLEW_BREW_PREFIX AND EXISTS "${GLEW_BREW_PREFIX}")
            list(APPEND CMAKE_PREFIX_PATH "${GLEW_BREW_PREFIX}")
            message(STATUS "[GLEW] Homebrew prefix: ${GLEW_BREW_PREFIX}")
        endif()
    endif()
endif()

if(WIN32)
    # vcpkg ships a GLEW package config; require it so the static triplet is used.
    find_package(GLEW CONFIG REQUIRED)
else()
    find_package(GLEW REQUIRED)
endif()

if(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SOURCES "src/*.cpp")
list(APPEND SOURCES "third_party/tinyply/tinyply.cpp")

# Maya loads macOS plug-ins as .bundle MH_BUNDLE images, which is what a CMake
# MODULE library produces; SHARED would emit a dylib Maya refuses to load.
if(APPLE)
    add_library(GaussianSplatPlugin MODULE ${SOURCES})
else()
    add_library(GaussianSplatPlugin SHARED ${SOURCES})
endif()

# ---------------------------------------------------------------------------
# Include / link
# ---------------------------------------------------------------------------
target_include_directories(GaussianSplatPlugin PRIVATE
    "${MAYA_DEVKIT}/include"
    third_party/tinyply
    src
)

target_link_directories(GaussianSplatPlugin PRIVATE
    "${MAYA_LIB_DIR}"
)

target_link_libraries(GaussianSplatPlugin PRIVATE
    GLEW::GLEW
    OpenMaya
    OpenMayaUI
    OpenMayaRender
    Foundation
)

if(APPLE)
    target_link_libraries(GaussianSplatPlugin PRIVATE "${OPENGL_FRAMEWORK}")
endif()

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
if(WIN32)
    set(MAYA_PLUGIN_SUFFIX ".mll")
elseif(APPLE)
    set(MAYA_PLUGIN_SUFFIX ".bundle")
else()
    set(MAYA_PLUGIN_SUFFIX ".so")
endif()

set_target_properties(GaussianSplatPlugin PROPERTIES
    SUFFIX "${MAYA_PLUGIN_SUFFIX}"
    PREFIX ""
    BUNDLE OFF          # a flat .bundle file, not a macOS bundle directory
)

target_compile_definitions(GaussianSplatPlugin PRIVATE
    REQUIRE_IOSTREAM
    _USE_MATH_DEFINES
    $<$<BOOL:${WIN32}>:NT_PLUGIN>
    $<$<BOOL:${APPLE}>:MAC_PLUGIN>
    $<$<BOOL:${APPLE}>:OSMac_>
    # macOS ships OpenGL as deprecated-but-functional; silence the wall of
    # deprecation warnings from <OpenGL/gl.h>, which GLEW pulls in.
    $<$<BOOL:${APPLE}>:GL_SILENCE_DEPRECATION>
)

# GLEW_STATIC must be defined when linking against the static GLEW library.
if(VCPKG_TARGET_TRIPLET MATCHES "static")
    target_compile_definitions(GaussianSplatPlugin PRIVATE GLEW_STATIC)
endif()

# ---------------------------------------------------------------------------
# Post-build: copy shaders alongside the plug-in binary
# ---------------------------------------------------------------------------
add_custom_command(TARGET GaussianSplatPlugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/shaders"
        "$<TARGET_FILE_DIR:GaussianSplatPlugin>/shaders"
    COMMENT "Copying shaders to output directory"
)
