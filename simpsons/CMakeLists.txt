# simpsons - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(simpsons LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

if(NOT ANDROID)
    option(SIMPSONS_MODKIT_TOOLS "Build the standalone desktop asset tools" ON)
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../modkit" "${CMAKE_CURRENT_BINARY_DIR}/modkit")

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
    src/modkit_game.cpp
    src/modkit_input.cpp
    src/modkit_text.cpp
    src/modkit_models.cpp
    src/modkit_models_hook.cpp
    src/modkit_meshes.cpp
    src/modkit_filesystem.cpp
    src/native_gpu_calls.cpp
    src/native_gpu_constants.cpp
    src/native_gpu_vertices.cpp
    src/native_gpu_state.cpp
    src/achievements.cpp
    src/achievements_screen.cpp
    src/achievements_ui.cpp
    src/audio.cpp
    src/camera_input.cpp
    src/characters.cpp
    src/crt_native.cpp
    src/draw_telemetry.cpp
    src/eye_shading.cpp
    src/entity_transform.cpp
    src/entity_health.cpp
    src/entity_render.cpp
    src/entity_assets.cpp
    src/entity_assets_hook.cpp
    src/frame_pacing.cpp
    src/freecam.cpp
    src/freeroam_coop.cpp
    src/game_clock.cpp
    src/game_menu.cpp
    src/game_menu_apt.cpp
    src/game_menu_pointer.cpp
    src/game_prompts.cpp
    src/cutscene_skip.cpp
    src/game_canvas.cpp
    src/game_thread.cpp
    src/game_ui.cpp
    src/gpu_preference.cpp
    src/hint_text.cpp
    src/host_memory.cpp
    src/ink_outlines.cpp
    src/debug_menu.cpp
    src/overlay.cpp
    src/pause_menu.cpp
    src/players.cpp
    src/native_ragdoll_joint.cpp
    src/native_ragdoll_body.cpp
    src/native_ragdoll_pose.cpp
    src/native_ragdoll_rig.cpp
    src/native_ragdoll_driver.cpp
    src/native_ragdoll_preview.cpp
    src/native_ragdoll_world.cpp
    src/refpack_native.cpp
    src/saves.cpp
    src/saves_screen.cpp
    src/spawner.cpp
    src/spawn_sources.cpp
    src/snapshot.cpp
    src/physics_step.cpp
    src/physics_keyframes.cpp
    src/ring_wait.cpp
    src/subtitles.cpp
    src/tick_count.cpp
    src/widescreen.cpp
)

if(ANDROID)
    list(APPEND SIMPSONS_SOURCES src/android.cpp)
    add_library(simpsons SHARED ${SIMPSONS_SOURCES})
    set_target_properties(simpsons PROPERTIES OUTPUT_NAME main DEBUG_POSTFIX ""
        RELWITHDEBINFO_POSTFIX "")
elseif(WIN32)
    add_executable(simpsons WIN32 ${SIMPSONS_SOURCES})
else()
    add_executable(simpsons ${SIMPSONS_SOURCES})
endif()

