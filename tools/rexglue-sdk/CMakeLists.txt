# ReXGlue SDK - Xbox 360 Recompilation Toolkit
# Modern C++23 cross-platform SDK for recompiling Xbox 360 executables
cmake_minimum_required(VERSION 3.25)

project(ReXGlue
    VERSION 0.8.0
    LANGUAGES C CXX
    DESCRIPTION "Xbox 360 Recompilation SDK"
)
# Floor only: bump MAJOR.MINOR for API changes. Patch is derived from git tags
# in cmake/rex_version.cmake. See docs/RELEASING.md.

#==========================================================
# Build Options
#==========================================================
option(REXGLUE_BUILD_TESTS       "Build PPC instruction and unit tests" OFF)
option(REXGLUE_ENABLE_SANITIZERS "Enable UndefinedBehaviorSanitizer" OFF)
option(REXGLUE_ENABLE_FIDELITYFX "Enable optional AMD FidelityFX integration (experimental)" OFF)
option(REXGLUE_ENABLE_TRACY                "Enable Tracy profiler integration" ON)
option(REXGLUE_ENABLE_PERF_COUNTERS        "Enable lightweight perf counter registry and overlay" ON)
# Opt-in: the ~82k mechanically-translated guest functions call each other
# constantly across ~90 separate translation units, so ordinary per-TU
# compilation can never inline any of it. ThinLTO lets the compiler see and
# optimize across that whole call graph at once -- the single biggest lever
# available for this code shape. Off by default: needs lld (clang's bundled
# toolchain has it; a bare apt/distro clang install may not), and meaningfully
# lengthens build+link time. Verified the weak/noinline hand-patch-override
# alias mechanism (used by simpsons/generated/default's DEFINE_REX_FUNC)
# survives ThinLTO correctly in an isolated repro before adding this.
option(REXGLUE_ENABLE_LTO "Enable ThinLTO (needs lld)" OFF)
# The default (small) x86-64 code model covers 2 GB of code and data, far more
# than recompiled executables need (The Simpsons Game is under 60 MB). The
# large model turns every call into an indirect call through a 64-bit GOT
# offset, which recompiled code pays on every guest call.
option(REXGLUE_LARGE_CODE_MODEL "Compile x86-64 code with -mcmodel=large" OFF)
# Propagate profiling defines globally (compiled out in Release)
if(REXGLUE_ENABLE_TRACY)
    add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()
if(REXGLUE_ENABLE_PERF_COUNTERS)
    add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

# Graphics backend selection
# D3D12: Windows only, ON by default.
# Vulkan: Cross-platform, ON by default everywhere - on Windows it is the
# primary backend (the fragment shader interlock render path plays the VP6
# FMVs and matches console colors; D3D12's host render target path does
# not), with D3D12 kept as the runtime fallback and the gpu="d3d12" choice.
if(WIN32)
    option(REXGLUE_USE_D3D11 "Build the development Direct3D 11 backend" OFF)
    option(REXGLUE_USE_D3D12  "Enable D3D12 graphics backend" ON)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" ON)
else()
    set(REXGLUE_USE_D3D11 OFF)
    set(REXGLUE_USE_D3D12 OFF)
    option(REXGLUE_USE_VULKAN "Enable Vulkan graphics backend" ON)
endif()

if(NOT REXGLUE_USE_D3D11 AND NOT REXGLUE_USE_D3D12 AND NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "At least one graphics backend must be enabled (REXGLUE_USE_D3D11, REXGLUE_USE_D3D12 or REXGLUE_USE_VULKAN)")
endif()

if (REXGLUE_ENABLE_FIDELITYFX AND
    NOT (REXGLUE_USE_VULKAN OR REXGLUE_USE_D3D12))
    message(FATAL_ERROR
        "REXGLUE_ENABLE_FIDELITYFX requires at least one graphics backend")
endif()

