# rexgraphics: GPU emulation core (ported from Xenia)
# Namespace: rex::graphics

# Core GPU types - minimal dependencies, fully ported
set(REXGRAPHICS_CORE_SOURCES
    flags.cpp
    xenos.cpp
    pipeline/texture/info.cpp
    pipeline/texture/info_formats.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp
)

# GPU system files - full backend
set(REXGRAPHICS_SYSTEM_SOURCES
    graphics_system.cpp
    command_processor.cpp
    native_records.cpp
    trace_writer.cpp
    pipeline/texture/extent.cpp
    pipeline/texture/util.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/texture/conversion.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGRAPHICS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGRAPHICS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/gpu_profiler.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# Shared Direct3D shader translation
if(REXGLUE_USE_D3D11 OR REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGRAPHICS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGRAPHICS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
        util/dxbc_geometry.cpp
    )

endif()

# D3D12 GPU backend
if(REXGLUE_USE_D3D12)
    set(REXGRAPHICS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

# Native Direct3D 11 backend
if(REXGLUE_USE_D3D11)
    set(REXGRAPHICS_D3D11_SOURCES
        d3d11/buffer_cache.cpp
        d3d11/command_processor.cpp
        d3d11/draw_context.cpp
        d3d11/draw_state.cpp
        d3d11/graphics_system.cpp
        d3d11/guest_draw.cpp
        d3d11/guest_sampler.cpp
        d3d11/pipeline_cache.cpp
        d3d11/primitive_processor.cpp
        d3d11/profile.cpp
        d3d11/render_target_cache.cpp
        d3d11/render_target_encoder.cpp
        d3d11/render_target_surface.cpp
        d3d11/resolve_copy.cpp
        d3d11/sampler_cache.cpp
        d3d11/scaled_resolve_memory.cpp
        d3d11/shader_bytecode.cpp
        d3d11/shader_cache.cpp
        d3d11/shared_memory.cpp
        d3d11/system_constants.cpp
        d3d11/texture_cache.cpp
        d3d11/texture_upload.cpp
        util/dxbc_float.cpp
        util/dxbc_render_target_dump.cpp
        util/dxbc_render_target_transfer.cpp
    )
endif()

# Combine sources
set(REXGRAPHICS_SOURCES
    ${REXGRAPHICS_CORE_SOURCES}
    ${REXGRAPHICS_SYSTEM_SOURCES}
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGRAPHICS_SOURCES ${REXGRAPHICS_SPIRV_SOURCES} ${REXGRAPHICS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D11 OR REXGLUE_USE_D3D12)
    list(APPEND REXGRAPHICS_SOURCES ${REXGRAPHICS_DXBC_THIRDPARTY_SOURCES} ${REXGRAPHICS_DXBC_SOURCES})
endif()
if(REXGLUE_USE_D3D11)
    list(APPEND REXGRAPHICS_SOURCES ${REXGRAPHICS_D3D11_SOURCES})
endif()
if(REXGLUE_USE_D3D12)
    list(APPEND REXGRAPHICS_SOURCES ${REXGRAPHICS_D3D12_SOURCES})
endif()

add_library(rexgraphics OBJECT ${REXGRAPHICS_SOURCES})
add_library(rex::graphics ALIAS rexgraphics)

# A hash of the shader translator's sources, which sets of shaders exported
# from runtime translations record and are checked against.
file(GLOB REX_TRANSLATOR_HASH_INPUTS
    ${CMAKE_CURRENT_SOURCE_DIR}/pipeline/shader/*.cpp
    ${REXGLUE_ROOT}/include/rex/graphics/pipeline/shader/*.h
)
list(APPEND REX_TRANSLATOR_HASH_INPUTS
    ${CMAKE_CURRENT_SOURCE_DIR}/format/ucode.cpp
    ${REXGLUE_ROOT}/include/rex/graphics/format/ucode.h
    ${REXGLUE_ROOT}/include/rex/graphics/xenos.h
)
string(REPLACE ";" "|" REX_TRANSLATOR_HASH_INPUTS_ARG "${REX_TRANSLATOR_HASH_INPUTS}")
set(REX_TRANSLATOR_HASH_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/rex/graphics/translator_hash.h)
add_custom_command(
    OUTPUT ${REX_TRANSLATOR_HASH_HEADER}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${REX_TRANSLATOR_HASH_HEADER}
            "-DINPUTS=${REX_TRANSLATOR_HASH_INPUTS_ARG}"
            -P ${REXGLUE_ROOT}/cmake/translator_hash.cmake
    DEPENDS ${REX_TRANSLATOR_HASH_INPUTS} ${REXGLUE_ROOT}/cmake/translator_hash.cmake
    COMMENT "Hashing the shader translator sources"
    VERBATIM
)
target_sources(rexgraphics PRIVATE ${REX_TRANSLATOR_HASH_HEADER})
target_include_directories(rexgraphics PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_include_directories(rexgraphics PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexgraphics PRIVATE
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
)

target_compile_options(rexgraphics PRIVATE -Wno-unused-variable)

target_link_libraries(rexgraphics PUBLIC
    rexcore
    rexui
    xxHash::xxhash
    snappy
)

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgraphics PUBLIC
        volk::volk
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator
    )
    # The presenter (rexui) uses DXGI for the swapchain/display on Windows
    # regardless of the graphics backend.
    if(WIN32)
        target_link_libraries(rexgraphics PUBLIC dxgi)
    endif()
    target_link_libraries(rexgraphics PRIVATE
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
    target_compile_definitions(rexgraphics PUBLIC REX_HAS_VULKAN=1)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexgraphics PUBLIC
        d3d12
        dxgi
        dxguid
    )
    target_compile_definitions(rexgraphics PUBLIC REX_HAS_D3D12=1)
endif()

if(REXGLUE_USE_D3D11)
    target_link_libraries(rexgraphics PUBLIC d3d11 dxgi d3dcompiler dxguid)
    target_compile_definitions(rexgraphics PUBLIC REX_HAS_D3D11=1)
endif()
