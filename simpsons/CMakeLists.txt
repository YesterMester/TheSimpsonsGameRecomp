# simpsons - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(simpsons LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Pass-through instrumentation of the guest draw-submission functions (see
# src/draw_telemetry.cpp) is always compiled in and gated at RUNTIME by the
# `draw_telemetry` cvar, which defaults off. It used to be a build option, but
# that made a measurement session require a full rebuild of the 82k recompiled
# functions on the player's machine -- and worse, `option()` writes a CACHE
# entry, so flipping the default could not override the OFF already sitting in
# an existing build directory: the build would silently produce a
# telemetry-free binary that looked correct. With the hooks always present,
# turning the cvar off costs one relaxed atomic load and a branch per draw.
set(SIMPSONS_SOURCES
    src/main.cpp
    src/draw_telemetry.cpp
    src/frame_pacing.cpp
    src/ink_outlines.cpp
    src/ring_wait.cpp
    src/subtitles.cpp
)

if(WIN32)
    add_executable(simpsons WIN32 ${SIMPSONS_SOURCES})
else()
    add_executable(simpsons ${SIMPSONS_SOURCES})
endif()

rexglue_setup_target(simpsons)

# draw_telemetry.cpp calls fmt directly; the prebuilt SDK exposes fmt headers
# transitively, but REXSDK_DIR (add_subdirectory) builds -- what CI uses --
# need the explicit link for the target to see fmt's usage requirements.
if(TARGET fmt::fmt)
    target_link_libraries(simpsons PRIVATE fmt::fmt)
endif()

# The generated translation units are where nearly all guest CPU time goes,
# but CMake's stock RelWithDebInfo only compiles them at -O2, and local builds
# never got the -march=x86-64-v3 that CI already ships in every release
# (build.yml/release.yml pass it on the command line). Apply both directly on
# the target so every build gets them: target_compile_options land after the
# per-config flags on the compile line, so -O3 wins over the default -O2.
# x86-64-v3 (AVX2/BMI2/FMA, Haswell/Zen1+) matches what releases already
# require, so this adds no new hardware exposure. Releases also ship an
# x86-64-v2 build for older CPUs (release.yml sets SIMPSONS_X86_MARCH).
set(SIMPSONS_X86_MARCH "x86-64-v3" CACHE STRING
    "x86-64 microarchitecture level to compile the game for (x86-64-v3 or x86-64-v2)")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(simpsons PRIVATE
        $<$<CONFIG:RelWithDebInfo,Release>:-O3>
        -march=${SIMPSONS_X86_MARCH}
    )
endif()

# Windows POSIX shims for the few generated TUs that reference <sys/mman.h>
# (mprotect, neutered/no-op) and <unistd.h> (usleep). Scoped to this target.
if(WIN32)
    target_include_directories(simpsons PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat_win)
endif()
