cmake_minimum_required(VERSION 3.25)
project(d3d11_check LANGUAGES CXX)
if(MSVC)
    add_compile_options(/utf-8)
endif()
if(NOT WIN32)
    message(FATAL_ERROR "This check uses the native Windows Direct3D 11 runtime")
endif()
get_filename_component(REXGLUE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk" ABSOLUTE)

add_executable(bytes_equal_check ../bytes_equal_check.cpp)
target_compile_features(bytes_equal_check PRIVATE cxx_std_23)
target_include_directories(bytes_equal_check PRIVATE
    ${REXGLUE_ROOT}/include ${REXGLUE_ROOT}/thirdparty/simde)

add_executable(bytes_equal_avx2_check ../bytes_equal_check.cpp)
target_compile_features(bytes_equal_avx2_check PRIVATE cxx_std_23)
target_include_directories(bytes_equal_avx2_check PRIVATE
    ${REXGLUE_ROOT}/include ${REXGLUE_ROOT}/thirdparty/simde)
if(MSVC)
    target_compile_options(bytes_equal_avx2_check PRIVATE /arch:AVX2)
else()
    target_compile_options(bytes_equal_avx2_check PRIVATE -mavx2)
endif()

add_executable(jump_context_check ../jump_context_check.cpp)
target_compile_features(jump_context_check PRIVATE cxx_std_23)

add_executable(d3d11_resolve_check
    ../d3d11_resolve_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_bytecode.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/buffer_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/profile.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/draw_context.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/render_target_surface.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/render_target_encoder.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/resolve_copy.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_render_target_dump.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_render_target_transfer.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_float.cpp
    ${REXGLUE_ROOT}/src/core/math_msvc.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_compile_features(d3d11_resolve_check PRIVATE cxx_std_23)
target_include_directories(d3d11_resolve_check PRIVATE ${REXGLUE_ROOT}/thirdparty/xxHash
    ${REXGLUE_ROOT}/include ${REXGLUE_ROOT} ${REXGLUE_ROOT}/thirdparty/simde
    ${REXGLUE_ROOT}/thirdparty/fmt/include ${REXGLUE_ROOT}/thirdparty/spdlog/include
)
target_link_libraries(d3d11_resolve_check PRIVATE d3d11 dxgi d3dcompiler dxguid)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_resolve_check PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

add_executable(d3d11_render_target_check
    ../d3d11_render_target_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_bytecode.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/buffer_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/profile.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/draw_context.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/render_target_surface.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/render_target_encoder.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/resolve_copy.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_render_target_dump.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_render_target_transfer.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_float.cpp
    ${REXGLUE_ROOT}/src/core/math_msvc.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_compile_features(d3d11_render_target_check PRIVATE cxx_std_23)
target_include_directories(d3d11_render_target_check PRIVATE ${REXGLUE_ROOT}/thirdparty/xxHash
    ${REXGLUE_ROOT}/include ${REXGLUE_ROOT} ${REXGLUE_ROOT}/thirdparty/simde
    ${REXGLUE_ROOT}/thirdparty/fmt/include ${REXGLUE_ROOT}/thirdparty/spdlog/include
)
target_link_libraries(d3d11_render_target_check PRIVATE d3d11 dxgi d3dcompiler dxguid)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_render_target_check PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

add_executable(d3d11_texture_swizzle_check
    ../d3d11_texture_swizzle_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_bytecode.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/draw_context.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/profile.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_compile_features(d3d11_texture_swizzle_check PRIVATE cxx_std_23)
target_include_directories(d3d11_texture_swizzle_check PRIVATE
    ${REXGLUE_ROOT}/include ${REXGLUE_ROOT} ${REXGLUE_ROOT}/thirdparty/simde)
target_link_libraries(d3d11_texture_swizzle_check PRIVATE d3d11 dxgi d3dcompiler dxguid)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_texture_swizzle_check PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(d3d11_texture_upload_check
    ../d3d11_texture_upload_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/buffer_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/profile.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/draw_context.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/texture_upload.cpp
)
target_compile_features(d3d11_texture_upload_check PRIVATE cxx_std_23)
target_include_directories(d3d11_texture_upload_check PRIVATE ${REXGLUE_ROOT}/thirdparty/xxHash
    ${REXGLUE_ROOT}/thirdparty/simde ${REXGLUE_ROOT}/include)
target_link_libraries(d3d11_texture_upload_check PRIVATE d3d11 dxgi d3dcompiler)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_texture_upload_check PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(d3d11_draw_check
    ../d3d11_draw_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_bytecode.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/buffer_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/profile.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/sampler_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/draw_context.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_compile_features(d3d11_draw_check PRIVATE cxx_std_23)
target_include_directories(d3d11_draw_check PRIVATE ${REXGLUE_ROOT}/thirdparty/xxHash
    ${REXGLUE_ROOT}/include
    ${REXGLUE_ROOT}
    ${REXGLUE_ROOT}/thirdparty/simde
)
target_link_libraries(d3d11_draw_check PRIVATE d3d11 dxgi d3dcompiler dxguid)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_draw_check PRIVATE -Wall -Wextra -Werror)
endif()
add_executable(d3d11_check
    ../d3d11_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_swap_chain.cpp
)
target_compile_features(d3d11_check PRIVATE cxx_std_23)
target_include_directories(d3d11_check PRIVATE ${REXGLUE_ROOT}/include)
target_link_libraries(d3d11_check PRIVATE d3d11 dxgi d3dcompiler)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_check PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(d3d11_shader_check
    ../d3d11_shader_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_bytecode.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_cache.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_compile_features(d3d11_shader_check PRIVATE cxx_std_23)
target_include_directories(d3d11_shader_check PRIVATE
    ${REXGLUE_ROOT}/include
    ${REXGLUE_ROOT}
    ${REXGLUE_ROOT}/thirdparty/simde
    ${REXGLUE_ROOT}/thirdparty/fmt/include
    ${REXGLUE_ROOT}/thirdparty/spdlog/include
)
target_link_libraries(d3d11_shader_check PRIVATE d3d11 dxgi d3dcompiler dxguid)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_shader_check PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

# Compile the production shader preparation and common geometry generator too.
# Their runtime integration is separate from the standalone GPU checks.
add_library(d3d11_shader_preparation OBJECT
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_image_renderer.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_presenter.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_immediate_drawer.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_provider.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/pipeline_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/draw_state.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/guest_sampler.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/primitive_processor.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/system_constants.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/guest_draw.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/profile.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/command_processor.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/graphics_system.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shared_memory.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/scaled_resolve_memory.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/texture_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/render_target_surface.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_geometry.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/render_target_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/resolve_copy.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/render_target_encoder.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_render_target_dump.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_render_target_transfer.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_float.cpp
)
target_compile_features(d3d11_shader_preparation PRIVATE cxx_std_23)
target_include_directories(d3d11_shader_preparation PRIVATE
    ${REXGLUE_ROOT}/include
    ${REXGLUE_ROOT}
    ${REXGLUE_ROOT}/thirdparty/simde
    ${REXGLUE_ROOT}/thirdparty/fmt/include
    ${REXGLUE_ROOT}/thirdparty/spdlog/include
    ${REXGLUE_ROOT}/thirdparty/xxHash
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_shader_preparation PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # TextureKey intentionally copies its packed representation in the common
    # SDK header. Keep all warnings in the native implementation as errors.
    target_compile_options(d3d11_shader_preparation PRIVATE -Wno-nontrivial-memcall)
endif()

add_executable(d3d11_kernel_check
    ../d3d11_kernel_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_bytecode.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_cache.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_compile_features(d3d11_kernel_check PRIVATE cxx_std_23)
target_include_directories(d3d11_kernel_check PRIVATE
    ${REXGLUE_ROOT}/include
    ${REXGLUE_ROOT}
    ${REXGLUE_ROOT}/thirdparty/simde
)
target_link_libraries(d3d11_kernel_check PRIVATE d3d11 dxgi d3dcompiler dxguid)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_kernel_check PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(dxbc_geometry
    ../../shaderkit/dxbc_geometry.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_geometry.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    ${REXGLUE_ROOT}/src/core/math_msvc.cpp
)
target_compile_features(dxbc_geometry PRIVATE cxx_std_23)
target_include_directories(dxbc_geometry PRIVATE
    ${REXGLUE_ROOT}/include
    ${REXGLUE_ROOT}
    ${REXGLUE_ROOT}/thirdparty/simde
    ${REXGLUE_ROOT}/thirdparty/fmt/include
    ${REXGLUE_ROOT}/thirdparty/spdlog/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dxbc_geometry PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

add_executable(dxbc_transfer
    ../../shaderkit/dxbc_transfer.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_render_target_transfer.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_float.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    ${REXGLUE_ROOT}/src/core/math_msvc.cpp
)
target_compile_features(dxbc_transfer PRIVATE cxx_std_23)
target_include_directories(dxbc_transfer PRIVATE
    ${REXGLUE_ROOT}/include ${REXGLUE_ROOT} ${REXGLUE_ROOT}/thirdparty/simde
    ${REXGLUE_ROOT}/thirdparty/fmt/include ${REXGLUE_ROOT}/thirdparty/spdlog/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dxbc_transfer PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

add_executable(dxbc_dump
    ../../shaderkit/dxbc_dump.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_render_target_dump.cpp
    ${REXGLUE_ROOT}/src/graphics/util/dxbc_float.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    ${REXGLUE_ROOT}/src/core/math_msvc.cpp
)
target_compile_features(dxbc_dump PRIVATE cxx_std_23)
target_include_directories(dxbc_dump PRIVATE
    ${REXGLUE_ROOT}/include ${REXGLUE_ROOT} ${REXGLUE_ROOT}/thirdparty/simde
    ${REXGLUE_ROOT}/thirdparty/fmt/include ${REXGLUE_ROOT}/thirdparty/spdlog/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dxbc_dump PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

add_executable(d3d11_buffer_check
    ../d3d11_buffer_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/buffer_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/profile.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/draw_context.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/scaled_resolve_memory.cpp
)
target_compile_features(d3d11_buffer_check PRIVATE cxx_std_23)
target_include_directories(d3d11_buffer_check PRIVATE ${REXGLUE_ROOT}/thirdparty/xxHash
    ${REXGLUE_ROOT}/thirdparty/simde ${REXGLUE_ROOT}/include)
target_link_libraries(d3d11_buffer_check PRIVATE d3d11 dxgi d3dcompiler)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_buffer_check PRIVATE -Wall -Wextra -Werror)
endif()

# LLVM-MinGW's libc++ lacks the clock conversion templates used by the SDK's
# custom clocks. Supply their explicit conversions for this compile check;
# Windows builds with MSVC's chrono implementation need no compatibility.
if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(d3d11_shader_preparation PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/chrono_compat.h)
endif()

add_executable(d3d11_present_check
    ../d3d11_present_check.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_device.cpp
    ${REXGLUE_ROOT}/src/ui/d3d11/d3d11_image_renderer.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/buffer_cache.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/profile.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_bytecode.cpp
    ${REXGLUE_ROOT}/src/graphics/d3d11/shader_cache.cpp
    ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_compile_features(d3d11_present_check PRIVATE cxx_std_23)
target_include_directories(d3d11_present_check PRIVATE ${REXGLUE_ROOT}/thirdparty/xxHash
    ${REXGLUE_ROOT}/include ${REXGLUE_ROOT} ${REXGLUE_ROOT}/thirdparty/simde)
target_link_libraries(d3d11_present_check PRIVATE d3d11 dxgi d3dcompiler dxguid)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(d3d11_present_check PRIVATE -Wall -Wextra -Werror)
endif()