rexglue_setup_target(simpsons)
# Keep the public SDK headers on handwritten adapters. Propagating their include
# paths to every recompiled function would rebuild the whole game when the kit
# changes, even though generated code never uses it.
target_link_libraries(simpsons PRIVATE "$<LINK_ONLY:simpsons_modkit>")
set(SIMPSONS_MODKIT_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../modkit/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../modkit/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/rexglue-sdk/thirdparty/inja/third_party/include")
set_property(SOURCE src/main.cpp src/modkit_game.cpp src/modkit_input.cpp src/modkit_filesystem.cpp src/freecam.cpp
    src/game_menu.cpp src/game_ui.cpp src/game_thread.cpp src/players.cpp src/spawner.cpp src/spawn_sources.cpp src/entity_assets.cpp src/saves.cpp
    src/physics_step.cpp src/saves_screen.cpp src/debug_menu.cpp src/modkit_text.cpp
    src/modkit_models.cpp src/modkit_models_hook.cpp src/modkit_meshes.cpp src/entity_assets_hook.cpp
    src/refpack_native.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
    ${SIMPSONS_MODKIT_INCLUDE_DIRS})
set(modkit_catalog_file "${CMAKE_CURRENT_SOURCE_DIR}/../modkit/api.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${modkit_catalog_file}")
file(READ "${modkit_catalog_file}" modkit_catalog)
string(JSON modkit_catalog SET "${modkit_catalog}" sdk "\"0.1.0\"")
string(JSON modkit_catalog SET "${modkit_catalog}" features
    "[\"lua\",\"native-plugins\",\"file-mods\",\"native-menu-pages\",\"editor-viewport-1\"]")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/simpsons-modkit.json" CONTENT "${modkit_catalog}\n")

# Fonts for the achievements screen and mod menu (overlay.cpp), embedded so
# every platform has them: the launcher's Luckiest Guy and the SDK's Roboto,
# both under the Apache License 2.0.
set(SIMPSONS_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded")
function(simpsons_embed symbol path)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" hex HEX)
    string(LENGTH "${hex}" length)
    math(EXPR size "${length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE "${SIMPSONS_EMBED_DIR}/${symbol}.h.new"
        "#pragma once\n// Generated from ${path}\n"
        "inline constexpr unsigned char k${symbol}[${size}] = {${bytes}};\n")
    file(COPY_FILE "${SIMPSONS_EMBED_DIR}/${symbol}.h.new" "${SIMPSONS_EMBED_DIR}/${symbol}.h"
        ONLY_IF_DIFFERENT)
endfunction()
simpsons_embed(LuckiestGuyRegular "${CMAKE_CURRENT_SOURCE_DIR}/../launcher/ui/fonts/LuckiestGuy-Regular.ttf")
simpsons_embed(RobotoRegular
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/rexglue-sdk/thirdparty/tracy/profiler/src/font/Roboto-Regular.ttf")
simpsons_embed(RobotoBold
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/rexglue-sdk/thirdparty/tracy/profiler/src/font/Roboto-Bold.ttf")
target_include_directories(simpsons PRIVATE "${SIMPSONS_EMBED_DIR}")

# draw_telemetry.cpp calls fmt directly; the prebuilt SDK exposes fmt headers
# transitively, but REXSDK_DIR (add_subdirectory) builds -- what CI uses --
# need the explicit link for the target to see fmt's usage requirements.
if(TARGET fmt::fmt)
    target_link_libraries(simpsons PRIVATE fmt::fmt)
endif()

# Exact saves compress game memory with snappy and hash it with xxHash (header
# only through rex/hash.h), which the SDK bundles; system copies aren't
# there on Android.
if(TARGET xxhash)
    target_include_directories(simpsons PRIVATE
        $<TARGET_PROPERTY:xxhash,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET snappy)
    target_link_libraries(simpsons PRIVATE snappy)
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

# Recompiled functions keep the guest condition register fields, CTR, XER and
# the reservation in locals instead of the shared context, so the compiler can
# hold them in host registers (register_locals.py). The compiled units are
# rewritten copies of the generated ones, which keep their hand patches.
option(SIMPSONS_REGISTER_LOCALS "Keep guest condition registers, CTR and XER in function locals" ON)
find_package(Python3 COMPONENTS Interpreter QUIET)
set(recomp_units)
foreach(source ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES})
    get_filename_component(name "${source}" NAME)
    if(name MATCHES "^simpsons_recomp\\.[0-9]+\\.cpp$")
        list(APPEND recomp_units "${source}")
    endif()
endforeach()
set(recomp_compiled_units ${recomp_units})
if(SIMPSONS_REGISTER_LOCALS AND Python3_Interpreter_FOUND)
    set(locals_dir "${CMAKE_CURRENT_BINARY_DIR}/register_locals")
    set(recomp_compiled_units)
    foreach(source ${recomp_units})
        get_filename_component(name "${source}" NAME)
        list(APPEND recomp_compiled_units "${locals_dir}/${name}")
    endforeach()
    set_source_files_properties(${recomp_units} PROPERTIES HEADER_FILE_ONLY TRUE)
    add_custom_command(
        OUTPUT "${locals_dir}/stamp"
        BYPRODUCTS ${recomp_compiled_units}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/register_locals.py"
            --out "${locals_dir}" --stamp "${locals_dir}/stamp" ${recomp_units}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/register_locals.py" ${recomp_units}
        COMMENT "Moving guest condition registers, CTR and XER into function locals"
        VERBATIM)
    add_custom_target(simpsons_register_locals DEPENDS "${locals_dir}/stamp")
    add_dependencies(simpsons simpsons_register_locals)
    target_sources(simpsons PRIVATE ${recomp_compiled_units})
endif()

# Calls between recompiled functions go straight to their implementations
# unless handwritten code may replace the function, so the compiler can inline
# them (direct_calls.py). Without this every guest call is a real call through
# a weak alias.
option(SIMPSONS_DIRECT_GUEST_CALLS "Let the compiler inline calls between recompiled functions" ON)
if(SIMPSONS_DIRECT_GUEST_CALLS AND Python3_Interpreter_FOUND
   AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    set(direct_dir "${CMAKE_CURRENT_BINARY_DIR}/direct_calls")
    set(direct_units ${recomp_units})
    set(direct_headers)
    foreach(source compiled IN ZIP_LISTS recomp_units recomp_compiled_units)
        get_filename_component(name "${source}" NAME)
        string(REGEX REPLACE "\\.cpp$" ".h" header "${name}")
        list(APPEND direct_headers "${direct_dir}/${header}")
        set_source_files_properties("${compiled}" PROPERTIES
            COMPILE_OPTIONS "-include;${direct_dir}/${header}"
            OBJECT_DEPENDS "${direct_dir}/${header}")
    endforeach()
    set(direct_scan "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../modkit")
    file(GLOB_RECURSE direct_scanned CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*" "${CMAKE_CURRENT_SOURCE_DIR}/../modkit/*.c*"
        "${CMAKE_CURRENT_SOURCE_DIR}/../modkit/*.h*")
    set(direct_args)
    foreach(dir ${direct_scan})
        list(APPEND direct_args --scan "${dir}")
    endforeach()
    add_custom_command(
        OUTPUT "${direct_dir}/stamp"
        BYPRODUCTS ${direct_headers}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/direct_calls.py"
            --out "${direct_dir}" --stamp "${direct_dir}/stamp" ${direct_args} ${direct_units}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/direct_calls.py" ${direct_units} ${direct_scanned}
        COMMENT "Finding the recompiled functions no handwritten code replaces"
        VERBATIM)
    add_custom_target(simpsons_direct_calls DEPENDS "${direct_dir}/stamp")
    add_dependencies(simpsons simpsons_direct_calls)
endif()

# Windows POSIX shims for the few generated TUs that reference <sys/mman.h>
# (mprotect, neutered/no-op) and <unistd.h> (usleep). Scoped to this target.
if(WIN32)
    target_include_directories(simpsons PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat_win)
endif()

# The Android launcher's pictures (android_art.cpp, LauncherArt.java): frames
# from the game's VP6 cutscenes, as the desktop launcher takes them with
# ffmpeg. The SDK's FFmpeg only builds what the audio needs, so FFmpeg's VP6
# decoder and its DSP code are compiled here with a config that turns them on
# (plain C: the launcher decodes a few frames), into a small library of its own
# for the launcher's process.
if(ANDROID AND TARGET libavcodec AND DEFINED REXSDK_DIR)
    enable_language(C)
    set(SIMPSONS_FFMPEG_DIR "${REXSDK_DIR}/thirdparty/FFmpeg")
    set(SIMPSONS_ART_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/art-ffmpeg-config")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${SIMPSONS_FFMPEG_DIR}/config_android_aarch64.h")
    file(READ "${SIMPSONS_FFMPEG_DIR}/config_android_aarch64.h" art_config)
    foreach(flag CONFIG_VP6_DECODER CONFIG_H264CHROMA CONFIG_HPELDSP CONFIG_HUFFMAN
                 CONFIG_VIDEODSP CONFIG_VP3DSP CONFIG_VP56DSP)
        string(REGEX REPLACE "#define ${flag} 0\n" "#define ${flag} 1\n" art_config "${art_config}")
    endforeach()
    foreach(flag ARCH_AARCH64 HAVE_NEON HAVE_ARMV8 HAVE_VFP HAVE_INLINE_ASM)
        string(REGEX REPLACE "#define ${flag} 1\n" "#define ${flag} 0\n" art_config "${art_config}")
    endforeach()
    file(WRITE "${SIMPSONS_ART_CONFIG}/config.h.new" "${art_config}")
    file(COPY_FILE "${SIMPSONS_ART_CONFIG}/config.h.new" "${SIMPSONS_ART_CONFIG}/config.h"
        ONLY_IF_DIFFERENT)
    file(CONFIGURE OUTPUT "${SIMPSONS_ART_CONFIG}/libavutil/avconfig.h" CONTENT
        "#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif /* AVUTIL_AVCONFIG_H */\n" @ONLY)
    file(CONFIGURE OUTPUT "${SIMPSONS_ART_CONFIG}/libavutil/ffversion.h" CONTENT
        "#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n#define FFMPEG_VERSION \"4.4\"\n#endif /* AVUTIL_FFVERSION_H */\n" @ONLY)
    set(art_sources)
    foreach(name vp6 vp6dsp vp56 vp56data vp56dsp vp56rac h264chroma hpeldsp huffman videodsp vp3dsp)
        list(APPEND art_sources "${SIMPSONS_FFMPEG_DIR}/libavcodec/${name}.c")
    endforeach()
    add_library(simpsons_vp6 STATIC ${art_sources})
    target_include_directories(simpsons_vp6 BEFORE PRIVATE "${SIMPSONS_ART_CONFIG}")
    target_include_directories(simpsons_vp6 PRIVATE "${SIMPSONS_FFMPEG_DIR}"
        "${SIMPSONS_FFMPEG_DIR}/compat/atomics/gcc")
    target_compile_definitions(simpsons_vp6 PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
    # FFmpeg relies on dead code elimination for the disabled architectures.
    target_compile_options(simpsons_vp6 PRIVATE -w -O2)
    set_target_properties(simpsons_vp6 PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_library(simpsons_art SHARED src/android_art.cpp src/modkit_android.cpp)
    target_include_directories(simpsons_art BEFORE PRIVATE "${SIMPSONS_ART_CONFIG}")
    target_include_directories(simpsons_art PRIVATE "${SIMPSONS_FFMPEG_DIR}")
    target_link_libraries(simpsons_art PRIVATE simpsons_vp6 libavcodec libavutil
        "$<LINK_ONLY:simpsons_modkit>")
    set_property(SOURCE src/modkit_android.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
        ${SIMPSONS_MODKIT_INCLUDE_DIRS})
    target_link_options(simpsons_art PRIVATE -Wl,--exclude-libs,ALL)
endif()
