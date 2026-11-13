cmake_minimum_required(VERSION 3.25)
project(SimpsonsModkit VERSION 0.1.0 LANGUAGES C CXX)

set(MODKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MODKIT_JSON_INCLUDE "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/inja/third_party/include")

add_library(simpsons_modkit_sdk INTERFACE)
add_library(simpsons::modkit_sdk ALIAS simpsons_modkit_sdk)
target_include_directories(simpsons_modkit_sdk INTERFACE
    "${MODKIT_ROOT}/include" "${MODKIT_JSON_INCLUDE}")
target_compile_features(simpsons_modkit_sdk INTERFACE cxx_std_20)

option(SIMPSONS_MODKIT_RUNTIME "Build the mod runtime" ON)
option(SIMPSONS_MODKIT_TESTS "Build the isolated modkit checks" OFF)
option(SIMPSONS_MODKIT_TOOLS "Build the creator asset tools" ${PROJECT_IS_TOP_LEVEL})
option(SIMPSONS_MODKIT_REFERENCE_CHECKS "Build native checks against the generated game routines" OFF)
if(SIMPSONS_MODKIT_RUNTIME)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(api_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_custom_command(OUTPUT "${api_generated}/api_catalog.h"
        COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/generate_api.py"
            --source "${MODKIT_ROOT}/api.json" --output "${api_generated}"
        DEPENDS api.json "${MODKIT_ROOT}/../tools/modkit/generate_api.py"
        VERBATIM)
    file(GLOB lua_sources CONFIGURE_DEPENDS "thirdparty/lua/*.c")
    set_source_files_properties(${lua_sources} PROPERTIES LANGUAGE CXX)
    add_library(simpsons_modkit_lua STATIC ${lua_sources})
    set_target_properties(simpsons_modkit_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(simpsons_modkit_lua PUBLIC "${MODKIT_ROOT}/thirdparty/lua")
    target_compile_definitions(simpsons_modkit_lua PRIVATE SIMPSONS_BUILD_LUA)
    if(MSVC)
        target_compile_options(simpsons_modkit_lua PRIVATE "/FI${MODKIT_ROOT}/src/lua_cxx.h")
    else()
        target_compile_options(simpsons_modkit_lua PRIVATE -include "${MODKIT_ROOT}/src/lua_cxx.h")
    endif()
    if(UNIX)
        target_compile_definitions(simpsons_modkit_lua PRIVATE LUA_USE_POSIX)
        target_link_libraries(simpsons_modkit_lua PRIVATE m)
    endif()
    add_library(simpsons_modkit STATIC src/manifest.cpp src/source.cpp src/runtime.cpp src/runtime_tools.cpp src/missions.cpp src/project_documents.cpp src/input_state.cpp src/resources.cpp src/camera.cpp src/models.cpp src/meshes.cpp src/textures.cpp src/entities.cpp src/scenes.cpp src/developer.cpp src/storage.cpp src/streams.cpp src/refpack_encoder.cpp src/stream_catalog.cpp src/localization.cpp src/assets.cpp src/text_table.cpp
        src/lua_mod.cpp src/native_mod.cpp
        "${api_generated}/api_catalog.h"
        "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/crypto/sha256.cpp")
    set_target_properties(simpsons_modkit PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(simpsons_modkit PUBLIC simpsons_modkit_sdk
        PRIVATE simpsons_modkit_lua ${CMAKE_DL_LIBS})
    target_include_directories(simpsons_modkit PUBLIC "${MODKIT_ROOT}/src")
    target_include_directories(simpsons_modkit PRIVATE
        "${api_generated}"
        "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/crypto")
    if(WIN32)
        target_include_directories(simpsons_modkit PRIVATE "${MODKIT_ROOT}/compat")
    endif()
    if(SIMPSONS_MODKIT_TOOLS)
        add_executable(modkit_streams src/stream_tool.cpp)
        target_link_libraries(modkit_streams PRIVATE simpsons_modkit)
        if(MINGW)
            target_link_options(modkit_streams PRIVATE -municode)
        endif()
    endif()
    if(SIMPSONS_MODKIT_REFERENCE_CHECKS)
        set(input_reference_source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.78.cpp")
        add_custom_command(OUTPUT "${api_generated}/native_input.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/input_reference.py"
                --source "${input_reference_source}" --output "${api_generated}/native_input.h"
            DEPENDS "${input_reference_source}" "${MODKIT_ROOT}/../tools/modkit/input_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        set(reference_source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.49.cpp")
        add_custom_command(OUTPUT "${api_generated}/native_refpack.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
                --source "${reference_source}" --output "${api_generated}/native_refpack.h"
            DEPENDS "${reference_source}" "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.65.cpp"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_refpack_check src/reference_check.cpp "${api_generated}/native_refpack.h")
        target_compile_features(modkit_refpack_check PRIVATE cxx_std_23)
        if(NOT MSVC)
            # Real archive checks decode gigabytes. Keep the reference's byte
            # loop practical even in a developer's Debug build.
            target_compile_options(modkit_refpack_check PRIVATE -O2)
        endif()
        target_link_libraries(modkit_refpack_check PRIVATE simpsons_modkit)
        target_include_directories(modkit_refpack_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        if(MINGW)
            target_link_options(modkit_refpack_check PRIVATE -municode)
        endif()
    endif()
endif()

if(SIMPSONS_MODKIT_TESTS)
    enable_testing()
    add_executable(modkit_check tests/runtime_check.cpp)
    target_link_libraries(modkit_check PRIVATE simpsons_modkit)
    add_library(modkit_fixture MODULE tests/native_fixture.cpp)
    target_link_libraries(modkit_fixture PRIVATE simpsons_modkit_sdk)
    set_target_properties(modkit_fixture PROPERTIES PREFIX "")
    add_test(NAME modkit_runtime COMMAND modkit_check $<TARGET_FILE:modkit_fixture>)
    add_executable(modkit_input_check tests/input_check.cpp)
    target_link_libraries(modkit_input_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_input COMMAND modkit_input_check "${MODKIT_ROOT}/examples/input-observer")
    add_executable(modkit_library_example_check tests/library_example_check.cpp)
    target_link_libraries(modkit_library_example_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_library_example COMMAND modkit_library_example_check "${MODKIT_ROOT}/examples/library-objects")
    add_executable(modkit_entities_check tests/entities_check.cpp)
    target_link_libraries(modkit_entities_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_entities COMMAND modkit_entities_check)
    add_executable(modkit_combat_project_check tests/combat_project_check.cpp)
    target_link_libraries(modkit_combat_project_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_combat_project COMMAND modkit_combat_project_check "${MODKIT_ROOT}/examples/combat-workshop")
    add_executable(modkit_streams_check tests/streams_check.cpp)
    target_link_libraries(modkit_streams_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_streams COMMAND modkit_streams_check)
    add_executable(modkit_source_state_check tests/source_state_check.cpp)
    target_link_libraries(modkit_source_state_check PRIVATE simpsons_modkit_sdk)
    target_include_directories(modkit_source_state_check PRIVATE "${MODKIT_ROOT}/src")
    add_test(NAME modkit_source_state COMMAND modkit_source_state_check)
    add_executable(modkit_source_leases_check tests/source_leases_check.cpp)
    target_link_libraries(modkit_source_leases_check PRIVATE simpsons_modkit_sdk)
    target_include_directories(modkit_source_leases_check PRIVATE "${MODKIT_ROOT}/src")
    add_test(NAME modkit_source_leases COMMAND modkit_source_leases_check)
    add_executable(modkit_scenes_check tests/scenes_check.cpp)
    target_link_libraries(modkit_scenes_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_scenes COMMAND modkit_scenes_check)
    add_executable(modkit_project_documents_check tests/project_documents_check.cpp)
    target_link_libraries(modkit_project_documents_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_project_documents COMMAND modkit_project_documents_check)
    add_executable(modkit_mission_editor_check tests/mission_editor_check.cpp)
    target_link_libraries(modkit_mission_editor_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_mission_editor COMMAND modkit_mission_editor_check "${MODKIT_ROOT}/../launcher/ui/modkit-templates.json")
    add_executable(modkit_refpack_native_check tests/refpack_check.cpp)
    target_link_libraries(modkit_refpack_native_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_refpack_native COMMAND modkit_refpack_native_check)
    add_executable(modkit_developer_check tests/developer_check.cpp)
    target_link_libraries(modkit_developer_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_developer COMMAND modkit_developer_check)
    add_executable(modkit_missions_check tests/missions_check.cpp)
    target_link_libraries(modkit_missions_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_missions COMMAND modkit_missions_check "${MODKIT_ROOT}/examples/mission-walk")
    add_executable(modkit_resources_check tests/resources_check.cpp)
    target_link_libraries(modkit_resources_check PRIVATE simpsons_modkit)
    add_executable(modkit_character_features_check tests/character_features_check.cpp)
    target_link_libraries(modkit_character_features_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_character_features COMMAND modkit_character_features_check
        "${MODKIT_ROOT}/../launcher/ui/bundled-mods/euphoria.ragdolls")
    add_executable(modkit_camera_check tests/camera_check.cpp)
    target_link_libraries(modkit_camera_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_camera COMMAND modkit_camera_check)
    add_executable(modkit_camera_project_check tests/camera_project_check.cpp)
    target_link_libraries(modkit_camera_project_check PRIVATE simpsons_modkit)
    add_test(NAME modkit_camera_project COMMAND modkit_camera_project_check "${MODKIT_ROOT}/examples/camera-workshop")
    add_library(modkit_fault_fixture MODULE tests/native_fault_fixture.cpp)
    target_link_libraries(modkit_fault_fixture PRIVATE simpsons_modkit_sdk)
    set_target_properties(modkit_fault_fixture PROPERTIES PREFIX "")
    add_test(NAME modkit_resources COMMAND modkit_resources_check $<TARGET_FILE:modkit_fault_fixture>)
    add_executable(modkit_reload_check tests/reload_check.cpp)
    target_link_libraries(modkit_reload_check PRIVATE simpsons_modkit)
    foreach(version RANGE 1 2)
        add_library(modkit_reload_fixture_${version} MODULE tests/native_reload_fixture.cpp)
        target_link_libraries(modkit_reload_fixture_${version} PRIVATE simpsons_modkit_sdk)
        target_compile_definitions(modkit_reload_fixture_${version} PRIVATE FIXTURE_VERSION=${version})
        set_target_properties(modkit_reload_fixture_${version} PROPERTIES PREFIX "")
    endforeach()
    add_library(modkit_reload_unsafe_fixture MODULE tests/native_reload_fixture.cpp)
    target_link_libraries(modkit_reload_unsafe_fixture PRIVATE simpsons_modkit_sdk)
    target_compile_definitions(modkit_reload_unsafe_fixture PRIVATE FIXTURE_UNSAFE_SAVE=1)
    set_target_properties(modkit_reload_unsafe_fixture PROPERTIES PREFIX "")
    add_test(NAME modkit_reload COMMAND modkit_reload_check
        $<TARGET_FILE:modkit_reload_fixture_1> $<TARGET_FILE:modkit_reload_fixture_2>
        $<TARGET_FILE:modkit_reload_unsafe_fixture>)
    if(SIMPSONS_MODKIT_REFERENCE_CHECKS)
        set(entity_reference_source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.36.cpp")
        add_custom_command(OUTPUT "${api_generated}/native_spawn.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/spawn_reference.py"
                --source "${entity_reference_source}" --output "${api_generated}/native_spawn.h"
            DEPENDS "${entity_reference_source}"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.41.cpp"
                "${MODKIT_ROOT}/../tools/modkit/spawn_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_spawn_templates_check tests/spawn_templates_check.cpp
            "${api_generated}/native_spawn.h")
        target_compile_features(modkit_spawn_templates_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_spawn_templates_check PRIVATE simpsons_modkit_sdk)
        target_include_directories(modkit_spawn_templates_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_spawn_templates COMMAND modkit_spawn_templates_check)
        add_custom_command(OUTPUT "${api_generated}/native_definitions.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/definition_reference.py"
                --source "${entity_reference_source}" --output "${api_generated}/native_definitions.h"
            DEPENDS "${entity_reference_source}"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.41.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.65.cpp"
                "${MODKIT_ROOT}/../tools/modkit/definition_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_compiled_objects_check tests/compiled_objects_check.cpp
            "${api_generated}/native_definitions.h")
        target_compile_features(modkit_compiled_objects_check PRIVATE cxx_std_23)
        target_include_directories(modkit_compiled_objects_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_compiled_objects COMMAND modkit_compiled_objects_check)
        add_custom_command(OUTPUT "${api_generated}/native_camera.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/camera_reference.py"
                --source "${entity_reference_source}" --output "${api_generated}/native_camera.h"
            DEPENDS "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.40.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.65.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.13.cpp"
                "${MODKIT_ROOT}/../tools/modkit/camera_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_camera_projection_check tests/camera_projection_check.cpp
            "${api_generated}/native_camera.h")
        target_compile_features(modkit_camera_projection_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_camera_projection_check PRIVATE simpsons_modkit)
        target_include_directories(modkit_camera_projection_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_camera_projection COMMAND modkit_camera_projection_check)
        add_custom_command(OUTPUT "${api_generated}/native_model.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/model_reference.py"
                --source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.40.cpp"
                --output "${api_generated}/native_model.h"
            DEPENDS "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.40.cpp"
                "${MODKIT_ROOT}/../tools/modkit/model_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_models_check tests/models_check.cpp "${api_generated}/native_model.h"
            "${MODKIT_ROOT}/../simpsons/src/modkit_models_hook.cpp")
        target_compile_features(modkit_models_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_models_check PRIVATE simpsons_modkit)
        target_include_directories(modkit_models_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_models COMMAND modkit_models_check)
        add_custom_command(OUTPUT "${api_generated}/native_mesh.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/mesh_reference.py"
                --source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.49.cpp"
                --output "${api_generated}/native_mesh.h"
            DEPENDS "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.49.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.65.cpp"
                "${MODKIT_ROOT}/../tools/modkit/mesh_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_meshes_check tests/meshes_check.cpp "${api_generated}/native_mesh.h")
        target_compile_features(modkit_meshes_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_meshes_check PRIVATE simpsons_modkit)
        target_include_directories(modkit_meshes_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_meshes COMMAND modkit_meshes_check)
        add_custom_command(OUTPUT "${api_generated}/native_texture.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/texture_reference.py"
                --source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.41.cpp"
                --output "${api_generated}/native_texture.h"
            DEPENDS "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.41.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.16.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.83.cpp"
                "${MODKIT_ROOT}/../tools/modkit/texture_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_textures_check tests/textures_check.cpp "${api_generated}/native_texture.h")
        target_compile_features(modkit_textures_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_textures_check PRIVATE simpsons_modkit)
        target_include_directories(modkit_textures_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_textures COMMAND modkit_textures_check)
        add_custom_command(OUTPUT "${api_generated}/native_entity.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/entity_reference.py"
                --source "${entity_reference_source}" --output "${api_generated}/native_entity.h"
            DEPENDS "${entity_reference_source}" "${MODKIT_ROOT}/../tools/modkit/entity_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_entity_game_check tests/entity_game_check.cpp
            "${api_generated}/native_entity.h")
        target_compile_features(modkit_entity_game_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_entity_game_check PRIVATE simpsons_modkit_sdk)
        target_include_directories(modkit_entity_game_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_entity_game COMMAND modkit_entity_game_check)
        add_custom_command(OUTPUT "${api_generated}/native_transform.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/transform_reference.py"
                --source "${entity_reference_source}" --output "${api_generated}/native_transform.h"
            DEPENDS "${entity_reference_source}" "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.13.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.52.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.65.cpp"
                "${MODKIT_ROOT}/../tools/modkit/transform_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_entity_transform_check tests/entity_transform_check.cpp
            "${MODKIT_ROOT}/../simpsons/src/entity_transform.cpp" "${api_generated}/native_transform.h")
        target_compile_features(modkit_entity_transform_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_entity_transform_check PRIVATE simpsons_modkit_sdk)
        target_include_directories(modkit_entity_transform_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_entity_transform COMMAND modkit_entity_transform_check)
        set(health_reference_source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.58.cpp")
        add_custom_command(OUTPUT "${api_generated}/native_health.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/health_reference.py"
                --source "${health_reference_source}" --output "${api_generated}/native_health.h"
            DEPENDS "${health_reference_source}" "${MODKIT_ROOT}/../tools/modkit/health_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.11.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.36.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.41.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.52.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.57.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.65.cpp"
            VERBATIM)
        add_executable(modkit_entity_health_check tests/entity_health_check.cpp
            "${MODKIT_ROOT}/../simpsons/src/entity_health.cpp"
            "${MODKIT_ROOT}/../simpsons/src/entity_transform.cpp" "${api_generated}/native_health.h")
        target_compile_features(modkit_entity_health_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_entity_health_check PRIVATE simpsons_modkit_sdk)
        target_include_directories(modkit_entity_health_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_entity_health COMMAND modkit_entity_health_check)
        add_custom_command(OUTPUT "${api_generated}/native_render.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/render_reference.py"
                --source "${entity_reference_source}" --output "${api_generated}/native_render.h"
            DEPENDS "${entity_reference_source}" "${MODKIT_ROOT}/../tools/modkit/render_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.37.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.65.cpp"
            VERBATIM)
        add_executable(modkit_entity_render_check tests/entity_render_check.cpp
            "${MODKIT_ROOT}/../simpsons/src/entity_render.cpp" "${api_generated}/native_render.h")
        target_compile_features(modkit_entity_render_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_entity_render_check PRIVATE simpsons_modkit_sdk)
        target_include_directories(modkit_entity_render_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_entity_render COMMAND modkit_entity_render_check)
        add_custom_command(OUTPUT "${api_generated}/native_resource.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/resource_reference.py"
                --source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.40.cpp"
                --output "${api_generated}/native_resource.h"
            DEPENDS "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.40.cpp"
                "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.41.cpp"
                "${MODKIT_ROOT}/../tools/modkit/resource_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_entity_assets_check tests/entity_assets_check.cpp
            "${MODKIT_ROOT}/../simpsons/src/entity_assets.cpp"
            "${MODKIT_ROOT}/../simpsons/src/entity_assets_hook.cpp" "${api_generated}/native_resource.h")
        target_compile_features(modkit_entity_assets_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_entity_assets_check PRIVATE simpsons_modkit_sdk)
        target_include_directories(modkit_entity_assets_check PRIVATE "${api_generated}" "${MODKIT_ROOT}/src"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_entity_assets COMMAND modkit_entity_assets_check)
        set(player_reference_source "${MODKIT_ROOT}/../simpsons/generated/default/simpsons_recomp.52.cpp")
        add_custom_command(OUTPUT "${api_generated}/native_player.h"
            COMMAND "${Python3_EXECUTABLE}" "${MODKIT_ROOT}/../tools/modkit/player_reference.py"
                --source "${player_reference_source}" --output "${api_generated}/native_player.h"
            DEPENDS "${player_reference_source}" "${MODKIT_ROOT}/../tools/modkit/player_reference.py"
                "${MODKIT_ROOT}/../tools/modkit/native_reference.py"
            VERBATIM)
        add_executable(modkit_player_control_check tests/player_control_check.cpp
            "${api_generated}/native_player.h")
        target_compile_features(modkit_player_control_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_player_control_check PRIVATE simpsons_modkit_sdk)
        target_include_directories(modkit_player_control_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_player_control COMMAND modkit_player_control_check)
        add_executable(modkit_game_input_check tests/game_input_check.cpp
            "${MODKIT_ROOT}/../simpsons/src/modkit_input.cpp" "${api_generated}/native_input.h")
        target_compile_features(modkit_game_input_check PRIVATE cxx_std_23)
        target_link_libraries(modkit_game_input_check PRIVATE simpsons_modkit)
        target_include_directories(modkit_game_input_check PRIVATE "${api_generated}"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/include"
            "${MODKIT_ROOT}/../tools/rexglue-sdk/thirdparty/simde")
        add_test(NAME modkit_game_input COMMAND modkit_game_input_check)
        add_test(NAME modkit_game_refpack COMMAND modkit_refpack_check --self-test
            "${CMAKE_CURRENT_BINARY_DIR}/refpack-self-test.json")
    endif()
endif()