# Root of the SDK source tree
set(REXGLUE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# SDK install support
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(UNIX AND NOT APPLE)
    # Keep installed binaries self-contained by resolving shared libs relative
    # to the install prefix (for example, bin -> ../lib).
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    list(APPEND CMAKE_INSTALL_RPATH
        "$ORIGIN"
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()

# Enforce Clang18 compiler
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "ReXGlue requires Clang compiler. Please set CMAKE_CXX_COMPILER to clang++")
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
    message(FATAL_ERROR "ReXGlue requires Clang 18 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

# Enforce x64 architecture
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ReXGlue only supports 64‑bit architectures (x86_64 or ARM64)")
endif()

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RELWITHDEBINFO_POSTFIX rd)

if(NOT WIN32)
    # Use appropriate code model based on architecture
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        if(REXGLUE_LARGE_CODE_MODEL)
            add_compile_options(-mcmodel=large)
        endif()
        # Baseline SIMD level for the whole SDK: rexcore's byte-swap helpers use
        # _mm_shuffle_epi8 (SSSE3) unconditionally, and rexglue_helpers.cmake
        # already assumes -msse4.1 for app/module targets -- make it global so
        # internal library targets (rexcore etc.) compile with the same baseline.
        add_compile_options(-msse4.1)
    else()
        # Small code model for other architectures (ARM64, etc.)
        add_compile_options(-mcmodel=small)
    endif()
    # Position-independent code for all targets
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Global compile options
add_compile_options(
    -fno-strict-aliasing
    -ffp-model=strict
    $<$<CONFIG:DEBUG>:-g>
    $<$<CONFIG:DEBUG>:-O0>
    $<$<CONFIG:RELEASE>:-O3>
    $<$<CONFIG:RELEASE>:-DNDEBUG>
    # RelWithDebInfo is what's actually shipped/played (see build.yml/release.yml
    # presets) -- CMake's own default for it is only -O2. Bump it to -O3 like
    # Release; -g stays on for crash diagnostics, the two aren't in conflict.
    $<$<CONFIG:RELWITHDEBINFO>:-O3>
    -fno-char8_t
)

if(REXGLUE_ENABLE_LTO)
    if(NOT WIN32)
        add_compile_options(-flto=thin)
        add_link_options(-flto=thin -fuse-ld=lld)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
           CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        # On Windows, clang's GNU-style driver already links with lld-link,
        # and CMake archives with llvm-ar, which indexes bitcode members.
        add_compile_options(-flto=thin)
        add_link_options(-flto=thin)
    else()
        message(WARNING "REXGLUE_ENABLE_LTO: needs clang's GNU-style driver on Windows, ignoring")
    endif()
endif()

# Platform detection (supports AMD64 and ARM64)
if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "win-amd64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
        set(REX_PLATFORM "win-arm64")
    else()
        message(FATAL_ERROR "Unsupported Windows architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_WINDOWS=1)
elseif(UNIX AND NOT APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        set(REX_PLATFORM "linux-amd64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
        set(REX_PLATFORM "linux-arm64")
    else()
        set(REX_PLATFORM "linux-${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    add_compile_definitions(REX_PLATFORM_LINUX=1)
else()
    message(FATAL_ERROR "Unsupported platform. ReXGlue supports Windows and Linux only.")
endif()

# Build config as compile definition (multi-config generators don't know config at configure time)
add_compile_definitions(REXGLUE_BUILD_CONFIG="$<CONFIG>")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rex_version.cmake)
rex_resolve_version(REXGLUE_FULL_VERSION
    FLOOR_MAJOR ${PROJECT_VERSION_MAJOR}
    FLOOR_MINOR ${PROJECT_VERSION_MINOR})

# Strip the -id trailer (if any) for the numeric-only version.
string(REGEX REPLACE "-.*$" "" REXGLUE_NUMERIC_VERSION "${REXGLUE_FULL_VERSION}")

# Parse up to 4 numeric components for consumers that want integer access.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?" _ "${REXGLUE_NUMERIC_VERSION}")
set(REXGLUE_DERIVED_MAJOR "${CMAKE_MATCH_1}")
set(REXGLUE_DERIVED_MINOR "${CMAKE_MATCH_2}")
if(CMAKE_MATCH_4)
    set(REXGLUE_DERIVED_PATCH "${CMAKE_MATCH_4}")
else()
    set(REXGLUE_DERIVED_PATCH "0")
endif()
if(CMAKE_MATCH_6)
    set(REXGLUE_DERIVED_TWEAK "${CMAKE_MATCH_6}")
else()
    set(REXGLUE_DERIVED_TWEAK "0")
endif()

# Generate version header -- must run after REX_PLATFORM is set
# Timestamp is set at configure time -- intentionally not updated on every rebuild
string(TIMESTAMP REXGLUE_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/rex/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/rex/version.h
    @ONLY
)

# Output directories (multi-config appends /<Config>/ automatically)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REXGLUE_ROOT}/out/${REX_PLATFORM}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${REXGLUE_ROOT}/out/${REX_PLATFORM}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${REXGLUE_ROOT}/out/${REX_PLATFORM}")

# Sanitizer support for debugging memory corruption and undefined behavior
if(REXGLUE_ENABLE_SANITIZERS)
    message(STATUS "Enabling UndefinedBehaviorSanitizer (UBSan only - ASan conflicts with custom memory mapping)")
    # Note: ASan doesn't work well with the custom mmap at 0x100000000, so we only use UBSan
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

if(REXGLUE_BUILD_TESTS)
    list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/cmake")
    list(APPEND CMAKE_MODULE_PATH "${REXGLUE_ROOT}/thirdparty/catch2/extras")
endif()

# Third-party dependencies (vendored sources only)
add_subdirectory(thirdparty)

# Enable warnings after thirdparty
add_compile_options(
    -Wall
    -Wextra
)

include_directories(${REXGLUE_ROOT}/include)

add_subdirectory(src/core)
add_subdirectory(src/filesystem)
add_subdirectory(src/ui)
add_subdirectory(src/input)
add_subdirectory(src/audio)
add_subdirectory(src/graphics)
add_subdirectory(src/system)
add_subdirectory(src/kernel)
add_subdirectory(src/codegen)
add_subdirectory(src/rexglue)

# Add tests subdirectory if tests are enabled
if(REXGLUE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Print configuration summary
message(STATUS "===========================================")
message(STATUS "ReXGlue Configuration Summary:")
message(STATUS "  Version: ${REXGLUE_FULL_VERSION}")
message(STATUS "  Platform: ${REX_PLATFORM}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Configurations: ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Output Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Graphics: D3D11=${REXGLUE_USE_D3D11} D3D12=${REXGLUE_USE_D3D12} Vulkan=${REXGLUE_USE_VULKAN}")
message(STATUS "  FidelityFX: ${REXGLUE_ENABLE_FIDELITYFX}")
message(STATUS "  Tracy: ${REXGLUE_ENABLE_TRACY}")
message(STATUS "  Perf Counters: ${REXGLUE_ENABLE_PERF_COUNTERS}")
message(STATUS "===========================================")

# SDK install/export rules (skipped when consumed as a subdirectory)
if(PROJECT_IS_TOP_LEVEL)
    include(cmake/rexglue_devenv.cmake)
    include(cmake/rexglue_install.cmake)
else()
    # When consumed via add_subdirectory(), provide the same interface as find_package().
    # This sets REXGLUE_SHARE_DIR and makes rexglue_configure_target() available.
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui" PARENT_SCOPE)
    set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rexglue_helpers.cmake)
endif()
